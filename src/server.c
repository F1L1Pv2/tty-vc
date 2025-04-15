#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <errno.h>
#include "common.h"

#define MAX_CLIENTS 10
#define MAX_EVENTS 64
#define READ_BUFFER_SIZE 4096

typedef struct {
    int fd;
    struct sockaddr_in addr;
    bool active;
    pthread_mutex_t write_mutex;
} Client;

typedef struct {
    Client* clients[MAX_CLIENTS];
    int count;
} ClientList;


struct ThreadData {
    int sender_fd;
    unsigned char* data;
    size_t data_size;
    ClientList recipients;
};

// Custom thread-safe queue implementation
struct QueueNode {
    struct ThreadData* data;
    struct QueueNode* next;
};

struct ThreadSafeQueue {
    struct QueueNode* head;
    struct QueueNode* tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    size_t size;
};

// Global variables
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
Client clients[MAX_CLIENTS];
struct ThreadSafeQueue work_queue;
bool server_running = true;

// Queue functions
void queue_init(struct ThreadSafeQueue* q) {
    q->head = q->tail = NULL;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    q->size = 0;
}

void queue_destroy(struct ThreadSafeQueue* q) {
    pthread_mutex_lock(&q->mutex);
    struct QueueNode* current = q->head;
    while (current) {
        struct QueueNode* next = current->next;
        free(current->data->data);
        free(current->data);
        free(current);
        current = next;
    }
    pthread_mutex_unlock(&q->mutex);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

void enqueue(struct ThreadSafeQueue* q, struct ThreadData* item) {
    struct QueueNode* node = malloc(sizeof(struct QueueNode));
    if (!node) {
        perror("Failed to allocate queue node");
        return;
    }
    node->data = item;
    node->next = NULL;

    pthread_mutex_lock(&q->mutex);
    
    if (q->tail) {
        q->tail->next = node;
    } else {
        q->head = node;
    }
    q->tail = node;
    q->size++;
    
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

struct ThreadData* dequeue(struct ThreadSafeQueue* q) {
    pthread_mutex_lock(&q->mutex);
    
    while (q->size == 0 && server_running) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }

    if (!server_running && q->size == 0) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }
    
    struct QueueNode* node = q->head;
    struct ThreadData* data = node->data;
    
    q->head = node->next;
    if (!q->head) {
        q->tail = NULL;
    }
    q->size--;
    
    pthread_mutex_unlock(&q->mutex);
    
    free(node);
    return data;
}

// Client management
ClientList get_recipients(int exclude_fd) {
    ClientList list = {0};
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].fd != exclude_fd) {
            list.clients[list.count++] = &clients[i];
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return list;
}

void remove_client(int fd) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd == fd) {
            close(fd);
            pthread_mutex_destroy(&clients[i].write_mutex);
            clients[i].active = false;
            printf("Client disconnected (fd: %d)\n", fd);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Network utilities
ssize_t send_all(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, (char*)buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return n;
        }
        sent += n;
    }
    return sent;
}

// Worker thread function
void* worker_thread_func(void* arg) {
    (void)arg;
    while (server_running) {
        struct ThreadData* data = dequeue(&work_queue);
        if (!data) break;

        // Prepare packet headers
        uint32_t header_count = 1;
        uint32_t header_count_net = htonl(header_count);
        struct AudioPacketHeader header = {
            .offset = htonl(0),
            .size = htonl(data->data_size)
        };

        // Build packet
        size_t packet_size = sizeof(header_count) + 
                            sizeof(header) + 
                            data->data_size;
        unsigned char packet[packet_size];
        memcpy(packet, &header_count_net, sizeof(header_count_net));
        memcpy(packet + sizeof(header_count_net), &header, sizeof(header));
        memcpy(packet + sizeof(header_count_net) + sizeof(header), 
              data->data, data->data_size);

        // Send to all recipients
        for (int i = 0; i < data->recipients.count; i++) {
            Client* c = data->recipients.clients[i];
            pthread_mutex_lock(&c->write_mutex);
            
            // Send packet size
            uint32_t size_net = htonl(packet_size);
            if (send_all(c->fd, &size_net, sizeof(size_net)) != sizeof(size_net)) {
                pthread_mutex_unlock(&c->write_mutex);
                remove_client(c->fd);
                continue;
            }

            // Send packet data
            if (send_all(c->fd, packet, packet_size) != packet_size) {
                pthread_mutex_unlock(&c->write_mutex);
                remove_client(c->fd);
                continue;
            }
            
            pthread_mutex_unlock(&c->write_mutex);
        }

        free(data->data);
        free(data);
    }
    return NULL;
}

void handle_client_input(int client_fd) {
    unsigned char buffer[READ_BUFFER_SIZE];
    
    // Read packet size
    uint32_t packet_size;
    if (recv(client_fd, &packet_size, sizeof(packet_size), MSG_WAITALL) != sizeof(packet_size)) {
        remove_client(client_fd);
        return;
    }
    packet_size = ntohl(packet_size);

    if (packet_size > MAX_PACKET_SIZE) {
        fprintf(stderr, "Packet too large: %u\n", packet_size);
        remove_client(client_fd);
        return;
    }

    // Read packet data
    if (recv(client_fd, buffer, packet_size, MSG_WAITALL) != packet_size) {
        remove_client(client_fd);
        return;
    }

    // Copy data and queue for processing
    unsigned char* data_copy = malloc(packet_size);
    if (!data_copy) {
        perror("Failed to allocate packet data");
        return;
    }
    memcpy(data_copy, buffer, packet_size);

    struct ThreadData* td = malloc(sizeof(struct ThreadData));
    if (!td) {
        free(data_copy);
        perror("Failed to allocate thread data");
        return;
    }
    td->sender_fd = client_fd;
    td->data = data_copy;
    td->data_size = packet_size;
    td->recipients = get_recipients(client_fd);

    enqueue(&work_queue, td);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
        return 1;
    }

    const char* host = argv[1];
    int port = atoi(argv[2]);

    // Initialize queue
    queue_init(&work_queue);

    // Create worker thread
    pthread_t worker_thread;
    if (pthread_create(&worker_thread, NULL, worker_thread_func, NULL) != 0) {
        perror("Failed to create worker thread");
        queue_destroy(&work_queue);
        return 1;
    }

    // Initialize clients array
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].active = false;
    }

    // Create server socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        queue_destroy(&work_queue);
        return 1;
    }

    // Set SO_REUSEADDR
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt failed");
        close(server_fd);
        queue_destroy(&work_queue);
        return 1;
    }

    // Bind socket
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port)
    };
    if (inet_pton(AF_INET, host, &address.sin_addr) <= 0) {
        perror("Invalid address");
        close(server_fd);
        queue_destroy(&work_queue);
        return 1;
    }

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        close(server_fd);
        queue_destroy(&work_queue);
        return 1;
    }

    // Listen
    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        close(server_fd);
        queue_destroy(&work_queue);
        return 1;
    }

    printf("Server listening on %s:%d\n", host, port);

    // Set up epoll
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1 failed");
        close(server_fd);
        queue_destroy(&work_queue);
        return 1;
    }

    struct epoll_event server_event = {
        .events = EPOLLIN,
        .data.fd = server_fd
    };
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &server_event) < 0) {
        perror("epoll_ctl failed");
        close(epoll_fd);
        close(server_fd);
        queue_destroy(&work_queue);
        return 1;
    }

    // Main event loop
    while (server_running) {
        struct epoll_event events[MAX_EVENTS];
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server_fd) {
                // Accept new connection
                struct sockaddr_in client_addr;
                socklen_t addrlen = sizeof(client_addr);
                int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addrlen);
                if (client_fd < 0) {
                    perror("Accept failed");
                    continue;
                }

                // Find empty slot for client
                pthread_mutex_lock(&clients_mutex);
                int client_index = -1;
                for (int j = 0; j < MAX_CLIENTS; j++) {
                    if (!clients[j].active) {
                        client_index = j;
                        break;
                    }
                }

                if (client_index == -1) {
                    close(client_fd);
                    printf("Rejected connection (max clients reached)\n");
                    pthread_mutex_unlock(&clients_mutex);
                    continue;
                }

                // Initialize client
                clients[client_index].fd = client_fd;
                clients[client_index].addr = client_addr;
                clients[client_index].active = true;
                pthread_mutex_init(&clients[client_index].write_mutex, NULL);

                // Add to epoll
                struct epoll_event ev = {
                    .events = EPOLLIN | EPOLLET,
                    .data.fd = client_fd
                };
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                    perror("epoll_ctl for client failed");
                    close(client_fd);
                    clients[client_index].active = false;
                    pthread_mutex_unlock(&clients_mutex);
                    continue;
                }

                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
                printf("New connection from %s:%d (fd: %d)\n", 
                      ip_str, ntohs(client_addr.sin_port), client_fd);

                pthread_mutex_unlock(&clients_mutex);
            } else {
                // Handle client input
                handle_client_input(events[i].data.fd);
            }
        }
    }

    // Cleanup
    printf("Shutting down server...\n");
    server_running = false;
    
    // Wake up worker thread
    pthread_cond_signal(&work_queue.cond);
    pthread_join(worker_thread, NULL);

    // Close all client connections
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            close(clients[i].fd);
            pthread_mutex_destroy(&clients[i].write_mutex);
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    close(epoll_fd);
    close(server_fd);
    queue_destroy(&work_queue);

    return 0;
}
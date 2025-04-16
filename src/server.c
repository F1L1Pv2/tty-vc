#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <assert.h>
#include <pthread.h>
#include <sys/socket.h>

#define MAX_CLIENTS 2
#define READ_ALLOC_SIZE (48000 * 2 * sizeof(float) + 128)

typedef struct {
    int fd;
    struct sockaddr_in addr;
    int index;
    bool active;
} client_info;

typedef struct queue_node {
    float* data;
    size_t size;
    int sender_index;
    struct queue_node* next;
} queue_node;

typedef struct {
    queue_node* head;
    queue_node* tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} queue_t;

client_info clients[MAX_CLIENTS];
int client_count = 0;
bool echoMode = false;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
queue_t message_queue;

void queue_init() {
    message_queue.head = message_queue.tail = NULL;
    pthread_mutex_init(&message_queue.mutex, NULL);
    pthread_cond_init(&message_queue.cond, NULL);
}

void enqueue(float* data, size_t size, int sender_index) {
    queue_node* node = malloc(sizeof(queue_node));
    if (!node) {
        perror("Failed to allocate queue node");
        free(data);
        return;
    }

    node->data = data;
    node->size = size;
    node->sender_index = sender_index;
    node->next = NULL;

    pthread_mutex_lock(&message_queue.mutex);
    
    if (message_queue.tail == NULL) {
        message_queue.head = message_queue.tail = node;
    } else {
        message_queue.tail->next = node;
        message_queue.tail = node;
    }
    
    pthread_cond_signal(&message_queue.cond);
    pthread_mutex_unlock(&message_queue.mutex);
}

queue_node* dequeue() {
    pthread_mutex_lock(&message_queue.mutex);
    
    while (message_queue.head == NULL) {
        pthread_cond_wait(&message_queue.cond, &message_queue.mutex);
    }

    queue_node* node = message_queue.head;
    message_queue.head = node->next;
    
    if (message_queue.head == NULL) {
        message_queue.tail = NULL;
    }
    
    pthread_mutex_unlock(&message_queue.mutex);
    return node;
}

void* worker_thread(void* arg) {
    (void)arg;
    while (1) {
        queue_node* node = dequeue();

        pthread_mutex_lock(&clients_mutex);
        client_info local_clients[MAX_CLIENTS];
        memcpy(local_clients, clients, sizeof(clients));
        pthread_mutex_unlock(&clients_mutex);

        for(int i = 0; i < MAX_CLIENTS; i++){
            if(i == node->sender_index) continue;
            if(!local_clients[i].active) continue;
            if (write(local_clients[i].fd, node->data, node->size) <= 0) {
                // TODO: Handle write error
            }
        }

        free(node->data);
        free(node);
    }
    return NULL;
}

void* handle_client(void* arg) {
    client_info* client = (client_info*)arg;
    int client_fd = client->fd;
    int client_index = client->index;

    printf("Client %d connected\n", client_index);
    float* buff = malloc(READ_ALLOC_SIZE);
    if (!buff) {
        perror("malloc failed");
        goto cleanup;
    }

    while (1) {
        int read_count = read(client_fd, buff, READ_ALLOC_SIZE);
        if (read_count <= 0) break;

        float* data_copy = malloc(read_count);
        if (!data_copy) {
            perror("Failed to allocate data copy");
            continue;
        }
        memcpy(data_copy, buff, read_count);
        enqueue(data_copy, read_count, client_index);
    }

cleanup:
    printf("Client %d disconnected\n", client_index);
    free(buff);

    pthread_mutex_lock(&clients_mutex);
    close(client_fd);
    client->active = false;
    client_count--;
    pthread_mutex_unlock(&clients_mutex);
    return NULL;
}

void usage(char* program) {
    fprintf(stderr, "Usage: %s <hostname> <port> [echo]\n", program);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc < 3) usage(argv[0]);
    
    char* hostname = argv[1];
    int port = atoi(argv[2]);
    echoMode = argc > 3 && strcmp(argv[3], "echo") == 0;

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %d\n", port);
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].active = false;
        clients[i].index = i;
    }

    queue_init();
    pthread_t worker;
    if (pthread_create(&worker, NULL, worker_thread, NULL) != 0) {
        perror("Failed to create worker thread");
        exit(EXIT_FAILURE);
    }
    pthread_detach(worker);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, hostname, &address.sin_addr);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) < 0) {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Listening on %s:%d...\n", hostname, port);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addrlen);
        if (client_fd < 0) {
            perror("accept failed");
            continue;
        }

        pthread_mutex_lock(&clients_mutex);
        if (client_count >= MAX_CLIENTS) {
            printf("Max clients reached. Rejecting connection.\n");
            close(client_fd);
            pthread_mutex_unlock(&clients_mutex);
            continue;
        }

        int client_index = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].active) {
                client_index = i;
                break;
            }
        }

        if (client_index == -1) {
            close(client_fd);
            pthread_mutex_unlock(&clients_mutex);
            continue;
        }

        clients[client_index].fd = client_fd;
        clients[client_index].addr = client_addr;
        clients[client_index].active = true;
        client_count++;

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, &clients[client_index]) != 0) {
            perror("pthread_create failed");
            clients[client_index].active = false;
            client_count--;
            close(client_fd);
        } else {
            pthread_detach(thread_id);
        }
        pthread_mutex_unlock(&clients_mutex);
    }

    close(server_fd);
    return 0;
}
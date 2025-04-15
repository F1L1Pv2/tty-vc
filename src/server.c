#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <fcntl.h>
#include <assert.h>
#include <pthread.h>
#include <sys/socket.h>
#define NOB_IMPLEMENATION
#include "../nob.h"

#include "common.h"

#define MAX_CLIENTS 10

typedef struct {
    int fd;
    struct sockaddr_in addr;
    int index;
    bool active;
    pthread_t thread_id;
} client_info;


bool echoMode = false;

client_info clients[MAX_CLIENTS];
int client_count = 0;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

ssize_t read_all(int fd, void *buf, size_t count) {
    size_t received = 0;
    while (received < count) {
        ssize_t n = read(fd, (char*)buf + received, count - received);
        if (n <= 0) return n;
        received += n;
    }
    return received;
}

ssize_t send_all(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, (char*)buf + sent, len - sent);
        if (n <= 0) return n;
        sent += n;
    }
    return sent;
}

void *handle_client(void *arg) {
    client_info *client = (client_info *)arg;
    int client_fd = client->fd;
    int client_index = client->index;

    unsigned char* buff = (unsigned char*)malloc(MAX_PACKET_SIZE);
    if (!buff) { /* handle error */ }

    while (1) {
        // Read packet size
        uint32_t packet_size;
        if (read_all(client_fd, &packet_size, 4) != 4) break;
        packet_size = ntohl(packet_size);

        if (packet_size > MAX_PACKET_SIZE) {
            fprintf(stderr, "Packet too large: %u\n", packet_size);
            break;
        }

        // Read Opus data
        if (read_all(client_fd, buff, packet_size) != packet_size) break;

        // Prepare new packet for other clients
        uint32_t count = 1;
        struct AudioPacketHeader header = {
            .offset = 0,
            .size = packet_size
        };

        // Network byte order conversion
        uint32_t count_net = htonl(count);
        uint32_t offset_net = htonl(header.offset);
        uint32_t size_net = htonl(header.size);

        // Construct new packet data (excluding leading size)
        size_t new_data_size = sizeof(count) + sizeof(header) + packet_size;
        unsigned char new_packet_data[sizeof(count) + sizeof(header) + MAX_PACKET_SIZE];
        memcpy(new_packet_data, &count_net, sizeof(count));
        memcpy(new_packet_data + sizeof(count), &offset_net, sizeof(header.offset));
        memcpy(new_packet_data + sizeof(count) + sizeof(header.offset), &size_net, sizeof(header.size));
        memcpy(new_packet_data + sizeof(count) + sizeof(header), buff, packet_size);

        // Send to all other clients
        pthread_mutex_lock(&clients_mutex);
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if ((clients[i].active && i != client_index) || (client_count == 1 && echoMode)) {
                // Send size prefix
                uint32_t new_size_net = htonl(new_data_size);
                if (send_all(clients[i].fd, &new_size_net, sizeof(new_size_net)) != sizeof(new_size_net)) {
                    pthread_mutex_unlock(&clients_mutex);
                    break;
                }

                // Send new packet data
                if (send_all(clients[i].fd, new_packet_data, new_data_size) != new_data_size) {
                    pthread_mutex_unlock(&clients_mutex);
                    break;
                }
            }
        }
        pthread_mutex_unlock(&clients_mutex);
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

void usage(char* program){
    fprintf(stderr, "Usage: %s <hostname> <port>\n", program);
    exit(1);
}

int main(int argc, char** argv) {
    char* program = nob_shift_args(&argc,&argv);

    if(argc == 0) usage(program);
    char *hostname = nob_shift_args(&argc,&argv);
    if(argc == 0) usage(program);
    int port = atoi(nob_shift_args(&argc,&argv));
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %d\n", port);
        exit(EXIT_FAILURE);
    }

    while (argc > 0){
        char* arg = nob_shift_args(&argc,&argv);
        if(strcmp(arg, "echo") == 0){
            echoMode = true;
        }
    }

    int server_fd;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    int opt = 1;

    // Initialize client array
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].active = false;
        clients[i].index = i;
    }

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (inet_pton(AF_INET, hostname, &address.sin_addr) <= 0) {
        perror("Invalid address/ Address not supported");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
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

    while (true) {
        int client_fd;
        struct sockaddr_in client_addr;
        socklen_t client_addrlen = sizeof(client_addr);

        if ((client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addrlen)) < 0) {
            perror("accept failed");
            continue;
        }

        pthread_mutex_lock(&clients_mutex);
        
        if (client_count >= MAX_CLIENTS) {
            printf("Rejected connection from %s:%d (max clients reached)\n", 
                  inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            close(client_fd);
            pthread_mutex_unlock(&clients_mutex);
            continue;
        }

        // Find empty slot
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

        // Store client info
        clients[client_index].fd = client_fd;
        clients[client_index].addr = client_addr;
        clients[client_index].active = true;
        client_count++;

        printf("Connection accepted from %s:%d (client %d/%d)\n", 
              inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port),
              client_count, MAX_CLIENTS);

        // Create thread
        if (pthread_create(&clients[client_index].thread_id, NULL, handle_client, &clients[client_index]) != 0) {
            perror("pthread_create failed");
            clients[client_index].active = false;
            client_count--;
            close(client_fd);
        } else {
            // Detach the thread so we don't need to join it later
            pthread_detach(clients[client_index].thread_id);
        }
        
        pthread_mutex_unlock(&clients_mutex);
    }

    close(server_fd);
    return 0;
}
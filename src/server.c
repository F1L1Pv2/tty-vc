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

size_t send_data(int sock, void *data, size_t size) {
    // First send the size of the packet (4 bytes)
    uint32_t packet_size = htonl((uint32_t)size);
    if (send(sock, (const char*)&packet_size, sizeof(packet_size), 0) != sizeof(packet_size)) {
        perror("Failed to send packet size");
        return 0;
    }
    
    // Then send the actual data
    ssize_t bytes_sent = send(sock, data, size, 0);
    if (bytes_sent < 0) {
        perror("Send failed");
    }
    return (size_t)bytes_sent;
}

size_t receive_data(int sock, char *buffer, size_t buffer_size) {
    // First receive the packet size
    uint32_t packet_size;
    if (recv(sock, (char*)&packet_size, sizeof(packet_size), 0) != sizeof(packet_size)) {
        perror("Failed to receive packet size");
        return 0;
    }
    packet_size = ntohl(packet_size);
    
    if (packet_size > buffer_size) {
        fprintf(stderr, "Packet too large: %u > %zu\n", packet_size, buffer_size);
        return 0;
    }
    
    // Then receive the actual data
    ssize_t bytes_received = recv(sock, buffer, packet_size, 0);
    if (bytes_received < 0) {
        perror("Receive failed");
        return 0;
    }
    return (size_t)bytes_received;
}

void *handle_client(void *arg) {
    client_info *client = (client_info *)arg;
    int client_fd = client->fd;
    int client_index = client->index;

    printf("Thread started for client %d\n", client_index);
    
    void* buff = (float*)malloc(MAX_PACKET_SIZE);
    if (!buff) {
        perror("malloc failed");
        goto cleanup;
    }

    while (true) {
        int read_count = receive_data(client->fd,(char*)buff,MAX_PACKET_SIZE);
        if(read_count <= 0) break;

        pthread_mutex_lock(&clients_mutex);
        
        if (client_count == 1 && echoMode) {
            // Echo mode - single client
            if (send_data(client->fd,buff,read_count) <= 0) {
                pthread_mutex_unlock(&clients_mutex);
                break;
            }
        } else if (client_count == 2) {
            // Forward mode - send to other client
            int other_index = (client_index == 0) ? 1 : 0;
            if (clients[other_index].active) {
                if (send_data(clients[other_index].fd,buff,read_count) <= 0) {
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
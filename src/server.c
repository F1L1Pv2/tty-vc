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

#define MAX_CLIENTS 10
#define MAX_PACKET_SIZE 1500


typedef struct {
    int fd;
    struct sockaddr_in addr;
    int id;  // Using client index as ID
    bool active;
    pthread_t thread_id;
} client_info;

client_info clients[MAX_CLIENTS];
int client_count = 0;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

size_t send_data(int sock, const char *data, size_t size) {
    uint32_t packet_size = htonl(size);
    if (send(sock, &packet_size, sizeof(packet_size), 0) != sizeof(packet_size)) {
        perror("send packet size failed");
        return 0;
    }
    return send(sock, data, size, 0);
}

size_t receive_data(int sock, char *buffer, size_t buffer_size) {
    uint32_t packet_size;
    if (recv(sock, &packet_size, sizeof(packet_size), 0) != sizeof(packet_size)) {
        perror("recv packet size failed");
        return 0;
    }
    packet_size = ntohl(packet_size);
    if (packet_size > buffer_size) {
        fprintf(stderr, "Packet too large: %u > %zu\n", packet_size, buffer_size);
        return 0;
    }
    return recv(sock, buffer, packet_size, 0);
}

void *handle_client(void *arg) {
    client_info *client = (client_info *)arg;
    char data_buffer[MAX_PACKET_SIZE];
    
    while (true) {
        size_t data_size = receive_data(client->fd, data_buffer, sizeof(data_buffer));
        if (data_size <= 0) break;

        // Prepend client ID to data
        uint32_t sender_id = htonl(client->id);
        char combined[4 + data_size];
        memcpy(combined, &sender_id, 4);
        memcpy(combined + 4, data_buffer, data_size);

        pthread_mutex_lock(&clients_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && clients[i].id != client->id) {
                send_data(clients[i].fd, combined, sizeof(combined));
            }
        }
        pthread_mutex_unlock(&clients_mutex);
    }

    printf("Client %d disconnected\n", client->id);
    pthread_mutex_lock(&clients_mutex);
    close(client->fd);
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

    int server_fd;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    int opt = 1;

    // Initialize client array
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].active = false;
        clients[i].id = i;
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
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addrlen);
        if (client_fd < 0) {
            perror("accept failed");
            continue;
        }

        pthread_mutex_lock(&clients_mutex);
        
        if (client_count >= MAX_CLIENTS) {
            printf("Rejected connection (max clients reached)\n");
            close(client_fd);
            pthread_mutex_unlock(&clients_mutex);
            continue;
        }

        int client_index = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].active) {
                client_index = i;
                clients[i].id = i;
                break;
            }
        }

        clients[client_index].fd = client_fd;
        clients[client_index].addr = client_addr;
        clients[client_index].active = true;
        client_count++;

        printf("Connection accepted from %s:%d (client %d/%d)\n", 
              inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port),
              client_count, MAX_CLIENTS);

        if (pthread_create(&clients[client_index].thread_id, NULL, handle_client, &clients[client_index]) != 0) {
            perror("pthread_create failed");
            clients[client_index].active = false;
            client_count--;
            close(client_fd);
        } else {
            pthread_detach(clients[client_index].thread_id);
        }
        
        pthread_mutex_unlock(&clients_mutex);
    }

    close(server_fd);
    return 0;
}
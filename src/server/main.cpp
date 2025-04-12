#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <fcntl.h>
#include <assert.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>

#define MAX_CLIENTS 2

typedef struct {
    int fd;
    struct sockaddr_in addr;
} client_info;

client_info clients[MAX_CLIENTS];
int client_count = 0;

void handle_client(int client_fd, int client_index) {
    if (prctl(PR_SET_PDEATHSIG, SIGHUP) == -1) {
        perror("prctl failed");
        exit(EXIT_FAILURE);
    }

    float* buff = (float*)malloc(48000 * 2 * sizeof(float) + 128);

    while (true) {
        int read_count = read(client_fd, buff, 48000 * 2 * sizeof(float) + 128);
        if (read_count <= 0) {
            break;
        }

        if (client_count == 1) {
            // Echo mode - single client
            if (write(client_fd, buff, read_count) <= 0) {
                break;
            }
        } else if (client_count == 2) {
            // Forward mode - send to other client
            int other_index = (client_index == 0) ? 1 : 0;
            if (write(clients[other_index].fd, buff, read_count) <= 0) {
                break;
            }
        }
    }

    printf("Client %d disconnected\n", client_index);
    close(client_fd);
    free(buff);
    
    // Remove client from array
    clients[client_index].fd = -1;
    client_count--;
    
    exit(0);
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <hostname> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *hostname = argv[1];
    int port = atoi(argv[2]);
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

    signal(SIGCHLD, SIG_IGN);

    while (true) {
        int client_fd;
        struct sockaddr_in client_addr;
        socklen_t client_addrlen = sizeof(client_addr);

        if ((client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addrlen)) < 0) {
            perror("accept failed");
            continue;
        }

        if (client_count >= MAX_CLIENTS) {
            printf("Rejected connection from %s:%d (max clients reached)\n", 
                  inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            close(client_fd);
            continue;
        }

        // Find empty slot
        int client_index = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd == -1) {
                client_index = i;
                break;
            }
        }

        if (client_index == -1) {
            close(client_fd);
            continue;
        }

        // Store client info
        clients[client_index].fd = client_fd;
        clients[client_index].addr = client_addr;
        client_count++;

        printf("Connection accepted from %s:%d (client %d/%d)\n", 
              inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port),
              client_count, MAX_CLIENTS);

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork failed");
            clients[client_index].fd = -1;
            client_count--;
            close(client_fd);
        } else if (pid == 0) {
            close(server_fd);
            handle_client(client_fd, client_index);
        } else {
            // Parent doesn't need this FD
            close(client_fd);
        }
    }

    close(server_fd);
    return 0;
}
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

#ifndef DA_INIT_CAP
#define DA_INIT_CAP 256
#endif

#define da_append(da, item)                                                          \
    do {                                                                             \
        if ((da)->count >= (da)->capacity) {                                         \
            (da)->capacity = (da)->capacity == 0 ? DA_INIT_CAP : (da)->capacity*2;   \
            (da)->items = realloc((da)->items, (da)->capacity*sizeof(*(da)->items)); \
            assert((da)->items != NULL && "Buy more RAM lol");                       \
        }                                                                            \
                                                                                     \
        (da)->items[(da)->count++] = (item);                                         \
    } while (0)

#define da_remove_unordered(da, i)                   \
    do {                                             \
        size_t j = (i);                              \
        assert(j < (da)->count);                     \
        (da)->items[j] = (da)->items[--(da)->count]; \
    } while(0)

#define BUFFER_SIZE 1024

void handle_client(int client_fd) {
    // Ensure the child process dies when the parent process dies
    if (prctl(PR_SET_PDEATHSIG, SIGHUP) == -1) {
        perror("prctl failed");
        exit(EXIT_FAILURE);
    }

    float* buff = malloc(48000 * 2 * sizeof(float) + 128);

    while (true) {
        int read_count = read(client_fd, buff, 48000 * 2 * sizeof(float) + 128);
        if (read_count <= 0) {
            break;
        }

        if (write(client_fd, buff, read_count) <= 0) {
            break;
        }
    }

    printf("Client disconnected\n");
    close(client_fd);
    free(buff);
    exit(0); // Exit the child process after handling the client
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

    int server_fd, client_fd;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    int opt = 1;

    // Create socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Define server address
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (inet_pton(AF_INET, hostname, &address.sin_addr) <= 0) {
        perror("Invalid address/ Address not supported");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Bind the socket to the network address and port
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections
    if (listen(server_fd, 3) < 0) {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Listening on %s:%d...\n", hostname, port);

    // Ignore SIGCHLD to avoid zombie processes
    signal(SIGCHLD, SIG_IGN);

    while (true) {
        // Accept a connection
        if ((client_fd = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept failed");
            close(server_fd);
            exit(EXIT_FAILURE);
        }

        printf("Connection accepted from %s:%d\n", inet_ntoa(address.sin_addr), ntohs(address.sin_port));

        // Fork a new process to handle the client
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork failed");
            close(client_fd);
        } else if (pid == 0) {
            // Child process
            close(server_fd); // Close the server socket in the child process
            handle_client(client_fd);
        } else {
            // Parent process
            close(client_fd); // Close the client socket in the parent process
        }
    }

    // Close the server socket
    close(server_fd);
    return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <fcntl.h>
#include <assert.h>

#include <coroutine.h>

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

typedef struct {
    int fd;
    struct sockaddr_in addr;
} Client;

typedef struct {
    Client* items;
    size_t count;
    size_t capacity;
} ClientArray;

ClientArray clients = {0};

typedef struct {
    int sender_fd;
    float* samples;
    size_t sample_count;
} AudioData;

typedef struct {
    AudioData* items;
    size_t count;
    size_t capacity;
} AudioQueue;

AudioQueue audio_queue = {0};

void broadcast_audio_coroutine(void* arg) {
    (void)arg; // Unused parameter

    while (true) {
        if (audio_queue.count > 0) {
            AudioData data = audio_queue.items[0];

            uint32_t num_users = clients.count - 1; // Exclude the sender
            for (size_t i = 0; i < clients.count; i++) {
                if (clients.items[i].fd != data.sender_fd) {
                    write(clients.items[i].fd, &num_users, sizeof(num_users));
                    uint32_t sample_count_net = data.sample_count;
                    write(clients.items[i].fd, &sample_count_net, sizeof(sample_count_net));
                    write(clients.items[i].fd, data.samples, data.sample_count * sizeof(float));
                }
            }

            // Free the samples buffer after broadcasting
            free(data.samples);

            // Remove the processed item from the queue
            da_remove_unordered(&audio_queue, 0);
        } else {
            // If the queue is empty, yield to other coroutines
            coroutine_yield();
        }
    }
}

void handle_client(void* arg) {
    int client_fd = (int)arg;
    float* buffer = malloc(48000 * 2 * sizeof(float) + 128);

    while (true) {
        coroutine_sleep_read(client_fd);
        int read_count = read(client_fd, buffer, 48000 * 2 * sizeof(float) + 128);
        if (read_count <= 0) {
            break;
        }

        size_t sample_count = read_count / sizeof(float);

        // Enqueue the audio data for broadcasting
        AudioData data = {client_fd, buffer, sample_count};
        da_append(&audio_queue, data);

        // Allocate a new buffer for the next read
        buffer = malloc(48000 * 2 * sizeof(float) + 128);
    }

    printf("Client disconnected\n");
    close(client_fd);
    free(buffer);

    // Remove client from the list
    for (size_t i = 0; i < clients.count; i++) {
        if (clients.items[i].fd == client_fd) {
            da_remove_unordered(&clients, i);
            break;
        }
    }
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <hostname> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    coroutine_init();

    const char* hostname = argv[1];
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

    int flags = fcntl(server_fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL failed");
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
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
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

    coroutine_go(broadcast_audio_coroutine, NULL);

    while (true) {
        coroutine_sleep_read(server_fd);
        // Accept a connection
        if ((client_fd = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept failed");
            close(server_fd);
            exit(EXIT_FAILURE);
        }

        int flags = fcntl(client_fd, F_GETFL, 0);
        if (flags == -1) {
            perror("fcntl F_GETFL failed");
            close(client_fd);
            continue;
        }

        if (fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            perror("fcntl F_GETFL failed");
            close(client_fd);
            continue;
        }

        printf("Connection accepted from %s:%d\n", inet_ntoa(address.sin_addr), ntohs(address.sin_port));

        Client new_client = {client_fd, address};
        da_append(&clients, new_client);

        coroutine_go(handle_client, (void*)client_fd);
    }

    // Close the server socket
    close(server_fd);
    coroutine_finish();
    return 0;
}
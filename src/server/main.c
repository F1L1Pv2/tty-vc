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
    char* username;
} Client;

typedef struct {
    Client *items;
    size_t count;
    size_t capacity;
} Clients;

Clients clients;

void remove_client_if_exists(int fd){
    for(int i = 0; i < clients.count; i++){
        if(clients.items[i].fd == fd){
            if(clients.items[i].username != NULL){
                free(clients.items[i].username);
            }
            da_remove_unordered(&clients,i);
            break;
        }
    }
}

size_t client_id(int fd){
    for(int i = 0; i < clients.count; i++){
        if(clients.items[i].fd == fd){
            return i;
        }
    }
    return -1;
}

char* get_username(int fd){
    Client* client = &clients.items[client_id(fd)];
    return client->username;
}

typedef struct {
    int from_fd;
    char* msg;
} BroadCastInfo;


void broadcast_msg(void* arg){
    BroadCastInfo* info = (BroadCastInfo*)arg;
    char* from_username;
    if(info->from_fd == -1){
        from_username = "server";
    }else{
        from_username = get_username(info->from_fd);
    }
    char msg[BUFFER_SIZE+256];

    for(int i = 0; i < clients.count; i++){
        int client_fd = clients.items[i].fd;
        if(client_fd == info->from_fd) continue;
        coroutine_sleep_write(client_fd);
        sprintf(msg,"[%s]: %s\n",from_username, info->msg);
        write(client_fd,msg,strlen(msg));
    }

    free(info->msg);
    free(info);
}

void client_reader(void* arg){
    int client_fd = (int)arg;

    while(true){
        coroutine_sleep_read(client_fd);
        char* buffer = (char*)malloc(BUFFER_SIZE);
        int read_size = read(client_fd, buffer, BUFFER_SIZE);
        if(read_size <= 0) break;
        buffer[read_size-1] = 0;

        printf("from %s: Received: %s\n", get_username(client_fd), buffer);
        BroadCastInfo* broadInfo = malloc(sizeof(BroadCastInfo));
        broadInfo->from_fd = client_fd;
        broadInfo->msg = buffer;
        coroutine_go(broadcast_msg,(void*)broadInfo);
    }

    printf("%s Connection closed.\n", get_username(client_fd));
    close(client_fd);
    remove_client_if_exists(client_fd);
}

void client_init(void* arg){
    int client_fd = (int)arg;
    char* buffer = (char*)malloc(BUFFER_SIZE);
    size_t index = client_id(client_fd);

    coroutine_sleep_write(client_fd);
    char* response = "Give your username: ";
    if(write(client_fd, response, strlen(response)) <= 0) goto client_end;

    coroutine_sleep_read(client_fd);
    char* username = (char*)malloc(128);
    int read_size = read(client_fd, username, 128);
    if(read_size <= 0) goto client_end;
    username[read_size-1] = 0;
    clients.items[index].username = username;

    printf("%s has joined\n", username);
    BroadCastInfo* welcomeMSG = (BroadCastInfo*)malloc(sizeof(BroadCastInfo));
    welcomeMSG->from_fd = -1;
    welcomeMSG->msg = (char*)malloc(BUFFER_SIZE);
    sprintf(welcomeMSG->msg,"%s has joined!",username);
    coroutine_go(broadcast_msg, welcomeMSG);

    coroutine_go(client_reader,arg);
    return;

client_end:
    printf("Connection closed.\n");
    close(client_fd);
    remove_client_if_exists(client_fd);
    free(buffer);
}

int main(int argc, char** argv){
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <hostname> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    coroutine_init();

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

    printf("Listening on %s:%d...\n", hostname,port);

    while(true){
        coroutine_sleep_read(server_fd);
        // Accept a connection
        if ((client_fd = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
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

        Client c = {0};
        c.fd = client_fd;
        da_append(&clients, c);

        printf("Connection accepted from %s:%d\n", inet_ntoa(address.sin_addr), ntohs(address.sin_port));
        coroutine_go(client_init,(void*)client_fd);
    }

    // Close the server socket
    close(server_fd);
    coroutine_finish();
    return 0;
}
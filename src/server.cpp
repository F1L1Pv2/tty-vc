#include <iostream>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <cstring>
#include <cstdlib>
#include <cassert>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "common.h"

struct ClientInfo {
    int fd = -1;
    struct sockaddr_in addr;
    std::atomic<int> index;
    std::atomic<bool> active{false};
    std::thread thread;
};

std::atomic<bool> echoMode{false};
std::atomic<int> clientCount{0};
ClientInfo clients[MAX_CLIENTS];

#include "concurrentqueue.h"

struct SendPacket{
    int fd = -1;
    void* data = nullptr;
    size_t size = 0;
};

moodycamel::ConcurrentQueue<SendPacket> queue;

size_t send_data(int sock, void *data, size_t size) {
    // First send the size of the packet (4 bytes)
    uint32_t packet_size = htonl(static_cast<uint32_t>(size));
    if (send(sock, reinterpret_cast<const char*>(&packet_size), sizeof(packet_size), 0) != sizeof(packet_size)) {
        perror("Failed to send packet size");
        return 0;
    }
    
    // Then send the actual data
    ssize_t bytes_sent = send(sock, data, size, 0);
    if (bytes_sent < 0) {
        perror("Send failed");
    }
    return static_cast<size_t>(bytes_sent);
}

size_t receive_data(int sock, char *buffer, size_t buffer_size) {
    // First receive the packet size
    uint32_t packet_size;
    if (recv(sock, reinterpret_cast<char*>(&packet_size), sizeof(packet_size), 0) != sizeof(packet_size)) {
        perror("Failed to receive packet size");
        return 0;
    }
    packet_size = ntohl(packet_size);
    
    if (packet_size > buffer_size) {
        std::cerr << "Packet too large: " << packet_size << " > " << buffer_size << std::endl;
        return 0;
    }
    
    // Then receive the actual data
    ssize_t bytes_received = recv(sock, buffer, packet_size, 0);
    if (bytes_received < 0) {
        perror("Receive failed");
        return 0;
    }
    return static_cast<size_t>(bytes_received);
}

void handle_client(ClientInfo* client) {
    int client_fd = client->fd;
    int client_index = client->index;

    std::cout << "Thread started for client " << client_index << std::endl;
    
    char* buff = nullptr;

    while (true) {
        buff = new char[sizeof(uint32_t)+MAX_PACKET_SIZE];
        int read_count = receive_data(client->fd, buff+sizeof(uint32_t),MAX_PACKET_SIZE);
        if (read_count <= 0) break;

        ((uint32_t*)buff)[0] = client->index.load();

        SendPacket packet;
        packet.fd = client->fd;
        packet.data = buff;
        packet.size = sizeof(uint32_t)+read_count;
        queue.enqueue(packet);
    }

cleanup:
    std::cout << "Client " << client_index << " disconnected" << std::endl;
    if(buff) delete[] buff;
    
    close(client_fd);
    client->active = false;
    clientCount--;
}

void sender(){
    SendPacket packet;
    while(true){
        if(!queue.try_dequeue(packet)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if(packet.data == nullptr || packet.size == 0) continue;

        for(int i = 0; i < MAX_CLIENTS; i++){
            if(!clients[i].active.load()) continue;
            if(clients[i].fd == packet.fd) continue;

            send_data(clients[i].fd, packet.data, packet.size);
        }
    }
}

void usage(const std::string& program) {
    std::cerr << "Usage: " << program << " <hostname> <port> [echo]" << std::endl;
    exit(1);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
    }

    std::string hostname = argv[1];
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        std::cerr << "Invalid port: " << port << std::endl;
        exit(EXIT_FAILURE);
    }

    if (argc > 3 && std::string(argv[3]) == "echo") {
        echoMode = true;
    }

    int server_fd;
    struct sockaddr_in address;
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

    if (inet_pton(AF_INET, hostname.c_str(), &address.sin_addr) <= 0) {
        perror("Invalid address/ Address not supported");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (bind(server_fd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) < 0) {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    std::cout << "Listening on " << hostname << ":" << port << "..." << std::endl;

    auto broadcasterThread = std::thread(sender);
    broadcasterThread.detach();

    while (true) {
        int client_fd;
        struct sockaddr_in client_addr;
        socklen_t client_addrlen = sizeof(client_addr);

        if ((client_fd = accept(server_fd, reinterpret_cast<struct sockaddr *>(&client_addr), &client_addrlen)) < 0) {
            perror("accept failed");
            continue;
        }

        if (clientCount >= MAX_CLIENTS) {
            std::cout << "Rejected connection from " << inet_ntoa(client_addr.sin_addr) 
                      << ":" << ntohs(client_addr.sin_port) << " (max clients reached)" << std::endl;
            close(client_fd);
            continue;
        }

        // Find empty slot
        int client_index = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            bool expected = false;
            if (clients[i].active.compare_exchange_strong(expected, true)) {
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
        clients[client_index].index = client_index;
        clientCount++;

        std::cout << "Connection accepted from " << inet_ntoa(client_addr.sin_addr) 
                  << ":" << ntohs(client_addr.sin_port) << " (client " 
                  << clientCount << "/" << MAX_CLIENTS << ")" << std::endl;

        // Create thread
        clients[client_index].thread = std::thread(handle_client, &clients[client_index]);
        clients[client_index].thread.detach();
    }

    close(server_fd);
    return 0;
}
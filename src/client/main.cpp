#include <stdio.h>
#include <atomic>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#endif

#include <stdlib.h>

#define BUFFER_SIZE 1024

void init_sockets() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        exit(EXIT_FAILURE);
    }
#endif
}

void cleanup_sockets() {
#ifdef _WIN32
    WSACleanup();
#endif
}

int create_socket() {
#ifdef _WIN32
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (sock < 0) {
        perror("Socket creation failed");
    }
    return sock;
}

void close_socket(int sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

int connect_to_server(int sock, const char *server_ip, int server_port) {
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address or address not supported");
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        return -1;
    }
    return 0;
}

size_t send_data(int sock, const char *data, size_t size) {
    size_t bytes_sent = send(sock, data, size, 0);
    if (bytes_sent < 0) {
        perror("Send failed");
    }
    return bytes_sent;
}

size_t receive_data(int sock, char *buffer, size_t buffer_size) {
    memset(buffer, 0, buffer_size);
    size_t bytes_received = recv(sock, buffer, buffer_size - 1, 0);
    if (bytes_received < 0) {
        perror("Receive failed");
    }
    return bytes_received;
}

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#define SAMPLE_RATE (48000)
#define CHANNELS (1)

int sock;

float* data;
std::atomic_int32_t readCount;

std::queue<float*> audioQueue;
std::mutex audioMutex;
std::condition_variable audioCond;

void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    send_data(sock, (const char*)pInput, sizeof(float) * frameCount * CHANNELS);

    std::unique_lock<std::mutex> lock(audioMutex);
    if (!audioQueue.empty()) {
        float* receivedData = audioQueue.front();
        memcpy(pOutput, receivedData, sizeof(float) * frameCount * CHANNELS);
        audioQueue.pop();
        free(receivedData);
    } else {
        memset(pOutput, 0, sizeof(float) * frameCount * CHANNELS);
    }
}

void receive_audio_data() {
    while (1) {
        float* buffer = (float*)malloc(sizeof(float) * SAMPLE_RATE * CHANNELS);
        size_t bytes_received = receive_data(sock, (char*)buffer, sizeof(float) * SAMPLE_RATE * CHANNELS);
        if (bytes_received > 0) {
            std::unique_lock<std::mutex> lock(audioMutex);
            audioQueue.push(buffer);
            audioCond.notify_one();
        } else {
            free(buffer);
        }
    }
}

int main(int argc, char* argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char* server_ip = argv[1];
    int server_port = atoi(argv[2]);

    init_sockets();
    readCount.store(0);

    sock = create_socket();
    if (sock < 0) {
        cleanup_sockets();
        return EXIT_FAILURE;
    }

    if (connect_to_server(sock, server_ip, server_port) < 0) {
        close_socket(sock);
        cleanup_sockets();
        return EXIT_FAILURE;
    }

    printf("Connected to the server at %s:%d.\n", server_ip, server_port);

    data = (float*)malloc(sizeof(float) * SAMPLE_RATE * CHANNELS);
    memset(data, 0, sizeof(float) * SAMPLE_RATE * CHANNELS);

    ma_device_config config = ma_device_config_init(ma_device_type_duplex);
    config.capture.format    = ma_format_f32;
    config.capture.channels  = CHANNELS;
    config.playback.format   = ma_format_f32;
    config.playback.channels = CHANNELS;
    config.sampleRate        = SAMPLE_RATE;
    config.dataCallback      = data_callback;
    config.pUserData         = NULL;

    ma_device device;
    if (ma_device_init(NULL, &config, &device) != MA_SUCCESS) {
        return -1;  // Failed to initialize the device.
    }

    ma_device_start(&device);

    std::thread receiverThread(receive_audio_data);

    // Main loop
    while (1) {
        // Keep the main thread alive
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ma_device_uninit(&device);
    close_socket(sock);
    cleanup_sockets();
    return 0;
}
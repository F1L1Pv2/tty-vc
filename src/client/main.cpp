#include <stdio.h>
#include <atomic>
#include <thread>
#include <cstring>

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
    size_t bytes_received = recv(sock, buffer, buffer_size, 0);
    if (bytes_received < 0) {
        perror("Receive failed");
    }
    return bytes_received;
}

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#define SAMPLE_RATE (48000)
#define CHANNELS (1)
#define RING_BUFFER_SIZE (SAMPLE_RATE * CHANNELS * 10) // 2 seconds of audio

struct RingBuffer {
    float buffer[RING_BUFFER_SIZE];
    std::atomic<size_t> writeIndex{0};
    std::atomic<size_t> readIndex{0};
};

RingBuffer audioBuffer;

int sock;

void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    // Send audio data to the server
    send_data(sock, (const char*)pInput, sizeof(float) * frameCount * CHANNELS);

    // Playback received audio data
    float* output = (float*)pOutput;
    size_t readIndex = audioBuffer.readIndex.load(std::memory_order_relaxed);
    size_t writeIndex = audioBuffer.writeIndex.load(std::memory_order_acquire);

    size_t available = (writeIndex >= readIndex) ? (writeIndex - readIndex) : (RING_BUFFER_SIZE - readIndex + writeIndex);
    size_t toCopy = (available > frameCount * CHANNELS) ? frameCount * CHANNELS : available;

    if (toCopy > 0) {
        if (readIndex + toCopy <= RING_BUFFER_SIZE) {
            memcpy(output, &audioBuffer.buffer[readIndex], toCopy * sizeof(float));
        } else {
            size_t firstPart = RING_BUFFER_SIZE - readIndex;
            memcpy(output, &audioBuffer.buffer[readIndex], firstPart * sizeof(float));
            memcpy(output + firstPart, &audioBuffer.buffer[0], (toCopy - firstPart) * sizeof(float));
        }
        audioBuffer.readIndex.store((readIndex + toCopy) % RING_BUFFER_SIZE, std::memory_order_release);
    }

    if (toCopy < frameCount * CHANNELS) {
        memset(output + toCopy, 0, (frameCount * CHANNELS - toCopy) * sizeof(float));
    }
}

void receive_audio_data() {
    const size_t metadata_size = sizeof(uint32_t) * 2; // Size of num_users and sample_count
    const size_t max_audio_data_size = SAMPLE_RATE * CHANNELS * sizeof(float);
    char packet[metadata_size + max_audio_data_size];

    while (1) {
        // Receive the entire packet (metadata + audio data)
        size_t bytes_received = receive_data(sock, packet, sizeof(packet));
        if (bytes_received < metadata_size) {
            fprintf(stderr, "Failed to receive complete metadata\n");
            continue;
        }

        // Extract metadata
        uint32_t num_users = *reinterpret_cast<uint32_t*>(packet);
        uint32_t sample_count = *reinterpret_cast<uint32_t*>(packet + sizeof(uint32_t));

        // Calculate the size of the audio data
        size_t audio_data_size = sample_count * sizeof(float);

        // Ensure we received the complete audio data
        if (bytes_received < metadata_size + audio_data_size) {
            fprintf(stderr, "Failed to receive complete audio data\n");
            continue;
        }

        // Extract the audio data
        float* audio_data = reinterpret_cast<float*>(packet + metadata_size);

        // Write the audio data to the ring buffer
        size_t writeIndex = audioBuffer.writeIndex.load(std::memory_order_relaxed);
        size_t readIndex = audioBuffer.readIndex.load(std::memory_order_acquire);

        size_t availableSpace = (readIndex > writeIndex) ? (readIndex - writeIndex - 1) : (RING_BUFFER_SIZE - writeIndex + readIndex - 1);
        size_t toWrite = (availableSpace > sample_count) ? sample_count : availableSpace;

        if (toWrite > 0) {
            if (writeIndex + toWrite <= RING_BUFFER_SIZE) {
                memcpy(&audioBuffer.buffer[writeIndex], audio_data, toWrite * sizeof(float));
            } else {
                size_t firstPart = RING_BUFFER_SIZE - writeIndex;
                memcpy(&audioBuffer.buffer[writeIndex], audio_data, firstPart * sizeof(float));
                memcpy(&audioBuffer.buffer[0], audio_data + firstPart, (toWrite - firstPart) * sizeof(float));
            }
            audioBuffer.writeIndex.store((writeIndex + toWrite) % RING_BUFFER_SIZE, std::memory_order_release);
        }
    }
}

int main(int argc, char** argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char* server_ip = argv[1];
    int server_port = atoi(argv[2]);

    if (server_port <= 0 || server_port > 65535) {
        fprintf(stderr, "Invalid port: %d\n", server_port);
        return EXIT_FAILURE;
    }

    init_sockets();

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
        return -1;
    }

    ma_device_start(&device);

    std::thread receiverThread(receive_audio_data);

    while (1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ma_device_uninit(&device);
    close_socket(sock);
    cleanup_sockets();
    return 0;
}
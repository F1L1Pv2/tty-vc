#include <stdio.h>
#include <atomic>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <chrono>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#endif

#include "../RingBuffer.h"
#include <stdlib.h>

#define BUFFER_SIZE 1024
#define SAMPLE_RATE 44100
#define CHANNELS 1
#define FRAME_COUNT 1024
#define JITTER_BUFFER_SIZE 8 //  packets of buffering to handle network jitter

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

int sock;
std::atomic<bool> running{true};

struct AudioPacket {
    std::vector<float> data;
    std::chrono::steady_clock::time_point timestamp;
};

class JitterBuffer {
private:
    std::queue<AudioPacket> buffer;
    std::mutex mtx;
    std::condition_variable cv;
    size_t max_size;
    
public:
    JitterBuffer(size_t size) : max_size(size) {}
    
    void push(AudioPacket&& packet) {
        std::unique_lock<std::mutex> lock(mtx);
        if (buffer.size() >= max_size) {
            buffer.pop(); // Drop oldest packet if buffer is full
        }
        buffer.push(std::move(packet));
        cv.notify_one();
    }
    
    bool pop(AudioPacket& packet) {
        std::unique_lock<std::mutex> lock(mtx);
        if (buffer.empty()) {
            return false;
        }
        packet = std::move(buffer.front());
        buffer.pop();
        return true;
    }
    
    void clear() {
        std::unique_lock<std::mutex> lock(mtx);
        while (!buffer.empty()) {
            buffer.pop();
        }
    }
    
    size_t size() {
        std::unique_lock<std::mutex> lock(mtx);
        return buffer.size();
    }
};

JitterBuffer jitterBuffer(JITTER_BUFFER_SIZE);

void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    // Send microphone data
    if (pInput) {
        send_data(sock, reinterpret_cast<const char*>(pInput), 
                 frameCount * CHANNELS * sizeof(float));
    }
    
    // Playback received audio
    AudioPacket packet;
    if (jitterBuffer.pop(packet)) {
        size_t samplesToCopy = std::min(packet.data.size(), static_cast<size_t>(frameCount * CHANNELS));
        memcpy(pOutput, packet.data.data(), samplesToCopy * sizeof(float));
        
        // If we didn't get enough samples, fill the rest with silence
        if (samplesToCopy < frameCount * CHANNELS) {
            memset(reinterpret_cast<float*>(pOutput) + samplesToCopy, 0, 
                  (frameCount * CHANNELS - samplesToCopy) * sizeof(float));
        }
    } else {
        // No data available - output silence
        memset(pOutput, 0, frameCount * CHANNELS * sizeof(float));
    }
}

void receive_audio_data() {
    std::vector<float> buffer(FRAME_COUNT * CHANNELS);
    
    while (running) {
        size_t bytes_received = receive_data(sock, reinterpret_cast<char*>(buffer.data()), 
                                     buffer.size() * sizeof(float));
        if (bytes_received > 0) {
            AudioPacket packet;
            packet.data.resize(bytes_received / sizeof(float));
            memcpy(packet.data.data(), buffer.data(), bytes_received);
            packet.timestamp = std::chrono::steady_clock::now();
            
            jitterBuffer.push(std::move(packet));
        } else if (bytes_received == 0) {
            // Connection closed
            running = false;
            break;
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char* server_ip = argv[1];
    int server_port = atoi(argv[2]);

    init_sockets();

    sock = create_socket();
    if (sock < 0) {
        cleanup_sockets();
        return EXIT_FAILURE;
    }

    // Set TCP_NODELAY to disable Nagle's algorithm for lower latency
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));

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
        fprintf(stderr, "Failed to initialize audio device\n");
        close_socket(sock);
        cleanup_sockets();
        return EXIT_FAILURE;
    }

    if (ma_device_start(&device) != MA_SUCCESS) {
        fprintf(stderr, "Failed to start audio device\n");
        ma_device_uninit(&device);
        close_socket(sock);
        cleanup_sockets();
        return EXIT_FAILURE;
    }

    std::thread receiverThread(receive_audio_data);

    // Main loop
    while (running) {
        // Monitor jitter buffer fill level
        size_t buffer_size = jitterBuffer.size();
        if (buffer_size < 1) {
            // Buffer underrun - we might want to increase buffer size
            printf("Warning: Jitter buffer underrun (%zu packets)\n", buffer_size);
        } else if (buffer_size > JITTER_BUFFER_SIZE * 2) {
            // Buffer overrun - we might want to drop some packets
            printf("Warning: Jitter buffer overrun (%zu packets)\n", buffer_size);
            jitterBuffer.clear();
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Cleanup
    receiverThread.join();
    ma_device_uninit(&device);
    close_socket(sock);
    cleanup_sockets();

    return 0;
}
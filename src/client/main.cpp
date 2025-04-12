#include <stdio.h>
#include <atomic>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>



#ifdef _WIN32
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
#define FRAME_COUNT (1024)  // Adjust based on latency requirements

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




struct SendPacket{
    void* data;
    size_t size;
};

ContigousAsyncBuffer* audioQueue;
ContigousAsyncBuffer* sendPackets;

void mic_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    SendPacket packet;
    packet.size = sizeof(float) * frameCount * CHANNELS;
    packet.data = malloc(packet.size);
    memcpy(packet.data,pInput,packet.size);
    sendPackets->write(&packet,sizeof(SendPacket));
}

void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    SendPacket packet;
    packet.size = sizeof(float) * frameCount * CHANNELS;
    packet.data = malloc(packet.size);
    memcpy(packet.data,pInput,packet.size);
    sendPackets->write(&packet,sizeof(SendPacket));

    ContigousAsyncBufferItem item = audioQueue->read();

    if (item.data != 0 && item.size != 0) {
        if(item.size > sizeof(float) * frameCount * CHANNELS){
            memcpy(pOutput, item.data, sizeof(float) * frameCount * CHANNELS);
            return;
        }
        memcpy(pOutput, item.data, item.size);
        if(item.size < sizeof(float) * frameCount * CHANNELS){
            memset((uint8_t*)pOutput+item.size, 0, (sizeof(float) * frameCount * CHANNELS) - item.size);
        }
    } else {
        memset(pOutput, 0, sizeof(float) * frameCount * CHANNELS);
    }
}

void send_audio_data(){
    while(running){
        {

            ContigousAsyncBufferItem item = sendPackets->read();

            if (item.data != 0 && item.size == sizeof(SendPacket)) {
                SendPacket* packet = (SendPacket*)item.data;
                send_data(sock, (const char*)packet->data, packet->size);
                free(packet->data);
            }
        }
    }
}

void receive_audio_data() {
    std::vector<float> buffer(FRAME_COUNT * CHANNELS);
    while (running) {
        size_t bytes_received = receive_data(sock, (char*)buffer.data(), sizeof(float) * FRAME_COUNT * CHANNELS);
        if (bytes_received > 0) {
            audioQueue->write(buffer.data(),bytes_received);
        }
    }
}

int main(int argc, char* argv[])
{
    audioQueue = new ContigousAsyncBuffer(SAMPLE_RATE*CHANNELS*BUFFER_SIZE);
    sendPackets = new ContigousAsyncBuffer(sizeof(SendPacket)*BUFFER_SIZE);

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
        close_socket(sock);
        cleanup_sockets();
        return -1;  // Failed to initialize the device.
    }

    ma_device_start(&device);

    std::thread receiverThread(receive_audio_data);

    std::thread senderThread(send_audio_data);

    // Main loop
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    running = false;
    receiverThread.join();
    senderThread.join();
    ma_device_uninit(&device);
    close_socket(sock);
    cleanup_sockets();

    free(audioQueue);
    free(sendPackets);

    return 0;
}
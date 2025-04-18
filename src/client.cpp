#include <stdio.h>
#include <atomic>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <chrono>
#include <opusfile/include/opusfile.h>

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
#include <netinet/tcp.h>
#include <netdb.h>
#endif

#include <stdlib.h>

#define BUFFER_SIZE 1024
#define SAMPLE_RATE 48000  // Opus native sample rate
#define CHANNELS 1
#define FRAME_SIZE 960     // 20ms frames at 48kHz
#define JITTER_BUFFER_SIZE 8 // packets of buffering to handle network jitter
#define OPUS_APPLICATION OPUS_APPLICATION_VOIP
#define MAX_PACKET_SIZE 1500

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

int connect_to_server(int sock, const char *server_name, int server_port) {
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    // First try to interpret as IP address
    if (inet_pton(AF_INET, server_name, &server_addr.sin_addr) <= 0) {
        // If not an IP address, try to resolve as hostname
        struct hostent *he = gethostbyname(server_name);
        if (he == NULL) {
#ifdef _WIN32
            fprintf(stderr, "gethostbyname failed with error: %d\n", WSAGetLastError());
#else
            herror("gethostbyname failed");
#endif
            return -1;
        }
        
        // Take the first address
        memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        return -1;
    }
    return 0;
}

size_t send_data(int sock, const char *data, size_t size) {
    // First send the size of the packet (4 bytes)
    uint32_t packet_size = htonl(static_cast<uint32_t>(size));
    if (send(sock, reinterpret_cast<const char*>(&packet_size), sizeof(packet_size), 0) != sizeof(packet_size)) {
        perror("Failed to send packet size");
        return 0;
    }
    
    // Then send the actual data
    size_t bytes_sent = send(sock, data, size, 0);
    if (bytes_sent < 0) {
        perror("Send failed");
    }
    return bytes_sent;
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
        fprintf(stderr, "Packet too large: %u > %zu\n", packet_size, buffer_size);
        return 0;
    }
    
    // Then receive the actual data
    size_t bytes_received = recv(sock, buffer, packet_size, 0);
    if (bytes_received < 0) {
        perror("Receive failed");
        return 0;
    }
    return static_cast<size_t>(bytes_received);
}

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

int sock;
std::atomic<bool> running{true};

struct AudioPacket {
    std::vector<unsigned char> data; // Compressed Opus data
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

// Opus encoder and decoder
OpusEncoder* encoder = nullptr;
OpusDecoder* decoder = nullptr;

void init_opus() {
    int error;
    
    // Initialize encoder
    encoder = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION, &error);
    if (error != OPUS_OK) {
        fprintf(stderr, "Failed to create Opus encoder: %s\n", opus_strerror(error));
        exit(EXIT_FAILURE);
    }
    
    // Set encoder options
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(16000)); // 16 kbps for voice
    opus_encoder_ctl(encoder, OPUS_SET_VBR(1)); // Enable VBR
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(8)); // Max complexity for best quality
    
    // Initialize decoder
    decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &error);
    if (error != OPUS_OK) {
        fprintf(stderr, "Failed to create Opus decoder: %s\n", opus_strerror(error));
        exit(EXIT_FAILURE);
    }
}

void cleanup_opus() {
    if (encoder) {
        opus_encoder_destroy(encoder);
        encoder = nullptr;
    }
    if (decoder) {
        opus_decoder_destroy(decoder);
        decoder = nullptr;
    }
}

// Capture callback for microphone input
void capture_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pOutput; // Unused in capture callback
    
    if (pInput) {
        // Encode the audio with Opus
        unsigned char compressed_data[MAX_PACKET_SIZE];
        int compressed_size = opus_encode_float(encoder, 
                                              reinterpret_cast<const float*>(pInput), 
                                              FRAME_SIZE, 
                                              compressed_data, 
                                              MAX_PACKET_SIZE);
        
        if (compressed_size > 0) {
            send_data(sock, reinterpret_cast<const char*>(compressed_data), compressed_size);
        } else {
            fprintf(stderr, "Opus encode error: %s\n", opus_strerror(compressed_size));
        }
    }
}

// Playback callback for headphone output
void playback_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pInput; // Unused in playback callback
    
    AudioPacket packet;
    memset(pOutput, 0, frameCount * CHANNELS * sizeof(float));
    if (jitterBuffer.pop(packet)) {
        float pcm_data[FRAME_SIZE * CHANNELS];
        int decoded_samples = opus_decode_float(decoder, 
                                              packet.data.data(), 
                                              packet.data.size(), 
                                              pcm_data,
                                              FRAME_SIZE, 
                                              0);
        
        if (decoded_samples > 0) {
            size_t samplesToCopy = std::min(static_cast<size_t>(decoded_samples * CHANNELS), 
                                           static_cast<size_t>(frameCount * CHANNELS));
            memcpy(pOutput, pcm_data, samplesToCopy * sizeof(float)); 
        } else {
            fprintf(stderr, "Opus decode error: %s\n", opus_strerror(decoded_samples));
        }
    }
}

void receive_audio_data() {
    std::vector<unsigned char> receive_buffer(MAX_PACKET_SIZE);
    
    while (running) {
        size_t bytes_received = receive_data(sock, reinterpret_cast<char*>(receive_buffer.data()), 
                                            receive_buffer.size());
        if (bytes_received > 0) {
            AudioPacket packet;
            packet.data.assign(receive_buffer.begin(), receive_buffer.begin() + bytes_received);
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
        fprintf(stderr, "Usage: %s <server_hostname_or_ip> <server_port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char* server_name = argv[1];
    int server_port = atoi(argv[2]);

    init_sockets();
    init_opus();

    sock = create_socket();
    if (sock < 0) {
        cleanup_opus();
        cleanup_sockets();
        return EXIT_FAILURE;
    }

    // Set TCP_NODELAY to disable Nagle's algorithm for lower latency
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));

    if (connect_to_server(sock, server_name, server_port) < 0) {
        close_socket(sock);
        cleanup_opus();
        cleanup_sockets();
        return EXIT_FAILURE;
    }

    printf("Connected to the server at %s:%d.\n", server_name, server_port);

    // Initialize separate capture (microphone) and playback (headphones) devices
    ma_device_config capture_config = ma_device_config_init(ma_device_type_capture);
    capture_config.capture.format   = ma_format_f32;
    capture_config.capture.channels = CHANNELS;
    capture_config.sampleRate      = SAMPLE_RATE;
    capture_config.dataCallback    = capture_callback;
    capture_config.periodSizeInFrames = FRAME_SIZE;

    ma_device_config playback_config = ma_device_config_init(ma_device_type_playback);
    playback_config.playback.format   = ma_format_f32;
    playback_config.playback.channels = CHANNELS;
    playback_config.sampleRate        = SAMPLE_RATE;
    playback_config.dataCallback      = playback_callback;
    playback_config.periodSizeInFrames = FRAME_SIZE;

    ma_device capture_device;
    ma_device playback_device;

    // Start capture device (microphone)
    if (ma_device_init(NULL, &capture_config, &capture_device) != MA_SUCCESS) {
        fprintf(stderr, "Failed to initialize capture device\n");
        close_socket(sock);
        cleanup_opus();
        cleanup_sockets();
        return EXIT_FAILURE;
    }

    if (ma_device_start(&capture_device) != MA_SUCCESS) {
        fprintf(stderr, "Failed to start capture device\n");
        ma_device_uninit(&capture_device);
        close_socket(sock);
        cleanup_opus();
        cleanup_sockets();
        return EXIT_FAILURE;
    }

    // Start playback device (headphones)
    if (ma_device_init(NULL, &playback_config, &playback_device) != MA_SUCCESS) {
        fprintf(stderr, "Failed to initialize playback device\n");
        ma_device_uninit(&capture_device);
        close_socket(sock);
        cleanup_opus();
        cleanup_sockets();
        return EXIT_FAILURE;
    }

    if (ma_device_start(&playback_device) != MA_SUCCESS) {
        fprintf(stderr, "Failed to start playback device\n");
        ma_device_uninit(&playback_device);
        ma_device_uninit(&capture_device);
        close_socket(sock);
        cleanup_opus();
        cleanup_sockets();
        return EXIT_FAILURE;
    }

    printf("Audio devices initialized:\n");
    printf("  Capture: %s\n", capture_device.capture.name);
    printf("  Playback: %s\n", playback_device.playback.name);

    std::thread receiverThread(receive_audio_data);

    // Main loop
    while (running) {
        // Monitor jitter buffer fill level
        size_t buffer_size = jitterBuffer.size();
        if (buffer_size < 1) {
            // Buffer underrun - we might want to increase buffer size
            // printf("Warning: Jitter buffer underrun (%zu packets)\n", buffer_size);
        } else if (buffer_size > JITTER_BUFFER_SIZE * 2) {
            // Buffer overrun - we might want to drop some packets
            // printf("Warning: Jitter buffer overrun (%zu packets)\n", buffer_size);
            jitterBuffer.clear();
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Cleanup
    receiverThread.join();
    ma_device_uninit(&playback_device);
    ma_device_uninit(&capture_device);
    close_socket(sock);
    cleanup_opus();
    cleanup_sockets();

    return 0;
}
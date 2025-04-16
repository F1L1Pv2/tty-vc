#include <stdio.h>
 #include <atomic>
 #include <thread>
 #include <queue>
 #include <mutex>
 #include <condition_variable>
 #include <vector>
 #include <chrono>
 #include <opusfile/include/opusfile.h>
 #include <unordered_map>
 #include <memory>
 #include <numeric>

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
 #include <fcntl.h>
 #endif

 #include <stdlib.h>

 #define BUFFER_SIZE 1024
 #define SAMPLE_RATE 48000
 #define CHANNELS 1
 #define FRAME_SIZE 960
 #define JITTER_BUFFER_SIZE 8
 #define OPUS_APPLICATION OPUS_APPLICATION_VOIP
 #define MAX_PACKET_SIZE 1500
 #define MAX_NETWORK_PACKET_SIZE (MAX_PACKET_SIZE + sizeof(uint32_t) + sizeof(uint32_t)) // Size + UserID + OpusData

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
     SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
 #else
     int sock = socket(AF_INET, SOCK_STREAM, 0);
 #endif
     if (sock < 0) {
         perror("Socket creation failed");
     }
     return (int)sock;
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

     if (inet_pton(AF_INET, server_name, &server_addr.sin_addr) <= 0) {
         struct hostent *he = gethostbyname(server_name);
         if (he == NULL) {
 #ifdef _WIN32
             fprintf(stderr, "gethostbyname failed with error: %d\n", WSAGetLastError());
 #else
             herror("gethostbyname failed");
 #endif
             return -1;
         }
         memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
     }

     if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
 #ifdef _WIN32
         fprintf(stderr, "Connection failed with error: %d\n", WSAGetLastError());
 #else
         perror("Connection failed");
 #endif
         return -1;
     }
     return 0;
 }

 size_t send_opus_packet(int sock, const unsigned char *opus_data, size_t opus_size) {
     if (opus_size > MAX_PACKET_SIZE) {
         fprintf(stderr, "Opus packet too large to send: %zu\n", opus_size);
         return 0;
     }

     uint32_t net_opus_size = htonl(static_cast<uint32_t>(opus_size));

     // Send size
     size_t sent = send(sock, reinterpret_cast<const char*>(&net_opus_size), sizeof(net_opus_size), 0);
     if (sent != sizeof(net_opus_size)) {
         perror("Failed to send packet size");
         return 0;
     }

     // Send Opus data
     sent = send(sock, reinterpret_cast<const char*>(opus_data), opus_size, 0);
     if (sent < 0) {
         perror("Send failed");
         return 0;
     }
      if ((size_t)sent != opus_size) {
         fprintf(stderr, "Incomplete send\n");
         return sent;
     }
     return sent;
 }

 size_t receive_full_packet(int sock, uint32_t& user_id, unsigned char* opus_buffer, size_t opus_buffer_capacity) {
     uint32_t payload_size_net;
     size_t bytes_received = recv(sock, reinterpret_cast<char*>(&payload_size_net), sizeof(payload_size_net), MSG_WAITALL);

     if (bytes_received == 0) {
         printf("Server closed connection.\n");
         return 0;
     }
     if (bytes_received < 0) {
 #ifdef _WIN32
        int error = WSAGetLastError();
        if (error == WSAECONNRESET || error == WSAECONNABORTED) {
             printf("Connection reset/aborted.\n"); return 0;
        }
        fprintf(stderr, "Failed to receive payload size: WSAError %d\n", error);
 #else
        if (errno == ECONNRESET || errno == EPIPE) {
            printf("Connection reset/pipe broken.\n"); return 0;
        }
        perror("Failed to receive payload size");
 #endif
        return -1;
     }
      if (bytes_received != sizeof(payload_size_net)) {
           fprintf(stderr, "Incomplete receive for payload size (%zd bytes)\n", bytes_received);
           return -1;
      }

     uint32_t payload_size = ntohl(payload_size_net);

     if (payload_size < sizeof(uint32_t)) {
         fprintf(stderr, "Invalid payload size received: %u (too small)\n", payload_size);
         return -1;
     }
      if (payload_size > MAX_NETWORK_PACKET_SIZE - sizeof(uint32_t)) { // Sanity check
           fprintf(stderr, "Payload size too large: %u\n", payload_size);
           return -1;
      }

     size_t opus_data_size = payload_size - sizeof(uint32_t);
     if (opus_data_size > opus_buffer_capacity) {
         fprintf(stderr, "Opus data too large for buffer: %zu > %zu\n", opus_data_size, opus_buffer_capacity);
         std::vector<char> discard_buffer(payload_size);
         recv(sock, discard_buffer.data(), payload_size, MSG_WAITALL);
         return -2;
     }

     std::vector<unsigned char> payload_buffer(payload_size);
     bytes_received = recv(sock, reinterpret_cast<char*>(payload_buffer.data()), payload_size, MSG_WAITALL);

     if (bytes_received == 0) {
         printf("Server closed connection during payload receive.\n");
         return 0;
     }
      if (bytes_received < 0) {
 #ifdef _WIN32
          fprintf(stderr, "Failed to receive payload: WSAError %d\n", WSAGetLastError());
 #else
          perror("Failed to receive payload");
 #endif
          return -1;
      }
      if ((uint32_t)bytes_received != payload_size) {
          fprintf(stderr, "Incomplete receive for payload (%zd / %u bytes)\n", bytes_received, payload_size);
          return -1;
      }


     uint32_t user_id_net;
     memcpy(&user_id_net, payload_buffer.data(), sizeof(uint32_t));
     user_id = ntohl(user_id_net);
     memcpy(opus_buffer, payload_buffer.data() + sizeof(uint32_t), opus_data_size);
     return static_cast<size_t>(opus_data_size);
 }


 #define MINIAUDIO_IMPLEMENTATION
 #include "miniaudio.h"

 int sock;
 std::atomic<bool> running{true};
 uint32_t my_user_id = -1;

 struct AudioPacket {
     std::vector<unsigned char> data;
     std::chrono::steady_clock::time_point received_time;
 };

 class JitterBuffer {
    private:
        std::queue<AudioPacket> buffer;
        std::mutex mtx;
        size_t max_size;
        size_t target_fill;
        std::chrono::milliseconds packet_duration{20};
   
    public:
        JitterBuffer(size_t size) : max_size(size), target_fill(size/2) {}
        JitterBuffer(const JitterBuffer&) = delete;
        JitterBuffer& operator=(const JitterBuffer&) = delete;
   
        JitterBuffer(JitterBuffer&& other) noexcept :
            max_size(other.max_size),
            target_fill(other.target_fill),
            packet_duration(other.packet_duration)
        {
            std::lock_guard<std::mutex> lock(other.mtx);
            buffer = std::move(other.buffer);
        }
   
        JitterBuffer& operator=(JitterBuffer&& other) noexcept {
            if (this != &other) {
                std::scoped_lock lock(mtx, other.mtx);
                buffer = std::move(other.buffer);
                max_size = other.max_size;
                target_fill = other.target_fill;
                packet_duration = other.packet_duration;
            }
            return *this;
        }
   
        ~JitterBuffer() = default;
        void push(AudioPacket&& packet) {
            std::unique_lock<std::mutex> lock(mtx);
            if (buffer.size() >= max_size) {
                buffer.pop();
            }
            buffer.push(std::move(packet));
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
            std::queue<AudioPacket> empty;
            std::swap(buffer, empty);
        }
   
        size_t size() {
            std::unique_lock<std::mutex> lock(mtx);
            return buffer.size();
        }
    };

 struct RemoteClient {
     uint32_t user_id;
     JitterBuffer jitterBuffer;
     OpusDecoder* decoder;
     std::vector<float> decoded_pcm;
     std::chrono::steady_clock::time_point last_packet_time;

     RemoteClient(uint32_t id) :
         user_id(id),
         jitterBuffer(JITTER_BUFFER_SIZE),
         decoder(nullptr),
         decoded_pcm(FRAME_SIZE * CHANNELS)
     {
         int error = 0;
         decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &error);
         if (error != OPUS_OK) {
             fprintf(stderr, "Failed to create Opus decoder for user %u: %s\n", id, opus_strerror(error));
         }
         last_packet_time = std::chrono::steady_clock::now();
     }

     ~RemoteClient() {
         if (decoder) {
             opus_decoder_destroy(decoder);
         }
     }

     RemoteClient(const RemoteClient&) = delete;
     RemoteClient& operator=(const RemoteClient&) = delete;

     RemoteClient(RemoteClient&& other) noexcept :
         user_id(other.user_id),
         jitterBuffer(std::move(other.jitterBuffer)),
         decoder(other.decoder),
         decoded_pcm(std::move(other.decoded_pcm)),
         last_packet_time(other.last_packet_time)
     {
         other.decoder = nullptr;
     }

     RemoteClient& operator=(RemoteClient&& other) noexcept {
         if (this != &other) {
             if (decoder) {
                 opus_decoder_destroy(decoder);
             }
             user_id = other.user_id;
             jitterBuffer = std::move(other.jitterBuffer);
             decoder = other.decoder;
             decoded_pcm = std::move(other.decoded_pcm);
             last_packet_time = other.last_packet_time;
             other.decoder = nullptr;
         }
         return *this;
     }
 };

 std::unordered_map<uint32_t, RemoteClient> remote_clients;
 std::mutex clients_mutex;

 OpusEncoder* encoder = nullptr;

 void init_opus_encoder() {
     int error;
     encoder = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION, &error);
     if (error != OPUS_OK) {
         fprintf(stderr, "Failed to create Opus encoder: %s\n", opus_strerror(error));
         exit(EXIT_FAILURE);
     }
     opus_encoder_ctl(encoder, OPUS_SET_BITRATE(32000));
     opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
     opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(8));
 }

 void cleanup_opus() {
     if (encoder) {
         opus_encoder_destroy(encoder);
         encoder = nullptr;
     }
      std::lock_guard<std::mutex> lock(clients_mutex);
      remote_clients.clear();
 }


 void capture_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
     (void)pOutput;
     (void)pDevice;

     if (!running || !encoder || sock < 0) {
         return;
     }

     if (pInput && frameCount == FRAME_SIZE) {
         unsigned char compressed_data[MAX_PACKET_SIZE];
         int compressed_size = opus_encode_float(encoder,
                                                 reinterpret_cast<const float*>(pInput),
                                                 FRAME_SIZE,
                                                 compressed_data,
                                                 MAX_PACKET_SIZE);

         if (compressed_size > 0) {
             send_opus_packet(sock, compressed_data, compressed_size);
         } else if (compressed_size < 0){
             fprintf(stderr, "Opus encode error: %s\n", opus_strerror(compressed_size));
         }
     } else if (frameCount != FRAME_SIZE) {
          fprintf(stderr,"Warning: Capture frameCount mismatch: %u != %d\n", frameCount, FRAME_SIZE);
     }
 }

 void playback_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
     (void)pInput;
     (void)pDevice;

     if (!running || frameCount == 0) return;

     if (frameCount != FRAME_SIZE) {
          fprintf(stderr,"Warning: Playback frameCount mismatch: %u != %d\n", frameCount, FRAME_SIZE);
          memset(pOutput, 0, frameCount * CHANNELS * sizeof(float));
         return;
     }

     float* out_buffer = static_cast<float*>(pOutput);
     memset(out_buffer, 0, frameCount * CHANNELS * sizeof(float));

     int active_mix_count = 0;

     std::lock_guard<std::mutex> lock(clients_mutex);

     auto it = remote_clients.begin();
     while (it != remote_clients.end()) {
         RemoteClient& client = it->second;
         AudioPacket packet;
         bool got_packet = client.jitterBuffer.pop(packet);
         int decoded_samples = 0;

         if (got_packet && client.decoder) {
             decoded_samples = opus_decode_float(client.decoder,
                                                 packet.data.empty() ? nullptr : packet.data.data(),
                                                 packet.data.size(),
                                                 client.decoded_pcm.data(),
                                                 FRAME_SIZE,
                                                 0);
             client.last_packet_time = std::chrono::steady_clock::now();
         } else if (client.decoder) {
             decoded_samples = opus_decode_float(client.decoder,
                                                 nullptr,
                                                 0,
                                                 client.decoded_pcm.data(),
                                                 FRAME_SIZE,
                                                 0);
              auto now = std::chrono::steady_clock::now();
              if (now - client.last_packet_time > std::chrono::seconds(5)) {
                   printf("Client %u timed out. Removing.\n", client.user_id);
                   it = remote_clients.erase(it);
                   continue;
              }
         } else {
              it++;
              continue;
         }


         if (decoded_samples > 0) {
             if (decoded_samples != FRAME_SIZE) {
                 if (decoded_samples < FRAME_SIZE) {
                      memset(client.decoded_pcm.data() + decoded_samples * CHANNELS, 0, (FRAME_SIZE - decoded_samples) * CHANNELS * sizeof(float));
                 }
             }
             for (ma_uint32 i = 0; i < (ma_uint32)FRAME_SIZE * CHANNELS; ++i) {
                 out_buffer[i] += client.decoded_pcm[i];
             }
             active_mix_count++;

         } else if (decoded_samples < 0) {
             fprintf(stderr, "Opus decode error for user %u: %s\n", client.user_id, opus_strerror(decoded_samples));
             client.jitterBuffer.clear();
         }

         it++;
     }

     if (active_mix_count > 0) {
         for (ma_uint32 i = 0; i < frameCount * CHANNELS; ++i) {
             if (out_buffer[i] > 1.0f) out_buffer[i] = 1.0f;
             else if (out_buffer[i] < -1.0f) out_buffer[i] = -1.0f;
         }
     }

 }
 void receive_audio_data() {
     std::vector<unsigned char> opus_receive_buffer(MAX_PACKET_SIZE);

     while (running) {
         uint32_t sender_user_id = 0;

         size_t opus_bytes_received = receive_full_packet(sock, sender_user_id, opus_receive_buffer.data(), opus_receive_buffer.size());

         if (opus_bytes_received > 0) {
             if (sender_user_id == my_user_id) {
                 continue;
             }

             AudioPacket packet;
             packet.data.assign(opus_receive_buffer.begin(), opus_receive_buffer.begin() + opus_bytes_received);
             packet.received_time = std::chrono::steady_clock::now();

             std::lock_guard<std::mutex> lock(clients_mutex);

             auto it = remote_clients.find(sender_user_id);
             if (it == remote_clients.end()) {
                  printf("Received first packet from new user ID: %u\n", sender_user_id);
                  auto result = remote_clients.emplace(std::piecewise_construct,
                                           std::forward_as_tuple(sender_user_id),
                                           std::forward_as_tuple(sender_user_id));
                  it = result.first;
                  if (!result.second || !it->second.decoder) {
                       fprintf(stderr,"Failed to create client entry or decoder for user %u\n", sender_user_id);
                       remote_clients.erase(it);
                       continue;
                  }
             }

             it->second.jitterBuffer.push(std::move(packet));

         } else if (opus_bytes_received == 0) {
             printf("Receive thread: Connection closed by server.\n");
             running = false;
             break;
         } else if (opus_bytes_received == -1) {
             fprintf(stderr, "Receive thread: Receive error.\n");
             running = false;
             break;
          } else if (opus_bytes_received == -2) {
          }
         std::this_thread::sleep_for(std::chrono::milliseconds(1));
     }
     printf("Receive thread finished.\n");
 }

 int main(int argc, char* argv[]) {
     if (argc != 3) {
         fprintf(stderr, "Usage: %s <server_hostname_or_ip> <server_port>\n", argv[0]);
         return EXIT_FAILURE;
     }

     const char* server_name = argv[1];
     int server_port = atoi(argv[2]);

     init_sockets();
     init_opus_encoder();

     sock = create_socket();
     if (sock < 0) {
         cleanup_opus();
         cleanup_sockets();
         return EXIT_FAILURE;
     }

 #ifdef _WIN32
     char flag = 1;
     if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == SOCKET_ERROR) {
         fprintf(stderr, "setsockopt(TCP_NODELAY) failed: %d\n", WSAGetLastError());
     }
 #else
     int flag = 1;
     if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (void*)&flag, sizeof(int)) < 0) {
         perror("setsockopt(TCP_NODELAY) failed");
     }
 #endif


     if (connect_to_server(sock, server_name, server_port) < 0) {
         close_socket(sock);
         cleanup_opus();
         cleanup_sockets();
         return EXIT_FAILURE;
     }

     printf("Connected to the server at %s:%d.\n", server_name, server_port);

     uint32_t net_user_id;
     size_t id_recv = recv(sock, reinterpret_cast<char*>(&net_user_id), sizeof(net_user_id), MSG_WAITALL);
      if (id_recv == sizeof(net_user_id)) {
           my_user_id = ntohl(net_user_id);
           printf("Received my User ID: %u\n", my_user_id);
      } else {
           fprintf(stderr, "Failed to receive User ID from server (recv returned %zd).\n", id_recv);
 #ifdef _WIN32
            fprintf(stderr, "WSAError: %d\n", WSAGetLastError());
 #else
            perror("recv User ID");
 #endif
           close_socket(sock);
           cleanup_opus();
           cleanup_sockets();
           return EXIT_FAILURE;
      }
     ma_device_config capture_config = ma_device_config_init(ma_device_type_capture);
     capture_config.capture.format   = ma_format_f32;
     capture_config.capture.channels = CHANNELS;
     capture_config.sampleRate       = SAMPLE_RATE;
     capture_config.dataCallback     = capture_callback;
     capture_config.periodSizeInFrames = FRAME_SIZE;

     ma_device_config playback_config = ma_device_config_init(ma_device_type_playback);
     playback_config.playback.format   = ma_format_f32;
     playback_config.playback.channels = CHANNELS;
     playback_config.sampleRate        = SAMPLE_RATE;
     playback_config.dataCallback      = playback_callback;
     playback_config.periodSizeInFrames = FRAME_SIZE;

     ma_device capture_device;
     ma_device playback_device;

     if (ma_device_init(NULL, &capture_config, &capture_device) != MA_SUCCESS) {
         fprintf(stderr, "Failed to initialize capture device\n");
         close_socket(sock);
         cleanup_opus();
         cleanup_sockets();
         return EXIT_FAILURE;
     }
     printf("Capture device initialized: %s\n", capture_device.capture.name);

     if (ma_device_init(NULL, &playback_config, &playback_device) != MA_SUCCESS) {
         fprintf(stderr, "Failed to initialize playback device\n");
         ma_device_uninit(&capture_device);
         close_socket(sock);
         cleanup_opus();
         cleanup_sockets();
         return EXIT_FAILURE;
     }
     printf("Playback device initialized: %s\n", playback_device.playback.name);

     if (ma_device_start(&capture_device) != MA_SUCCESS) {
         fprintf(stderr, "Failed to start capture device\n");
         ma_device_uninit(&playback_device);
         ma_device_uninit(&capture_device);
         close_socket(sock);
         cleanup_opus();
         cleanup_sockets();
         return EXIT_FAILURE;
     }
     if (ma_device_start(&playback_device) != MA_SUCCESS) {
         fprintf(stderr, "Failed to start playback device\n");
         ma_device_stop(&capture_device);
         ma_device_uninit(&playback_device);
         ma_device_uninit(&capture_device);
         close_socket(sock);
         cleanup_opus();
         cleanup_sockets();
         return EXIT_FAILURE;
     }

     printf("Audio devices started.\n");

     std::thread receiverThread(receive_audio_data);

     printf("Client running. Press Ctrl+C to exit.\n");

     while (running) {
         std::this_thread::sleep_for(std::chrono::milliseconds(100));
     }

     printf("Shutting down...\n");
     if (receiverThread.joinable()) {
         receiverThread.join();
     }
     printf("Receiver thread joined.\n");

     ma_device_stop(&playback_device);
     ma_device_stop(&capture_device);
     printf("Audio devices stopped.\n");
     ma_device_uninit(&playback_device);
     ma_device_uninit(&capture_device);
     printf("Audio devices uninitialized.\n");

     if (sock >= 0) {
         close_socket(sock);
         sock = -1;
     }
     printf("Socket closed.\n");

     cleanup_opus();
     printf("Opus resources cleaned up.\n");
     cleanup_sockets();
     printf("Sockets cleaned up.\n");

     printf("Client finished.\n");
     return 0;
 }
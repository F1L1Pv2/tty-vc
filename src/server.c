#include <stdio.h>
 #include <stdlib.h>
 #include <stdbool.h>
 #include <string.h>
 #include <unistd.h>
 #include <arpa/inet.h>
 #include <netinet/tcp.h> 
 #include <malloc.h>
 #include <assert.h>
 #include <pthread.h>
 #include <sys/socket.h>
 #include <errno.h>

 #define MAX_CLIENTS 10
 #define MAX_OPUS_PACKET_SIZE 1500 // Max expected Opus data size from client
 #define READ_BUFFER_SIZE (MAX_OPUS_PACKET_SIZE + 128)

 typedef struct {
     int fd;
     struct sockaddr_in addr;
     int user_id;
     bool active;
     pthread_t thread_id;
 } client_info;

 typedef struct queue_node {
     unsigned char* opus_data;
     size_t opus_data_size;
     int sender_user_id;
     struct queue_node* next;
 } queue_node;

 typedef struct {
     queue_node* head;
     queue_node* tail;
     pthread_mutex_t mutex;
     pthread_cond_t cond;
     bool shutdown;
 } queue_t;

 client_info clients[MAX_CLIENTS];
 int client_count = 0;
 pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
 queue_t broadcast_queue;

 void queue_init(queue_t* q) {
     q->head = q->tail = NULL;
     pthread_mutex_init(&q->mutex, NULL);
     pthread_cond_init(&q->cond, NULL);
     q->shutdown = false;
 }

 void queue_shutdown(queue_t* q) {
      pthread_mutex_lock(&q->mutex);
      q->shutdown = true;
      pthread_cond_broadcast(&q->cond);
      pthread_mutex_unlock(&q->mutex);
 }

 void queue_destroy(queue_t* q) {
      queue_node* current = q->head;
      while(current != NULL) {
           queue_node* next = current->next;
           free(current->opus_data);
           free(current);
           current = next;
      }
      q->head = q->tail = NULL;
      pthread_mutex_destroy(&q->mutex);
      pthread_cond_destroy(&q->cond);
 }

 bool enqueue(queue_t* q, const unsigned char* data, size_t size, int sender_id) {
     queue_node* node = malloc(sizeof(queue_node));
     if (!node) {
         perror("Failed to allocate queue node");
         return false;
     }
     node->opus_data = malloc(size);
     if (!node->opus_data) {
         perror("Failed to allocate data copy for queue");
         free(node);
         return false;
     }

     memcpy(node->opus_data, data, size);
     node->opus_data_size = size;
     node->sender_user_id = sender_id;
     node->next = NULL;

     pthread_mutex_lock(&q->mutex);
     if (q->shutdown) {
          pthread_mutex_unlock(&q->mutex);
          free(node->opus_data);
          free(node);
          return false;
     }

     if (q->tail == NULL) {
         q->head = q->tail = node;
     } else {
         q->tail->next = node;
         q->tail = node;
     }

     pthread_cond_signal(&q->cond);
     pthread_mutex_unlock(&q->mutex);
     return true;
 }

 queue_node* dequeue(queue_t* q) {
     pthread_mutex_lock(&q->mutex);

     while (q->head == NULL && !q->shutdown) {
         pthread_cond_wait(&q->cond, &q->mutex);
     }

     if (q->shutdown && q->head == NULL) {
          pthread_mutex_unlock(&q->mutex);
          return NULL;
     }

     queue_node* node = q->head;
     q->head = node->next;
     if (q->head == NULL) {
         q->tail = NULL;
     }

     pthread_mutex_unlock(&q->mutex);
     return node;
 }

 ssize_t send_all(int sockfd, const void *buf, size_t len, int flags) {
     size_t total = 0;
     const char *ptr = (const char*) buf;
     while (total < len) {
         ssize_t sent = send(sockfd, ptr + total, len - total, flags);
         if (sent < 0) {
             if (errno == EINTR) continue;
             if (errno == EPIPE || errno == ECONNRESET) return -2;
             perror("send failed");
             return -1;
         }
         if (sent == 0) {
             return -2;
         }
         total += sent;
     }
     return total;
 }

 ssize_t recv_all(int sockfd, void *buf, size_t len, int flags) {
      size_t total = 0;
      char *ptr = (char*) buf;
      while (total < len) {
           ssize_t received = recv(sockfd, ptr + total, len - total, flags);
           if (received < 0) {
                if (errno == EINTR) continue;
                 if (errno == ECONNRESET || errno == ETIMEDOUT) return -2;
                perror("recv failed");
                return -1;
           }
           if (received == 0) {
                return 0;
           }
           total += received;
      }
      return total;
 }


 void* broadcaster_thread(void* arg) {
     printf("Broadcaster thread started.\n");
     queue_t* q = (queue_t*)arg;

     while (1) {
         queue_node* node = dequeue(q);
         if (node == NULL) {
              // Queue is shutting down and empty
              break;
         }

         // Prepare the outgoing packet: [payload_size][user_id][opus_data]
         uint32_t user_id = (uint32_t)node->sender_user_id;
         size_t opus_data_size = node->opus_data_size;
         size_t user_id_size = sizeof(uint32_t);
         size_t payload_size = user_id_size + opus_data_size;
         size_t total_packet_size = sizeof(uint32_t) + payload_size;

         unsigned char* outgoing_packet = malloc(total_packet_size);
         if (!outgoing_packet) {
             perror("Failed to allocate memory for outgoing packet");
             free(node->opus_data);
             free(node);
             continue;
         }

         uint32_t net_payload_size = htonl(payload_size);
         uint32_t net_user_id = htonl(user_id);

         memcpy(outgoing_packet, &net_payload_size, sizeof(uint32_t));
         memcpy(outgoing_packet + sizeof(uint32_t), &net_user_id, user_id_size);
         memcpy(outgoing_packet + sizeof(uint32_t) + user_id_size, node->opus_data, opus_data_size);

         // Broadcast to all *other* active clients
         pthread_mutex_lock(&clients_mutex);
         for (int i = 0; i < MAX_CLIENTS; i++) {
             if (clients[i].active && clients[i].user_id != node->sender_user_id) {
                 ssize_t sent = send_all(clients[i].fd, outgoing_packet, total_packet_size, 0);
                  if (sent < 0) {
                       fprintf(stderr, "Error sending to client %d (fd %d). Code: %zd\n", clients[i].user_id, clients[i].fd, sent);
                        if(sent == -2) {
                             printf("Client %d disconnected (send error).\n", clients[i].user_id);
                             close(clients[i].fd);
                             clients[i].active = false;
                             client_count--;
                        }
                  }
             }
         }
         pthread_mutex_unlock(&clients_mutex);

         free(outgoing_packet);
         free(node->opus_data);
         free(node);
     }

     printf("Broadcaster thread finished.\n");
     return NULL;
 }

 void* handle_client(void* arg) {
     client_info* client = (client_info*)arg;
     int client_fd = client->fd;
     int client_user_id = client->user_id;

     printf("Client %d connected (fd: %d)\n", client_user_id, client_fd);

     unsigned char opus_buffer[MAX_OPUS_PACKET_SIZE];

     while (true) {
         uint32_t opus_data_size_net;
         ssize_t recv_ret = recv_all(client_fd, &opus_data_size_net, sizeof(opus_data_size_net), 0);

         if (recv_ret == 0) {
             printf("Client %d disconnected gracefully (read size).\n", client_user_id);
             break;
         }
          if (recv_ret < 0) {
               if (recv_ret == -2) {
                    printf("Client %d connection closed/reset (read size).\n", client_user_id);
               } else {
                    perror("recv_all size failed");
               }
               break;
          }

         uint32_t opus_data_size = ntohl(opus_data_size_net);

         if (opus_data_size == 0 || opus_data_size > MAX_OPUS_PACKET_SIZE) {
             fprintf(stderr, "Client %d sent invalid Opus data size: %u\n", client_user_id, opus_data_size);
             break;
         }

         recv_ret = recv_all(client_fd, opus_buffer, opus_data_size, 0);

         if (recv_ret == 0) {
             printf("Client %d disconnected gracefully (read data).\n", client_user_id);
             break;
         }
          if (recv_ret < 0) {
                if (recv_ret == -2) {
                     printf("Client %d connection closed/reset (read data).\n", client_user_id);
                } else {
                     perror("recv_all data failed");
                }
               break;
          }
          if ((uint32_t)recv_ret != opus_data_size) {
                fprintf(stderr, "Client %d: Incomplete data received (%zd / %u)\n", client_user_id, recv_ret, opus_data_size);
                break;
          }

         if (!enqueue(&broadcast_queue, opus_buffer, opus_data_size, client_user_id)) {
              fprintf(stderr,"Failed to enqueue packet from user %d\n", client_user_id);
         }
     }

 cleanup:
    pthread_mutex_lock(&clients_mutex);
    close(client_fd);
    client->active = false;
    client_count--;
    pthread_mutex_unlock(&clients_mutex);
    return NULL;
}

 int main(int argc, char** argv) {
     if (argc < 3) {
         fprintf(stderr, "Usage: %s <listen_ip> <port>\n", argv[0]);
         exit(EXIT_FAILURE);
     }

     char* listen_ip = argv[1];
     int port = atoi(argv[2]);

     if (port <= 0 || port > 65535) {
         fprintf(stderr, "Invalid port: %d\n", port);
         exit(EXIT_FAILURE);
     }

     for (int i = 0; i < MAX_CLIENTS; i++) {
         clients[i].fd = -1;
         clients[i].active = false;
         clients[i].user_id = -1;
     }

     queue_init(&broadcast_queue);
     pthread_t broadcaster_tid;
     if (pthread_create(&broadcaster_tid, NULL, broadcaster_thread, &broadcast_queue) != 0) {
         perror("Failed to create broadcaster thread");
         queue_destroy(&broadcast_queue);
         exit(EXIT_FAILURE);
     }

     int server_fd = socket(AF_INET, SOCK_STREAM, 0);
     if (server_fd < 0) {
         perror("socket failed");
         queue_shutdown(&broadcast_queue);
         pthread_join(broadcaster_tid, NULL);
         queue_destroy(&broadcast_queue);
         exit(EXIT_FAILURE);
     }

     int opt = 1;
     if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
         perror("setsockopt(SO_REUSEADDR) failed");
     }

     struct sockaddr_in address;
     memset(&address, 0, sizeof(address));
     address.sin_family = AF_INET;
     address.sin_port = htons(port);
     if (inet_pton(AF_INET, listen_ip, &address.sin_addr) <= 0) {
         fprintf(stderr, "Invalid listen IP address: %s\n", listen_ip);
         close(server_fd);
          queue_shutdown(&broadcast_queue);
          pthread_join(broadcaster_tid, NULL);
          queue_destroy(&broadcast_queue);
         exit(EXIT_FAILURE);
     }


     if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
         perror("bind failed");
         close(server_fd);
         queue_shutdown(&broadcast_queue);
         pthread_join(broadcaster_tid, NULL);
         queue_destroy(&broadcast_queue);
         exit(EXIT_FAILURE);
     }

     if (listen(server_fd, MAX_CLIENTS) < 0) {
         perror("listen failed");
         close(server_fd);
         queue_shutdown(&broadcast_queue);
         pthread_join(broadcaster_tid, NULL);
         queue_destroy(&broadcast_queue);
         exit(EXIT_FAILURE);
     }

     printf("Server listening on %s:%d...\n", listen_ip, port);

     while (1) {
         struct sockaddr_in client_addr;
         socklen_t addrlen = sizeof(client_addr);
         int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addrlen);

         if (client_fd < 0) {
             perror("accept failed");
             if (errno == EINTR) continue;
             break;
         }

         int flag = 1;
         if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, (void*)&flag, sizeof(int)) < 0) {
              perror("setsockopt(TCP_NODELAY) failed for client");
         }

         pthread_mutex_lock(&clients_mutex);

         if (client_count >= MAX_CLIENTS) {
             printf("Max clients reached (%d). Rejecting connection from %s.\n", MAX_CLIENTS, inet_ntoa(client_addr.sin_addr));
             close(client_fd);
             pthread_mutex_unlock(&clients_mutex);
             continue;
         }

         int client_slot = -1;
         for (int i = 0; i < MAX_CLIENTS; i++) {
             if (!clients[i].active) {
                 client_slot = i;
                 break;
             }
         }

         if (client_slot == -1) {
             fprintf(stderr, "Consistency error: No free slots found despite client_count < MAX_CLIENTS.\n");
             close(client_fd);
             pthread_mutex_unlock(&clients_mutex);
             continue;
         }

         int assigned_user_id = client_slot;

         uint32_t net_user_id = htonl((uint32_t)assigned_user_id);
         ssize_t sent = send(client_fd, &net_user_id, sizeof(net_user_id), 0);
         if (sent != sizeof(net_user_id)) {
              fprintf(stderr, "Failed to send User ID (%d) to new client. Closing connection.\n", assigned_user_id);
              perror("send user id");
              close(client_fd);
              pthread_mutex_unlock(&clients_mutex);
              continue;
         }

         clients[client_slot].fd = client_fd;
         clients[client_slot].addr = client_addr;
         clients[client_slot].active = true;
         clients[client_slot].user_id = assigned_user_id;
         client_count++;
         printf("Client %d accepted from %s. Total clients: %d\n",
                assigned_user_id, inet_ntoa(client_addr.sin_addr), client_count);

         if (pthread_create(&clients[client_slot].thread_id, NULL, handle_client, &clients[client_slot]) != 0) {
             perror("pthread_create for client failed");
             clients[client_slot].active = false;
             clients[client_slot].user_id = -1;
             client_count--;
             close(client_fd);
         } else {
             pthread_detach(clients[client_slot].thread_id);
         }

         pthread_mutex_unlock(&clients_mutex);
     }

     printf("Server shutting down...\n");
     close(server_fd);

     queue_shutdown(&broadcast_queue);
     pthread_join(broadcaster_tid, NULL);
     queue_destroy(&broadcast_queue);
     printf("Broadcaster shut down.\n");

     pthread_mutex_lock(&clients_mutex);
     for(int i=0; i<MAX_CLIENTS; ++i) {
         if(clients[i].active) {
              close(clients[i].fd); // Force close
              clients[i].active = false;
         }
     }
     pthread_mutex_unlock(&clients_mutex);

     printf("Server cleanup complete.\n");
     return 0;
 }
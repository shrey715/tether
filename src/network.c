#include "network.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

int create_socket(void) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        return -1;
    }
    return sockfd;
}

int bind_socket(int sockfd, int port) {
    struct sockaddr_in addr;
    setup_address(&addr, NULL, port);
    
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        return -1;
    }
    return 0;
}

int send_packet(int sockfd, struct sockaddr_in *dest, struct sham_packet *packet, double loss_rate) {
    // Simulate packet loss before sending
    if (simulate_packet_loss(loss_rate)) {
        if (is_logging_enabled() && packet->data_len > 0) {
            log_event(LOG_DROP_DATA, packet->header.seq_num, 0, packet->data_len, 0);
        }
        return sizeof(struct sham_header) + packet->data_len; // Pretend it was sent
    }
    
    uint8_t buffer[MAX_PACKET_SIZE];
    size_t packet_size;
    
    // Convert packet to network byte order
    uint32_t net_seq = htonl(packet->header.seq_num);
    uint32_t net_ack = htonl(packet->header.ack_num);
    uint16_t net_flags = htons(packet->header.flags);
    uint16_t net_window = htons(packet->header.window_size);
    
    // Pack header into buffer
    memcpy(buffer, &net_seq, sizeof(net_seq));
    memcpy(buffer + 4, &net_ack, sizeof(net_ack));
    memcpy(buffer + 8, &net_flags, sizeof(net_flags));
    memcpy(buffer + 10, &net_window, sizeof(net_window));
    
    // Add data payload
    memcpy(buffer + sizeof(struct sham_header), packet->data, packet->data_len);
    packet_size = sizeof(struct sham_header) + packet->data_len;
    
    // Send the packet
    ssize_t bytes_sent = sendto(sockfd, buffer, packet_size, 0, 
                               (struct sockaddr*)dest, sizeof(*dest));
    
    if (bytes_sent < 0) {
        perror("sendto failed");
        return -1;
    }
    
    return (int)bytes_sent;
}

int receive_packet(int sockfd, struct sockaddr_in *src, struct sham_packet *packet) {
    uint8_t buffer[MAX_PACKET_SIZE];
    socklen_t src_len = sizeof(*src);
    
    ssize_t bytes_received = recvfrom(sockfd, buffer, sizeof(buffer), 0,
                                     (struct sockaddr*)src, &src_len);
    
    if (bytes_received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; // Timeout occurred
        }
        perror("recvfrom failed");
        return -1;
    }
    
    // Validate minimum packet size
    if (bytes_received < (ssize_t)sizeof(struct sham_header)) {
        fprintf(stderr, "Received packet too small: %zd bytes\n", bytes_received);
        return -1;
    }
    
    // Extract header fields and convert from network byte order
    uint32_t net_seq, net_ack;
    uint16_t net_flags, net_window;
    
    memcpy(&net_seq, buffer, sizeof(net_seq));
    memcpy(&net_ack, buffer + 4, sizeof(net_ack));
    memcpy(&net_flags, buffer + 8, sizeof(net_flags));
    memcpy(&net_window, buffer + 10, sizeof(net_window));
    
    packet->header.seq_num = ntohl(net_seq);
    packet->header.ack_num = ntohl(net_ack);
    packet->header.flags = ntohs(net_flags);
    packet->header.window_size = ntohs(net_window);
    
    // Extract data payload
    packet->data_len = bytes_received - sizeof(struct sham_header);
    if (packet->data_len > MAX_DATA_SIZE) {
        fprintf(stderr, "Received data too large: %zu bytes\n", packet->data_len);
        return -1;
    }
    
    memcpy(packet->data, buffer + sizeof(struct sham_header), packet->data_len);
    
    return (int)bytes_received;
}

int simulate_packet_loss(double loss_rate) {
    if (loss_rate <= 0.0) {
        return 0; // No loss
    }
    
    // Initialize random seed if first call
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = 1;
    }
    
    // Generate random number between 0.0 and 1.0
    double random_val = (double)rand() / RAND_MAX;
    
    // Drop packet if random value is less than loss rate
    return (random_val < loss_rate) ? 1 : 0;
}

void set_socket_timeout(int sockfd, int timeout_ms) {
    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt SO_RCVTIMEO failed");
    }
}

void setup_address(struct sockaddr_in *addr, const char *ip, int port) {
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    
    if (ip == NULL) {
        // Server binding - accept from any IP
        addr->sin_addr.s_addr = INADDR_ANY;
    } else {
        // Client connecting - convert IP string to binary
        if (inet_pton(AF_INET, ip, &addr->sin_addr) <= 0) {
            fprintf(stderr, "Invalid IP address: %s\n", ip);
            // Set to localhost as fallback
            inet_pton(AF_INET, "127.0.0.1", &addr->sin_addr);
        }
    }
}

char* addr_to_string(struct sockaddr_in *addr) {
    static char buffer[32];
    char ip_str[INET_ADDRSTRLEN];
    
    if (inet_ntop(AF_INET, &addr->sin_addr, ip_str, INET_ADDRSTRLEN) == NULL) {
        snprintf(buffer, sizeof(buffer), "INVALID:PORT");
        return buffer;
    }
    
    snprintf(buffer, sizeof(buffer), "%s:%d", ip_str, ntohs(addr->sin_port));
    return buffer;
}

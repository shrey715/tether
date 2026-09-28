#ifndef CONNECTION_H
#define CONNECTION_H

#include "sham.h"
#include <sys/time.h>

// Timer structure for retransmission
struct timer {
    struct timeval start_time;
    int timeout_ms;
    int active;
};

// Sliding window entry
struct window_entry {
    struct sham_packet packet;
    struct timer timer;
    int acked;
    uint32_t seq_num;
    int retry_count;
};

// Receive buffer entry for out-of-order packets
struct recv_buffer_entry {
    uint32_t seq_num;
    uint8_t data[MAX_DATA_SIZE];
    size_t data_len;
    int valid;  // 1 if slot is occupied, 0 if free
};

// Connection context
struct connection {
    int sockfd;
    struct sockaddr_in peer_addr;
    connection_state_t state;
    double loss_rate;
    
    // Sequence numbers
    uint32_t send_seq;
    uint32_t recv_seq;
    uint32_t send_base;
    
    // Sliding window
    struct window_entry window[SLIDING_WINDOW_SIZE];
    int window_start;
    int window_count;
    
    // Flow control
    uint16_t recv_window;
    uint16_t send_window;
    
    // Receive buffer for out-of-order packets
    struct recv_buffer_entry recv_buffer[SLIDING_WINDOW_SIZE];
    uint32_t expected_seq;
};

// Connection management functions
int connection_init(struct connection *conn, int sockfd, double loss_rate);
int perform_handshake_client(struct connection *conn, const char *server_ip, int port);
int perform_handshake_server(struct connection *conn);
int connection_send_data(struct connection *conn, const uint8_t *data, size_t len);
int connection_receive_data(struct connection *conn, uint8_t *data, size_t *len);
int connection_close(struct connection *conn);

// Window management
int add_to_window(struct connection *conn, struct sham_packet *packet);
int process_ack(struct connection *conn, uint32_t ack_num);
int retransmit_expired(struct connection *conn);

// Receive buffer management
int buffer_out_of_order_packet(struct connection *conn, struct sham_packet *packet);
int deliver_buffered_packets(struct connection *conn, uint8_t *data, size_t *len);

#endif // CONNECTION_H

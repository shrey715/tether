#ifndef SHAM_H
#define SHAM_H

#include <stdint.h>
#include <sys/time.h>
#include <netinet/in.h>

// S.H.A.M. Packet Structure as per assignment requirements
struct sham_header {
    uint32_t seq_num;      // Sequence Number
    uint32_t ack_num;      // Acknowledgment Number
    uint16_t flags;        // Control flags (SYN, ACK, FIN)
    uint16_t window_size;  // Flow control window size
};

// Flag definitions
#define SHAM_SYN 0x1    // Synchronise flag
#define SHAM_ACK 0x2    // Acknowledge flag
#define SHAM_FIN 0x4    // Finish flag

// Protocol constants
#define MAX_DATA_SIZE 1024          // Maximum data payload per packet
#define MAX_PACKET_SIZE (sizeof(struct sham_header) + MAX_DATA_SIZE)
#define SLIDING_WINDOW_SIZE 10      // Fixed sliding window size
#define RTO_TIMEOUT_MS 500          // Retransmission timeout in milliseconds
#define MAX_RETRIES 5               // Maximum retransmission attempts

// Packet structure for transmission
struct sham_packet {
    struct sham_header header;
    uint8_t data[MAX_DATA_SIZE];
    size_t data_len;
};

// Connection state
typedef enum {
    CONN_CLOSED,
    CONN_SYN_SENT,
    CONN_SYN_RECEIVED,
    CONN_ESTABLISHED,
    CONN_FIN_WAIT_1,
    CONN_FIN_WAIT_2,
    CONN_CLOSE_WAIT,
    CONN_LAST_ACK
} connection_state_t;

// Function prototypes
uint32_t generate_initial_sequence(void);
int is_valid_packet(struct sham_packet *packet);

#endif // SHAM_H

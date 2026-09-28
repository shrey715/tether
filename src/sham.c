#include "sham.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <time.h>

uint32_t generate_initial_sequence(void) {
    // Generate cryptographically random initial sequence number
    // Use current time and random number for better entropy
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = 1;
    }
    
    // Generate a random 32-bit number
    uint32_t seq = ((uint32_t)rand() << 16) | ((uint32_t)rand() & 0xFFFF);
    
    // Ensure it's not zero
    if (seq == 0) {
        seq = 1000;
    }
    
    return seq;
}

int is_valid_packet(struct sham_packet *packet) {
    // Validate S.H.A.M. packet structure and contents
    
    // Check data length doesn't exceed maximum
    if (packet->data_len > MAX_DATA_SIZE) {
        return 0;
    }
    
    // Check flag combinations are valid
    uint16_t flags = packet->header.flags;
    
    // SYN and FIN cannot be set together
    if ((flags & SHAM_SYN) && (flags & SHAM_FIN)) {
        return 0;
    }
    
    // If only SYN is set (connection establishment)
    if ((flags & SHAM_SYN) && !(flags & SHAM_ACK) && !(flags & SHAM_FIN)) {
        return 1;
    }
    
    // If SYN+ACK are set (connection establishment response)
    if ((flags & SHAM_SYN) && (flags & SHAM_ACK) && !(flags & SHAM_FIN)) {
        return 1;
    }
    
    // If only ACK is set (acknowledgment/data packet)
    if (!(flags & SHAM_SYN) && (flags & SHAM_ACK) && !(flags & SHAM_FIN)) {
        return 1;
    }
    
    // If FIN is set (connection termination)
    if ((flags & SHAM_FIN)) {
        return 1;
    }
    
    return 0;
}

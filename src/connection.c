#include "connection.h"
#include "network.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

// Timer utility functions
static int is_timer_expired(struct timer *timer) {
    if (!timer->active) return 0;
    
    struct timeval now;
    gettimeofday(&now, NULL);
    
    long elapsed_ms = (now.tv_sec - timer->start_time.tv_sec) * 1000 +
                     (now.tv_usec - timer->start_time.tv_usec) / 1000;
    
    return elapsed_ms >= timer->timeout_ms;
}

static void start_timer(struct timer *timer, int timeout_ms) {
    gettimeofday(&timer->start_time, NULL);
    timer->timeout_ms = timeout_ms;
    timer->active = 1;
}

static void stop_timer(struct timer *timer) {
    timer->active = 0;
}

int connection_init(struct connection *conn, int sockfd, double loss_rate) {
    memset(conn, 0, sizeof(*conn));
    conn->sockfd = sockfd;
    conn->state = CONN_CLOSED;
    conn->loss_rate = loss_rate;
    
    // Initialize sequence numbers
    conn->send_seq = 0;
    conn->recv_seq = 0;
    conn->send_base = 0;
    conn->expected_seq = 0;
    
    // Set initial window sizes
    conn->recv_window = MAX_DATA_SIZE * SLIDING_WINDOW_SIZE;
    conn->send_window = MAX_DATA_SIZE * SLIDING_WINDOW_SIZE;
    
    // Initialize sliding window - all slots start as available
    conn->window_count = 0;
    conn->window_start = 0;
    
    for (int i = 0; i < SLIDING_WINDOW_SIZE; i++) {
        conn->window[i].acked = 1;  // Available slot
        conn->window[i].timer.active = 0;
        conn->window[i].retry_count = 0;
        conn->window[i].seq_num = 0;
    }
    
    // Initialize receive buffer
    for (int i = 0; i < SLIDING_WINDOW_SIZE; i++) {
        conn->recv_buffer[i].valid = 0;
        conn->recv_buffer[i].seq_num = 0;
        conn->recv_buffer[i].data_len = 0;
        memset(conn->recv_buffer[i].data, 0, MAX_DATA_SIZE);
    }
    
    return 0;
}

int perform_handshake_client(struct connection *conn, const char *server_ip, int port) {
    struct sockaddr_in server_addr;
    setup_address(&server_addr, server_ip, port);
    
    uint32_t initial_seq = generate_initial_sequence();
    conn->send_seq = initial_seq;
    
    // Create SYN packet
    struct sham_packet syn_packet;
    memset(&syn_packet, 0, sizeof(syn_packet));
    syn_packet.header.seq_num = initial_seq;
    syn_packet.header.ack_num = 0;
    syn_packet.header.flags = SHAM_SYN;
    syn_packet.header.window_size = conn->recv_window;
    syn_packet.data_len = 0;
    
    set_socket_timeout(conn->sockfd, RTO_TIMEOUT_MS);
    
    int retries = 0;
    while (retries < MAX_RETRIES) {
        printf("Client: Sending SYN with seq %u (attempt %d)\n", 
               syn_packet.header.seq_num, retries + 1);
        
        if (is_logging_enabled()) {
            log_event(LOG_SND_SYN, syn_packet.header.seq_num, 0, 0, 0);
        }
        
        if (send_packet(conn->sockfd, &server_addr, &syn_packet, conn->loss_rate) < 0) {
            return -1;
        }
        
        conn->state = CONN_SYN_SENT;
        
        // Wait for SYN-ACK response
        struct sham_packet recv_packet;
        struct sockaddr_in from_addr;
        
        int ret = receive_packet(conn->sockfd, &from_addr, &recv_packet);
        if (ret < 0) {
            printf("Client: Error receiving packet\n");
            return -1;
        } else if (ret == 0) {
            printf("Client: Timeout waiting for SYN-ACK, retransmitting...\n");
            retries++;
            continue;
        }
        
        // Validate SYN-ACK packet
        if ((recv_packet.header.flags & (SHAM_SYN | SHAM_ACK)) == (SHAM_SYN | SHAM_ACK) &&
            recv_packet.header.ack_num == initial_seq + 1) {
            
            printf("Client: Received SYN-ACK with seq %u, ack %u\n", 
                   recv_packet.header.seq_num, recv_packet.header.ack_num);
            
            if (is_logging_enabled()) {
                log_event(LOG_RCV_SYN, recv_packet.header.seq_num, recv_packet.header.ack_num, 0, 0);
            }
            
            // Send final ACK
            struct sham_packet ack_packet;
            memset(&ack_packet, 0, sizeof(ack_packet));
            ack_packet.header.seq_num = initial_seq + 1;
            ack_packet.header.ack_num = recv_packet.header.seq_num + 1;
            ack_packet.header.flags = SHAM_ACK;
            ack_packet.header.window_size = conn->recv_window;
            ack_packet.data_len = 0;
            
            printf("Client: Sending final ACK with seq %u, ack %u\n",
                   ack_packet.header.seq_num, ack_packet.header.ack_num);
            
            if (send_packet(conn->sockfd, &server_addr, &ack_packet, conn->loss_rate) < 0) {
                return -1;
            }
            
            if (is_logging_enabled()) {
                log_event(LOG_RCV_ACK_FOR_SYN, 0, ack_packet.header.ack_num, 0, 0);
            }
            
            // Update connection state
            conn->state = CONN_ESTABLISHED;
            conn->send_seq = ack_packet.header.seq_num;
            conn->recv_seq = ack_packet.header.ack_num;
            conn->expected_seq = ack_packet.header.ack_num;
            conn->send_base = ack_packet.header.seq_num;
            conn->peer_addr = server_addr;
            
            printf("Client: Connection established!\n");
            return 0;
        } else {
            printf("Client: Received unexpected packet during handshake\n");
            retries++;
        }
    }
    
    printf("Client: Handshake failed after %d retries\n", MAX_RETRIES);
    conn->state = CONN_CLOSED;
    return -1;
}

int perform_handshake_server(struct connection *conn) {
    set_socket_timeout(conn->sockfd, 0); // Blocking receive for server
    
    printf("Server: Waiting for client connection...\n");
    
    // Wait for SYN packet
    struct sham_packet syn_packet;
    struct sockaddr_in client_addr;
    
    while (1) {
        int ret = receive_packet(conn->sockfd, &client_addr, &syn_packet);
        if (ret <= 0) {
            continue;
        }
        
        if ((syn_packet.header.flags & SHAM_SYN) && !(syn_packet.header.flags & SHAM_ACK)) {
            printf("Server: Received SYN with seq %u\n", syn_packet.header.seq_num);
            
            if (is_logging_enabled()) {
                log_event(LOG_RCV_SYN, syn_packet.header.seq_num, 0, 0, 0);
            }
            
            break;
        }
    }
    
    uint32_t server_seq = generate_initial_sequence();
    
    // Send SYN-ACK response
    struct sham_packet syn_ack_packet;
    memset(&syn_ack_packet, 0, sizeof(syn_ack_packet));
    syn_ack_packet.header.seq_num = server_seq;
    syn_ack_packet.header.ack_num = syn_packet.header.seq_num + 1;
    syn_ack_packet.header.flags = SHAM_SYN | SHAM_ACK;
    syn_ack_packet.header.window_size = conn->recv_window;
    syn_ack_packet.data_len = 0;
    
    printf("Server: Sending SYN-ACK with seq %u, ack %u\n",
           syn_ack_packet.header.seq_num, syn_ack_packet.header.ack_num);
    
    if (send_packet(conn->sockfd, &client_addr, &syn_ack_packet, conn->loss_rate) < 0) {
        return -1;
    }
    
    if (is_logging_enabled()) {
        log_event(LOG_SND_SYN_ACK, syn_ack_packet.header.seq_num, 
                  syn_ack_packet.header.ack_num, 0, 0);
    }
    
    conn->state = CONN_SYN_RECEIVED;
    
    // Wait for final ACK
    set_socket_timeout(conn->sockfd, RTO_TIMEOUT_MS);
    int retries = 0;
    
    while (retries < MAX_RETRIES) {
        struct sham_packet ack_packet;
        struct sockaddr_in from_addr;
        
        int ret = receive_packet(conn->sockfd, &from_addr, &ack_packet);
        if (ret < 0) {
            return -1;
        } else if (ret == 0) {
            printf("Server: Timeout waiting for ACK, retransmitting SYN-ACK...\n");
            send_packet(conn->sockfd, &client_addr, &syn_ack_packet, conn->loss_rate);
            retries++;
            continue;
        }
        
        if ((ack_packet.header.flags & SHAM_ACK) && 
            ack_packet.header.ack_num == server_seq + 1) {
            
            printf("Server: Received final ACK with seq %u, ack %u\n",
                   ack_packet.header.seq_num, ack_packet.header.ack_num);
            
            if (is_logging_enabled()) {
                log_event(LOG_RCV_ACK_FOR_SYN, 0, ack_packet.header.ack_num, 0, 0);
            }
            
            // Update connection state
            conn->state = CONN_ESTABLISHED;
            conn->send_seq = server_seq + 1;
            conn->recv_seq = ack_packet.header.seq_num;
            conn->expected_seq = ack_packet.header.seq_num;
            conn->send_base = server_seq + 1;
            conn->peer_addr = client_addr;
            
            printf("Server: Connection established!\n");
            return 0;
        } else {
            printf("Server: Received unexpected packet during handshake\n");
            retries++;
        }
    }
    
    printf("Server: Handshake failed after %d retries\n", MAX_RETRIES);
    conn->state = CONN_CLOSED;
    return -1;
}

// Enhanced send buffer management
int add_to_window(struct connection *conn, struct sham_packet *packet) {
    if (conn->window_count >= SLIDING_WINDOW_SIZE) {
        return -1; // Window full
    }
    
    int index = (conn->window_start + conn->window_count) % SLIDING_WINDOW_SIZE;
    
    // Copy packet to window
    memcpy(&conn->window[index].packet, packet, sizeof(*packet));
    conn->window[index].seq_num = packet->header.seq_num;
    conn->window[index].acked = 0;  // Mark as unacknowledged
    conn->window[index].retry_count = 0;
    
    // Start retransmission timer
    start_timer(&conn->window[index].timer, RTO_TIMEOUT_MS);
    
    conn->window_count++;
    
    printf("Added packet seq %u to window (slot %d, count=%d)\n", 
           packet->header.seq_num, index, conn->window_count);
    
    return 0;
}

int process_ack(struct connection *conn, uint32_t ack_num) {
    if (is_logging_enabled()) {
        log_event(LOG_RCV_ACK, 0, ack_num, 0, 0);
    }
    
    int acked_packets = 0;
    
    // Process cumulative ACK - slide window forward
    while (conn->window_count > 0) {
        int index = conn->window_start;
        uint32_t packet_end = conn->window[index].seq_num + conn->window[index].packet.data_len;
        
        if (packet_end <= ack_num) {
            // This packet is acknowledged
            printf("ACK %u acknowledges packet seq %u\n", ack_num, conn->window[index].seq_num);
            
            stop_timer(&conn->window[index].timer);
            conn->window[index].acked = 1;
            
            // Advance send_base
            conn->send_base = packet_end;
            
            // Move window forward
            conn->window_start = (conn->window_start + 1) % SLIDING_WINDOW_SIZE;
            conn->window_count--;
            acked_packets++;
        } else {
            break; // No more packets to acknowledge
        }
    }
    
    printf("Processed ACK %u, acknowledged %d packets, window_count=%d\n", 
           ack_num, acked_packets, conn->window_count);
    
    return acked_packets;
}

int retransmit_expired(struct connection *conn) {
    int retransmitted = 0;
    
    // Check all active window slots for timeouts
    for (int i = 0; i < conn->window_count && i < SLIDING_WINDOW_SIZE; i++) {
        int slot = (conn->window_start + i) % SLIDING_WINDOW_SIZE;
        
        if (conn->window[slot].acked) continue;  // Already acknowledged
        if (!is_timer_expired(&conn->window[slot].timer)) continue;  // Not timed out
        
        // Timeout occurred
        printf("Timeout detected for packet seq %u (retry %d)\n", 
               conn->window[slot].seq_num, conn->window[slot].retry_count);
        
        if (is_logging_enabled()) {
            log_event(LOG_TIMEOUT, conn->window[slot].seq_num, 0, 0, 0);
        }
        
        conn->window[slot].retry_count++;
        
        if (conn->window[slot].retry_count >= MAX_RETRIES) {
            printf("Max retries exceeded for seq %u, giving up\n", conn->window[slot].seq_num);
            // Mark as acknowledged to remove from window
            conn->window[slot].acked = 1;
            continue;
        }
        
        // Retransmit packet
        if (send_packet(conn->sockfd, &conn->peer_addr, 
                       &conn->window[slot].packet, conn->loss_rate) >= 0) {
            
            if (is_logging_enabled()) {
                log_event(LOG_RETX_DATA, conn->window[slot].seq_num, 0, 
                         conn->window[slot].packet.data_len, 0);
            }
            
            // Restart timer with exponential backoff
            int new_timeout = RTO_TIMEOUT_MS * (1 << conn->window[slot].retry_count);
            if (new_timeout > 5000) new_timeout = 5000;  // Cap at 5 seconds
            start_timer(&conn->window[slot].timer, new_timeout);
            
            retransmitted++;
            printf("Retransmitted packet seq %u (attempt %d, timeout %dms)\n",
                   conn->window[slot].seq_num, conn->window[slot].retry_count + 1, new_timeout);
        }
    }
    
    return retransmitted;
}

// Enhanced bidirectional packet processing
int process_incoming_packet(struct connection *conn, struct sham_packet *packet) {
    // Process ACK if present
    if (packet->header.flags & SHAM_ACK) {
        process_ack(conn, packet->header.ack_num);
    }
    
    // Handle FIN packets
    if (packet->header.flags & SHAM_FIN) {
        if (is_logging_enabled()) {
            log_event(LOG_RCV_FIN, packet->header.seq_num, packet->header.ack_num, 0, 0);
        }
        return -2; // Signal FIN received
    }
    
    // Handle data packets
    if (packet->data_len > 0) {
        if (is_logging_enabled()) {
            log_event(LOG_RCV_DATA, packet->header.seq_num, 
                      packet->header.ack_num, packet->data_len, 0);
        }
        return 1; // Signal data received
    }
    
    return 0; // ACK only packet
}

int connection_send_data(struct connection *conn, const uint8_t *data, size_t len) {
    if (conn->state != CONN_ESTABLISHED) {
        printf("Error: Connection not established\n");
        return (conn->state == CONN_CLOSED) ? -2 : -1;
    }
    
    size_t bytes_sent = 0;
    
    while (bytes_sent < len) {
        // Process any incoming packets (ACKs, etc.) while sending
        retransmit_expired(conn);
        
        struct sham_packet recv_packet;
        struct sockaddr_in from_addr;
        set_socket_timeout(conn->sockfd, 10); // Very short timeout
        
        int recv_ret = receive_packet(conn->sockfd, &from_addr, &recv_packet);
        if (recv_ret > 0) {
            int process_ret = process_incoming_packet(conn, &recv_packet);
            if (process_ret == -2) {
                return -2; // FIN received
            }
        }
        
        // Check if sliding window has space
        if (conn->window_count >= SLIDING_WINDOW_SIZE) {
            printf("Sliding window full (%d/%d), waiting...\n", 
                   conn->window_count, SLIDING_WINDOW_SIZE);
            sleep(10000); // 10ms delay
            continue;
        }
        
        size_t chunk_size = len - bytes_sent;
        if (chunk_size > MAX_DATA_SIZE) {
            chunk_size = MAX_DATA_SIZE;
        }
        
        // Create data packet
        struct sham_packet data_packet;
        memset(&data_packet, 0, sizeof(data_packet));
        data_packet.header.seq_num = conn->send_seq;
        data_packet.header.ack_num = conn->expected_seq;
        data_packet.header.flags = SHAM_ACK;
        data_packet.header.window_size = conn->recv_window;
        data_packet.data_len = chunk_size;
        memcpy(data_packet.data, data + bytes_sent, chunk_size);
        
        // Add to sliding window
        if (add_to_window(conn, &data_packet) < 0) {
            printf("Failed to add packet to window\n");
            break;
        }
        
        // Send packet
        if (send_packet(conn->sockfd, &conn->peer_addr, &data_packet, conn->loss_rate) < 0) {
            return -1;
        }
        
        if (is_logging_enabled()) {
            log_event(LOG_SND_DATA, data_packet.header.seq_num, 
                      data_packet.header.ack_num, chunk_size, conn->recv_window);
        }
        
        conn->send_seq += chunk_size;
        bytes_sent += chunk_size;
        
        printf("Sent %zu bytes (seq %u, total sent %zu/%zu)\n", 
               chunk_size, data_packet.header.seq_num, bytes_sent, len);
    }
    
    // Wait for all packets to be acknowledged
    printf("Waiting for all packets to be acknowledged...\n");
    int wait_cycles = 0;
    const int MAX_WAIT_CYCLES = 100;
    
    while (conn->window_count > 0 && wait_cycles < MAX_WAIT_CYCLES) {
        retransmit_expired(conn);
        
        struct sham_packet recv_packet;
        struct sockaddr_in from_addr;
        set_socket_timeout(conn->sockfd, 100);
        
        int recv_ret = receive_packet(conn->sockfd, &from_addr, &recv_packet);
        if (recv_ret > 0) {
            process_incoming_packet(conn, &recv_packet);
        }
        
        wait_cycles++;
        
        if (wait_cycles % 10 == 0) {
            printf("Still waiting for %d packets to be acknowledged...\n", conn->window_count);
        }
    }
    
    return bytes_sent;
}

// Fixed receive buffer management
int buffer_out_of_order_packet(struct connection *conn, struct sham_packet *packet) {
    // Check if packet is already buffered (avoid duplicates)
    for (int i = 0; i < SLIDING_WINDOW_SIZE; i++) {
        if (conn->recv_buffer[i].valid && 
            conn->recv_buffer[i].seq_num == packet->header.seq_num) {
            printf("Packet seq %u already buffered, ignoring duplicate\n", packet->header.seq_num);
            return 0;
        }
    }
    
    // Find empty slot in buffer
    for (int i = 0; i < SLIDING_WINDOW_SIZE; i++) {
        if (!conn->recv_buffer[i].valid) {
            conn->recv_buffer[i].valid = 1;
            conn->recv_buffer[i].seq_num = packet->header.seq_num;
            conn->recv_buffer[i].data_len = packet->data_len;
            memcpy(conn->recv_buffer[i].data, packet->data, packet->data_len);
            
            printf("Buffered out-of-order packet seq %u (slot %d)\n", packet->header.seq_num, i);
            return 1;
        }
    }
    
    printf("Receive buffer full, dropping packet seq %u\n", packet->header.seq_num);
    return -1;
}

int deliver_buffered_packets(struct connection *conn, uint8_t *data, size_t *total_len) {
    size_t delivered = 0;
    size_t max_space = *total_len;
    int progress_made = 1;
    
    while (progress_made && delivered < max_space) {
        progress_made = 0;
        
        // Look for packet with expected sequence number
        for (int i = 0; i < SLIDING_WINDOW_SIZE; i++) {
            if (conn->recv_buffer[i].valid && 
                conn->recv_buffer[i].seq_num == conn->expected_seq) {
                
                // Check if we have space
                if (delivered + conn->recv_buffer[i].data_len <= max_space) {
                    // Deliver this buffered packet
                    memcpy(data + delivered, 
                           conn->recv_buffer[i].data, 
                           conn->recv_buffer[i].data_len);
                    
                    delivered += conn->recv_buffer[i].data_len;
                    conn->expected_seq += conn->recv_buffer[i].data_len;
                    
                    printf("Delivered buffered packet seq %u (%zu bytes)\n", 
                           conn->recv_buffer[i].seq_num, conn->recv_buffer[i].data_len);
                    
                    // Mark slot as free
                    conn->recv_buffer[i].valid = 0;
                    progress_made = 1;
                    break;
                } else {
                    printf("Output buffer full, cannot deliver more buffered packets\n");
                    break;
                }
            }
        }
    }
    
    *total_len = delivered;
    return delivered;
}

int connection_receive_data(struct connection *conn, uint8_t *data, size_t *len) {
    if (conn->state != CONN_ESTABLISHED && conn->state != CONN_CLOSE_WAIT) {
        *len = 0;
        return (conn->state == CONN_CLOSED) ? -2 : -1;
    }
    
    // Process retransmissions first
    retransmit_expired(conn);
    
    struct sham_packet recv_packet;
    struct sockaddr_in from_addr;
    
    int ret = receive_packet(conn->sockfd, &from_addr, &recv_packet);
    if (ret <= 0) {
        *len = 0;
        return ret;
    }
    
    // Handle FIN packets for connection termination
    if (recv_packet.header.flags & SHAM_FIN) {
        if (is_logging_enabled()) {
            log_event(LOG_RCV_FIN, recv_packet.header.seq_num, recv_packet.header.ack_num, 0, 0);
        }
        
        // Send ACK for received FIN
        struct sham_packet ack_packet;
        memset(&ack_packet, 0, sizeof(ack_packet));
        ack_packet.header.seq_num = conn->send_seq;
        ack_packet.header.ack_num = recv_packet.header.seq_num + 1;
        ack_packet.header.flags = SHAM_ACK;
        ack_packet.header.window_size = conn->recv_window;
        ack_packet.data_len = 0;
        
        send_packet(conn->sockfd, &conn->peer_addr, &ack_packet, conn->loss_rate);
        
        if (is_logging_enabled()) {
            log_event(LOG_SND_ACK, 0, ack_packet.header.ack_num, 0, conn->recv_window);
        }
        
        conn->state = CONN_CLOSE_WAIT;
        
        // Send our own FIN
        struct sham_packet our_fin;
        memset(&our_fin, 0, sizeof(our_fin));
        our_fin.header.seq_num = conn->send_seq;
        our_fin.header.ack_num = recv_packet.header.seq_num + 1;
        our_fin.header.flags = SHAM_FIN | SHAM_ACK;
        our_fin.header.window_size = conn->recv_window;
        our_fin.data_len = 0;
        
        send_packet(conn->sockfd, &conn->peer_addr, &our_fin, conn->loss_rate);
        
        if (is_logging_enabled()) {
            log_event(LOG_SND_FIN, our_fin.header.seq_num, our_fin.header.ack_num, 0, 0);
        }
        
        conn->state = CONN_LAST_ACK;
        *len = 0;
        return -2; // Signal connection closing
    }
    
    // Process ACK if present
    if (recv_packet.header.flags & SHAM_ACK) {
        process_ack(conn, recv_packet.header.ack_num);
        
        // Handle ACK for our FIN in LAST_ACK state
        if (conn->state == CONN_LAST_ACK) {
            conn->state = CONN_CLOSED;
            if (is_logging_enabled()) {
                log_event(LOG_RCV_ACK, 0, recv_packet.header.ack_num, 0, 0);
            }
            *len = 0;
            return -2; // Signal connection closed
        }
    }
    
    // Handle data packets
    if (recv_packet.data_len > 0) {
        if (is_logging_enabled()) {
            log_event(LOG_RCV_DATA, recv_packet.header.seq_num, 
                      recv_packet.header.ack_num, recv_packet.data_len, 0);
        }
        
        printf("Received data packet seq %u, expected %u\n", 
               recv_packet.header.seq_num, conn->expected_seq);
        
        // Check if this is the expected in-order packet
        if (recv_packet.header.seq_num == conn->expected_seq) {
            // In-order packet - deliver immediately
            if (recv_packet.data_len <= *len) {
                size_t total_delivered = 0;
                
                // Copy this packet's data
                memcpy(data, recv_packet.data, recv_packet.data_len);
                total_delivered = recv_packet.data_len;
                conn->expected_seq += recv_packet.data_len;
                
                printf("Delivered in-order packet seq %u (%zu bytes)\n", 
                       recv_packet.header.seq_num, recv_packet.data_len);
                
                // Try to deliver any buffered packets that are now in order
                size_t remaining_space = *len - total_delivered;
                if (remaining_space > 0) {
                    size_t buffered_delivered = remaining_space;
                    deliver_buffered_packets(conn, data + total_delivered, &buffered_delivered);
                    total_delivered += buffered_delivered;
                }
                
                // Send cumulative ACK
                struct sham_packet ack_packet;
                memset(&ack_packet, 0, sizeof(ack_packet));
                ack_packet.header.seq_num = conn->send_seq;
                ack_packet.header.ack_num = conn->expected_seq;
                ack_packet.header.flags = SHAM_ACK;
                ack_packet.header.window_size = conn->recv_window;
                ack_packet.data_len = 0;
                
                send_packet(conn->sockfd, &conn->peer_addr, &ack_packet, conn->loss_rate);
                
                if (is_logging_enabled()) {
                    log_event(LOG_SND_ACK, 0, ack_packet.header.ack_num, 0, conn->recv_window);
                }
                
                printf("Sent cumulative ACK %u\n", conn->expected_seq);
                
                *len = total_delivered;
                return total_delivered;
            } else {
                printf("Error: Received data larger than buffer\n");
                return -1;
            }
        } else if (recv_packet.header.seq_num > conn->expected_seq) {
            // Out-of-order packet - buffer it
            printf("Buffering out-of-order packet (seq %u, expected %u)\n",
                   recv_packet.header.seq_num, conn->expected_seq);
            
            buffer_out_of_order_packet(conn, &recv_packet);
            
            // Send duplicate ACK for expected sequence
            struct sham_packet ack_packet;
            memset(&ack_packet, 0, sizeof(ack_packet));
            ack_packet.header.seq_num = conn->send_seq;
            ack_packet.header.ack_num = conn->expected_seq;
            ack_packet.header.flags = SHAM_ACK;
            ack_packet.header.window_size = conn->recv_window;
            ack_packet.data_len = 0;
            
            send_packet(conn->sockfd, &conn->peer_addr, &ack_packet, conn->loss_rate);
            
            if (is_logging_enabled()) {
                log_event(LOG_SND_ACK, 0, ack_packet.header.ack_num, 0, conn->recv_window);
            }
            
            printf("Sent duplicate ACK %u for out-of-order packet\n", conn->expected_seq);
            
            *len = 0;
            return 0;
        } else {
            // Duplicate packet (seq_num < expected_seq) - ignore but send ACK
            printf("Received duplicate packet (seq %u, expected %u)\n",
                   recv_packet.header.seq_num, conn->expected_seq);
            
            struct sham_packet ack_packet;
            memset(&ack_packet, 0, sizeof(ack_packet));
            ack_packet.header.seq_num = conn->send_seq;
            ack_packet.header.ack_num = conn->expected_seq;
            ack_packet.header.flags = SHAM_ACK;
            ack_packet.header.window_size = conn->recv_window;
            ack_packet.data_len = 0;
            
            send_packet(conn->sockfd, &conn->peer_addr, &ack_packet, conn->loss_rate);
            
            if (is_logging_enabled()) {
                log_event(LOG_SND_ACK, 0, ack_packet.header.ack_num, 0, conn->recv_window);
            }
            
            *len = 0;
            return 0;
        }
    }
    
    *len = 0;
    return 0;
}

int connection_close(struct connection *conn) {
    if (conn->state == CONN_CLOSED) {
        return 0; // Already closed
    }
    
    if (conn->state != CONN_ESTABLISHED) {
        printf("Connection not established, closing socket\n");
        close(conn->sockfd);
        conn->state = CONN_CLOSED;
        return 0;
    }
    
    printf("Initiating 4-way FIN handshake\n");
    
    // Step 1: Send FIN packet
    struct sham_packet fin_packet;
    memset(&fin_packet, 0, sizeof(fin_packet));
    fin_packet.header.seq_num = conn->send_seq;
    fin_packet.header.ack_num = conn->expected_seq;
    fin_packet.header.flags = SHAM_FIN | SHAM_ACK;
    fin_packet.header.window_size = conn->recv_window;
    fin_packet.data_len = 0;
    
    if (send_packet(conn->sockfd, &conn->peer_addr, &fin_packet, conn->loss_rate) < 0) {
        return -1;
    }
    
    if (is_logging_enabled()) {
        log_event(LOG_SND_FIN, fin_packet.header.seq_num, fin_packet.header.ack_num, 0, 0);
    }
    
    conn->state = CONN_FIN_WAIT_1;
    set_socket_timeout(conn->sockfd, RTO_TIMEOUT_MS);
    
    // Step 2: Wait for ACK of our FIN
    int retries = 0;
    while (retries < MAX_RETRIES) {
        struct sham_packet recv_packet;
        struct sockaddr_in from_addr;
        
        int ret = receive_packet(conn->sockfd, &from_addr, &recv_packet);
        if (ret < 0) {
            printf("Error receiving ACK for FIN\n");
            return -1;
        } else if (ret == 0) {
            // Timeout - retransmit FIN
            printf("Timeout waiting for ACK, retransmitting FIN...\n");
            if (send_packet(conn->sockfd, &conn->peer_addr, &fin_packet, conn->loss_rate) < 0) {
                return -1;
            }
            
            if (is_logging_enabled()) {
                log_event(LOG_TIMEOUT, fin_packet.header.seq_num, 0, 0, 0);
                log_event(LOG_RETX_DATA, fin_packet.header.seq_num, 0, 0, 0);
            }
            
            retries++;
            continue;
        }
        
        // Check if this is ACK for our FIN
        if ((recv_packet.header.flags & SHAM_ACK) && 
            recv_packet.header.ack_num == fin_packet.header.seq_num + 1) {
            
            printf("Received ACK for FIN\n");
            if (is_logging_enabled()) {
                log_event(LOG_RCV_ACK, 0, recv_packet.header.ack_num, 0, 0);
            }
            
            conn->state = CONN_FIN_WAIT_2;
            break;
        }
        
        // Check if this is simultaneous close (peer also sent FIN)
        if (recv_packet.header.flags & SHAM_FIN) {
            printf("Simultaneous close detected\n");
            
            if (is_logging_enabled()) {
                log_event(LOG_RCV_FIN, recv_packet.header.seq_num, recv_packet.header.ack_num, 0, 0);
            }
            
            // Send ACK for peer's FIN
            struct sham_packet ack_packet;
            memset(&ack_packet, 0, sizeof(ack_packet));
            ack_packet.header.seq_num = conn->send_seq + 1;
            ack_packet.header.ack_num = recv_packet.header.seq_num + 1;
            ack_packet.header.flags = SHAM_ACK;
            ack_packet.header.window_size = conn->recv_window;
            ack_packet.data_len = 0;
            
            if (send_packet(conn->sockfd, &conn->peer_addr, &ack_packet, conn->loss_rate) < 0) {
                return -1;
            }
            
            if (is_logging_enabled()) {
                log_event(LOG_SND_ACK_FOR_FIN, 0, ack_packet.header.ack_num, 0, 0);
            }
            
            conn->state = CONN_CLOSED;
            printf("Connection closed (simultaneous)\n");
            return 0;
        }
        
        // Handle any other packets (like duplicate ACKs)
        if (recv_packet.header.flags & SHAM_ACK) {
            if (is_logging_enabled()) {
                log_event(LOG_RCV_ACK, 0, recv_packet.header.ack_num, 0, 0);
            }
        }
    }
    
    if (conn->state != CONN_FIN_WAIT_2) {
        printf("Failed to receive ACK for FIN after %d retries\n", MAX_RETRIES);
        conn->state = CONN_CLOSED;
        return -1;
    }
    
    // Step 3: Wait for peer's FIN
    printf("Waiting for peer's FIN...\n");
    retries = 0;
    
    while (retries < MAX_RETRIES) {
        struct sham_packet recv_packet;
        struct sockaddr_in from_addr;
        
        int ret = receive_packet(conn->sockfd, &from_addr, &recv_packet);
        if (ret < 0) {
            printf("Error receiving peer's FIN\n");
            return -1;
        } else if (ret == 0) {
            // Timeout waiting for peer's FIN
            printf("Timeout waiting for peer's FIN (attempt %d)\n", retries + 1);
            retries++;
            continue;
        }
        
        // Check if this is FIN from peer
        if (recv_packet.header.flags & SHAM_FIN) {
            printf("Received FIN from peer\n");
            
            if (is_logging_enabled()) {
                log_event(LOG_RCV_FIN, recv_packet.header.seq_num, recv_packet.header.ack_num, 0, 0);
            }
            
            // Step 4: Send final ACK for peer's FIN
            struct sham_packet final_ack_packet;
            memset(&final_ack_packet, 0, sizeof(final_ack_packet));
            final_ack_packet.header.seq_num = conn->send_seq + 1;
            final_ack_packet.header.ack_num = recv_packet.header.seq_num + 1;
            final_ack_packet.header.flags = SHAM_ACK;
            final_ack_packet.header.window_size = conn->recv_window;
            final_ack_packet.data_len = 0;
            
            if (send_packet(conn->sockfd, &conn->peer_addr, &final_ack_packet, conn->loss_rate) < 0) {
                return -1;
            }
            
            if (is_logging_enabled()) {
                log_event(LOG_SND_ACK_FOR_FIN, 0, final_ack_packet.header.ack_num, 0, 0);
            }
            
            printf("4-way handshake completed successfully\n");
            conn->state = CONN_CLOSED;
            return 0;
        }
        
        // Handle any other packets (like duplicate ACKs)
        if (recv_packet.header.flags & SHAM_ACK) {
            if (is_logging_enabled()) {
                log_event(LOG_RCV_ACK, 0, recv_packet.header.ack_num, 0, 0);
            }
        }
    }
    
    printf("Timeout waiting for peer's FIN after %d attempts\n", MAX_RETRIES);
    conn->state = CONN_CLOSED;
    return -1;
}

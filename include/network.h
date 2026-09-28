#ifndef NETWORK_H
#define NETWORK_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "sham.h"

// Network utility functions
int create_socket(void);
int bind_socket(int sockfd, int port);
int send_packet(int sockfd, struct sockaddr_in *dest, struct sham_packet *packet, double loss_rate);
int receive_packet(int sockfd, struct sockaddr_in *src, struct sham_packet *packet);
int simulate_packet_loss(double loss_rate);
void set_socket_timeout(int sockfd, int timeout_ms);

// Address utility functions
void setup_address(struct sockaddr_in *addr, const char *ip, int port);
char* addr_to_string(struct sockaddr_in *addr);

#endif // NETWORK_H

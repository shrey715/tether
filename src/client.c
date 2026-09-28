#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include "sham.h"
#include "network.h"
#include "connection.h"
#include "logging.h"

// Function prototypes
void print_usage(const char *program_name);
int parse_arguments(int argc, char *argv[], char **server_ip, int *port, 
                   char **input_file, char **output_file, int *chat_mode, double *loss_rate);
int file_transfer_mode(const char *server_ip, int port, const char *input_file, 
                      const char *output_file, double loss_rate);
int chat_mode_client(const char *server_ip, int port, double loss_rate);

int main(int argc, char *argv[]) {
    char *server_ip = NULL;
    int port = 0;
    char *input_file = NULL;
    char *output_file = NULL;
    int chat_mode = 0;
    double loss_rate = 0.0;
    
    if (parse_arguments(argc, argv, &server_ip, &port, &input_file, 
                       &output_file, &chat_mode, &loss_rate) != 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    
    if (init_logging("client") != 0) {
        fprintf(stderr, "Failed to initialize logging\n");
        return EXIT_FAILURE;
    }
    
    printf("Client starting...\n");
    printf("Server: %s:%d\n", server_ip, port);
    printf("Loss rate: %.2f\n", loss_rate);
    
    int result;
    if (chat_mode) {
        printf("Mode: Chat\n");
        result = chat_mode_client(server_ip, port, loss_rate);
    } else {
        printf("Mode: File Transfer\n");
        printf("Input file: %s\n", input_file);
        printf("Output file: %s\n", output_file);
        result = file_transfer_mode(server_ip, port, input_file, output_file, loss_rate);
    }
    
    cleanup_logging();
    
    if (result == 0) {
        printf("Client completed successfully\n");
    } else {
        printf("Client failed with error code: %d\n", result);
    }
    
    return result;
}

void print_usage(const char *program_name) {
    printf("Usage:\n");
    printf("  File Transfer Mode:\n");
    printf("    %s <server_ip> <port> <input_file> <output_file> [loss_rate]\n", program_name);
    printf("\n");
    printf("  Chat Mode:\n");
    printf("    %s <server_ip> <port> --chat [loss_rate]\n", program_name);
    printf("\n");
    printf("Arguments:\n");
    printf("  server_ip    IP address of the server\n");
    printf("  port         Port number to connect to\n");
    printf("  input_file   File to send to server (file transfer mode)\n");
    printf("  output_file  Name for received file (file transfer mode)\n");
    printf("  --chat       Enable chat mode\n");
    printf("  loss_rate    Packet loss probability (0.0-1.0, default: 0.0)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s 127.0.0.1 8080 input.txt output.txt 0.1\n", program_name);
    printf("  %s 127.0.0.1 8080 --chat 0.05\n", program_name);
    printf("\n");
    printf("Environment Variables:\n");
    printf("  RUDP_LOG=1   Enable verbose logging to clientlog.txt\n");
}

int parse_arguments(int argc, char *argv[], char **server_ip, int *port,
                   char **input_file, char **output_file, int *chat_mode, double *loss_rate) {
    if (argc < 3) {
        return -1;
    }
    
    *server_ip = argv[1];
    *port = atoi(argv[2]);
    
    if (*port <= 0 || *port > 65535) {
        fprintf(stderr, "Error: Invalid port number\n");
        return -1;
    }
    
    if (argc >= 4 && strcmp(argv[3], "--chat") == 0) {
        *chat_mode = 1;
        if (argc >= 5) {
            *loss_rate = atof(argv[4]);
        }
    } else {
        *chat_mode = 0;
        if (argc < 5) {
            fprintf(stderr, "Error: File transfer mode requires input_file and output_file\n");
            return -1;
        }
        
        *input_file = argv[3];
        *output_file = argv[4];
        
        if (argc >= 6) {
            *loss_rate = atof(argv[5]);
        }
    }
    
    if (*loss_rate < 0.0 || *loss_rate > 1.0) {
        fprintf(stderr, "Error: Loss rate must be between 0.0 and 1.0\n");
        return -1;
    }
    
    return 0;
}

int file_transfer_mode(const char *server_ip, int port, const char *input_file,
                      const char *output_file, double loss_rate) {
    // Create socket
    int sockfd = create_socket();
    if (sockfd < 0) {
        return -1;
    }
    
    // Initialize connection
    struct connection conn;
    if (connection_init(&conn, sockfd, loss_rate) < 0) {
        close(sockfd);
        return -1;
    }
    
    // Perform 3-way handshake
    if (perform_handshake_client(&conn, server_ip, port) < 0) {
        close(sockfd);
        return -1;
    }
    
    // Send filename first
    printf("Sending filename: %s\n", output_file);
    if (connection_send_data(&conn, (uint8_t*)output_file, strlen(output_file)) < 0) {
        printf("Failed to send filename\n");
        connection_close(&conn);
        close(sockfd);
        return -1;
    }
    
    // Open and send file
    FILE *file = fopen(input_file, "rb");
    if (!file) {
        perror("Failed to open input file");
        connection_close(&conn);
        close(sockfd);
        return -1;
    }
    
    uint8_t buffer[MAX_DATA_SIZE];
    size_t bytes_read;
    size_t total_sent = 0;
    
    printf("Sending file: %s\n", input_file);
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (connection_send_data(&conn, buffer, bytes_read) < 0) {
            printf("Failed to send data\n");
            break;
        }
        total_sent += bytes_read;
        printf("Sent %zu bytes (total: %zu)\n", bytes_read, total_sent);
    }
    
    fclose(file);
    printf("File transfer completed. Total bytes sent: %zu\n", total_sent);
    
    // Close connection
    connection_close(&conn);
    close(sockfd);
    
    return 0;
}

int chat_mode_client(const char *server_ip, int port, double loss_rate) {
    // Create socket
    int sockfd = create_socket();
    if (sockfd < 0) {
        return -1;
    }
    
    // Initialize connection
    struct connection conn;
    if (connection_init(&conn, sockfd, loss_rate) < 0) {
        close(sockfd);
        return -1;
    }
    
    // Perform 3-way handshake
    if (perform_handshake_client(&conn, server_ip, port) < 0) {
        close(sockfd);
        return -1;
    }
    
    printf("Connected to server. Type 'quit' to exit.\n");
    
    // Set socket to non-blocking for chat mode
    set_socket_timeout(conn.sockfd, 100);
    
    char input_buffer[MAX_DATA_SIZE];
    uint8_t recv_buffer[MAX_DATA_SIZE];
    int connection_active = 1;
    
    while (connection_active) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100ms
        
        int activity = select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &timeout);
        
        if (activity > 0 && FD_ISSET(STDIN_FILENO, &read_fds)) {
            // Handle user input
            if (fgets(input_buffer, sizeof(input_buffer), stdin)) {
                // Remove newline
                input_buffer[strcspn(input_buffer, "\n")] = 0;
                
                if (strcmp(input_buffer, "quit") == 0) {
                    printf("Exiting chat...\n");
                    connection_close(&conn);
                    connection_active = 0;
                    break;
                }
                
                // Send message
                int send_result = connection_send_data(&conn, (uint8_t*)input_buffer, strlen(input_buffer));
                if (send_result == -2) {
                    printf("Connection closed by peer\n");
                    connection_active = 0;
                    break;
                } else if (send_result < 0) {
                    printf("Failed to send message\n");
                    connection_active = 0;
                    break;
                }
            }
        }
        
        // Check for incoming messages
        size_t recv_len = sizeof(recv_buffer);
        int ret = connection_receive_data(&conn, recv_buffer, &recv_len);
        if (ret > 0) {
            recv_buffer[recv_len] = '\0';
            printf("Server: %s\n", recv_buffer);
        } else if (ret == -2) {
            printf("Connection closed by peer\n");
            connection_active = 0;
            break;
        }
    }
    
    // Close connection and socket
    if (conn.state != CONN_CLOSED) {
        connection_close(&conn);
    }
    close(sockfd);
    
    return 0;
}

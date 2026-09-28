#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/select.h>
#include <openssl/md5.h>
#include "sham.h"
#include "network.h"
#include "connection.h"
#include "logging.h"

volatile sig_atomic_t running = 1;

// Function prototypes
void print_usage(const char *program_name);
int parse_arguments(int argc, char *argv[], int *port, int *chat_mode, double *loss_rate);
void signal_handler(int signum);
int file_transfer_mode_server(int port, double loss_rate);
int chat_mode_server(int port, double loss_rate);
int calculate_file_md5(const char *filename, char *md5_string);

int main(int argc, char *argv[]) {
    int port = 0;
    int chat_mode = 0;
    double loss_rate = 0.0;
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    if (parse_arguments(argc, argv, &port, &chat_mode, &loss_rate) != 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    
    if (init_logging("server") != 0) {
        fprintf(stderr, "Failed to initialize logging\n");
        return EXIT_FAILURE;
    }
    
    printf("Server starting...\n");
    printf("Port: %d\n", port);
    printf("Loss rate: %.2f\n", loss_rate);
    
    int result;
    if (chat_mode) {
        printf("Mode: Chat\n");
        result = chat_mode_server(port, loss_rate);
    } else {
        printf("Mode: File Transfer\n");
        result = file_transfer_mode_server(port, loss_rate);
    }
    
    cleanup_logging();
    
    if (result == 0) {
        printf("Server completed successfully\n");
    } else {
        printf("Server failed with error code: %d\n", result);
    }
    
    return result;
}

void print_usage(const char *program_name) {
    printf("Usage:\n");
    printf("  File Transfer Mode:\n");
    printf("    %s <port> [loss_rate]\n", program_name);
    printf("\n");
    printf("  Chat Mode:\n");
    printf("    %s <port> --chat [loss_rate]\n", program_name);
    printf("\n");
    printf("Arguments:\n");
    printf("  port         Port number to listen on\n");
    printf("  --chat       Enable chat mode\n");
    printf("  loss_rate    Packet loss probability (0.0-1.0, default: 0.0)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s 8080 0.1\n", program_name);
    printf("  %s 8080 --chat 0.05\n", program_name);
    printf("\n");
    printf("Environment Variables:\n");
    printf("  RUDP_LOG=1   Enable verbose logging to serverlog.txt\n");
    printf("\n");
    printf("Note: Press Ctrl+C to gracefully shutdown the server\n");
}

int parse_arguments(int argc, char *argv[], int *port, int *chat_mode, double *loss_rate) {
    if (argc < 2) {
        return -1;
    }
    
    *port = atoi(argv[1]);
    
    if (*port <= 0 || *port > 65535) {
        fprintf(stderr, "Error: Invalid port number\n");
        return -1;
    }
    
    if (argc >= 3 && strcmp(argv[2], "--chat") == 0) {
        *chat_mode = 1;
        if (argc >= 4) {
            *loss_rate = atof(argv[3]);
        }
    } else {
        *chat_mode = 0;
        if (argc >= 3) {
            *loss_rate = atof(argv[2]);
        }
    }
    
    if (*loss_rate < 0.0 || *loss_rate > 1.0) {
        fprintf(stderr, "Error: Loss rate must be between 0.0 and 1.0\n");
        return -1;
    }
    
    return 0;
}

void signal_handler(int signum) {
    printf("\nReceived signal %d, shutting down gracefully...\n", signum);
    running = 0;
}

int file_transfer_mode_server(int port, double loss_rate) {
    // Create socket
    int sockfd = create_socket();
    if (sockfd < 0) {
        return -1;
    }
    
    // Bind to port
    if (bind_socket(sockfd, port) < 0) {
        close(sockfd);
        return -1;
    }
    
    printf("Server listening on port %d...\n", port);
    
    while (running) {
        // Initialize connection
        struct connection conn;
        if (connection_init(&conn, sockfd, loss_rate) < 0) {
            continue;
        }
        
        // Wait for client connection (3-way handshake)
        if (perform_handshake_server(&conn) < 0) {
            continue;
        }
        
        // Receive filename first
        uint8_t filename_buffer[MAX_DATA_SIZE];
        size_t filename_len = sizeof(filename_buffer) - 1; // Leave space for null terminator
        char filename[256] = "received_file.dat"; // Default filename
        
        printf("Waiting for filename...\n");
        set_socket_timeout(conn.sockfd, 5000); // 5 second timeout for filename
        
        int filename_ret = connection_receive_data(&conn, filename_buffer, &filename_len);
        if (filename_ret > 0 && filename_len > 0) {
            // Null terminate and copy filename
            filename_buffer[filename_len] = '\0';
            
            // Sanitize filename (remove path separators for security)
            char *sanitized_name = strrchr((char*)filename_buffer, '/');
            if (sanitized_name) {
                sanitized_name++; // Skip the '/'
            } else {
                sanitized_name = strrchr((char*)filename_buffer, '\\');
                if (sanitized_name) {
                    sanitized_name++; // Skip the '\'
                } else {
                    sanitized_name = (char*)filename_buffer;
                }
            }
            
            // Copy sanitized filename
            strncpy(filename, sanitized_name, sizeof(filename) - 1);
            filename[sizeof(filename) - 1] = '\0';
            
            printf("Received filename: %s\n", filename);
        } else {
            printf("Failed to receive filename, using default: %s\n", filename);
        }
        
        // Receive file data
        FILE *output_file = fopen(filename, "wb");
        if (!output_file) {
            perror("Failed to create output file");
            // Try with timestamp if original name fails
            snprintf(filename, sizeof(filename), "received_file_%ld.dat", (long)time(NULL));
            output_file = fopen(filename, "wb");
            if (!output_file) {
                perror("Failed to create fallback output file");
                connection_close(&conn);
                continue;
            }
        }
        
        uint8_t buffer[MAX_DATA_SIZE];
        size_t total_received = 0;
        int consecutive_timeouts = 0;
        const int MAX_CONSECUTIVE_TIMEOUTS = 30;  // 30 seconds max
        
        printf("Receiving file data...\n");
        
        // Set socket to short timeout for receiving
        set_socket_timeout(conn.sockfd, 1000);  // 1 second timeout
        
        while (running && consecutive_timeouts < MAX_CONSECUTIVE_TIMEOUTS) {
            size_t recv_len = sizeof(buffer);
            int ret = connection_receive_data(&conn, buffer, &recv_len);
            
            if (ret > 0) {
                fwrite(buffer, 1, recv_len, output_file);
                total_received += recv_len;
                printf("Received %zu bytes (total: %zu)\n", recv_len, total_received);
                consecutive_timeouts = 0; // Reset timeout counter
            } else if (ret == 0) {
                // Timeout - increment counter
                consecutive_timeouts++;
                if (consecutive_timeouts % 5 == 0) {
                    printf("Timeout %d/%d (total received: %zu bytes)\n", 
                           consecutive_timeouts, MAX_CONSECUTIVE_TIMEOUTS, total_received);
                }
            } else if (ret == -2) {
                // Connection closed by peer
                printf("Connection closed by peer\n");
                break;
            } else {
                printf("Error receiving data: %d\n", ret);
                break;
            }
        }
        
        fclose(output_file);
        
        printf("File transfer completed. Total bytes received: %zu\n", total_received);
        
        if (total_received > 0) {
            char md5_string[33];
            if (calculate_file_md5(filename, md5_string) == 0) {
                printf("MD5 %s\n", md5_string);
            }
            
            printf("File saved as: %s\n", filename);
        } else {
            // Remove empty file
            remove(filename);
            printf("No data received, file not created\n");
        }
        
        // Close connection
        connection_close(&conn);
        
        break; // Exit after one file transfer
    }
    
    close(sockfd);
    return 0;
}

int chat_mode_server(int port, double loss_rate) {
    // Create socket
    int sockfd = create_socket();
    if (sockfd < 0) {
        return -1;
    }
    
    // Bind to port
    if (bind_socket(sockfd, port) < 0) {
        close(sockfd);
        return -1;
    }
    
    printf("Server listening on port %d...\n", port);
    
    while (running) {
        // Initialize connection
        struct connection conn;
        if (connection_init(&conn, sockfd, loss_rate) < 0) {
            continue;
        }
        
        // Wait for client connection (3-way handshake)
        if (perform_handshake_server(&conn) < 0) {
            continue;
        }
        
        printf("Client connected. Type 'quit' to exit.\n");
        
        // Set socket to non-blocking for chat mode
        set_socket_timeout(conn.sockfd, 100);
        
        char input_buffer[MAX_DATA_SIZE];
        uint8_t recv_buffer[MAX_DATA_SIZE];
        int connection_active = 1;
        
        while (running && connection_active) {
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
                printf("Client: %s\n", recv_buffer);
            } else if (ret == -2) {
                printf("Connection closed by peer\n");
                connection_active = 0;
                break;
            }
        }
        
        // Close connection
        if (conn.state != CONN_CLOSED) {
            connection_close(&conn);
        }
        break; // Exit after one chat session
    }
    
    close(sockfd);
    return 0;
}

int calculate_file_md5(const char *filename, char *md5_string) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        return -1;
    }
    
    MD5_CTX md5_ctx;
    MD5_Init(&md5_ctx);
    
    unsigned char buffer[1024];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        MD5_Update(&md5_ctx, buffer, bytes_read);
    }
    
    fclose(file);
    
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5_Final(digest, &md5_ctx);
    
    // Convert to lowercase hex string
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        sprintf(&md5_string[i * 2], "%02x", digest[i]);
    }
    md5_string[32] = '\0';
    
    return 0;
}

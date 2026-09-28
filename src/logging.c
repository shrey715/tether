#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static FILE *log_file = NULL;
static int logging_enabled = 0;

int init_logging(const char *role) {
    char *log_env = getenv("RUDP_LOG");
    if (log_env && strcmp(log_env, "1") == 0) {
        logging_enabled = 1;
        
        const char *filename = (strcmp(role, "client") == 0) ? "clientlog.txt" : "serverlog.txt";
        log_file = fopen(filename, "w");
        
        if (!log_file) {
            perror("Failed to open log file");
            logging_enabled = 0;
            return -1;
        }
        
        printf("Logging enabled for %s - writing to %s\n", role, filename);
    }
    
    return 0;
}

void log_event(log_type_t type, uint32_t seq_num, uint32_t ack_num, 
               size_t data_len, uint16_t window_size) {
    if (!logging_enabled || !log_file) {
        return;
    }
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm_info = localtime(&tv.tv_sec);
    
    char timestamp[64];
    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d%02d%02d.%06ld", 
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec, tv.tv_usec);
    
    switch (type) {
        case LOG_SND_SYN:
            fprintf(log_file, "[%s] [LOG] SND SYN SEQ=%u\n", timestamp, seq_num);
            break;
        case LOG_RCV_SYN:
            fprintf(log_file, "[%s] [LOG] RCV SYN SEQ=%u\n", timestamp, seq_num);
            break;
        case LOG_SND_SYN_ACK:
            fprintf(log_file, "[%s] [LOG] SND SYN-ACK SEQ=%u ACK=%u\n", timestamp, seq_num, ack_num);
            break;
        case LOG_RCV_ACK_FOR_SYN:
            fprintf(log_file, "[%s] [LOG] RCV ACK FOR SYN\n", timestamp);
            break;
        case LOG_SND_DATA:
            fprintf(log_file, "[%s] [LOG] SND DATA SEQ=%u LEN=%zu\n", timestamp, seq_num, data_len);
            break;
        case LOG_RCV_DATA:
            fprintf(log_file, "[%s] [LOG] RCV DATA SEQ=%u LEN=%zu\n", timestamp, seq_num, data_len);
            break;
        case LOG_SND_ACK:
            fprintf(log_file, "[%s] [LOG] SND ACK SEQ=%u WIN=%u\n", timestamp, ack_num, window_size);
            break;
        case LOG_RCV_ACK:
            fprintf(log_file, "[%s] [LOG] RCV ACK SEQ=%u\n", timestamp, ack_num);
            break;
        case LOG_TIMEOUT:
            fprintf(log_file, "[%s] [LOG] TIMEOUT SEQ=%u\n", timestamp, seq_num);
            break;
        case LOG_RETX_DATA:
            fprintf(log_file, "[%s] [LOG] RETX DATA SEQ=%u LEN=%zu\n", timestamp, seq_num, data_len);
            break;
        case LOG_FLOW_WIN_UPDATE:
            fprintf(log_file, "[%s] [LOG] FLOW WIN UPDATE=%u\n", timestamp, window_size);
            break;
        case LOG_DROP_DATA:
            fprintf(log_file, "[%s] [LOG] DROP DATA SEQ=%u\n", timestamp, seq_num);
            break;
        case LOG_SND_FIN:
            fprintf(log_file, "[%s] [LOG] SND FIN SEQ=%u\n", timestamp, seq_num);
            break;
        case LOG_RCV_FIN:
            fprintf(log_file, "[%s] [LOG] RCV FIN SEQ=%u\n", timestamp, seq_num);
            break;
        case LOG_SND_ACK_FOR_FIN:
            fprintf(log_file, "[%s] [LOG] SND ACK FOR FIN\n", timestamp);
            break;
        default:
            fprintf(log_file, "[%s] [LOG] UNKNOWN EVENT\n", timestamp);
            break;
    }
    fflush(log_file);
}

void cleanup_logging(void) {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    logging_enabled = 0;
}

int is_logging_enabled(void) {
    return logging_enabled;
}

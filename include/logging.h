#ifndef LOGGING_H
#define LOGGING_H

#include <stdio.h>
#include <time.h>
#include "sham.h"

// Logging types as per assignment requirements
typedef enum {
    LOG_SND_SYN,
    LOG_RCV_SYN,
    LOG_SND_SYN_ACK,
    LOG_RCV_ACK_FOR_SYN,
    LOG_SND_DATA,
    LOG_RCV_DATA,
    LOG_SND_ACK,
    LOG_RCV_ACK,
    LOG_TIMEOUT,
    LOG_RETX_DATA,
    LOG_FLOW_WIN_UPDATE,
    LOG_DROP_DATA,
    LOG_SND_FIN,
    LOG_RCV_FIN,
    LOG_SND_ACK_FOR_FIN
} log_type_t;

// Logging functions
int init_logging(const char *role);
void log_event(log_type_t type, uint32_t seq_num, uint32_t ack_num, size_t data_len, uint16_t window_size);
void cleanup_logging(void);
int is_logging_enabled(void);

#endif // LOGGING_H

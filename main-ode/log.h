#pragma once
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/util/queue.h"

#define LOG_SLOTS 10
#define LOG_PREFIX_SIZE 14
#define LOG_TEXT_SIZE 65

extern queue_t log_queue;
void log_init(void);
char *log_alloc(void);
void log_free(char *msg);

// Core 0 prints directly; core 1 formats into a pooled buffer and queues it for core 0.
#define LOG(tag, ...)                                                                   \
    do {                                                                                \
        if (get_core_num() == 1) {                                                      \
            if (!queue_is_full(&log_queue)) {                                           \
                char *log_msg_ = log_alloc();                                           \
                if (log_msg_ != NULL) {                                                 \
                    snprintf(log_msg_, LOG_PREFIX_SIZE, "Core 1: %s ", tag);            \
                    snprintf(log_msg_ + strlen("Core 1: ") + strlen(tag) + 1,           \
                             LOG_TEXT_SIZE, __VA_ARGS__);                               \
                    queue_try_add(&log_queue, &log_msg_);                               \
                }                                                                       \
            }                                                                           \
        } else {                                                                        \
            printf("Core 0: %s ", tag);                                                 \
            printf(__VA_ARGS__);                                                        \
        }                                                                               \
    } while (0)

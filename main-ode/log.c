// Fixed pool of log buffers shared by both cores. Names are ours.
#include "pico/critical_section.h"
#include "log.h"

typedef struct {
    int used;
    char text[LOG_PREFIX_SIZE + LOG_TEXT_SIZE + 1];
} log_slot_t;

static critical_section_t log_cs;
static log_slot_t log_slots[LOG_SLOTS];

void log_init(void)
{
    critical_section_init(&log_cs);
}

char *log_alloc(void)
{
    critical_section_enter_blocking(&log_cs);
    for (int i = 0; i < LOG_SLOTS; i++) {
        if (!log_slots[i].used) {
            log_slots[i].used = 1;
            critical_section_exit(&log_cs);
            return log_slots[i].text;
        }
    }
    critical_section_exit(&log_cs);
    return NULL;
}

void log_free(char *msg)
{
    critical_section_enter_blocking(&log_cs);
    for (int i = 0; i < LOG_SLOTS; i++) {
        if (msg == log_slots[i].text) {
            log_slots[i].used = 0;
            critical_section_exit(&log_cs);
            return;
        }
    }
    critical_section_exit(&log_cs);
}

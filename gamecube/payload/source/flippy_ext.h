#include <gctypes.h>

// Layout from later cubeboot ipc.h; 160 bytes, matching the payload's transfer length.
typedef struct {
    u8 net_flags;
    u8 net_status;
    char ssid[33];
    char addrstr[40];
    u8 nm;
    u8 gw[16];
    u8 dns0[16];
    u8 dns1[16];
    u8 server[16];
    u8 pad[20];
} __attribute__((aligned(32))) flippydrive_net_status_t;

void dvd_custom_cmd18(void);
int dvd_custom_reload_config(void *dst);
int dvd_custom_net_status(flippydrive_net_status_t *dst);

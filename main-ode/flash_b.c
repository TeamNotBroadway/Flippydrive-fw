// inih handler for /config.ini. Names are ours.
#include <string.h>
#include <strings.h>
#include "pico/stdlib.h"
#include "main_ode.h"
#include "log.h"
#include "pico/runtime.h"
#include "flippy.h"

extern int ini_result;
extern struct ipc_cfg_t ipc_cfg;

int config_ini_handler(void *user, const char *section, const char *name, const char *value)
{
    int ret = 1;
    if (strcasecmp(section, "network") == 0) {
        if (strcasecmp(name, "server") == 0)
            strncpy(ipc_cfg.server, value, 39);
        if (strcasecmp(name, "ssid") == 0)
            strncpy(ipc_cfg.ssid, value, 32);
        if (strcasecmp(name, "password") == 0)
            strncpy(ipc_cfg.password, value, 63);
        if (strcasecmp(name, "is_default") == 0 && *value) {
            ipc_cfg.flags &= ~IPC_CFG_IS_DEFAULT;
            ipc_cfg.flags |= (*value == '1') << 7;
        }
    } else if (strcasecmp(section, "flippydrive") == 0) {
        if (strcasecmp(name, "boot_mode") == 0) {
            int mode = 1;
            if (strcasecmp(value, "bypass") != 0)
                mode = (strcasecmp(value, "enhanced_bypass") == 0) << 1;
            ini_result = mode;
            LOG("DI", "Config boot_mode set to %u\n", ini_result);
        }
    } else {
        ret = 0;
    }
    return ret;
}

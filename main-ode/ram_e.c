// RAM-resident IPC command dispatcher (ESP link / SD file commands, jump table on a & 31). Names are ours.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/structs/sio.h"
#include "hardware/watchdog.h"
#include "ff.h"
#include "main_ode.h"
#include "log.h"
#include "ioexp.h"
#include "flippy.h"

#define DI_READ_ERROR DI_ERR_READ
#define DI_BAD_FD DI_ERR_BAD_HANDLE

extern uint di_sm;
extern uint8_t di_buffer[0x4000];
extern DIR root_dir;
extern FIL di_file;
extern uint8_t root_dir_open;
extern uint8_t root_dir_res;
extern uint8_t net_stream_active;
extern bool disc_valid(int v);
extern bool disc_is_file(int v);
extern bool id_bitmap_test(uint32_t idx);
extern void id_bitmap_set(uint32_t idx, int val);
extern void di_stream_file(uint32_t len);
extern int di_stream_net(uint32_t addr, uint32_t len, uint sm, uint32_t fd);
extern int ini_parse_string(const char *str, int (*handler)(void *, const char *, const char *, const char *),
                            void *user);
extern int config_ini_handler(void *user, const char *section, const char *name, const char *value);

extern int req_status(uint8_t *buf, uint8_t arg);
extern int req_net_disable(void);
extern int req_net_status(uint8_t *buf);
extern int req_fs_info(uint8_t *buf);
extern int req_close(uint8_t arg);
extern int req_readdir(uint8_t *buf, uint8_t arg);
extern int req_open(struct fd_path_req *p);
extern int req_rename(struct fd_rename_req *p);
extern int req_write(uint8_t *p, uint8_t arg);
extern int req_config_set(struct ipc_cfg_t *p);
extern int req_presence(uint8_t *p);
extern int req_remote(uint8_t *p);
extern int req_unlink(struct fd_path_req *p);
extern int req_mkdir(struct fd_path_req *p);
extern void req_clear_busy(void);

extern uint32_t di_error_status;
extern uint8_t ipc_cfg_dirty;
extern uint8_t ipc_open_ro;
extern int ini_result;
extern int mount_h1;
extern int mount_h2;
extern uint64_t di_file_size;
extern struct ipc_cfg_t ipc_cfg;

#define PUT(v)                                  \
    do {                                        \
        pio_sm_put_blocking(pio0, di_sm, (v));  \
        (void)gpio_get_all();                   \
    } while (0)

static inline void send_words(const uint32_t *p, const uint32_t *end)
{
    do {
        PUT(*p);
        p++;
    } while (p != end);
}

int __not_in_flash_func(cmd_ipc)(uint32_t a, uint32_t b, uint32_t c)
{
    if (di_error_status) {
        ioexp_set_pin(2, true);
        di_error_status = 0;
    }
    uint32_t arg = IPC_HANDLE(a);
    switch (IPC_SUB(a)) {
    case IPC_NET_DISABLE: {
        req_net_disable();
        LOG("DI", "IPC NET DISABLE\n");
        PUT(0);
        return 0;
    }
    case IPC_STATUS: {
        uint32_t st[8];
        struct fd_file_reply *s = (struct fd_file_reply *)st;
        if (net_stream_active) {
            req_status((uint8_t *)st, arg);
            id_bitmap_set(s->handle, !((s->flags >> 2) & 1));
            LOG("DI", "IPC STATUS 0x%08X fd %d flags 0x%02X\n", st[0], s->handle, s->flags);
        } else {
            st[0] = root_dir_res;
            *(uint64_t *)&st[1] = di_file_size;
            s->handle = FD_HANDLE_FLASH;
            if (di_file.flag & FA_WRITE)
                s->flags = FD_OPEN_WRITE;
            LOG("DI", "IPC STATUS FLASH 0x%08X %d\n", st[0], arg);
        }
        send_words(st, st + 8);
        return 0;
    }
    case IPC_MOUNT: {
        if (mount_h1 != arg)
            req_clear_busy();
        mount_h1 = arg;
        mount_h2 = IPC_MOUNT_HANDLE2(a);
        LOG("DI", "IPC SET DEFAULT FD: %u 2ND DISC: %u\n", arg, mount_h2);
        PUT(0);
        return 0;
    }
    case IPC_RELOAD_CONFIG: {
        struct fd_path_req req;
        ipc_cfg.flags = 0;
        ipc_cfg_dirty = 0;
        ini_result = 0;
        ipc_open_ro = 1;
        memset(&req, 0, sizeof req);
        memcpy(req.path.name, "/config.ini", 12);
        req.path.flags = FD_OPEN_NO_FASTSEEK | FD_OPEN_NO_DVDSPEED;
        req_open(&req);
        struct fd_file_reply st;
        memset(&st, 0, sizeof st);
        req_status((uint8_t *)&st, 0);
        uint8_t fd = st.handle;
        id_bitmap_set(fd, !((st.flags >> 2) & 1));
        uint32_t size = *(uint32_t *)((uint8_t *)&st + 4);
        if (st.result != 0) {
            LOG("DI", "Unable to open config.ini, ret %d\n", st.result);
        } else {
            size = (size + 3) & ~3u;
            if (size <= sizeof esp_rx.data) {
                aud_read(0, size, esp_rx.data, fd);
                req_close(fd);
                int ret = ini_parse_string((const char *)esp_rx.data, config_ini_handler, 0);
                if (ret < 0)
                    LOG("DI", "Unable to parse config.ini, ret %d\n", ret);
                if (ipc_cfg.server[0]) {
                    ipc_cfg.flags |= IPC_CFG_SERVER_SET;
                    ipc_cfg_dirty = 1;
                }
                req_config_set(&ipc_cfg);
                LOG("DI", "config.ini loaded\n");
            } else {
                LOG("DI", "config.ini too large %u\n", size);
                req_close(fd);
            }
        }
        uint8_t *buf = (uint8_t *)&req;
        memset(buf + 4, 0, 28);
        *(uint32_t *)buf = __builtin_bswap32(ini_result);
        LOG("DI", "IPC RELOAD CONFIG, %u\n", ini_result);
        send_words((uint32_t *)buf, (uint32_t *)(buf + 32));
        return 0;
    }
    case IPC_UNLINK: {
        struct fd_path_req req;
        net_stream_active = 1;
        req.hdr.magic = ESP_REQ_MAGIC;
        di_receive(sizeof req.path, 0);
        memcpy(&req.path, di_buffer, sizeof req.path);
        req.path.name[255] = 0;
        LOG("DI", "UNLINK %s\n", req.path.name);
        req_unlink(&req);
        return 0;
    }
    case IPC_READDIR: {
        struct fd_filestat reply __attribute__((aligned(4)));
        uint8_t *buf = (uint8_t *)&reply;
        FILINFO fno;
        if (disc_is_file(arg)) {
            net_stream_active = 1;
            req_readdir(buf, arg);
            struct fd_filestat *r = &reply;
            if (r->result != 0)
                LOG("DI", "READDIR Failure:%d\n", r->result);
            LOG("DI", "READDIR Result:%s\n", buf);
            send_words((uint32_t *)buf, (uint32_t *)(buf + 288));
            return 0;
        }
        net_stream_active = 0;
        struct fd_filestat *r = &reply;
        if (!root_dir_open) {
            LOG("DI", "READDIR NOT OPEN\n");
            root_dir_res = f_opendir(&root_dir, "/");
            if (root_dir_res != 0) {
                LOG("DI", "Unable to open root directory, terminating\n");
                return 0;
            }
            root_dir_open = 1;
        }
        FRESULT fr = f_readdir(&root_dir, &fno);
        root_dir_res = fr;
        if (fr != 0 || fno.fname[0] == 0) {
            r->result = fr;
            f_closedir(&root_dir);
            root_dir_open = 0;
            if (fno.fname[0] != 0)
                return 0;
        }
        strncpy(r->name, fno.fname, 256);
        r->type = (fno.fattrib >> 4) & 1;
        r->size = fno.fsize;
        r->date = fno.fdate;
        r->time = fno.ftime;
        r->attributes = fno.fattrib;
        LOG("DI", "READDIR Result:%s\n", buf);
        send_words((uint32_t *)buf, (uint32_t *)(buf + 288));
        return 0;
    }
    case IPC_FLASH_OPEN: {
        struct fd_path_req req;
        net_stream_active = 0;
        di_receive(sizeof req.path, 0);
        memcpy(&req.path, di_buffer, sizeof req.path);
        req.path.name[255] = 0;
        char *name = req.path.name;
        uint8_t type = req.path.type;
        uint8_t flags = req.path.flags;
        LOG("DI", "OPEN FLASH (%s) %s\n", type ? "DIR" : "FILE", name);
        if (type == 0) {
            BYTE mode = (flags & FD_OPEN_WRITE) ? (FA_READ | FA_WRITE | FA_OPEN_ALWAYS) : FA_READ;
            root_dir_res = f_open(&di_file, name, mode);
            if (root_dir_res != 0) {
                LOG("DI", "OPEN FAILED %d\n", root_dir_res);
                return 0;
            }
            di_file_size = di_file.obj.objsize;
        } else if (type == 1) {
            if (root_dir_open) {
                f_closedir(&root_dir);
                root_dir_open = 0;
            }
            root_dir_res = f_opendir(&root_dir, name);
            if (root_dir_res != 0) {
                LOG("DI", "OPENDIR FAILED\n");
                return 0;
            }
            root_dir_open = 1;
        } else {
            LOG("DI", "OPEN invalid type\n");
        }
        return 0;
    }
    case IPC_FLASH_UNLINK: {
        struct fd_path path;
        net_stream_active = 0;
        di_receive(sizeof path, 0);
        memcpy(&path, di_buffer, sizeof path);
        path.name[255] = 0;
        LOG("DI", "UNLINK FLASH %s\n", path.name);
        root_dir_res = f_unlink(path.name);
        return 0;
    }
    case IPC_RENAME: {
        struct fd_rename_req req;
        net_stream_active = 1;
        req.hdr.magic = ESP_REQ_MAGIC;
        di_receive(sizeof req.from + sizeof req.to, 0);
        memcpy(&req.from, di_buffer, sizeof req.from + sizeof req.to);
        req.from.name[255] = 0;
        req.to.name[255] = 0;
        LOG("DI", "RENAME FILE %s %s\n", req.from.name, req.to.name);
        req_rename(&req);
        return 0;
    }
    case IPC_PRESENCE: {
        uint8_t buf[32 + 256];
        di_receive(256, 0);
        memcpy(buf + 32, di_buffer, 256);
        buf[72] = 0;
        buf[112] = 0;
        buf[152] = 0;
        LOG("DI", "IPC PRESENCE\n");
        req_presence(buf);
        return 0;
    }
    case IPC_REMOTE: {
        uint8_t buf[32 + 32];
        di_receive(32, 0);
        memcpy(buf + 32, di_buffer, 32);
        LOG("DI", "IPC REMOTE\n");
        req_remote(buf);
        return 0;
    }
    case IPC_MKDIR: {
        struct fd_path_req req;
        net_stream_active = 1;
        req.hdr.magic = ESP_REQ_MAGIC;
        di_receive(sizeof req.path, 0);
        memcpy(&req.path, di_buffer, sizeof req.path);
        req.path.name[255] = 0;
        LOG("DI", "MKDIR %s\n", req.path.name);
        req_mkdir(&req);
        return 0;
    }
    case IPC_READ: {
        LOG("DI", "READ IPC 0x%08X len 0x%X fd %u\n", b, c, arg);
        if (!disc_valid(arg)) {
            LOG("DI", "READ IPC INVALID FILE DESCRIPTOR %ul\n", arg);
            di_error(DI_BAD_FD);
            return 1;
        }
        if (id_bitmap_test(a >> 16))
            sleep_us(300);
        if (disc_is_file(arg)) {
            net_stream_active = 1;
            if (di_stream_net(b, c, di_sm, arg) == 0)
                return 0;
            LOG("DI", "READ ERROR\n");
        } else {
            net_stream_active = 0;
            root_dir_res = f_lseek(&di_file, b);
            if (root_dir_res == 0) {
                di_stream_file(c);
                return 0;
            }
            LOG("DI", "READ FLASH SEEK ERROR\n");
        }
        di_error(DI_READ_ERROR);
        return 1;
    }
    case IPC_WRITE: {
        if (c > 0x3fe0)
            c = 0x3fe0;
        uint32_t n = c ? (c + 31) & ~31u : 32;
        di_receive(n, 32);
        struct fd_req_hdr *h = (struct fd_req_hdr *)di_buffer;
        h->magic = ESP_REQ_MAGIC;
        h->cmd = IPC_WRITE;
        h->len = c;
        h->offset = b;
        if (disc_is_file(arg)) {
            net_stream_active = 1;
            LOG("DI", "WRITE addr: 0x%08X DIlen: 0x%08X len: 0x%08X fd: %d\n", b, n, c, arg);
            req_write(di_buffer, arg);
            return 0;
        }
        net_stream_active = 0;
        LOG("DI", "WRITE FLASH addr: 0x%08X DIlen: 0x%08X len: 0x%08X fd: %d\n", b, n, c, arg);
        root_dir_res = f_lseek(&di_file, b);
        if (root_dir_res == 0) {
            UINT bw;
            root_dir_res = f_write(&di_file, di_buffer + 32, c, &bw);
            return 0;
        }
        LOG("DI", "WRITE FLASH SEEK ERROR\n");
        return 0;
    }
    case IPC_OPEN: {
        struct fd_path_req req;
        net_stream_active = 1;
        req.hdr.magic = ESP_REQ_MAGIC;
        di_receive(sizeof req.path, 0);
        memcpy(&req.path, di_buffer, sizeof req.path);
        req.path.name[255] = 0;
        LOG("DI", "OPEN FLAG:0x%02X %s\n", req.path.flags, req.path.name);
        req_open(&req);
        ipc_open_ro = req.path.flags & FD_OPEN_NO_CACHE;
        return 0;
    }
    case IPC_RESET: {
        LOG("DI", "RESET %08x\n", b);
        if (b == IPC_RESET_MAGIC) {
            PUT(0);
            busy_wait_ms(100);
            watchdog_reboot(0, 0, 0);
            busy_wait_ms(100);
            return 0;
        }
        PUT(1);
        return 0;
    }
    case IPC_FS_INFO: {
        uint32_t st[8];
        req_fs_info((uint8_t *)st);
        struct fd_fsinfo *f = (struct fd_fsinfo *)st;
        LOG("DI", "IPC FS INFO free: %llu total: %llu 0x%08X\n", f->free, f->total, f->status);
        send_words(st, st + 8);
        return 0;
    }
    case IPC_NET_STATUS: {
        uint32_t st[40];
        req_net_status((uint8_t *)st);
        LOG("DI", "IPC NET STATUS 0x%02X\n", ((uint8_t *)st)[1]);
        send_words(st, st + 40);
        return 0;
    }
    case IPC_CLOSE: {
        if (disc_is_file(arg)) {
            net_stream_active = 1;
            req_close(arg);
            LOG("DI", "CLOSE %d\n", arg);
        } else {
            net_stream_active = 0;
            root_dir_res = f_close(&di_file);
            LOG("DI", "CLOSE FLASH %d\n", arg);
        }
        PUT(0);
        return 0;
    }
    default:
        return 0;
    }
}

// Host-link request submit/helper commands and INI parser from main-ode. Names are ours.
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/mutex.h"
#include "main_ode.h"
#include "log.h"
#include "flippy.h"

#define REQ_MAGIC ESP_REQ_MAGIC

struct req { uint32_t magic; uint8_t cmd, arg; uint16_t pad; uint32_t w[6]; };

__attribute__((noinline, noclone)) void req_submit(void *req, uint32_t req_len, uint8_t *buf, uint32_t len) {
    mutex_t *fm = &flash_mutex;
    mutex_enter_blocking(&link_mutex);
    mutex_enter_blocking(fm);
    if (esp_rx.wr != esp_rx.rd) {
        esp_rx.wr = esp_rx.rd;
        esp_drain();
    }
    mutex_exit(&link_mutex);
    int done;
    if (qspi_send(req, req_len, len) == 0 && (done = 0, len != 0)) {
        do {
            uint32_t got[2];
            got[0] = 0;
            esp_dma_read(buf + done, len - done, got, 0);
            done += got[0];
        } while (done < len);
    }
    mutex_exit(fm);
}

void req_clear_busy(void) {
    mutex_enter_blocking(&link_mutex);
    esp_rx.active = 0;
    mutex_exit(&link_mutex);
}

bool disc_valid(int v)
{
    return v != 0;
}

bool disc_is_file(int v)
{
    return v != FD_HANDLE_FLASH;
}

int aud_read(uint32_t pos, uint32_t len, void *buf, int fd) {
    struct req r;
    r.magic = REQ_MAGIC; r.cmd = IPC_READ; r.w[0] = pos; r.w[1] = len; r.arg = fd;
    req_submit(&r, 32, buf, len);
    return 0;
}

int req_status(uint8_t *buf, uint8_t arg) {
    struct req r;
    r.magic = REQ_MAGIC; r.cmd = IPC_STATUS; r.arg = arg;
    req_submit(&r, 32, buf, 32);
    return 0;
}

int req_net_disable(void) {
    struct req r;
    r.magic = REQ_MAGIC; r.cmd = IPC_NET_DISABLE;
    req_submit(&r, 32, 0, 0);
    return 0;
}

int req_net_status(uint8_t *buf) {
    struct req r;
    r.magic = REQ_MAGIC; r.cmd = IPC_NET_STATUS;
    req_submit(&r, 32, buf, 160);
    return 0;
}

int req_fs_info(uint8_t *buf) {
    struct req r;
    r.magic = REQ_MAGIC; r.cmd = IPC_FS_INFO;
    req_submit(&r, 32, buf, 32);
    return 0;
}

int req_close(uint8_t arg) {
    struct req r;
    r.magic = REQ_MAGIC; r.cmd = IPC_CLOSE; r.arg = arg;
    req_submit(&r, 32, 0, 0);
    return 0;
}

int req_readdir(uint8_t *buf, uint8_t arg) {
    struct req r;
    r.magic = REQ_MAGIC; r.cmd = IPC_READDIR; r.arg = arg;
    req_submit(&r, 32, buf, 288);
    LOG("FILE", "READDIR complete\n");
    return 0;
}

#define REQ_HDR(p, c) ((p)[4] = (c), ((struct fd_req_hdr *)(p))->magic = REQ_MAGIC)

int req_open(struct fd_path_req *p) {
    p->hdr.cmd = IPC_OPEN;
    p->hdr.magic = REQ_MAGIC;
    req_submit(p, 320, 0, 0);
    req_clear_busy();
    return 0;
}

int req_rename(struct fd_rename_req *p) {
    p->hdr.cmd = IPC_RENAME;
    p->hdr.magic = REQ_MAGIC;
    req_submit(p, 608, 0, 0);
    return 0;
}

int req_write(uint8_t *p, uint8_t arg) {
    REQ_HDR(p, IPC_WRITE);
    p[5] = arg;
    uint32_t len = ((struct fd_req_hdr *)p)->len + 32;
    if (len > 0x4000) len = 0x4000;
    req_submit(p, len, 0, 0);
    req_clear_busy();
    return 0;
}

int req_config_set(struct ipc_cfg_t *p) {
    REQ_HDR((uint8_t *)p, IPC_CONFIG_SET);
    req_submit(p, 256, 0, 0);
    return 0;
}

int req_presence(uint8_t *p) {
    REQ_HDR(p, IPC_PRESENCE);
    req_submit(p, 288, 0, 0);
    return 0;
}

int req_remote(uint8_t *p) {
    REQ_HDR(p, IPC_REMOTE);
    req_submit(p, 64, 0, 0);
    return 0;
}

int req_unlink(struct fd_path_req *p) {
    p->hdr.cmd = IPC_UNLINK;
    p->hdr.magic = REQ_MAGIC;
    req_submit(p, 320, 0, 0);
    return 0;
}

int req_mkdir(struct fd_path_req *p) {
    p->hdr.cmd = IPC_MKDIR;
    p->hdr.magic = REQ_MAGIC;
    req_submit(p, 320, 0, 0);
    return 0;
}

struct str_stream { const char *ptr; size_t left; };

char *ini_str_reader(char *str, int num, void *stream) {
    struct str_stream *ctx = stream;
    const char *ptr = ctx->ptr;
    size_t left = ctx->left;
    char *strp = str;
    char c;
    if (left == 0 || num < 2) return NULL;
    while (num > 1 && left != 0) {
        c = *ptr++;
        left--;
        *strp++ = c;
        if (c == 10) break;
        num--;
    }
    *strp = 0;
    ctx->ptr = ptr;
    ctx->left = left;
    return str;
}

typedef char *(*ini_reader)(char *, int, void *);
typedef int (*ini_handler)(void *, const char *, const char *, const char *);

static char *strncpy0(char *dest, const char *src, size_t size) {
    size_t i;
    for (i = 0; i < size - 1 && src[i]; i++) dest[i] = src[i];
    dest[i] = 0;
    return dest;
}
static char *rstrip(char *s) {
    char *p = s + strlen(s);
    while (p > s && isspace((unsigned char)*--p)) *p = 0;
    return s;
}
static char *lskip(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}
static char *find_chars(char *s, const char *chars) {
    while (*s && !strchr(chars, *s)) s++;
    return s;
}

int ini_parse_stream(ini_reader reader, void *stream, ini_handler handler, void *user) {
    char line[200];
    char section[50] = "";
    char prev[50] = "";
    int lineno = 0, error = 0;
    while (reader(line, 200, stream)) {
        char *start = line;
        lineno++;
        if (lineno == 1 && (uint8_t)start[0] == 0xef && (uint8_t)start[1] == 0xbb && (uint8_t)start[2] == 0xbf)
            start += 3;
        start = lskip(rstrip(start));
        if (strchr("#", *start)) {
        } else if (*start == '[') {
            char *end = find_chars(start + 1, "]");
            if (*end == ']') {
                *end = 0;
                strncpy0(section, start + 1, 50);
                prev[0] = 0;
            } else if (!error) error = lineno;
        } else if (*start) {
            char *end = find_chars(start, "=:");
            if (*end == '=' || *end == ':') {
                *end = 0;
                char *name = rstrip(start);
                char *value = lskip(end + 1);
                rstrip(value);
                strncpy0(prev, name, 50);
                int r = handler(user, section, name, value);
                if (r == 0 && error == 0) error = lineno;
            } else if (!error) error = lineno;
        }
    }
    return error;
}

int ini_parse_string(const char *str, ini_handler handler, void *user) {
    struct str_stream s = { str, strlen(str) };
    return ini_parse_stream(ini_str_reader, &s, handler, user);
}

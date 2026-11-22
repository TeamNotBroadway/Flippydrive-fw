#pragma once
// FlippyDrive wire definitions shared by main-ode and the bootloader. Names are ours.
#include <stdint.h>

// DI command byte (top byte of the first command word).
enum di_cmd {
    DI_CMD_INQUIRY = 0x12,
    DI_CMD_READ = 0xa8,
    DI_CMD_SEEK = 0xab,
    DI_CMD_REQUEST_ERROR = 0xe0,
    DI_CMD_AUDIO_STREAM = 0xe1,
    DI_CMD_AUDIO_STATUS = 0xe2,
    DI_CMD_STOP_MOTOR = 0xe3,
    DI_CMD_AUDIO_CONFIG = 0xe4,
    DI_CMD_WKF = 0xdf,
    DI_CMD_BYPASS = 0xdc,
    DI_CMD_DOL_LIST = 0xb3,
    DI_CMD_FD_STATUS = 0xb4,
    DI_CMD_IPC = 0xb5,
};

// 0xB5 subcommand: low 5 bits of the command word; bits 16..23 are the handle.
enum ipc_sub {
    IPC_STATUS = 0,
    IPC_MOUNT = 1,
    IPC_RELOAD_CONFIG = 2,
    IPC_NET_STATUS = 3,
    IPC_RESET = 5,
    IPC_FS_INFO = 6,
    IPC_MKDIR = 7,
    IPC_READ = 8,
    IPC_WRITE = 9,
    IPC_OPEN = 10,
    IPC_CLOSE = 11,
    IPC_UNLINK = 14,
    IPC_CONFIG_SET = 16,
    IPC_READDIR = 15,
    IPC_FLASH_OPEN = 17,
    IPC_FLASH_UNLINK = 18,
    IPC_RENAME = 19,
    IPC_PRESENCE = 20,
    IPC_REMOTE = 21,
    IPC_NET_DISABLE = 24,
};
#define IPC_SUB(a) ((a) & 31)
#define IPC_HANDLE(a) (((a) >> 16) & 0xff)
#define IPC_MOUNT_HANDLE2(a) (((a) >> 8) & 0xff)

#define FD_HANDLE_FLASH 64
#define FD_MAX_HANDLES 31
#define FD_PATH_MAX 256

#define FD_OPEN_NO_CACHE 1
#define FD_OPEN_NO_FASTSEEK 2
#define FD_OPEN_NO_DVDSPEED 4
#define FD_OPEN_WRITE 8

// Second and third command words.
#define IPC_RESET_MAGIC 0xaa55f641u
#define INQUIRY_BOOT_MAGIC 0xabadbeefu
#define INQUIRY_UPDATE 0xdabfed69u
#define INQUIRY_BOOT 0xdecaf420u
#define INQUIRY_ESP_READY 0xcafe6969u
#define BYPASS_EXIT_B 0xe3f72babu
#define BYPASS_EXIT_C 0x72648977u

// DI error status words.
#define DI_ERR_MOTOR_STOPPED 0x20400
#define DI_ERR_READ 0x31100
#define DI_ERR_BAD_HANDLE 0x52400

// ESP request header: first word is the magic, then command and handle.
#define ESP_REQ_MAGIC IPC_RESET_MAGIC

// ESP request header (32 bytes). offset/len are the read/write window of IPC_READ and IPC_WRITE.
struct __attribute__((packed)) fd_req_hdr {
    uint32_t magic;
    uint8_t cmd;
    uint8_t handle;
    uint16_t pad;
    uint32_t offset;
    uint32_t len;
    uint32_t w[4];
};

// Path record the GC sends with OPEN, UNLINK, MKDIR, RENAME (288 bytes). type: 0 file, 1 dir.
struct __attribute__((packed)) fd_path {
    char name[FD_PATH_MAX];
    uint8_t type;
    uint8_t flags;
    uint8_t pad[30];
};

struct __attribute__((packed)) fd_path_req {
    struct fd_req_hdr hdr;
    struct fd_path path;
};

struct __attribute__((packed)) fd_rename_req {
    struct fd_req_hdr hdr;
    struct fd_path from;
    struct fd_path to;
};

struct __attribute__((packed)) fd_file_reply {
    uint32_t result;
    uint64_t size;
    uint8_t handle;
    uint8_t flags;
    uint8_t pad[18];
};

struct __attribute__((packed)) fd_filestat {
    char name[256];
    uint8_t type;
    uint8_t flags;
    uint64_t size;
    uint32_t date;
    uint32_t time;
    uint32_t result;
    uint8_t attributes;
    uint8_t pad[9];
};

struct __attribute__((packed)) fd_version {
    uint32_t flags;
    uint8_t major;
    uint8_t minor;
    uint16_t build : 15;
    uint16_t dirty : 1;
    uint8_t pad[16];
};

struct __attribute__((packed)) fd_fsinfo {
    uint32_t status;
    uint64_t free;
    uint64_t total;
};

// Boot status block read by 0xB4 (256 bytes).
struct fd_boot_status {
    char magic[4];
    uint8_t show_video;
    uint8_t show_progress_bar;
    uint16_t progress;
    char text[64];
    char subtext[64];
    uint8_t pad[120];
} __attribute__((aligned(4)));

// Persistent config sent to the ESP (256 bytes); config.ini [network] fills it.
#define IPC_CFG_IS_DEFAULT 0x80
#define IPC_CFG_SERVER_SET 0x01
struct __attribute__((packed)) ipc_cfg_t {
    uint8_t pad0[0x20];  // overwritten by the request header on send
    uint8_t flags;
    char ssid[0x21];
    char password[0x81];
    char server[0x3d];
};

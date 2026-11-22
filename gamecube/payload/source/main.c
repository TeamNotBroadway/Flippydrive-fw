// The payload's menu: firmware status and update screens, and DOL loading.
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <gccore.h>

#include "cubeboot/flippy_sync.h"
#include "cubeboot/sidestep.h"
#include <ogc/lwp_watchdog.h>
#include "flippy_ext.h"
#include "gfx.h"
#include "video.h"

#ifdef NONMATCHING
#define ASM_REF
#else
// Used only from the asm bodies, which address it by number: GCC must still emit it, and
// gc_appasm.py gives main.inc a reference to it for --gc-sections.
#define ASM_REF __attribute__((used))
// A function whose body is the original's code (gc_appasm.py), until its C matches. noipa:
// GCC must neither inline it nor learn from its body (it ends in __builtin_unreachable).
#define ASM_BODY ASM_REF __attribute__((noipa))
#endif

// DI command 0xB3's reply: up to 8 boot entry names.
typedef struct {
    u32 unknown;
    char names[8][40];
} __attribute__((aligned(32))) boot_entries_t;

typedef struct {
    char text[256];
    void (*action)(void);
} menu_item_t;

// Data in the reverse of its address order, which is the order GCC emits it. The small
// globals go to .sbss; the statics, small or not, go to .bss.
dvd_info_t *drive_info;
u32 state;
void *dol_buf;
u8 boot_requested;   // START pressed while the firmware waits (progress 0xffff)
u8 video_ready;
u8 skip_stub;
u8 ignore_config;    // set when the menu was entered: config bits are not applied
ASM_REF u32 dol_fd;

ASM_REF static menu_item_t main_items[5];
ASM_REF static menu_item_t menu_items[9];
static flippydrive_net_status_t net_status;
static GCN_ALIGNED(firmware_status_blob_t) fw_status;
ASM_REF static boot_entries_t boot_entries;
static texture_t logo;
static texture_t font;
ASM_REF static GCN_ALIGNED(u8) dol_hdr[256];
static u64 start_time;      // ms
ASM_REF static menu_item_t *selected_item;
static char dprintf_buf[256];
static GCN_ALIGNED(u32) config[8];   // DI reload-config reply, 32 bytes; word 0 is the bits

extern u8 font_tpl[], logo_tpl[];

// Defined in gfx.c; const here, so the loads can be hoisted out of the menu loops.
extern const int text_margin;
extern const int char_width;

static void boot_entry(void);
int dvd_cover_status(void);   // cubeboot/gc_dvd.c
int load_dol(DOLHEADER *hdr, int from_flash, u32 fd);
void status_screen(const char *flash_path);
void gfx_init(void);

// "vMAJOR.MINOR.BUILD", with "-dirtyboi" for a build from a modified tree.
static int format_version(char *buf) {
    return snprintf(buf, 20, "v%u.%u.%u%s", drive_info->fw_ver.major, drive_info->fw_ver.minor,
                    drive_info->fw_ver.build >> 1, (drive_info->fw_ver.build & 1) ? "-dirtyboi" : "");
}

void bypass(void) {
    puts("Bypass mode");
    dvd_custom_bypass();
    exit(1);
}

#ifdef NONMATCHING
void boot_menu(void) {
    dvd_bootloader_status_ext(&boot_entries);

    int n = 0;
    for (int i = 0; i < 8; i++) {
        if (boot_entries.names[i][0] == 0)
            continue;
        strncpy(menu_items[n].text, boot_entries.names[i], 255);
        menu_items[n].action = boot_entry;
        n++;
    }
    strcpy(menu_items[n].text, "  Back");

    int count = n + 1;
    int sel = 0;
    bool armed = false;
    while (1) {
        PAD_ScanPads();
        u32 down = PAD_ButtonsDown(0);
        set_clear_color(255, 255, 255);
        frame_begin();
        draw_texture(40, 30, logo);

        char version[20] = { 0 };
        int len = format_version(version);
        int width = len * char_width;

        char line[40];
        for (int i = 0; i <= n; i++) {
            memset(line, 0, sizeof(line));
            if (i < n) {
                int name_len = strlen(boot_entries.names[i]);
                if (name_len >= 5 && name_len < 40) {
                    strncpy(line, menu_items[i].text, sizeof(line));
                    line[name_len - 4] = 0;
                }
            } else {
                strcpy(line, menu_items[i].text);
            }
            if (sel == i)
                draw_textf(140, 120 + i * 30, font, 0xff, 1.4f, "> %s", line);
            else
                draw_textf(140, 120 + i * 30, font, 0xff, 1.4f, "  %s", line);
        }
        draw_textf(rmode->fbWidth - (width + 30), rmode->xfbHeight - (text_margin + 24), font,
                   0xff, 0.8f, version);

        if (down & PAD_BUTTON_UP)
            sel = (sel - 1 + count) % count;
        if (down & PAD_BUTTON_DOWN)
            sel = (sel + 1) % count;
        if (down & PAD_BUTTON_A) {
            if (armed) {
                if (sel == n)
                    return;
                selected_item = &menu_items[sel];
                menu_items[sel].action();
                armed = true;
            }
        } else {
            armed = true;
        }
        if (down & PAD_BUTTON_B)
            break;
        frame_end();
    }
    frame_end();
}
#else
ASM_BODY void boot_menu(void) {
    asm(".include \"boot_menu.inc\"");
    __builtin_unreachable();
}
#endif


int log_printf(const char *fmt, ...) {
    u32 level = IRQ_Disable();
    va_list args;
    va_start(args, fmt);
    int len = vsprintf(dprintf_buf, fmt, args);
    va_end(args);
    write(2, dprintf_buf, len);
    IRQ_Restore(level);
    return len;
}

void draw_progress(void) {
    u16 progress = fw_status.current_progress;
    if (progress > 1000)
        progress = 1000;
    double bar = (double)progress / 1000 * 340;
    if (fw_status.show_progress_bar)
        draw_textf(150, 260, font, 0xff, 0.8f, "(%3d/100) %s", (u16)(progress / 10),
                   fw_status.status_sub);
    else
        draw_textf(150, 260, font, 0xff, 0.8f, "          %s", fw_status.status_sub);
    draw_rect(149, 224, 340, 30, 0xaaaaaaff);
    draw_rect(149, 224, bar, 30, 0xff);
}

// Loads /stub.bin from flash to 0x80001800, where DOLtoARAM's loader returns to.
void load_stub(void) {
    log_printf("Loading stub...\n");
    GCN_ALIGNED(file_status_t) status;
    dvd_custom_open_flash("/stub.bin", 0, 0);
    dvd_custom_status(&status);
    if (status.result != 0) {
        log_printf("Failed to open stub\n");
        return;
    }
    u32 size = (__builtin_bswap64(status.fsize) + 31) & ~31;
    dvd_read((void *)0x80001800, size, 0, status.fd);
    DCInvalidateRange((void *)0x80001800, size);
    ICInvalidateRange((void *)0x80001800, size);
    dvd_custom_close(status.fd);
}

void chainload(void) {
    if (video_ready) {
        VIDEO_SetBlack(TRUE);
        VIDEO_Flush();
        VIDEO_WaitVSync();
    }
    log_printf("Chainloading DOL...\n");
    if (skip_stub) {
        *(u32 *)0x80001800 = 0x50415353;   // "PASS"
        DOLtoARAM(dol_buf, 0, NULL);
    } else {
        load_stub();
        DOLtoARAM(dol_buf, 0, NULL);
    }
}

static int is_fnet(DOLHEADER *hdr) {
    for (int i = 0; i < 7; i++)
        if (hdr->unused[i] == 0x464e4554)   // "FNET"
            return 1;
    return 0;
}

int load_dol(DOLHEADER *hdr, int from_flash, u32 fd) {
    u32 size = DOLMinMax(hdr);
    log_printf("DOL size = %d\n", size);
    size = (size + 31) & ~31;
    dol_buf = memalign(32, size);
    if (!dol_buf)
        return 2;
    dvd_read(dol_buf, size, 0, fd);
    log_printf("booting...\n");
    if (!(net_status.net_flags & 0x80)) {
        log_printf("network disabled\n");
        chainload();
        return 1;
    }
    log_printf("network enabled\n");
    if (is_fnet(hdr)) {
        log_printf("enhanced DOL\n");
        chainload();
    } else {
        log_printf("normal DOL\n");
        chainload();
    }
    return 1;
}


static u64 now_ms(void) {
    return gettime() / 40500;
}

static int open_dol(u32 fd, int from_flash) {
    dol_fd = fd;
    dvd_set_default_fd(dol_fd);
    dvd_read(dol_hdr, 256, 0, dol_fd);
    return load_dol((DOLHEADER *)dol_hdr, from_flash, dol_fd);
}

#ifdef NONMATCHING
int boot_main_app(const char *flash_path) {
    GCN_ALIGNED(file_status_t) status;
    char path[256];

    log_printf("STATE_MAIN_APP\n");
    memset(path, 0, sizeof(path));
    if (flash_path) {
        log_printf("reading Flash %s\n", flash_path);
        dvd_custom_open_flash(flash_path, 0, 4);
        dvd_custom_status(&status);
        if (status.result == 0 && status.fd != 0)
            return open_dol(status.fd, 1);
        snprintf(fw_status.status_text, 64, "Could not load requested %s", path);
        strcpy(fw_status.status_sub, "Please reboot or update firmware");
        return 1;
    }
    strcpy(path, "/boot.dol");
    log_printf("reading SD %s\n", path);
    dvd_custom_open(path, 0, 4);
    dvd_custom_status(&status);
    if (status.result == 0 && status.fd != 0)
        return open_dol(status.fd, 0);
    log_printf("reading Flash /cubeboot.dol\n");
    dvd_custom_open_flash("/cubeboot.dol", 0, 4);
    dvd_custom_status(&status);
    if (status.result == 0 && status.fd != 0)
        return open_dol(status.fd, 1);
    snprintf(fw_status.status_text, 64, "No internal cubeboot.dol and %s on SD", path);
    strcpy(fw_status.status_sub, "Please provide a boot.dol or update firmware");
    return 2;
}
#else
ASM_BODY int boot_main_app(const char *flash_path) {
    asm(".include \"boot_main_app.inc\"");
    __builtin_unreachable();
}
#endif

void step_state(const char *flash_path) {
    switch (state) {
    case 0:
        if (boot_requested) {
            dvd_bootloader_boot();
            start_time = now_ms();
            state = 1;
        }
        break;
    case 1:
        if (now_ms() - start_time <= 6000) {
            if (dvd_cover_status() == 0)
                state = 2;
        } else {
            fw_status.show_video = 1;
            strcpy(fw_status.status_text, "Update software or do USB rescue");
            fw_status.status_sub[0] = 0;
            state = 6;
        }
        break;
    case 2:
        if (boot_requested) {
            dvd_custom_unlink("/update.fpkg");
            dvd_custom_unlink_flash("/boot.dol");
        }
        if (!skip_stub) {
            dvd_custom_reload_config(config);
            if (!ignore_config) {
                if (config[0] & 1)
                    bypass();
                if (config[0] & 2) {
                    puts("Enhanced bypass mode");
                    skip_stub = 1;
                }
            }
            log_printf("config bits %08x\n", config[0]);
        }
        dvd_custom_net_status(&net_status);
        log_printf("got net flag = %02x\n", net_status.net_flags);
        if (net_status.net_flags & 0x80) {
            fw_status.show_video = 1;
            state = 3;
        } else {
            state = 5;
        }
        break;
    case 3: {
        dvd_custom_net_status(&net_status);
        u8 s = net_status.net_status & 7;
        if (net_status.net_status & 8) {
            switch (s) {
            case 1: strcpy(fw_status.status_text, "Connecting to network failed!"); break;
            case 2: strcpy(fw_status.status_text, "Setting IP address failed!"); break;
            case 3: strcpy(fw_status.status_text, "Connecting to server failed!"); break;
            }
            log_printf("Network failed: %02x\n", net_status.net_status);
            dvd_custom_cmd18();
            strcpy(fw_status.status_sub, "is_default set but network failed, check server/settings");
            state = 4;
        } else {
            switch (s) {
            case 0: strcpy(fw_status.status_text, "Connecting to network..."); return;
            case 1: strcpy(fw_status.status_text, "Setting IP address..."); break;
            case 2: strcpy(fw_status.status_text, "Connecting to server..."); break;
            }
            if (net_status.net_status == 3) {
                strcpy(fw_status.status_text, "Loading DOL...");
                state = 5;
            }
        }
        break;
    }
    case 5:
        boot_main_app(flash_path);
        state = 6;
        return;
    case 7:
        if (dvd_cover_status() == 0 && start_time != 0) {
            dvd_custom_unlink_flash("/factory");
            start_time = 0;
        }
        break;
    }
}

void gfx_init(void) {
    TPLFile tpl;
    video_init();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();
    TPL_OpenTPLFromMemory(&tpl, logo_tpl, 0x5f40);
    logo = texture_load(&tpl, 0);
    TPLFile tpl2;
    TPL_OpenTPLFromMemory(&tpl2, font_tpl, 0xf040);
    font = texture_load(&tpl2, 0);
    video_ready = 1;
}

void status_screen(const char *flash_path) {
    char version[20] = { 0 };
    int len = format_version(version);
    int width = len * char_width;
    u32 frame = 0;
    while (1) {
        PAD_ScanPads();
        u32 down = PAD_ButtonsDown(0);
        if (frame % 6 == 0 && state == 0)
            dvd_bootloader_status(&fw_status);
        step_state(flash_path);
        if (fw_status.show_video && !video_ready)
            gfx_init();
        u32 st = state;
        if (fw_status.current_progress == 0xffff) {
            if (down & PAD_BUTTON_START) {
                boot_requested = 1;
                if (st == 7) {
                    load_stub();
                    exit(0);
                }
            }
        } else if (st == 7 && (down & PAD_BUTTON_START)) {
            load_stub();
            exit(0);
        }
        if (!video_ready)
            continue;
        switch (st) {
        case 0:
            set_clear_color(255, 255, 255);
            frame_begin();
            draw_texture(40, 30, logo);
            draw_textf(150, 196, font, 0xff, 1.0f, "%s", fw_status.status_text);
            draw_progress();
            break;
        case 1:
            set_clear_color(255, 255, 255);
            frame_begin();
            draw_texture(40, 30, logo);
            draw_textf(150, 196, font, 0xff, 1.0f, "FlippyDrive reboot...");
            break;
        case 2:
            set_clear_color(255, 255, 255);
            frame_begin();
            draw_texture(40, 30, logo);
            draw_textf(150, 196, font, 0xff, 1.0f, "Firmware is booting... (%5ums)",
                       (u32)(now_ms() - start_time));
            break;
        case 3:
            set_clear_color(90, 201, 244);
            frame_begin();
            draw_texture(40, 30, logo);
            draw_textf(150, 196, font, 0xff, 1.0f, "NETWORK LOADING...");
            draw_textf(150, 216, font, 0xff, 1.0f, fw_status.status_text);
            break;
        case 4:
            set_clear_color(235, 130, 130);
            frame_begin();
            draw_texture(40, 30, logo);
            draw_textf(150, 196, font, 0xff, 1.0f, "Network boot failed");
            draw_textf(150, 216, font, 0xff, 1.0f, fw_status.status_text);
            draw_textf(150, 236, font, 0xff, 1.0f, fw_status.status_sub);
            draw_textf(150, 256, font, 0xff, 1.0f, "Press A to continue without networking.");
            if (down & PAD_BUTTON_A)
                state = 5;
            break;
        case 6:
            set_clear_color(235, 130, 130);
            frame_begin();
            draw_texture(40, 30, logo);
            draw_textf(150, 196, font, 0xff, 1.0f, "Main app failed to boot");
            draw_textf(150, 216, font, 0xff, 1.0f, fw_status.status_text);
            draw_textf(150, 236, font, 0xff, 1.0f, fw_status.status_sub);
            break;
        case 7:
            set_clear_color(51, 204, 51);
            frame_begin();
            draw_texture(40, 30, logo);
            draw_textf(150, 196, font, 0xff, 1.0f, "Recovery complete!");
            draw_textf(150, 216, font, 0xff, 1.0f, "Press START to continue");
            break;
        default:
            set_clear_color(0, 0, 0);
            frame_begin();
            break;
        }
        draw_textf(rmode->fbWidth - (width + 30), rmode->xfbHeight - (text_margin + 24), font,
                   0xff, 0.8f, version);
        frame_end();
        frame++;
    }
}

void update_firmware(void) {
    dvd_bootloader_update();
    if (!video_ready)
        gfx_init();
    status_screen(NULL);
}

void start_firmware(const char *flash_path) {
    dvd_bootloader_noupdate();
    state = 1;
    start_time = now_ms();
    status_screen(flash_path);
}

ASM_REF static void boot_entry(void) {
    char path[41];
    memset(path, 0, sizeof(path));
    snprintf(path, sizeof(path), "/%s", selected_item->text);
    start_firmware(path);
}

ASM_REF void exit_menu(void) {
    start_firmware(NULL);
}

void enhanced_bypass(void) {
    puts("Enhanced bypass mode");
    skip_stub = 1;
    start_firmware("/cubeboot.dol");
}

void recovery(void) {
    dvd_bootloader_noupdate();
    gfx_init();
    state = 7;
    start_time = now_ms();
    status_screen(NULL);
}

#ifdef NONMATCHING
void main_menu(void) {
    if (!video_ready)
        gfx_init();
    int sel = 0;
    while (1) {
        PAD_ScanPads();
        u32 down = PAD_ButtonsDown(0);
        set_clear_color(255, 255, 255);
        frame_begin();
        draw_texture(40, 30, logo);
        char version[20] = { 0 };
        int len = format_version(version);
        int width = len * char_width;
        for (int i = 0; i < 5; i++) {
            int y = 120 + i * 30;
            if (i == sel)
                draw_textf(140, y, font, 0xff, 1.4f, "> %s", main_items[i].text);
            else
                draw_textf(140, y, font, 0xff, 1.4f, "  %s", main_items[i].text);
        }
        draw_textf(rmode->fbWidth - (width + 30), rmode->xfbHeight - (text_margin + 24), font,
                   0xff, 0.8f, version);
        if (down & PAD_BUTTON_UP)
            sel = (sel + 4) % 5;
        if (down & PAD_BUTTON_DOWN)
            sel = (sel + 1) % 5;
        if (down & PAD_BUTTON_A)
            main_items[sel].action();
        frame_end();
    }
}
#else
ASM_BODY void main_menu(void) {
    asm(".include \"main_menu.inc\"");
    __builtin_unreachable();
}
#endif

#ifdef NONMATCHING
int main(void) {
    PAD_Init();
    PAD_ScanPads();
    drive_info = dvd_inquiry();
    if (drive_info->fd_flags & 1)
        update_firmware();
    if (drive_info->fd_flags & 2)
        recovery();
    u64 t0 = now_ms();
    strncpy(main_items[0].text, "Update/Reinstall Firmware", 255);
    main_items[0].action = update_firmware;
    strncpy(main_items[1].text, "Boot Onboard DOL", 255);
    main_items[1].action = boot_menu;
    strncpy(main_items[2].text, "Boot Disc Drive/Pure Bypass", 255);
    main_items[2].action = bypass;
    strncpy(main_items[3].text, "Boot Disc Drive/Enhanced Bypass", 255);
    main_items[3].action = enhanced_bypass;
    strncpy(main_items[4].text, "Exit", 255);
    main_items[4].action = exit_menu;
    while (now_ms() - t0 <= 75) {
        PAD_ScanPads();
        u32 down = PAD_ButtonsDown(0);
        if (down & PAD_BUTTON_X) {
            ignore_config = 1;
            main_menu();
        } else if (down & (PAD_BUTTON_LEFT | PAD_TRIGGER_L))
            bypass();
        else if (down & (PAD_BUTTON_RIGHT | PAD_TRIGGER_R))
            enhanced_bypass();
    }
    start_firmware(NULL);
}
#else
// Not named main in C: GCC would add the call to __eabi that the body already makes.
int main_body(void) __asm__("main");
ASM_BODY int main_body(void) {
    asm(".include \"main.inc\"");
    __builtin_unreachable();
}
#endif

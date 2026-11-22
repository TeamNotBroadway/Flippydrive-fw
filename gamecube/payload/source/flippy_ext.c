// FlippyDrive DI commands the payload uses that cubeboot's flippy_sync.c lacks. Written to
// match the binary in flippy_sync.c's style; command numbers from the binary, names ours
// (IPC numbers 0x02 and 0x03 as in later cubeboot ipc.h).
#include <gctypes.h>
#include <ogc/cache.h>

#include "flippy_ext.h"

#define DI_SR      0
#define DI_SR_BRKINTMASK (1 << 5)
#define DI_SR_TCINTMASK  (1 << 3)
#define DI_SR_DEINT      (1 << 2)
#define DI_SR_DEINTMASK  (1 << 1)
#define DI_CVR     1
#define DI_CMDBUF0 2
#define DI_CMDBUF1 3
#define DI_CMDBUF2 4
#define DI_MAR     5
#define DI_LENGTH  6
#define DI_CR      7
#define DI_CR_DMA    (1 << 1)
#define DI_CR_TSTART (1 << 0)

#define DVD_FLIPPY_FILEAPI_BASE 0xB5000000
#define IPC_RELOAD_CONFIG 0x02
#define IPC_NET_STATUS    0x03
#define IPC_CMD_18        0x18

static vu32* const _di_regs = (vu32*)0xCC006000;

void dvd_custom_cmd18(void) {
    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0;

    _di_regs[DI_CMDBUF0] = DVD_FLIPPY_FILEAPI_BASE | IPC_CMD_18;
    _di_regs[DI_CMDBUF1] = 0;
    _di_regs[DI_CMDBUF2] = 0;

    _di_regs[DI_MAR] = 0;
    _di_regs[DI_LENGTH] = 0;
    _di_regs[DI_CR] = DI_CR_TSTART;

    while (_di_regs[DI_CR] & DI_CR_TSTART);
}

int dvd_custom_reload_config(void *dst) {
    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0;

    _di_regs[DI_CMDBUF0] = DVD_FLIPPY_FILEAPI_BASE | IPC_RELOAD_CONFIG;
    _di_regs[DI_CMDBUF1] = 0;
    _di_regs[DI_CMDBUF2] = 0;

    _di_regs[DI_MAR] = (u32)dst & 0x1FFFFFFF;
    _di_regs[DI_LENGTH] = 32;
    _di_regs[DI_CR] = (DI_CR_DMA | DI_CR_TSTART);

    while (_di_regs[DI_CR] & DI_CR_TSTART);

    DCInvalidateRange(dst, 32);

    if (_di_regs[DI_SR] & DI_SR_DEINT) {
        return 1;
    }
    return 0;
}

int dvd_custom_net_status(flippydrive_net_status_t *dst) {
    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0;

    _di_regs[DI_CMDBUF0] = DVD_FLIPPY_FILEAPI_BASE | IPC_NET_STATUS;
    _di_regs[DI_CMDBUF1] = 0;
    _di_regs[DI_CMDBUF2] = 0;

    _di_regs[DI_MAR] = (u32)dst & 0x1FFFFFFF;
    _di_regs[DI_LENGTH] = sizeof(flippydrive_net_status_t);
    _di_regs[DI_CR] = (DI_CR_DMA | DI_CR_TSTART);

    while (_di_regs[DI_CR] & DI_CR_TSTART);

    DCInvalidateRange(dst, sizeof(flippydrive_net_status_t));

    if (_di_regs[DI_SR] & DI_SR_DEINT) {
        return 1;
    }
    return 0;
}

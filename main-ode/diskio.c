// FatFs disk glue: a FAT volume in flash, 0xd00 sectors of 4 KiB from 3 MiB in.
// mount_fat is our name.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "ff.h"
#include "diskio.h"

#define FAT_SECTOR_SIZE 0x1000
#define FAT_SECTOR_COUNT 0xd00
#define FAT_FLASH_OFFSET 0x300000

static FATFS fs;

DSTATUS disk_status(BYTE pdrv)
{
    return pdrv != 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return pdrv != 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    DRESULT res = RES_PARERR;
    if (pdrv == 0) {
        memcpy(buff, (const void *)(XIP_NOCACHE_NOALLOC_BASE + FAT_FLASH_OFFSET + sector * FAT_SECTOR_SIZE),
               count * FAT_SECTOR_SIZE);
        res = RES_OK;
    }
    return res;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    DRESULT res = RES_PARERR;
    if (pdrv == 0) {
        switch (cmd) {
        case CTRL_SYNC: res = RES_OK; break;
        case GET_SECTOR_COUNT: if (buff) { *(LBA_t *)buff = FAT_SECTOR_COUNT; res = RES_OK; } break;
        case GET_SECTOR_SIZE: if (buff) { *(WORD *)buff = FAT_SECTOR_SIZE; res = RES_OK; } break;
        case GET_BLOCK_SIZE: if (buff) { *(DWORD *)buff = 1; res = RES_OK; } break;
        }
    }
    return res;
}

int mount_fat(void)
{
    puts("\nMounting internal FAT");
    FRESULT res = f_mount(&fs, "", 1);
    if (res != FR_OK) {
        printf("FAT filesystem failed to initialize, formatting (%d)\n", res);
        res = f_mkfs("", 0, 0, 0x4000);
        if (res != FR_OK) {
            printf("Unable to create FAT filesystem, aborting (%d)\n", res);
            for (;;)
                ;
        }
        res = f_mount(&fs, "", 1);
        if (res != FR_OK) {
            printf("FAT filesystem failed to initialize, aborting (%d)\n", res);
            for (;;)
                ;
        }
    }
    puts("Internal FAT Mounted");
    return 0;
}

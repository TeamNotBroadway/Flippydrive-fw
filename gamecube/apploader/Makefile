# GameCube apploader (disc 0x2440). Run in WSL.
# Sources: OffBroadway/gc-boot-tools 8d59fb2 (branch save-ipl-fastboot), ppc/apploader and
# ppc/common, built as its ppc/apploader/Makefile does, except that ppc/common/lib.c is
# compiled without -fno-builtin: the original's memset is GCC's loop-to-memset call.
DEVKITPRO ?= /mnt/c/Tools/devkitppc-20250727/root/opt/devkitpro
DEVKITPPC := $(DEVKITPRO)/devkitPPC
BUILD ?= $(HOME)/fd-build/gc-apploader

CROSS := $(DEVKITPPC)/bin/powerpc-eabi-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy

ENTRY := 0x81200000
DATE := 2023/07/23

SRCS := apploader.c crc32.c libc.c ipl.c addr.c tinyprintf.c usbgecko.c save.c hacks.c
OBJS := $(SRCS:%.c=$(BUILD)/%.o) $(BUILD)/lib.o $(BUILD)/misc.o

all: $(BUILD)/apploader.img

# Header: date (16 bytes), entry point, code size, trailer size, padding.
$(BUILD)/apploader.img: $(BUILD)/apploader.bin
	python3 -c 'import os,struct,sys; b=open(sys.argv[1],"rb").read(); \
	open(sys.argv[2],"wb").write(sys.argv[3].encode().ljust(16,b"\0") + \
	struct.pack(">III4x", int(sys.argv[4],16), len(b), 0) + b)' $< $@ $(DATE) $(ENTRY)

$(BUILD)/apploader.bin: $(BUILD)/apploader.elf
	$(OBJCOPY) --strip-all -O binary $< $@

$(BUILD)/apploader.elf: $(OBJS) ppc/apploader/apploader_ldscript.txt
	$(CC) -nostartfiles -nodefaultlibs -Wl,-Map,$(BUILD)/apploader.map -Wl,-Ttext=$(ENTRY) \
		-Wl,-e,al_start -Wl,--defsym,EXI=0xCC006800 -Wl,-T,ppc/apploader/apploader_ldscript.txt \
		$(OBJS) -o $@

$(BUILD)/%.o: ppc/apploader/%.c | $(BUILD)
	$(CC) -O2 -fno-builtin -fno-toplevel-reorder -c $< -o $@

$(BUILD)/lib.o: ppc/common/lib.c | $(BUILD)
	$(CC) -O2 -fno-toplevel-reorder -c $< -o $@

$(BUILD)/misc.o: ppc/common/misc.S | $(BUILD)
	$(CC) -c $< -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean

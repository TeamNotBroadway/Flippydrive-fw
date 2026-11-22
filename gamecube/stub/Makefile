# main.dol, the GameCube loader stub: gunzips payload.dol (stored as updater.dol) and boots it.
# Source: OffBroadway/cubeboot entry/ at 873893b; changes: base address 0x81100000
# (nosys.ld) and distinct tinf error codes (source/tinf). Run in WSL:
#   make -C gamecube/stub [PAYLOAD=...]
DEVKITPRO ?= /mnt/c/Tools/devkitppc-20250727/root/opt/devkitpro
LIBOGC2   ?= /mnt/c/Tools/libogc2
PAYLOAD   ?= $(HOME)/fd-build/gc-payload/payload.dol
BUILD     ?= $(HOME)/fd-build/gc-stub
# mtime of updater.dol recorded in the original gzip header
PAYLOAD_MTIME := 1749186142

CC      := $(DEVKITPRO)/devkitPPC/bin/powerpc-eabi-gcc
ELF2DOL := $(DEVKITPRO)/tools/bin/elf2dol
BIN2S   := $(DEVKITPRO)/tools/bin/bin2s
MACHDEP := -DGEKKO -mogc -mcpu=750 -meabi -mhard-float
CFLAGS  := -Os -Wall $(MACHDEP) -I$(CURDIR)/include -I$(CURDIR)/source -I$(BUILD) -I$(LIBOGC2)/gc

# Link order as in cubeboot's Makefile: data, C files by directory, then assembly.
CSRC := main.c usbgecko.c tinf/crc32.c tinf/tinfgzip.c tinf/tinflate.c tinyprintf/tinyprintf.c
SSRC := cache_asm.S exception_handler.S start.S system_asm.S
OBJS := $(BUILD)/cubeboot.gz.o $(addprefix $(BUILD)/,$(notdir $(CSRC:.c=.o) $(SSRC:.S=.o)))

all: $(BUILD)/main.dol

$(BUILD)/main.dol: $(BUILD)/main.elf
	$(ELF2DOL) $< $@

$(BUILD)/main.elf: $(OBJS) nosys.ld
	$(CC) $(MACHDEP) -Wl,-Map,$(BUILD)/main.map --specs=nosys.specs -T nosys.ld $(OBJS) -o $@

$(BUILD)/updater.dol: $(PAYLOAD)
	@mkdir -p $(BUILD)
	cp $< $@
	touch -d @$(PAYLOAD_MTIME) $@

# gzip stores the file name and mtime; -6 is gzip's default level.
$(BUILD)/cubeboot.gz: $(BUILD)/updater.dol
	cd $(BUILD) && gzip -6 -c updater.dol > cubeboot.gz

$(BUILD)/cubeboot.gz.o $(BUILD)/cubeboot_gz.h: $(BUILD)/cubeboot.gz
	cd $(BUILD) && $(BIN2S) -a 32 -H cubeboot_gz.h cubeboot.gz > cubeboot_gz.s
	$(CC) -x assembler-with-cpp $(MACHDEP) -c $(BUILD)/cubeboot_gz.s -o $(BUILD)/cubeboot.gz.o

vpath %.c source source/tinf source/tinyprintf
vpath %.S source

$(BUILD)/%.o: %.c $(BUILD)/cubeboot_gz.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(BUILD)
	$(CC) -x assembler-with-cpp $(MACHDEP) -I$(CURDIR)/source -I$(LIBOGC2)/gc -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean

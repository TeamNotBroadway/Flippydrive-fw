# GameCube payload (payload.dol, gzipped into the stub). Run in WSL.
# Links against libogc2 70f80d8 built by tools/match_gc.sh.
DEVKITPRO ?= /mnt/c/Tools/devkitppc-20250727/root/opt/devkitpro
DEVKITPPC := $(DEVKITPRO)/devkitPPC
OGC ?= $(HOME)/fd-build/libogc2-70f80d8
BUILD ?= $(HOME)/fd-build/gc-payload

CC := $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL := $(DEVKITPRO)/tools/bin/elf2dol
MACHDEP := -DGEKKO -mogc -mcpu=750 -meabi -mhard-float
CFLAGS := $(MACHDEP) -O2 -ffunction-sections -fdata-sections -I$(OGC)/gc -I$(OGC)/gc/ogc
# make NONMATCHING=1 BUILD=...: build every function from its C. The result runs but is not
# byte-identical (docs/HACKS.md).
ifdef NONMATCHING
CFLAGS += -DNONMATCHING
endif

PAYLOAD ?= $(CURDIR)/../../original/extracted/gc/payload.dol

# Link order is address order. app0.o holds the data not yet in C; the *.inc files are the
# bodies main.c includes for the functions whose C does not yet match (NONMATCHING).
STANDIN := $(BUILD)/app0.s $(BUILD)/boot_menu.inc $(BUILD)/boot_main_app.inc \
	$(BUILD)/main_menu.inc $(BUILD)/main.inc
OBJS := $(BUILD)/textures.o $(BUILD)/app0.o $(BUILD)/gfx.o $(BUILD)/flippy_sync.o $(BUILD)/flippy_ext.o \
	$(BUILD)/gc_dvd.o $(BUILD)/video.o $(BUILD)/main.o $(BUILD)/textf.o $(BUILD)/sidestep.o \
	$(BUILD)/ssaram.o

$(BUILD)/payload.dol: $(BUILD)/payload.elf
	$(ELF2DOL) $< $@

$(BUILD)/payload.elf: $(OBJS) $(OGC)/lib/cube/libogc.a
	$(CC) $(MACHDEP) -Wl,-Map,$(BUILD)/payload.map $(OBJS) -L$(OGC)/lib/cube -logc -lm -o $@

# Stand-ins until the application is fully decompiled (see tools/gc_appasm.py).
$(STANDIN) &: ../../tools/gc_appasm.py ../../syms/gc-payload.lib.tsv $(PAYLOAD) | $(BUILD)
	python3 -I ../../tools/gc_appasm.py $(PAYLOAD) ../../syms/gc-payload.lib.tsv $(BUILD)

$(BUILD)/%.o: $(BUILD)/%.s $(PAYLOAD)
	$(CC) $(MACHDEP) -Wa,-I$(dir $(PAYLOAD)) -c $< -o $@

$(BUILD)/textures.o: source/textures.s $(dir $(PAYLOAD))font.tpl $(dir $(PAYLOAD))logo.tpl | $(BUILD)
	$(CC) $(MACHDEP) -Wa,-I$(dir $(PAYLOAD)) -c $< -o $@

$(BUILD)/main.o: source/main.c source/*.h source/cubeboot/*.h $(STANDIN)
	$(CC) $(CFLAGS) -Wa,-I$(BUILD) -c $< -o $@

$(BUILD)/%.o: source/%.c source/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# cubeboot (OffBroadway), GPLv2; see source/cubeboot/README.md.
$(BUILD)/%.o: source/cubeboot/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: clean

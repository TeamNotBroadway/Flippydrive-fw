# The GameCube disc image the bootloader serves from flash (bootloader/flash_blob.S). Run in
# WSL. create_iso_header writes the disc's first 0x8000 bytes (header, apploader, FST,
# banner); the stub main.dol follows.
#   make -C gamecube/disc [APPLOADER=...] [DOL=...]
BUILD ?= $(HOME)/fd-build/gc-disc
APPLOADER ?= $(HOME)/fd-build/gc-apploader/apploader.bin
DOL ?= $(HOME)/fd-build/gc-stub/main.dol

all: $(BUILD)/disc.iso

$(BUILD)/disc.iso: $(BUILD)/disc_header.iso $(DOL)
	cat $^ > $@

$(BUILD)/disc_header.iso: $(BUILD)/create_iso_header $(APPLOADER) icons/opening.bnr
	cd $(BUILD) && ./create_iso_header $(APPLOADER) $(CURDIR)/icons/opening.bnr > /dev/null

$(BUILD)/create_iso_header: create_iso_header/create_iso_header.c include/gcm.h | $(BUILD)
	gcc -O2 -o $@ $<

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean

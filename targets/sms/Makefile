# RetroDJ - Sega Master System / Game Gear Target Makefile (SDCC + devkitSMS)
# GPLv3 - RetroDJ Project

CC      := sdcc
AS      := sdasz80
MAKEBIN := makebin

INCS := -I. \
        -Isrc \
        -Ismslib \
        -I../../ \
        -I../../core \
        -I../../drivers/sn76489 \
        -I../../backends \
        -I../../targets/genesis/src

# --peep-file applies the devkitSMS peephole rules, which teach SDCC the Z80
# VRAM-safe store sequences the SMS VDP requires.
CFLAGS := -mz80 --std-c99 -D__SMS__ --peep-file smslib/peep-rules.txt $(INCS)
ASFLAGS := -g

# `make GG=1` builds the Game Gear variant: it defines TARGET_GG, which makes
# SMSlib read the pad's START key and emit the GG region code in the ROM header.
ifeq ($(GG),1)
CFLAGS += -DTARGET_GG
endif

OUT_DIR := out
TARGET  := $(OUT_DIR)/rom.sms
IHX     := $(OUT_DIR)/rom.ihx

# Project sources.
C_SRCS := src/main.c \
          src/ui_sms.c \
          ../../drivers/sn76489/sn76489_rdj.c \
          ../../backends/backend_sms_hw.c

# Shared with the other targets. Compile into this target's own directory so
# the toolchains never clobber each other's object files.
SHARED_SRC := ../../targets/genesis/src/input_mapper.c
SHARED_REL := src/input_mapper.rel

# Vendored devkitSMS/SMSlib (public domain) subset.
SMSLIB_SRCS := smslib/SMSlib.c \
               smslib/SMSlib_autotext.c \
               smslib/SMSlib_load1bppTiles.c \
               smslib/SMSlib_textrenderer.c \
               smslib/SMSlib_string.c \
               smslib/SMSlib_VRAMmemset.c \
               smslib/SMSlib_sprite.c

C_RELS   := $(C_SRCS:.c=.rel) $(SHARED_REL) $(SMSLIB_SRCS:.c=.rel)
CRT0_REL := crt0_sms.rel

RELS := $(CRT0_REL) $(C_RELS)

.PHONY: all clean

all: $(TARGET)

# The crt0 must be assembled with -g: its forward references to symbols defined
# in the C objects are resolved at link time, and without -g sdasz80 rejects
# them as undefined.
$(CRT0_REL): crt0_sms.s
	$(AS) $(ASFLAGS) -o $@ $<

%.rel: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(SHARED_REL): $(SHARED_SRC)
	$(CC) $(CFLAGS) -c $< -o $@

$(IHX): $(RELS)
	mkdir -p $(OUT_DIR)
	$(CC) -mz80 --no-std-crt0 --data-loc 0xC000 -o $@ $(RELS)

# makebin -p pads the image to the next power of two (32 KB) so the ROM header
# lands at $7FF0 where the BIOS and emulators look for it.
$(TARGET): $(IHX)
	$(MAKEBIN) -p $(IHX) $@

clean:
	rm -f $(TARGET) $(IHX)
	rm -f $(CRT0_REL) $(C_RELS)
	rm -f src/*.rel src/*.lst src/*.asm src/*.sym
	rm -f smslib/*.rel smslib/*.lst smslib/*.asm smslib/*.sym
	rm -f ../../drivers/sn76489/*.rel ../../drivers/sn76489/*.lst ../../drivers/sn76489/*.asm
	rm -f ../../backends/*.rel ../../backends/*.lst ../../backends/*.asm

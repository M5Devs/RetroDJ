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

OUT_DIR := out

# `make GG=1` builds the Game Gear variant: it defines TARGET_GG, which makes
# SMSlib read the pad's START key and emit the GG region code in the ROM header.
# Each variant gets its own object directory, so switching between them never
# reuses the other's objects and no `clean` is needed.
VARIANT := sms
ifeq ($(GG),1)
VARIANT := gg
CFLAGS  += -DTARGET_GG
endif

OBJ_DIR := $(OUT_DIR)/$(VARIANT)
TARGET  := $(OUT_DIR)/rom.$(VARIANT)
IHX     := $(OBJ_DIR)/rom.ihx

# Sources live in several directories but share no basenames, so VPATH lets a
# single pattern rule collect them into the variant's object directory.
VPATH := src:smslib:../../drivers/sn76489:../../backends:../../targets/genesis/src

# Project sources.
C_SRCS := src/main.c \
          src/ui_sms.c \
          ../../drivers/sn76489/sn76489_rdj.c \
          ../../backends/backend_sms_hw.c \
          ../../targets/genesis/src/input_mapper.c

# Vendored devkitSMS/SMSlib (public domain) subset.
SMSLIB_SRCS := smslib/SMSlib.c \
               smslib/SMSlib_autotext.c \
               smslib/SMSlib_load1bppTiles.c \
               smslib/SMSlib_textrenderer.c \
               smslib/SMSlib_string.c \
               smslib/SMSlib_VRAMmemset.c \
               smslib/SMSlib_sprite.c

OBJS := $(addprefix $(OBJ_DIR)/,$(notdir $(C_SRCS:.c=.rel))) \
        $(addprefix $(OBJ_DIR)/,$(notdir $(SMSLIB_SRCS:.c=.rel)))

CRT0_REL := $(OBJ_DIR)/crt0_sms.rel

RELS := $(CRT0_REL) $(OBJS)

.PHONY: all clean

all: $(TARGET)

# The crt0 must be assembled with -g: its forward references to symbols defined
# in the C objects are resolved at link time, and without -g sdasz80 rejects
# them as undefined.
$(CRT0_REL): crt0_sms.s
	@mkdir -p $(OBJ_DIR)
	$(AS) $(ASFLAGS) -o $@ $<

$(OBJ_DIR)/%.rel: %.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(IHX): $(RELS)
	$(CC) -mz80 --no-std-crt0 --data-loc 0xC000 -o $@ $(RELS)

# makebin -p pads the image to the next power of two (32 KB) so the ROM header
# lands at $7FF0 where the BIOS and emulators look for it.
$(TARGET): $(IHX)
	$(MAKEBIN) -p $(IHX) $@

clean:
	rm -rf $(OUT_DIR)

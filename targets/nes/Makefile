# RetroDJ - NES Target Makefile (cc65)
# GPLv3 - RetroDJ Project

CC := cc65
AS := ca65
LD := ld65

INCS := -I. -Isrc -I../../ -I../../core -I../../drivers/nes_2a03 -I../../backends -I../../targets/genesis/src
CFLAGS := -t nes -O -Cl $(INCS) -D__NES__
ASFLAGS := -t nes

CFG := nrom_32k_vert.cfg
OUT_DIR := out
TARGET := $(OUT_DIR)/rom.nes

C_SRCS := src/main.c \
          src/ui_nes.c \
          ../../drivers/nes_2a03/2a03_rdj.c \
          ../../backends/backend_nes_hw.c

# Shared with the Game Boy target. Compile it into this target's own
# directory so the two toolchains (cc65 here, SDCC for GB) never clobber
# the same object file.
SHARED_SRC := ../../targets/genesis/src/input_mapper.c
SHARED_OBJ := src/input_mapper.o

ASM_SRCS := src/crt0.s

C_OBJS := $(C_SRCS:.c=.o) $(SHARED_OBJ)
ASM_OBJS := $(ASM_SRCS:.s=.o)

OBJS := $(ASM_OBJS) $(C_OBJS)

.SUFFIXES:
.SUFFIXES: .c .s .o

.PHONY: all clean

all: $(TARGET)

src/crt0.o: src/crt0.s
	$(AS) $(ASFLAGS) $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $< -o $(@:.o=.s)
	$(AS) $(ASFLAGS) $(@:.o=.s) -o $@

$(SHARED_OBJ): $(SHARED_SRC)
	$(CC) $(CFLAGS) $< -o $(SHARED_OBJ:.o=.s)
	$(AS) $(ASFLAGS) $(SHARED_OBJ:.o=.s) -o $@

$(TARGET): $(OBJS)
	mkdir -p $(OUT_DIR)
	$(LD) -C $(CFG) $(OBJS) nes.lib -o $(TARGET)

clean:
	rm -f $(TARGET) src/*.o ../../drivers/nes_2a03/*.o ../../backends/*.o
	rm -f src/main.s src/ui_nes.s ../../drivers/nes_2a03/2a03_rdj.s ../../backends/backend_nes_hw.s src/input_mapper.s

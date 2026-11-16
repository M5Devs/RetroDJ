/* GPLv3 - RetroDJ Project - SN76489 (Sega PSG) Sound Hardware Driver */

/*
 * Copyright (C) 2025 RetroDJ Project
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should receive a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#if defined(RDJ_GENESIS_HARDWARE) || defined(SGDK_GCC) || defined(__m68k__)
    #include <genesis.h>
#else
    #include <stdint.h>
    #include <stddef.h>
#endif

#include "core/rdj_driver.h"
#include "core/rdj_backend.h"

/* The SN76489 is a write-only device: every transfer is one byte to the PSG
 * data port. A latch byte (bit 7 set) selects a channel and whether the low
 * nibble is a tone frequency or a volume attenuation; a data byte (bit 7
 * clear) carries the six high bits of a tone frequency. */
#define PSG_LATCH       0x80
#define PSG_CH_SHIFT    5
#define PSG_TYPE_VOL    0x10

/* Channel 3 is the noise generator. Its latch byte is `1110 f nnn`: f selects
 * white (1) or periodic (0) noise and nnn the clock divider / source. */
#define PSG_NOISE_LATCH 0xE0

/* Attenuation is 4-bit and inverted relative to the driver's 0..128 volume:
 * 0 is full volume and 15 is silence. */
#define PSG_SILENCE     0x0F

void psg_rdj_init(uint32_t clock);
void psg_rdj_reset(void);
void psg_rdj_write(uint8_t val);
void psg_rdj_note_on(uint8_t ch, uint8_t note, uint8_t inst, uint8_t vol);
void psg_rdj_note_off(uint8_t ch);

void psg_rdj_write(uint8_t val) {
    if (rdj_active_backend && rdj_active_backend->write_psg) {
        rdj_active_backend->write_psg(val);
    }
}

/* Precomputed 128-note MIDI wavelength lookup table for the 3,579,545 Hz
 * Sega PSG clock:
 *   wavelength = 3579545 / (32 * note_freq_hz)
 * Clamped to the 10-bit counter range (0..1023); a value of 0 behaves as
 * 1024 on hardware, so it is never emitted. */
static const uint16_t psg_wavelength_table[128] = {
    1023, 1023, 1023, 1023, 1023, 1023, 1023, 1023,
    1023, 1023, 1023, 1023, 1023, 1023, 1023, 1023,
    1023, 1023, 1023, 1023, 1023, 1023, 1023, 1023,
    1023, 1023, 1023, 1023, 1023, 1023, 1023, 1023,
    1023, 1023, 1023, 1023, 1023, 1023, 1023, 1023,
    1023, 1023, 1023, 1023, 1023, 1017,  960,  906,
     855,  807,  762,  719,  679,  641,  605,  571,
     539,  508,  480,  453,  428,  404,  381,  360,
     339,  320,  302,  285,  269,  254,  240,  226,
     214,  202,  190,  180,  170,  160,  151,  143,
     135,  127,  120,  113,  107,  101,   95,   90,
      85,   80,   76,   71,   67,   64,   60,   57,
      53,   50,   48,   45,   42,   40,   38,   36,
      34,   32,   30,   28,   27,   25,   24,   22,
      21,   20,   19,   18,   17,   16,   15,   14,
      13,   13,   12,   11,   11,   10,    9,    9,
};

static uint16_t psg_calc_wavelength(uint8_t note) {
    if (note > 127) note = 127;
    return psg_wavelength_table[note];
}

/* Driver volume is 0..128 (see input_mapper's audition) while the PSG takes a
 * 4-bit attenuation. Invert and clamp: 128 maps to 0 (loudest) rather than
 * wrapping past 15 back to silence. */
static uint8_t psg_map_attenuation(uint8_t vol) {
    uint8_t shift = (uint8_t)(vol >> 3);
    if (shift > 15) {
        shift = 15;
    }
    return (uint8_t)(15 - shift);
}

static void psg_write_tone(uint8_t ch, uint16_t wavelength) {
    psg_rdj_write((uint8_t)(PSG_LATCH | (ch << PSG_CH_SHIFT) |
                            (wavelength & 0x0F)));
    psg_rdj_write((uint8_t)((wavelength >> 4) & 0x3F));
}

static void psg_write_volume(uint8_t ch, uint8_t attenuation) {
    psg_rdj_write((uint8_t)(PSG_LATCH | (ch << PSG_CH_SHIFT) | PSG_TYPE_VOL |
                            (attenuation & 0x0F)));
}

void psg_rdj_init(uint32_t clock) {
    /* The wavelength table is precomputed for the standard 3,579,545 Hz Sega
     * clock; the argument exists only for interface parity with the other
     * drivers and is otherwise unused. */
    (void)clock;
    psg_rdj_reset();
}

void psg_rdj_reset(void) {
    uint8_t ch;
    /* Silence every channel: latch byte with the volume bit set and the
     * attenuation nibble at 0xF (0x9F, 0xBF, 0xDF, 0xFF for channels 0..3). */
    for (ch = 0; ch < 4; ch++) {
        psg_write_volume(ch, PSG_SILENCE);
    }
}

void psg_rdj_note_on(uint8_t ch, uint8_t note, uint8_t inst, uint8_t vol) {
    uint8_t attenuation = psg_map_attenuation(vol);

    if (ch == 3) {
        /* Noise: the divider/source comes from the low three note bits and the
         * instrument's low bit picks white (1) over periodic (0) noise. */
        uint8_t noise = (uint8_t)(PSG_NOISE_LATCH |
                                  ((inst & 0x01) << 3) | (note & 0x07));
        psg_rdj_write(noise);
        psg_write_volume(ch, attenuation);
        return;
    }

    if (ch > 3) {
        return;
    }

    psg_write_tone(ch, psg_calc_wavelength(note));
    psg_write_volume(ch, attenuation);
}

void psg_rdj_note_off(uint8_t ch) {
    if (ch > 3) {
        return;
    }
    psg_write_volume(ch, PSG_SILENCE);
}

/* The PSG has no addressable registers, so write_reg ignores its 16-bit
 * argument and pushes val as a raw PSG byte. */
static void psg_wrapper_write_reg(uint16_t reg, uint8_t val) {
    (void)reg;
    psg_rdj_write(val);
}

static void psg_wrapper_shutdown(void) {
    psg_rdj_reset();
}

static const char psg_driver_name[] = "SN76489 Sega PSG Driver";
static const char psg_chip_name[] = "SN76489";

/* Global unified driver structure instance */
rdj_driver_t sn76489_driver = {
    psg_driver_name,
    psg_chip_name,
    4,
    16,
    psg_rdj_init,
    psg_rdj_reset,
    psg_rdj_note_on,
    psg_rdj_note_off,
    psg_wrapper_write_reg,
    psg_wrapper_shutdown
};

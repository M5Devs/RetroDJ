/* RDJ Sequencer Structures */
/* GPLv3 - RetroDJ Project */
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

#ifndef RDJ_SEQUENCER_H
#define RDJ_SEQUENCER_H

#if defined(__NES__) || defined(__CC65__)
    #define RDJ_MAX_CHANNELS 4
    #define RDJ_MAX_PATTERNS 2
    #define RDJ_MAX_ROWS     32
#elif defined(__PORT_sm83) || defined(__TARGET_gb) || defined(GBDK)
    /* Game Boy: 4 APU channels. The song must fit in 8 KB WRAM alongside the
     * console/font runtime, so keep patterns and rows small. */
    #define RDJ_MAX_CHANNELS 4
    #define RDJ_MAX_PATTERNS 2
    #define RDJ_MAX_ROWS     32
#elif defined(__SMS__) || defined(__SDCC_z80)
    /* Sega Master System / Game Gear: 4 PSG channels. The Z80 has 8 KB of work
     * RAM and the VDP tile map lives in VRAM, so the song has to stay small
     * enough to share the RAM with the tracker UI state. */
    #define RDJ_MAX_CHANNELS 4
    #define RDJ_MAX_PATTERNS 2
    #define RDJ_MAX_ROWS     32
#elif defined(RDJ_GENESIS_HARDWARE) || defined(SGDK_GCC) || defined(__m68k__)
    #define RDJ_MAX_CHANNELS 6
    #define RDJ_MAX_PATTERNS 16
    #define RDJ_MAX_ROWS     64
#else
    #include <stdint.h>
    #include <stddef.h>
    #define RDJ_MAX_CHANNELS 8
    #define RDJ_MAX_PATTERNS 64
    #define RDJ_MAX_ROWS     64
#endif

#include "rdj_driver.h"

#define RDJ_NOTE_EMPTY   0x00
#define RDJ_NOTE_OFF     0xFF
#define RDJ_MAX_INSTR    32

typedef struct {
    uint8_t note;       /* 0=empty 1-127=MIDI 255=off */
    uint8_t instrument;
    uint8_t volume;     /* 0=empty 1-128 */
    uint8_t fx_cmd;     /* effect A-Z */
    uint8_t fx_val;     /* effect param 00-FF */
} rdj_row_t;

typedef struct {
    rdj_row_t rows[RDJ_MAX_ROWS][RDJ_MAX_CHANNELS];
    uint8_t   length;
    uint8_t   speed;
} rdj_pattern_t;

typedef struct {
    rdj_pattern_t patterns[RDJ_MAX_PATTERNS];
    uint8_t       order[RDJ_MAX_PATTERNS];
    uint8_t       order_length;
    uint8_t       bpm;
    uint8_t       speed;
    rdj_driver_t *driver;
} rdj_song_t;

#if defined(__PORT_sm83) || defined(__TARGET_gb) || defined(GBDK)
/* The song must fit in the Game Boy's 8 KB WRAM along with the console/font
 * runtime. Without this, the 16-bit address space silently wraps and the
 * song corrupts VRAM/IO registers (garbage tiles, dead display). */
typedef char rdj_gb_song_fits_wram[(sizeof(rdj_song_t) <= 4096) ? 1 : -1];
#endif

#if defined(__SMS__) || defined(__SDCC_z80)
/* The Sega Master System's Z80 has 8 KB of work RAM. The song shares it with
 * the tracker state and the runtime, so fail the build rather than silently
 * corrupt the UI if the pattern data grows. */
typedef char rdj_sms_song_fits_ram[(sizeof(rdj_song_t) <= 2048) ? 1 : -1];
#endif

#endif

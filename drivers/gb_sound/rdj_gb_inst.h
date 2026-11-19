/* RDJ Game Boy APU Instrument Model */
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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef RDJ_GB_INST_H
#define RDJ_GB_INST_H

#include <stdint.h>

#define GB_INST_COUNT    16
#define GB_INST_NUM_PARAMS 8

/* Wave RAM presets. The APU's wave channel plays 32 4-bit samples stored two
 * per byte, so a preset is 16 bytes of packed nibbles. */
#define GB_WAVE_PRESET_COUNT 4

/* Parameter rows, in on-screen order (matches the [INST] screen). */
#define GB_INST_ROW_CHANNEL 0
#define GB_INST_ROW_DUTY    1
#define GB_INST_ROW_VOL     2
#define GB_INST_ROW_DIR     3
#define GB_INST_ROW_ENVPACE 4
#define GB_INST_ROW_SWEEP   5
#define GB_INST_ROW_WAVE    6
#define GB_INST_ROW_NOISE   7

/* Channel classes an instrument can target. */
#define GB_INST_CH_PULSE 0  /* CH1/CH2: duty + envelope (+sweep on CH1) */
#define GB_INST_CH_WAVE  1  /* CH3: wave RAM preset, no envelope */
#define GB_INST_CH_NOISE 2  /* CH4: noise mode + envelope */

#define GB_INST_ENV_DOWN 0
#define GB_INST_ENV_UP   1

typedef struct {
    uint8_t channel;   /* GB_INST_CH_* */
    uint8_t duty;      /* 0..3 -> 12.5/25/50/75% */
    uint8_t volume;    /* 0..15 initial envelope volume */
    uint8_t env_dir;   /* GB_INST_ENV_DOWN (decay) / GB_INST_ENV_UP (attack) */
    uint8_t env_pace;  /* 0..7 envelope sweep pace, 0 = hold */
    uint8_t sweep;     /* 0..7 CH1 NR10 frequency sweep pace, 0 = off */
    uint8_t wave;      /* 0..GB_WAVE_PRESET_COUNT-1 wave RAM preset */
    uint8_t noise;     /* 0..1 CH4 mode: 0 = 15-bit, 1 = 7-bit metallic */
} gb_inst_t;

/* Per-parameter bounds and names, indexed by GB_INST_ROW_*. */
extern const uint8_t gb_inst_param_max[GB_INST_NUM_PARAMS];

/* Apply one increment/decrement to a parameter, clamped to its range. Kept
 * separate from the driver so it is unit-testable on the host. */
void gb_inst_step(gb_inst_t *inst, uint8_t row, int delta);

/* Duty cycle as a percentage, for display. */
uint8_t gb_inst_duty_percent(uint8_t duty);

/* Fill a 16-byte buffer with the packed wave RAM preset. */
void gb_inst_wave_data(uint8_t preset, uint8_t *out16);

#endif

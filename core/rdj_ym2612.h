/* RDJ YM2612 FM Patch Interface */
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
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef RDJ_YM2612_H
#define RDJ_YM2612_H

#if defined(RDJ_GENESIS_HARDWARE) || defined(SGDK_GCC) || defined(__m68k__)
    #include <genesis.h>
#else
    #include <stdint.h>
    #include <stddef.h>
#endif

/* Number of instrument slots held in the driver's patch bank. */
#define YM2612_MAX_PATCHES 32

/* One YM2612 operator (per-operator register block). Field values are the raw
 * register bytes so a patch can be written back to the chip unmodified. */
typedef struct {
    uint8_t DT_MUL;   /* Detune (bits 6-4) / Multiple (bits 3-0) */
    uint8_t TL;       /* Total Level 0..127 (attenuation) */
    uint8_t AR;       /* Attack Rate (bits 4-0), Key Scale (bits 7-6) */
    uint8_t DR;       /* Decay Rate (bits 4-0), AM enable (bit 7) */
    uint8_t SR;       /* Sustain Rate (bits 4-0) */
    uint8_t SL_RR;    /* Sustain Level (bits 7-4) / Release Rate (bits 3-0) */
    uint8_t SSG_EG;   /* SSG-EG envelope (bits 3-0) */
} ym2612_operator_t;

/* A full FM voice: four operators plus the channel-level algorithm/feedback
 * and stereo/LFO register. */
typedef struct {
    ym2612_operator_t ops[4];
    uint8_t FB_ALG;      /* Feedback (bits 5-3) / Algorithm (bits 2-0) */
    uint8_t LR_AMS_PMS;  /* L/R pan (bits 7-6) / AMS (bits 5-4) / PMS (bits 2-0) */
} ym2612_patch_t;

void ym2612_rdj_init(uint32_t clock);
void ym2612_rdj_reset(void);
void ym2612_rdj_write(uint8_t port, uint8_t reg, uint8_t val);
void ym2612_rdj_note_on(uint8_t channel, uint8_t note, uint8_t instrument);
void ym2612_rdj_note_off(uint8_t channel);

/* Patch bank access. get_patch() returns a writable pointer into the live bank
 * (NULL for an out-of-range slot) so an editor can mutate it in place. */
ym2612_patch_t *ym2612_rdj_get_patch(uint8_t slot);
void ym2612_rdj_set_patch(uint8_t inst_slot, const ym2612_patch_t *patch);

/* Push every operator and channel register of a patch to a live channel
 * without keying a note. Used by the patch editor for real-time updates. */
void ym2612_rdj_apply_patch(uint8_t channel, const ym2612_patch_t *patch);

#endif

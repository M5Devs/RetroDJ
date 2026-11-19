/* GPLv3 - RetroDJ Project - Game Boy Sound Hardware Driver */

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
#elif defined(__PORT_sm83) || defined(__TARGET_gb) || defined(GBDK)
    #include <gb/gb.h>
    #include <stdint.h>
    #include <stddef.h>
#else
    #include <stdint.h>
    #include <stddef.h>
#endif

#include "core/rdj_driver.h"
#include "core/rdj_backend.h"
#include "rdj_gb_inst.h"

/* Register map (Game Boy APU hardware registers $FF10 - $FF26) */
#define GB_NR10 0xFF10  /* CH1 Sweep */
#define GB_NR11 0xFF11  /* CH1 Duty / Length */
#define GB_NR12 0xFF12  /* CH1 Envelope */
#define GB_NR13 0xFF13  /* CH1 Frequency Low */
#define GB_NR14 0xFF14  /* CH1 Frequency High / Trigger */

#define GB_NR21 0xFF16  /* CH2 Duty / Length */
#define GB_NR22 0xFF17  /* CH2 Envelope */
#define GB_NR23 0xFF18  /* CH2 Frequency Low */
#define GB_NR24 0xFF19  /* CH2 Frequency High / Trigger */

#define GB_NR30 0xFF1A  /* CH3 Sound On/Off */
#define GB_NR31 0xFF1B  /* CH3 Length */
#define GB_NR32 0xFF1C  /* CH3 Output Level */
#define GB_NR33 0xFF1D  /* CH3 Frequency Low */
#define GB_NR34 0xFF1E  /* CH3 Frequency High / Trigger */

#define GB_NR41 0xFF20  /* CH4 Length */
#define GB_NR42 0xFF21  /* CH4 Envelope */
#define GB_NR43 0xFF22  /* CH4 Polynomial Counter */
#define GB_NR44 0xFF23  /* CH4 Control / Trigger */

#define GB_NR50 0xFF24  /* Master Volume / VIN */
#define GB_NR51 0xFF25  /* Panning / Sound Output Terminal */
#define GB_NR52 0xFF26  /* Audio Master On/Off */

#define GB_WAVE_RAM 0xFF30  /* 16 bytes Wave RAM (32 4-bit samples) */

typedef void (*apu_write_fn)(uint16_t addr, uint8_t val);

/* Forward Declarations */
void gb_sound_rdj_init(uint32_t clock);
void gb_sound_rdj_reset(void);
void gb_sound_rdj_write(uint16_t addr, uint8_t val);
void gb_sound_note_on(uint8_t ch, uint8_t note, uint8_t inst, uint8_t vol);
void gb_sound_note_off(uint8_t ch);

void gb_sound_rdj_write(uint16_t addr, uint8_t val) {
    if (rdj_active_backend) {
        apu_write_fn fn = rdj_active_backend->write_apu;
        if (fn) {
            fn(addr, val);
        }
    }
}

/* Precomputed 128-note MIDI frequency timer values for Game Boy:
 * raw_freq = 2048 - (131072 / freq_hz)
 * Clamped to 11-bit limit (0..2047)
 */
static const uint16_t gb_frequency_table[128] = {
       0,    0,    0,    0,    0,    0,    0,    0,
       0,    0,    0,    0,    0,    0,    0,    0,
       0,    0,    0,    0,    0,    0,    0,    0,
       0,    0,    0,    0,    0,    0,    0,    0,
       0,    0,    0,    0,   44,  157,  263,  363,
     457,  547,  631,  711,  786,  856,  923,  986,
    1046, 1102, 1155, 1205, 1253, 1297, 1339, 1379,
    1417, 1452, 1486, 1517, 1547, 1575, 1602, 1627,
    1650, 1673, 1694, 1714, 1732, 1750, 1767, 1783,
    1798, 1812, 1825, 1837, 1849, 1860, 1871, 1881,
    1890, 1899, 1907, 1915, 1923, 1930, 1936, 1943,
    1949, 1954, 1959, 1964, 1969, 1974, 1978, 1982,
    1985, 1989, 1992, 1995, 1998, 2001, 2004, 2006,
    2009, 2011, 2013, 2015, 2017, 2018, 2020, 2022,
    2023, 2025, 2026, 2027, 2028, 2029, 2030, 2031,
    2032, 2033, 2034, 2035, 2036, 2036, 2037, 2038
};

/* Standard 16-byte (32 4-bit samples) Triangle / Saw RAM pattern */
static const uint8_t gb_default_wave_ram[16] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    0xED, 0xCB, 0xA9, 0x87, 0x65, 0x43, 0x21, 0x00
};

/* Instrument bank edited by the [INST] screen. Slot 0 keeps the historical
 * default sound (50% duty, full volume, no envelope) so existing songs are
 * unchanged. */
static gb_inst_t gb_inst_bank[GB_INST_COUNT];
static uint8_t  gb_inst_ready = 0;

static void gb_inst_bank_init(void) {
    uint8_t i;
    for (i = 0; i < GB_INST_COUNT; i++) {
        gb_inst_bank[i].channel  = (uint8_t)(i % 3);
        gb_inst_bank[i].duty     = 2;   /* 50% */
        gb_inst_bank[i].volume   = 15;
        gb_inst_bank[i].env_dir  = GB_INST_ENV_DOWN;
        gb_inst_bank[i].env_pace = 0;
        gb_inst_bank[i].sweep    = 0;
        gb_inst_bank[i].wave     = 0;
        gb_inst_bank[i].noise    = 0;
    }
    gb_inst_ready = 1;
}

gb_inst_t *gb_sound_inst(uint8_t slot) {
    if (!gb_inst_ready) gb_inst_bank_init();
    return &gb_inst_bank[slot % GB_INST_COUNT];
}


static uint16_t gb_calc_frequency(uint8_t note) {
    if (note > 127) note = 127;
    return gb_frequency_table[note];
}

void gb_sound_rdj_init(uint32_t clock) {
    (void)clock;
    gb_sound_rdj_reset();
}

void gb_sound_rdj_reset(void) {
    uint8_t i;

    /* 1. Master enable APU: Bit 7 MUST be set to 1 in NR52 */
    gb_sound_rdj_write(GB_NR52, 0x80);

    /* 2. Master Volume: Left 7, Right 7 */
    gb_sound_rdj_write(GB_NR50, 0x77);

    /* 3. Panning: Enable CH1-CH4 on both Left & Right channels */
    gb_sound_rdj_write(GB_NR51, 0xFF);

    /* 4. Disable sweep on CH1 */
    gb_sound_rdj_write(GB_NR10, 0x00);

    /* 5. Silence CH1 & CH2 envelopes */
    gb_sound_rdj_write(GB_NR12, 0x00);
    gb_sound_rdj_write(GB_NR22, 0x00);

    /* 6. Disable CH3 DAC, silence CH3 output level */
    gb_sound_rdj_write(GB_NR30, 0x00);
    gb_sound_rdj_write(GB_NR32, 0x00);

    /* 7. Silence CH4 noise envelope */
    gb_sound_rdj_write(GB_NR42, 0x00);

    /* 8. Load default Wave RAM for CH3 */
    gb_sound_rdj_write(GB_NR30, 0x80); /* Enable CH3 DAC to access Wave RAM */
    for (i = 0; i < 16; i++) {
        gb_sound_rdj_write(GB_WAVE_RAM + i, gb_default_wave_ram[i]);
    }
}

/* Game Boy envelope register byte: initial volume, direction, pace. `vol_4bit`
 * is the per-note volume from the pattern, or 0 when the note leaves volume to
 * the instrument, in which case the instrument's own level is used. */
static uint8_t gb_env_byte(const gb_inst_t *in, uint8_t vol_4bit) {
    uint8_t vol = vol_4bit ? vol_4bit : (in ? in->volume : 15);
    uint8_t dir = in ? in->env_dir : GB_INST_ENV_DOWN;
    uint8_t pace = in ? in->env_pace : 0;
    if (vol > 15) vol = 15;
    /* Pace 0 means "no envelope change", which the APU expresses by holding
     * the initial volume; a non-zero pace with direction 0 decays. */
    if (pace == 0) return (uint8_t)(vol << 4);
    return (uint8_t)((vol << 4) | ((dir ? 1 : 0) << 3) | (pace & 0x07));
}

void gb_sound_note_on(uint8_t ch, uint8_t note, uint8_t inst, uint8_t vol) {
    uint16_t freq;
    uint8_t vol_4bit;
    uint8_t duty_bits;
    uint8_t noise_poly;
    gb_inst_t *in = gb_sound_inst(inst);

    /* Map the pattern's 0..128 volume into 0..15; 0 means "instrument default",
     * which gb_env_byte resolves. */
    vol_4bit = (uint8_t)(vol >> 3);
    if (vol_4bit > 15) vol_4bit = 15;

    /* Duty bits live in NRx1 bits 7-6; the instrument's duty is used when the
     * instrument was explicitly edited, otherwise the 50% default. */
    duty_bits = (uint8_t)((in->duty & 0x03) << 6);

    if (ch == 0) {
        /* CH1: Pulse 1 with frequency sweep (NR10). */
        freq = gb_calc_frequency(note);
        /* NR10: pace in bits 6-4, direction up (0x08) so sweep raises pitch,
         * shift 0 keeps the step gentle. */
        gb_sound_rdj_write(GB_NR10, in->sweep ? (uint8_t)(((in->sweep & 0x07) << 4) | 0x08) : 0x00);
        gb_sound_rdj_write(GB_NR11, duty_bits);
        gb_sound_rdj_write(GB_NR12, gb_env_byte(in, vol_4bit));
        gb_sound_rdj_write(GB_NR13, (uint8_t)(freq & 0xFF));
        gb_sound_rdj_write(GB_NR14, (uint8_t)(0x80 | ((freq >> 8) & 0x07)));
    }
    else if (ch == 1) {
        /* CH2: Pulse 2 */
        freq = gb_calc_frequency(note);
        gb_sound_rdj_write(GB_NR21, duty_bits);
        gb_sound_rdj_write(GB_NR22, gb_env_byte(in, vol_4bit));
        gb_sound_rdj_write(GB_NR23, (uint8_t)(freq & 0xFF));
        gb_sound_rdj_write(GB_NR24, (uint8_t)(0x80 | ((freq >> 8) & 0x07)));
    }
    else if (ch == 2) {
        /* CH3: Wavetable. The instrument's wave preset is loaded into Wave RAM
         * on every note so switching presets is audible immediately. */
        uint8_t wave[16];
        uint8_t i;
        uint8_t level;

        freq = gb_calc_frequency(note);
        gb_inst_wave_data(in->wave, wave);

        if (vol_4bit == 0) {
            level = 0x00; /* Mute */
        } else if (vol_4bit < 6) {
            level = 0x60; /* 25% */
        } else if (vol_4bit < 11) {
            level = 0x40; /* 50% */
        } else {
            level = 0x20; /* 100% */
        }

        gb_sound_rdj_write(GB_NR30, 0x80); /* Enable CH3 DAC to reach Wave RAM */
        for (i = 0; i < 16; i++) gb_sound_rdj_write(GB_WAVE_RAM + i, wave[i]);
        gb_sound_rdj_write(GB_NR32, level);
        gb_sound_rdj_write(GB_NR33, (uint8_t)(freq & 0xFF));
        gb_sound_rdj_write(GB_NR34, (uint8_t)(0x80 | ((freq >> 8) & 0x07)));
    }
    else if (ch == 3) {
        /* CH4: Noise. Note picks the clock shift; the instrument picks the
         * 7-bit (metallic) vs 15-bit (hiss) polynomial width. */
        noise_poly = (uint8_t)((note & 0x0F) | ((note & 0x70) >> 1));
        if (in->noise) noise_poly |= 0x08; /* NR43 bit 3 = 7-bit counter */

        gb_sound_rdj_write(GB_NR41, 0x00);
        gb_sound_rdj_write(GB_NR42, gb_env_byte(in, vol_4bit));
        gb_sound_rdj_write(GB_NR43, noise_poly);
        gb_sound_rdj_write(GB_NR44, 0x80); /* Trigger */
    }
}

void gb_sound_note_off(uint8_t ch) {
    /* A note is silenced by zeroing the channel's volume envelope and then
     * re-triggering it: the length/envelope registers only take effect on a
     * trigger edge, so writing NRx2 alone leaves the old envelope running and
     * the channel drones. CH3 has no envelope, so its DAC is switched off
     * instead, which is the only way to stop a wave channel mid-note. */
    if (ch == 0) {
        gb_sound_rdj_write(GB_NR12, 0x00); /* Zero volume envelope */
        gb_sound_rdj_write(GB_NR14, 0x80); /* Trigger cut */
    } else if (ch == 1) {
        gb_sound_rdj_write(GB_NR22, 0x00);
        gb_sound_rdj_write(GB_NR24, 0x80);
    } else if (ch == 2) {
        gb_sound_rdj_write(GB_NR30, 0x00); /* Disable Wave DAC */
    } else if (ch == 3) {
        gb_sound_rdj_write(GB_NR42, 0x00);
        gb_sound_rdj_write(GB_NR44, 0x80);
    }
}

static void gb_sound_wrapper_write_reg(uint16_t reg, uint8_t val) {
    gb_sound_rdj_write(reg, val);
}

static void gb_sound_wrapper_shutdown(void) {
    gb_sound_rdj_reset();
    gb_sound_rdj_write(GB_NR52, 0x00); /* Disable APU */
}

static const char gb_driver_name[] = "Game Boy APU Sound Driver";
static const char gb_chip_name[] = "DMG/GBC APU";

/* Global unified driver structure instance */
rdj_driver_t gb_sound_driver = {
    gb_driver_name,
    gb_chip_name,
    4,
    16,
    gb_sound_rdj_init,
    gb_sound_rdj_reset,
    gb_sound_note_on,
    gb_sound_note_off,
    gb_sound_wrapper_write_reg,
    gb_sound_wrapper_shutdown
};

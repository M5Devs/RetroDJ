/* GPLv3 - RetroDJ Project - Sega Master System / Game Gear Hardware Backend */

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

#if defined(__SMS__) || defined(__SDCC_z80)
    #include <stdint.h>
    #include <stddef.h>
#elif defined(RDJ_GENESIS_HARDWARE) || defined(SGDK_GCC) || defined(__m68k__)
    #include <genesis.h>
#else
    #include <stdint.h>
    #include <stddef.h>
#endif

#include "core/rdj_backend.h"

/* Sega Master System / Game Gear memory-mapped VDP ports. */
#define VDP_DATA   0xBE
#define VDP_CTRL   0xBF
#define VDP_STATUS 0xBF

/* Player 1 controller port. */
#define SMS_PORT_DC 0xDC

#if defined(__SMS__) || defined(__SDCC_z80)
/* The SN76489 is write-only and lives behind Z80 I/O port $7F. */
__sfr __at 0x7F SMS_PSG_PORT;
#endif

static void sms_hw_write_psg(uint8_t val) {
#if defined(__SMS__) || defined(__SDCC_z80)
    SMS_PSG_PORT = val;
#else
    (void)val;
#endif
}

static void sms_hw_init(void) {
    /* Silence the PSG so a reset never leaves a channel droning. */
    sms_hw_write_psg(0x9F);
    sms_hw_write_psg(0xBF);
    sms_hw_write_psg(0xDF);
    sms_hw_write_psg(0xFF);
}

static void sms_hw_shutdown(void) {
    sms_hw_init();
}

static void sms_hw_write_ym2612(uint8_t port, uint8_t reg, uint8_t val) {
    (void)port;
    (void)reg;
    (void)val;
}

static void sms_hw_write_apu(uint16_t addr, uint8_t val) {
    (void)addr;
    (void)val;
}

static void sms_hw_wait_samples(uint16_t samples) {
    (void)samples;
}

static void sms_hw_wait_frame(void) {
#if defined(__SMS__) || defined(__SDCC_z80)
    /* Poll the VDP status register's VBlank flag (bit 7). The flag is cleared
     * when the status port is read, so a second read is needed to be sure the
     * previous frame's flag has been consumed before waiting for the next. */
    volatile uint8_t *status = (volatile uint8_t *)VDP_STATUS;
    while (*status & 0x80) {
    }
    while (!(*status & 0x80)) {
    }
#endif
}

static const char sms_backend_name[] = "Sega Master System Real Hardware Backend";

rdj_backend_t backend_sms_hw = {
    sms_backend_name,
    sms_hw_init,
    sms_hw_shutdown,
    sms_hw_write_ym2612,
    sms_hw_write_apu,
    sms_hw_write_psg,
    sms_hw_wait_samples,
    sms_hw_wait_frame
};

#if defined(__SMS__) || defined(__SDCC_z80)
rdj_backend_t *rdj_active_backend = &backend_sms_hw;

void rdj_set_backend(rdj_backend_t *backend) {
    if (backend) {
        if (rdj_active_backend && rdj_active_backend->shutdown) {
            rdj_active_backend->shutdown();
        }
        rdj_active_backend = backend;
        if (rdj_active_backend->init) {
            rdj_active_backend->init();
        }
    } else {
        rdj_active_backend = &backend_sms_hw;
    }
}
#endif

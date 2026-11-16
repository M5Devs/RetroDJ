/* RetroDJ - Sega Master System Main Entry Point */
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

#include "ui_sms.h"
#include "core/rdj_driver.h"
#include "core/rdj_backend.h"

#if defined(__SMS__) || defined(__SDCC_z80)
#include "SMSlib.h"
#endif

void main(void) {
    /* Initialize Tracker UI (loads the font and draws the static layout). */
    ui_init();

    /* Initialize and activate the SMS hardware backend. */
    rdj_set_backend(&backend_sms_hw);

    /* Initialize the SN76489 PSG driver for the standard NTSC Sega clock. */
    sn76489_driver.init(3579545);

    while (1) {
        /* Poll input and run the sequencer during visible frame time. */
        ui_update();

        /* Wait for the start of VBlank before touching VRAM. */
        SMS_waitForVBlank();

        /* Push only the rows whose text changed. */
        ui_render();
    }
}

#if defined(__SMS__) || defined(__SDCC_z80)
SMS_EMBED_SEGA_ROM_HEADER(9999, 0);
SMS_EMBED_SDSC_HEADER_AUTO_DATE(1, 0, "RetroDJ", "RetroDJ SMS",
                                "RetroDJ tracker for Sega Master System");
#endif

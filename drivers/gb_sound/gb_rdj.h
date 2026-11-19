/* RetroDJ - Game Boy APU Driver Header */
/* GPLv3 - RetroDJ Project */

#ifndef RDJ_GB_SOUND_H
#define RDJ_GB_SOUND_H

#include <stdint.h>
#include "rdj_gb_inst.h"

/* Instrument bank shared by the driver and the [INST] editor. Slot is taken
 * modulo GB_INST_COUNT. The bank is populated with defaults on first use. */
gb_inst_t *gb_sound_inst(uint8_t slot);

void gb_sound_note_on(uint8_t ch, uint8_t note, uint8_t inst, uint8_t vol);
void gb_sound_note_off(uint8_t ch);
void gb_sound_rdj_init(uint32_t clock);
void gb_sound_rdj_reset(void);
void gb_sound_rdj_write(uint16_t addr, uint8_t val);

#endif

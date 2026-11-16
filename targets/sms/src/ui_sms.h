/* RetroDJ - Sega Master System Tracker UI Header */
/* GPLv3 - RetroDJ Project */

#ifndef RDJ_SMS_UI_H
#define RDJ_SMS_UI_H

#include <stdint.h>
#include <stdbool.h>

#include "targets/genesis/src/input_mapper.h"
#include "core/sequencer.h"
#include "core/rdj_driver.h"
#include "core/rdj_backend.h"

extern rdj_driver_t sn76489_driver;
extern rdj_backend_t backend_sms_hw;

void ui_init(void);
void ui_update(void);
void ui_render(void);
void ui_process_playback(void);

void get_note_name(uint8_t note, char *out);
input_tracker_state_t *ui_get_tracker_state(void);
rdj_song_t *ui_get_song(void);

#endif

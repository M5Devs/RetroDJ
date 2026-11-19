/* RetroDJ - Game Boy UI Header */
/* GPLv3 - RetroDJ Project */

#ifndef RDJ_GB_UI_H
#define RDJ_GB_UI_H

#if defined(__PORT_sm83) || defined(__TARGET_gb) || defined(GBDK)
    #include <gb/gb.h>
    #include <stdint.h>
    #include <stddef.h>
#else
    #include <stdint.h>
    #include <stddef.h>
#endif

#include "targets/genesis/src/input_mapper.h"
#include "core/sequencer.h"
#include "core/rdj_driver.h"
#include "core/rdj_backend.h"
#include "drivers/gb_sound/rdj_gb_inst.h"

extern rdj_driver_t gb_sound_driver;
extern rdj_backend_t backend_gb_hw;

/* The Game Boy's three editor screens. PAT and SONG reuse the shared state
 * machine; INST is this target's own APU instrument editor. */
typedef enum {
    GB_SCREEN_PAT = 0,   /* Main 4-channel pattern matrix */
    GB_SCREEN_SONG,      /* Song order / playlist sequence */
    GB_SCREEN_INST       /* Game Boy instrument / wave editor */
} gb_screen_mode_t;

void ui_init(void);
void ui_update(void);
void ui_render(void);
void ui_process_playback(void);

void get_note_name(uint8_t note, char *out);
input_tracker_state_t *ui_get_tracker_state(void);
rdj_song_t *ui_get_song(void);

/* Screen currently displayed, derived from the shared tracker state. */
uint8_t ui_get_screen(void);

#endif

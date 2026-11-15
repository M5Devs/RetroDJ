/* RetroDJ - Genesis Input Mapper Implementation */
/* GPLv3 - RetroDJ Project */

#include "input_mapper.h"

/* The YM2612 entry points are only a fallback for songs with no bound driver
 * (see audition_note_on/off below). */
#if defined(RDJ_GENESIS_HARDWARE) || defined(SGDK_GCC) || defined(__m68k__)
extern void ym2612_rdj_note_on(uint8_t channel, uint8_t note, uint8_t instrument);
extern void ym2612_rdj_note_off(uint8_t channel);
#endif

/* Audition through whichever sound driver the song is bound to, so the same
 * input code previews notes on the YM2612, the NES 2A03 and the Game Boy APU
 * alike. Songs with no bound driver (host-side tests) fall back to the YM2612. */
static void audition_note_on(rdj_song_t *song, uint8_t ch, uint8_t note,
                             uint8_t inst, uint8_t vol) {
    if (song->driver && song->driver->note_on) {
        song->driver->note_on(ch, note, inst, vol);
        return;
    }
#if defined(RDJ_GENESIS_HARDWARE) || defined(SGDK_GCC) || defined(__m68k__)
    (void)vol;
    ym2612_rdj_note_on(ch, note, inst);
#else
    (void)ch; (void)note; (void)inst; (void)vol;
#endif
}

static void audition_note_off(rdj_song_t *song, uint8_t ch) {
    if (song->driver && song->driver->note_off) {
        song->driver->note_off(ch);
        return;
    }
#if defined(RDJ_GENESIS_HARDWARE) || defined(SGDK_GCC) || defined(__m68k__)
    ym2612_rdj_note_off(ch);
#else
    (void)ch;
#endif
}

/* Channel with an A/B audition in flight, or 0xFF when none. The Genesis pad
 * has a dedicated C button whose release ends an audition; the NES and Game Boy
 * pads do not, so their A/B auditions are released by the button coming up.
 * Without this a tapped note would drone on the hardware forever. */
static uint8_t s_audition_chan = 0xFF;

static void audition_note_on_held(rdj_song_t *song, uint8_t ch, uint8_t note,
                                  uint8_t inst) {
    s_audition_chan = ch;
    audition_note_on(song, ch, note, inst, 128);
}

/* Pitch to audition: the cell's note, or the current octave's root note when
 * the cell is empty, so a bare audition still sounds. */
static uint8_t audition_pitch(const input_tracker_state_t *state,
                              const rdj_row_t *cell) {
    if (cell->note != RDJ_NOTE_EMPTY && cell->note != RDJ_NOTE_OFF) {
        return cell->note;
    }
    return (uint8_t)(state->octave * 12 + 12);
}

void input_mapper_init(input_tracker_state_t *state) {
    if (!state) return;
    state->cursor_row = 0;
    state->cursor_channel = 0;
    state->cursor_subcol = RDJ_SUBCOL_NOTE;
    state->octave = 4;
    state->is_playing = 0;
    state->last_joy = 0;
    s_audition_chan = 0xFF;
}

void input_mapper_update(input_tracker_state_t *state, rdj_song_t *song, uint16_t joy_state) {
    uint16_t pressed;
    bool btn_a;
    bool btn_b;
    rdj_row_t *cell;

    if (!state || !song) return;

    pressed = joy_state & ~state->last_joy;
    btn_a = (joy_state & BUTTON_A) != 0;
    btn_b = (joy_state & BUTTON_B) != 0;
    cell = &song->patterns[0].rows[state->cursor_row][state->cursor_channel];

    /* BUTTON_B + BUTTON_A combo: Clear current cell */
    if ((btn_b && (pressed & BUTTON_A)) || (btn_a && (pressed & BUTTON_B))) {
        cell->note = RDJ_NOTE_EMPTY;
        cell->instrument = 0;
        cell->volume = 0;
        cell->fx_cmd = 0;
        cell->fx_val = 0;
        state->last_joy = joy_state;
        return;
    }

    /* BUTTON_START: Toggle playback */
    if (pressed & BUTTON_START) {
        state->is_playing = !state->is_playing;
    }

    /* Note editing: BUTTON_A + Direction transposes the cell, auditioning the
     * result immediately (the LSDj workflow). A and B are handled as distinct
     * branches so the A path stays reachable on pads that have no C button. */
    if (btn_a && !btn_b) {
        uint8_t before = cell->note;

        /* BUTTON_A + UP/DOWN: Transpose note by +/- 1 semitone */
        /* BUTTON_A + LEFT/RIGHT: Transpose note by +/- 1 octave (12 semitones) */
        if (pressed & BUTTON_UP) {
            if (cell->note == RDJ_NOTE_EMPTY) cell->note = state->octave * 12 + 12;
            else if (cell->note < 127) cell->note++;
        }
        if (pressed & BUTTON_DOWN) {
            if (cell->note == RDJ_NOTE_EMPTY) cell->note = state->octave * 12 + 12;
            else if (cell->note > 1) cell->note--;
        }
        if (pressed & BUTTON_RIGHT) {
            if (cell->note == RDJ_NOTE_EMPTY) cell->note = state->octave * 12 + 12;
            else if (cell->note <= 115) cell->note += 12;
            else cell->note = 127;
        }
        if (pressed & BUTTON_LEFT) {
            if (cell->note == RDJ_NOTE_EMPTY) cell->note = state->octave * 12 + 12;
            else if (cell->note > 12) cell->note -= 12;
            else cell->note = 1;
        }

        if (cell->note != before) {
            /* Note changed: audition it. */
            audition_note_on_held(song, state->cursor_channel, cell->note,
                                  cell->instrument);
        } else if ((pressed & BUTTON_A) && state->cursor_subcol == RDJ_SUBCOL_NOTE) {
            /* Plain A tap on the note column: audition without editing. */
            audition_note_on_held(song, state->cursor_channel,
                                  audition_pitch(state, cell), cell->instrument);
        }
    } else if (btn_b && !btn_a) {
        /* BUTTON_B + UP/DOWN: Increment/decrement current value by 1 */
        /* BUTTON_B + LEFT/RIGHT: Increment/decrement current value by 16 (0x10) */
        int delta = 0;

        if (pressed & BUTTON_UP) delta = 1;
        if (pressed & BUTTON_DOWN) delta = -1;
        if (pressed & BUTTON_RIGHT) delta = 16;
        if (pressed & BUTTON_LEFT) delta = -16;

        if (delta != 0) {
            switch (state->cursor_subcol) {
                case RDJ_SUBCOL_NOTE: {
                    int new_note;
                    if (cell->note == RDJ_NOTE_EMPTY) cell->note = state->octave * 12 + 12;
                    new_note = (int)cell->note + delta;
                    if (new_note < 1) new_note = 1;
                    if (new_note > 127) new_note = 127;
                    cell->note = (uint8_t)new_note;
                    audition_note_on_held(song, state->cursor_channel, cell->note,
                                          cell->instrument);
                    break;
                }
                case RDJ_SUBCOL_INST: {
                    int new_inst = (int)cell->instrument + delta;
                    if (new_inst < 0) new_inst = 0;
                    if (new_inst > RDJ_MAX_INSTR - 1) new_inst = RDJ_MAX_INSTR - 1;
                    cell->instrument = (uint8_t)new_inst;
                    break;
                }
                case RDJ_SUBCOL_VOL: {
                    int new_vol = (int)cell->volume + delta;
                    if (new_vol < 0) new_vol = 0;
                    if (new_vol > 128) new_vol = 128;
                    cell->volume = (uint8_t)new_vol;
                    break;
                }
                case RDJ_SUBCOL_FX: {
                    int new_fx = (int)cell->fx_val + delta;
                    if (new_fx < 0) new_fx = 0;
                    if (new_fx > 255) new_fx = 255;
                    cell->fx_val = (uint8_t)new_fx;
                    break;
                }
            }
        } else if ((pressed & BUTTON_B) && state->cursor_subcol == RDJ_SUBCOL_NOTE) {
            /* Plain B tap on the note column: audition (pads without a C button). */
            audition_note_on_held(song, state->cursor_channel,
                                  audition_pitch(state, cell), cell->instrument);
        }
    } else if (!btn_a && !btn_b) {
        /* Normal Cursor Navigation */
        if (pressed & BUTTON_UP) {
            if (state->cursor_row > 0) state->cursor_row--;
            else state->cursor_row = RDJ_MAX_ROWS - 1;
        }
        if (pressed & BUTTON_DOWN) {
            if (state->cursor_row < RDJ_MAX_ROWS - 1) state->cursor_row++;
            else state->cursor_row = 0;
        }
        if (pressed & BUTTON_LEFT) {
            if (state->cursor_subcol > 0) {
                state->cursor_subcol--;
            } else if (state->cursor_channel > 0) {
                state->cursor_channel--;
                state->cursor_subcol = RDJ_NUM_SUBCOLS - 1;
            }
        }
        if (pressed & BUTTON_RIGHT) {
            if (state->cursor_subcol < RDJ_NUM_SUBCOLS - 1) {
                state->cursor_subcol++;
            } else if (state->cursor_channel < RDJ_MAX_CHANNELS - 1) {
                state->cursor_channel++;
                state->cursor_subcol = 0;
            }
        }
    }

    /* BUTTON_C (Genesis only): audition while held. */
    if (pressed & BUTTON_C) {
        audition_note_on_held(song, state->cursor_channel,
                              audition_pitch(state, cell), cell->instrument);
    } else if ((state->last_joy & BUTTON_C) && !(joy_state & BUTTON_C)) {
        audition_note_off(song, state->cursor_channel);
        s_audition_chan = 0xFF;
    }

    /* Release an A/B audition once the held button comes back up. BUTTON_C is
     * excluded: the Genesis C button auditions for as long as it is held and is
     * released by the block above, so it must not be cut while still down. */
    if (s_audition_chan != 0xFF &&
        !(joy_state & (BUTTON_A | BUTTON_B | BUTTON_C))) {
        audition_note_off(song, s_audition_chan);
        s_audition_chan = 0xFF;
    }

    state->last_joy = joy_state;
}

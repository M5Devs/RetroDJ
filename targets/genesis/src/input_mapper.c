/* RetroDJ - Genesis Input Mapper Implementation */
/* GPLv3 - RetroDJ Project */

#include "input_mapper.h"

/* The YM2612 entry points are only a fallback for songs with no bound driver
 * (see audition_note_on/off below). */
#if defined(RDJ_GENESIS_HARDWARE) || defined(SGDK_GCC) || defined(__m68k__)
extern void ym2612_rdj_note_on(uint8_t channel, uint8_t note, uint8_t instrument);
extern void ym2612_rdj_note_off(uint8_t channel);
#endif

/* FM patch access is routed through platform-installed hooks so this file links
 * on targets that have no YM2612 driver (see input_mapper.h). Kept as plain
 * function pointers rather than struct members: SDCC miscompiles indirect calls
 * through a struct field. */
static ym2612_patch_t *(*s_get_patch)(uint8_t slot);
static void (*s_apply_patch)(uint8_t channel, const ym2612_patch_t *patch);

void input_mapper_set_patch_ops(const input_mapper_patch_ops_t *ops) {
    if (ops) {
        s_get_patch = ops->get_patch;
        s_apply_patch = ops->apply_patch;
    } else {
        s_get_patch = NULL;
        s_apply_patch = NULL;
    }
}

static ym2612_patch_t *patch_get(uint8_t slot) {
    return s_get_patch ? s_get_patch(slot) : NULL;
}

static void patch_apply(uint8_t channel, const ym2612_patch_t *patch) {
    if (s_apply_patch) s_apply_patch(channel, patch);
}

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

/* ---- screen helpers ---- */
static uint8_t next_screen(uint8_t s) { return (uint8_t)((s + 1) % UI_NUM_SCREENS); }
static uint8_t prev_screen(uint8_t s) { return (uint8_t)((s + UI_NUM_SCREENS - 1) % UI_NUM_SCREENS); }

/* Channel the patch editor programmes and auditions on. */
#define PATCH_AUD_CH 0

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Apply one increment/decrement to the parameter selected in the patch editor,
 * then push the whole voice to the chip so the change is audible immediately. */
static void patch_step(input_tracker_state_t *state, int delta) {
    ym2612_patch_t *p = patch_get(state->patch_slot);
    ym2612_operator_t *o;

    if (!p) return;
    o = &p->ops[state->patch_op & 0x03];

    switch (state->patch_row) {
        case UI_PATCH_ROW_ALGO: {
            int a = clamp_int((p->FB_ALG & 0x07) + delta, 0, 7);
            p->FB_ALG = (uint8_t)((p->FB_ALG & 0x38) | a);
            break;
        }
        case UI_PATCH_ROW_FB: {
            int f = clamp_int(((p->FB_ALG >> 3) & 0x07) + delta, 0, 7);
            p->FB_ALG = (uint8_t)((p->FB_ALG & 0x07) | (f << 3));
            break;
        }
        case UI_PATCH_ROW_PAN: {
            /* Cycle the L/R stereo bits: L -> R -> both -> L. */
            uint8_t pan = (uint8_t)(p->LR_AMS_PMS & 0xC0);
            if (delta > 0) {
                if (pan == 0x80) pan = 0x40;
                else if (pan == 0x40) pan = 0xC0;
                else pan = 0x80;
            } else {
                if (pan == 0x80) pan = 0xC0;
                else if (pan == 0xC0) pan = 0x40;
                else pan = 0x80;
            }
            p->LR_AMS_PMS = (uint8_t)((p->LR_AMS_PMS & 0x3F) | pan);
            break;
        }
        case UI_PATCH_ROW_DTMUL: o->DT_MUL  = (uint8_t)clamp_int((int)o->DT_MUL + delta, 0, 255); break;
        case UI_PATCH_ROW_TL:    o->TL      = (uint8_t)clamp_int((int)o->TL + delta, 0, 127);     break;
        case UI_PATCH_ROW_AR:    o->AR      = (uint8_t)clamp_int((int)o->AR + delta, 0, 31);      break;
        case UI_PATCH_ROW_DR:    o->DR      = (uint8_t)clamp_int((int)o->DR + delta, 0, 31);      break;
        case UI_PATCH_ROW_SR:    o->SR      = (uint8_t)clamp_int((int)o->SR + delta, 0, 31);      break;
        case UI_PATCH_ROW_SLRR:  o->SL_RR   = (uint8_t)clamp_int((int)o->SL_RR + delta, 0, 255);  break;
        case UI_PATCH_ROW_SSG:   o->SSG_EG  = (uint8_t)clamp_int((int)o->SSG_EG + delta, 0, 15);  break;
        default: break;
    }

    patch_apply(PATCH_AUD_CH, p);
}

/* ---- pattern editor input (with cursor-walk into the sidebar tabs) ---- */
static void handle_pat(input_tracker_state_t *state, rdj_song_t *song,
                       uint16_t pressed, bool btn_a, bool btn_b) {
    rdj_row_t *cell = &song->patterns[state->edit_pattern].rows[state->cursor_row]
                                                        [state->cursor_channel];

    if (state->sidebar_focus) {
        if (pressed & BUTTON_UP)   { if (state->sidebar_tab > 0) state->sidebar_tab--; }
        if (pressed & BUTTON_DOWN) { if (state->sidebar_tab < UI_NUM_SCREENS - 1) state->sidebar_tab++; }
        if (pressed & BUTTON_LEFT) { state->sidebar_focus = 0; }
        if (btn_a && !btn_b)       { state->screen = state->sidebar_tab; state->sidebar_focus = 0; }
        return;
    }

    /* B + A combo: clear current cell */
    if ((btn_b && (pressed & BUTTON_A)) || (btn_a && (pressed & BUTTON_B))) {
        cell->note = RDJ_NOTE_EMPTY;
        cell->instrument = 0;
        cell->volume = 0;
        cell->fx_cmd = 0;
        cell->fx_val = 0;
        return;
    }

    if (btn_a && !btn_b) {
        uint8_t before = cell->note;

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
            audition_note_on_held(song, state->cursor_channel, cell->note, cell->instrument);
        } else if ((pressed & BUTTON_A) && state->cursor_subcol == RDJ_SUBCOL_NOTE) {
            audition_note_on_held(song, state->cursor_channel,
                                  audition_pitch(state, cell), cell->instrument);
        }
    } else if (btn_b && !btn_a) {
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
                    audition_note_on_held(song, state->cursor_channel, cell->note, cell->instrument);
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
            audition_note_on_held(song, state->cursor_channel,
                                  audition_pitch(state, cell), cell->instrument);
        }
    } else if (!btn_a && !btn_b) {
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
            } else {
                /* Walk past FM6 into the sidebar tab column. */
                state->sidebar_focus = 1;
                state->sidebar_tab = UI_TAB_PAT;
            }
        }
    }
}

/* ---- song playlist input ---- */
static void handle_song(input_tracker_state_t *state, rdj_song_t *song,
                        uint16_t pressed, bool btn_a, bool btn_b) {
    uint8_t slot = state->song_cursor;

    if (btn_a && !btn_b) {
        /* A + UP/DOWN: change the pattern assigned to the selected slot. */
        if (pressed & BUTTON_UP) {
            song->order[slot] = (uint8_t)((song->order[slot] + 1) % RDJ_MAX_PATTERNS);
        }
        if (pressed & BUTTON_DOWN) {
            song->order[slot] = (uint8_t)((song->order[slot] + RDJ_MAX_PATTERNS - 1) % RDJ_MAX_PATTERNS);
        }
        /* A + LEFT/RIGHT: open the selected slot's pattern in the PAT editor. */
        if (pressed & (BUTTON_LEFT | BUTTON_RIGHT)) {
            state->edit_pattern = song->order[slot];
            state->cursor_row = 0;
            state->screen = UI_SCREEN_PAT;
        }
    } else if (btn_b && !btn_a) {
        /* B + UP/DOWN: adjust the order length. */
        if ((pressed & BUTTON_UP) && song->order_length < RDJ_MAX_PATTERNS) song->order_length++;
        if ((pressed & BUTTON_DOWN) && song->order_length > 1) song->order_length--;
        if (state->song_cursor >= song->order_length) {
            state->song_cursor = (uint8_t)(song->order_length - 1);
        }
    } else if (!btn_a && !btn_b) {
        if ((pressed & BUTTON_UP) && state->song_cursor > 0) state->song_cursor--;
        if ((pressed & BUTTON_DOWN) && state->song_cursor < RDJ_MAX_PATTERNS - 1) state->song_cursor++;
    }
}

/* ---- patch editor input ---- */
static void handle_patch(input_tracker_state_t *state, rdj_song_t *song,
                         uint16_t pressed, bool btn_a, bool btn_b) {
    if (btn_a && !btn_b) {
        /* A + LEFT/RIGHT: switch instrument slot. */
        if (pressed & BUTTON_LEFT) {
            if (state->patch_slot > 0) state->patch_slot--;
        }
        if (pressed & BUTTON_RIGHT) {
            if (state->patch_slot < YM2612_MAX_PATCHES - 1) state->patch_slot++;
        }
        /* A tap (no direction): audition the patch. */
        if (!(pressed & (BUTTON_LEFT | BUTTON_RIGHT | BUTTON_UP | BUTTON_DOWN))) {
            audition_note_on_held(song, PATCH_AUD_CH, (uint8_t)(state->octave * 12 + 12),
                                  state->patch_slot);
        }
    } else if (btn_b && !btn_a) {
        int delta = 0;
        if (pressed & BUTTON_UP) delta = 1;
        if (pressed & BUTTON_DOWN) delta = -1;
        if (delta != 0) patch_step(state, delta);
    } else if (!btn_a && !btn_b) {
        if ((pressed & BUTTON_UP) && state->patch_row > 0) state->patch_row--;
        if ((pressed & BUTTON_DOWN) && state->patch_row < UI_PATCH_NUM_ROWS - 1) state->patch_row++;
        if ((pressed & BUTTON_LEFT) && state->patch_op > 0) state->patch_op--;
        if ((pressed & BUTTON_RIGHT) && state->patch_op < 3) state->patch_op++;
    }
}

void input_mapper_init(input_tracker_state_t *state) {
    if (!state) return;
    state->cursor_row = 0;
    state->cursor_channel = 0;
    state->cursor_subcol = RDJ_SUBCOL_NOTE;
    state->octave = 4;
    state->is_playing = 0;
    state->last_joy = 0;

    state->screen = UI_SCREEN_PAT;
    state->sidebar_focus = 0;
    state->sidebar_tab = UI_TAB_PAT;
    state->song_cursor = 0;
    state->play_order_pos = 0;
    state->edit_pattern = 0;
    state->patch_slot = 0;
    state->patch_row = 0;
    state->patch_op = 0;
    state->start_consumed = 0;

    s_audition_chan = 0xFF;
}

void input_mapper_update(input_tracker_state_t *state, rdj_song_t *song, uint16_t joy_state) {
    uint16_t pressed;
    bool btn_a;
    bool btn_b;

    if (!state || !song) return;

    pressed = joy_state & ~state->last_joy;
    btn_a = (joy_state & BUTTON_A) != 0;
    btn_b = (joy_state & BUTTON_B) != 0;

    /* ---- Shortcut mode: START + LEFT/RIGHT cycles the active screen ---- */
    if ((joy_state & BUTTON_START) && (pressed & (BUTTON_LEFT | BUTTON_RIGHT))) {
        state->screen = (pressed & BUTTON_RIGHT) ? next_screen(state->screen)
                                                 : prev_screen(state->screen);
        state->start_consumed = 1;
        state->sidebar_focus = 0;
        state->sidebar_tab = state->screen;
    }
    /* 6-button pads: MODE cycles the screen on its own. */
    if (pressed & BUTTON_MODE) {
        state->screen = next_screen(state->screen);
        state->sidebar_focus = 0;
        state->sidebar_tab = state->screen;
    }

    /* ---- START alone (no direction while held): toggle Play/Stop on release ---- */
    if (!(joy_state & BUTTON_START) && (state->last_joy & BUTTON_START)) {
        if (!state->start_consumed) state->is_playing = !state->is_playing;
        state->start_consumed = 0;
    }
    if (!(joy_state & BUTTON_START) && !(state->last_joy & BUTTON_START)) {
        state->start_consumed = 0;
    }

    /* ---- Per-screen editing ---- */
    switch (state->screen) {
        case UI_SCREEN_SONG:  handle_song(state, song, pressed, btn_a, btn_b);  break;
        case UI_SCREEN_PATCH: handle_patch(state, song, pressed, btn_a, btn_b); break;
        default:              handle_pat(state, song, pressed, btn_a, btn_b);   break;
    }

    /* BUTTON_C (Genesis only): audition while held. */
    if (pressed & BUTTON_C) {
        uint8_t ch = (state->screen == UI_SCREEN_PATCH) ? PATCH_AUD_CH : state->cursor_channel;
        uint8_t inst = (state->screen == UI_SCREEN_PATCH) ? state->patch_slot
                                                          : song->patterns[state->edit_pattern]
                                                                 .rows[state->cursor_row][state->cursor_channel].instrument;
        uint8_t note = (state->screen == UI_SCREEN_PATCH)
                       ? (uint8_t)(state->octave * 12 + 12)
                       : audition_pitch(state, &song->patterns[state->edit_pattern]
                                                      .rows[state->cursor_row][state->cursor_channel]);
        audition_note_on_held(song, ch, note, inst);
    } else if ((state->last_joy & BUTTON_C) && !(joy_state & BUTTON_C)) {
        audition_note_off(song, s_audition_chan == 0xFF ? state->cursor_channel : s_audition_chan);
        s_audition_chan = 0xFF;
    }

    /* Release an A/B/C audition once the held button comes back up. */
    if (s_audition_chan != 0xFF &&
        !(joy_state & (BUTTON_A | BUTTON_B | BUTTON_C))) {
        audition_note_off(song, s_audition_chan);
        s_audition_chan = 0xFF;
    }

    state->last_joy = joy_state;
}

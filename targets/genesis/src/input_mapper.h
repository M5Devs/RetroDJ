/* RetroDJ - Genesis Input Mapper Header */
/* GPLv3 - RetroDJ Project */

#ifndef RDJ_GENESIS_INPUT_MAPPER_H
#define RDJ_GENESIS_INPUT_MAPPER_H

#if defined(RDJ_GENESIS_HARDWARE) || defined(SGDK_GCC) || defined(__m68k__)
    #include <genesis.h>
#else
    #include <stdint.h>
    #include <stddef.h>
    #include <stdbool.h>

    #ifndef BUTTON_UP
    #define BUTTON_UP    0x0001
    #define BUTTON_DOWN  0x0002
    #define BUTTON_LEFT  0x0004
    #define BUTTON_RIGHT 0x0008
    #define BUTTON_A     0x0040
    #define BUTTON_B     0x0010
    #define BUTTON_C     0x0020
    #define BUTTON_START 0x0080
    #define BUTTON_X     0x0400
    #define BUTTON_Y     0x0200
    #define BUTTON_Z     0x0100
    #define BUTTON_MODE  0x0800
    #endif
#endif

#include "core/sequencer.h"

#define RDJ_SUBCOL_NOTE 0
#define RDJ_SUBCOL_INST 1
#define RDJ_SUBCOL_VOL  2
#define RDJ_SUBCOL_FX   3
#define RDJ_NUM_SUBCOLS 4

/* Active editor screen. The sidebar tabs and the START+direction shortcut both
 * drive this state machine. */
typedef enum {
    UI_SCREEN_PAT = 0,   /* Main 6-channel pattern tracker */
    UI_SCREEN_SONG,      /* Song order / playlist sequence */
    UI_SCREEN_PATCH      /* 4-Operator YM2612 FM patch editor */
} ui_screen_mode_t;

#define UI_NUM_SCREENS 3

/* Sidebar tab indices (order matches the on-screen tab column). */
#define UI_TAB_PAT   0
#define UI_TAB_SONG  1
#define UI_TAB_PATCH 2

/* Patch editor parameter rows: three channel globals then seven per-operator
 * parameters (matching the on-screen matrix). */
#define UI_PATCH_ROW_ALGO  0
#define UI_PATCH_ROW_FB    1
#define UI_PATCH_ROW_PAN   2
#define UI_PATCH_ROW_DTMUL 3
#define UI_PATCH_ROW_TL    4
#define UI_PATCH_ROW_AR    5
#define UI_PATCH_ROW_DR    6
#define UI_PATCH_ROW_SR    7
#define UI_PATCH_ROW_SLRR  8
#define UI_PATCH_ROW_SSG   9
#define UI_PATCH_NUM_ROWS  10

typedef struct {
    uint8_t cursor_row;      /* 0..63 */
    uint8_t cursor_channel;  /* 0..5 */
    uint8_t cursor_subcol;   /* 0..3 */
    uint8_t octave;          /* 0..9, default 4 */
    uint8_t is_playing;      /* 0=Stop, 1=Play */
    uint16_t last_joy;       /* Previous joypad state */

    /* --- screen state machine --- */
    uint8_t screen;          /* ui_screen_mode_t */
    uint8_t sidebar_focus;   /* 0=grid, 1=sidebar tabs (cursor walk mode) */
    uint8_t sidebar_tab;     /* UI_TAB_* highlighted while in the sidebar */

    /* --- SONG screen --- */
    uint8_t song_cursor;     /* selected order slot 0..RDJ_MAX_PATTERNS-1 */
    uint8_t play_order_pos;  /* order index currently playing */
    uint8_t edit_pattern;    /* pattern the PAT screen edits */

    /* --- PATCH screen --- */
    uint8_t patch_slot;      /* instrument slot 0..YM2612_MAX_PATCHES-1 */
    uint8_t patch_row;       /* selected parameter row */
    uint8_t patch_op;        /* selected operator 0..3 for operator rows */

    /* START consumed as a screen-cycle modifier, so releasing it does not
     * also toggle playback. */
    uint8_t start_consumed;
} input_tracker_state_t;

void input_mapper_init(input_tracker_state_t *state);
void input_mapper_update(input_tracker_state_t *state, rdj_song_t *song, uint16_t joy_state);

#endif

/* RetroDJ - Game Boy Tracker UI Implementation */
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

#include "ui_gb.h"
#include "core/rdj_driver.h"
#include "core/rdj_backend.h"
#include "core/rdj_gb_inst.h"
#include "drivers/gb_sound/gb_rdj.h"

#include <stdio.h>
#include <string.h>

#if defined(__PORT_sm83) || defined(__TARGET_gb) || defined(GBDK)
#include <gb/gb.h>
#include <gb/cgb.h>
#include <gbdk/font.h>
#include <gbdk/console.h>
#endif

/* Screen geometry: 20x18 tiles. */
#define GB_COLS      20
#define GB_ROWS      18
#define GB_GRID_TOP  3
#define GB_GRID_ROWS 12
#define GB_VU_COL    18

/* CGB palette indices (colour 0..3 within a palette). */
#define C_BG     0
#define C_ACCENT 1
#define C_INK    2
#define C_CURSOR 3

/* Palette slots. The font writes its ink with attribute 0 (palette 0, colour 3
 * by default), so palette 0 holds the body text colours. Palette 1 carries the
 * cursor/highlight pair, and palettes 2/3 the accent colours used by the
 * headers, the VU strip and the [PLAY] indicator. */
#define PAL_TEXT    0
#define PAL_CURSOR  1
#define PAL_ACCENT  2
#define PAL_ALERT   3

/* Per-tile CGB background attribute: palette in bits 0-2. */
#define ATTR(pal) ((uint8_t)(pal))

static rdj_song_t g_song;
static input_tracker_state_t g_tracker_state;

static uint8_t s_tick_counter = 0;
static uint8_t s_current_play_row = 0;
static uint8_t s_prev_is_playing = 0;

/* Last rendered state, so ui_render only repaints what changed. */
static uint8_t s_last_screen = 0xFF;
static uint8_t s_last_cursor_row = 0;
static uint8_t s_last_cursor_chan = 0;
static uint8_t s_last_cursor_subcol = 0;
static uint8_t s_last_start_row = 0;
static uint8_t s_last_playing = 0;
static uint8_t s_last_bpm = 0;
static uint8_t s_last_song_cursor = 0xFF;
static uint8_t s_last_inst_slot = 0xFF;
static uint8_t s_last_inst_row = 0xFF;
static gb_inst_t s_last_inst;
static uint8_t s_last_row_notes[4];
static uint8_t s_last_edit_pattern = 0xFF;
static uint8_t s_vu_peak[4] = {0, 0, 0, 0};

static void gb_handle_inst(input_tracker_state_t *state, rdj_song_t *song,
                           uint16_t pressed, uint8_t btn_a, uint8_t btn_b);

/* True when any note on the cursor row changed since the last repaint, so an
 * A/B edit repaints even though the cursor itself did not move. */
static uint8_t row_notes_changed(void) {
    uint8_t i;
    for (i = 0; i < 4; i++) {
        if (s_last_row_notes[i] != g_song.patterns[g_tracker_state.edit_pattern]
                                          .rows[g_tracker_state.cursor_row][i].note) {
            return 1;
        }
    }
    return 0;
}

/* Channel labels used across the PAT grid and the VU strip. */
static const char *const s_chan_name[4] = {"P1", "P2", "WV", "NO"};

static char hex_char(uint8_t val) {
    val &= 0x0F;
    return (val < 10) ? (char)('0' + val) : (char)('A' + (val - 10));
}

void get_note_name(uint8_t note, char *out) {
    uint8_t note_idx;
    int octave;
    static const char note_names[12][2] = {
        {'C', '-'}, {'C', '#'}, {'D', '-'}, {'D', '#'},
        {'E', '-'}, {'F', '-'}, {'F', '#'}, {'G', '-'},
        {'G', '#'}, {'A', '-'}, {'A', '#'}, {'B', '-'}
    };

    if (note == RDJ_NOTE_EMPTY || note == 0) {
        out[0] = '-'; out[1] = '-'; out[2] = '-'; out[3] = '\0';
        return;
    }
    if (note == RDJ_NOTE_OFF) {
        out[0] = 'O'; out[1] = 'F'; out[2] = 'F'; out[3] = '\0';
        return;
    }
    note_idx = (uint8_t)((note - 12) % 12);
    octave = (note - 12) / 12;
    if (octave < 0) octave = 0;
    if (octave > 9) octave = 9;

    out[0] = note_names[note_idx][0];
    out[1] = note_names[note_idx][1];
    out[2] = (char)('0' + octave);
    out[3] = '\0';
}

/* Compact 3-char cell: "C 3" for naturals, "C#3" for accidentals, "---" empty.
 * The trailing space of a natural keeps the grid perfectly aligned. */
static void format_cell(uint8_t note, char *out) {
    char name[4];
    get_note_name(note, name);
    if (name[0] == '-') { out[0] = '-'; out[1] = '-'; out[2] = '-'; }
    else if (name[1] == '-') { out[0] = name[0]; out[1] = ' '; out[2] = name[2]; }
    else { out[0] = name[0]; out[1] = name[1]; out[2] = name[2]; }
    out[3] = '\0';
}

/* ---- screen state ---- */

uint8_t ui_get_screen(void) {
    return g_tracker_state.screen;
}

/* ---- palette ---- */

#if defined(__PORT_sm83) || defined(__TARGET_gb) || defined(GBDK)
/* High-contrast LSDj-style CGB palettes. Each palette holds four colours used
 * as [background, accent, ink, cursor] per the C_* indices above. */
static void ui_palette_init(void) {
    palette_color_t pal[4][4];

    /* Palette 0: body text. Pale background, cyan accents, dark navy ink. */
    pal[PAL_TEXT][C_BG]     = RGB8(232, 236, 244);
    pal[PAL_TEXT][C_ACCENT] = RGB8(24, 144, 255);
    pal[PAL_TEXT][C_INK]    = RGB8(20, 25, 45);
    pal[PAL_TEXT][C_CURSOR] = RGB8(20, 25, 45);

    /* Palette 1: active cursor row/cell. Amber on dark navy. */
    pal[PAL_CURSOR][C_BG]     = RGB8(20, 25, 45);
    pal[PAL_CURSOR][C_ACCENT] = RGB8(255, 176, 0);
    pal[PAL_CURSOR][C_INK]    = RGB8(255, 220, 120);
    pal[PAL_CURSOR][C_CURSOR] = RGB8(255, 176, 0);

    /* Palette 2: headers and the VU strip. Electric cyan on pale. */
    pal[PAL_ACCENT][C_BG]     = RGB8(232, 236, 244);
    pal[PAL_ACCENT][C_ACCENT] = RGB8(24, 144, 255);
    pal[PAL_ACCENT][C_INK]    = RGB8(24, 144, 255);
    pal[PAL_ACCENT][C_CURSOR] = RGB8(0, 90, 180);

    /* Palette 3: alerts, [PLAY] and peak VU segments. Ruby red on pale. */
    pal[PAL_ALERT][C_BG]     = RGB8(232, 236, 244);
    pal[PAL_ALERT][C_ACCENT] = RGB8(220, 40, 60);
    pal[PAL_ALERT][C_INK]    = RGB8(220, 40, 60);
    pal[PAL_ALERT][C_CURSOR] = RGB8(150, 20, 30);

    set_bkg_palette(PAL_TEXT, 4, (const palette_color_t *)pal);
}

/* Paint a whole row with one palette. */
static void ui_attr_row(uint8_t y, uint8_t pal) {
    uint8_t row[GB_COLS];
    uint8_t i;
    for (i = 0; i < GB_COLS; i++) row[i] = ATTR(pal);
    set_bkg_attributes(0, y, GB_COLS, 1, row);
}

/* Paint one tile with a palette. */
static void ui_attr_tile(uint8_t x, uint8_t y, uint8_t pal) {
    uint8_t a = ATTR(pal);
    set_bkg_attributes(x, y, 1, 1, &a);
}

/* Write a string and tint it in one call. */
static void ui_puts(uint8_t x, uint8_t y, const char *s, uint8_t pal) {
    gotoxy(x, y);
    printf("%s", s);
    if (pal != PAL_TEXT) ui_attr_row(y, pal);
}

/* Write a string to a fixed-width field, padding with spaces so a shorter
 * string never leaves the tail of the previous value on screen. */
static void ui_putf(uint8_t x, uint8_t y, const char *s, uint8_t width, uint8_t pal) {
    char buf[GB_COLS + 1];
    uint8_t i;
    for (i = 0; i < width && s[i]; i++) buf[i] = s[i];
    for (; i < width; i++) buf[i] = ' ';
    buf[width] = '\0';
    gotoxy(x, y);
    printf("%s", buf);
    if (pal != PAL_TEXT) {
        for (i = 0; i < width; i++) ui_attr_tile((uint8_t)(x + i), y, pal);
    }
}
#endif

/* ---- PAT screen ---- */

static void format_pat_status(char *out) {
    uint16_t bpm = g_song.bpm;
    uint8_t pat = g_tracker_state.edit_pattern;
    /* 20 columns: "BPM:120 [PLAY] P:01" */
    out[0] = 'B'; out[1] = 'P'; out[2] = 'M'; out[3] = ':';
    out[4] = (char)('0' + ((bpm / 100) % 10));
    out[5] = (char)('0' + ((bpm / 10) % 10));
    out[6] = (char)('0' + (bpm % 10));
    out[7] = ' ';
    out[8] = '[';
    if (g_tracker_state.is_playing) {
        out[9] = 'P'; out[10] = 'L'; out[11] = 'A'; out[12] = 'Y';
    } else {
        out[9] = 'S'; out[10] = 'T'; out[11] = 'O'; out[12] = 'P';
    }
    out[13] = ']';
    out[14] = ' ';
    out[15] = 'P'; out[16] = ':';
    out[17] = hex_char((uint8_t)(pat >> 4));
    out[18] = hex_char(pat);
    out[19] = '\0';
}

/* One grid row: "00|C 3|C 3|C 3|S 1" -> 2 + 4*4 = 18 columns, leaving 18-19
 * for the VU strip. */
static void format_pat_row(uint8_t r, char *out) {
    bool cursor_row = (r == g_tracker_state.cursor_row);
    uint8_t c;
    int pos = 0;

    out[pos++] = hex_char((uint8_t)(r >> 4));
    out[pos++] = hex_char(r);
    out[pos++] = cursor_row ? '>' : '|';

    for (c = 0; c < 4; c++) {
        char cell[4];
        format_cell(g_song.patterns[g_tracker_state.edit_pattern].rows[r][c].note, cell);
        out[pos++] = cell[0];
        out[pos++] = cell[1];
        out[pos++] = cell[2];
        /* No separator after the last channel: columns 18-19 are the VU strip. */
        if (c < 3) out[pos++] = '|';
    }
    out[pos] = '\0';
}

/* Column (in tiles) where a channel's cell starts. */
static uint8_t pat_chan_x(uint8_t c) { return (uint8_t)(3 + c * 4); }

static void draw_pat_chrome(void) {
    char buf[GB_COLS + 1];
    uint8_t i;

    format_pat_status(buf);
    ui_putf(0, 0, buf, GB_COLS, PAL_TEXT);
    ui_puts(0, 1, "--------------------", PAL_ACCENT);
    ui_puts(0, 2, "RW|P1 |P2 |WV |NO|VU", PAL_ACCENT);
    ui_puts(0, 16, "--------------------", PAL_ACCENT);
    ui_puts(0, 17, "A:Note B:Val ST:Play", PAL_TEXT);

    /* Clear the grid body so a fresh screen never shows stale cells. */
    for (i = 0; i < GB_GRID_ROWS; i++) ui_putf(0, (uint8_t)(GB_GRID_TOP + i), "", GB_COLS, PAL_TEXT);
}

/* Vertical mini VU meter for one channel, drawn in columns 18-19. A channel
 * with a live note shows a solid bar; peaks decay one segment per frame. */
static void draw_pat_vu(void) {
    uint8_t c, i;
    /* While playing, the meter must follow the row being sounded rather than the
     * edit cursor, which may be scrolled elsewhere. */
    uint8_t row = g_tracker_state.is_playing ? s_current_play_row
                                             : g_tracker_state.cursor_row;

    for (c = 0; c < 4; c++) {
        uint8_t note = g_song.patterns[g_tracker_state.edit_pattern].rows[row][c].note;
        uint8_t level = 0;
        char seg[3];

        if (note != RDJ_NOTE_EMPTY && note != RDJ_NOTE_OFF) {
            /* Map pitch to a 1..3 segment height so the bar visibly tracks the
             * note instead of being a binary on/off. */
            level = (uint8_t)(1 + (note % 3));
        }
        if (s_vu_peak[c] > level) level = s_vu_peak[c];
        s_vu_peak[c] = level;

        /* 12 grid rows split into four 3-row channel bands, two tiles wide. */
        for (i = 0; i < 3; i++) {
            uint8_t y = (uint8_t)(GB_GRID_TOP + c * 3 + i);
            uint8_t filled = (uint8_t)(level > i);
            seg[0] = filled ? ':' : '.';
            seg[1] = filled ? ':' : '.';
            seg[2] = '\0';
            ui_putf(GB_VU_COL, y, seg, 2, filled ? PAL_ALERT : PAL_ACCENT);
        }
    }
}

/* Repaint one visible grid row, including its cursor highlight. */
static void draw_pat_one_row(uint8_t r, uint8_t start_row) {
    char buf[GB_COLS + 1];
    uint8_t y = (uint8_t)(GB_GRID_TOP + (r - start_row));

    format_pat_row(r, buf);
    ui_putf(0, y, buf, GB_VU_COL, PAL_TEXT);

    if (r == g_tracker_state.cursor_row) {
        /* Highlight the whole cursor row, then re-tint the active cell in the
         * cursor palette so the edit position stands out. */
        ui_attr_row(y, PAL_CURSOR);
        ui_attr_tile(pat_chan_x(g_tracker_state.cursor_channel), y, PAL_CURSOR);
    } else {
        ui_attr_row(y, PAL_TEXT);
    }
}

static uint8_t pat_start_row(void) {
    uint8_t start_row = g_tracker_state.cursor_row >= 6
                        ? (uint8_t)(g_tracker_state.cursor_row - 6) : 0;
    if (start_row > RDJ_MAX_ROWS - GB_GRID_ROWS) start_row = RDJ_MAX_ROWS - GB_GRID_ROWS;
    return start_row;
}

/* Full repaint: chrome plus every visible row. */
static void draw_pat(void) {
    uint8_t i;

    draw_pat_chrome();
    s_last_start_row = pat_start_row();

    for (i = 0; i < GB_GRID_ROWS; i++) {
        draw_pat_one_row((uint8_t)(s_last_start_row + i), s_last_start_row);
    }
    draw_pat_vu();
}

/* ---- SONG screen ---- */

static void draw_song(void) {
    char buf[GB_COLS + 1];
    uint8_t i, col, row;

    ui_puts(0, 0, "--- SONG PLAYLIST --", PAL_ACCENT);

    /* "LEN: 08   POS: 00/08" */
    {
        uint8_t len = g_song.order_length ? g_song.order_length : 1;
        uint8_t pos = g_tracker_state.song_cursor;
        buf[0] = 'L'; buf[1] = 'E'; buf[2] = 'N'; buf[3] = ':'; buf[4] = ' ';
        buf[5] = hex_char((uint8_t)(len >> 4)); buf[6] = hex_char(len);
        buf[7] = ' '; buf[8] = ' '; buf[9] = ' ';
        buf[10] = 'P'; buf[11] = 'O'; buf[12] = 'S'; buf[13] = ':'; buf[14] = ' ';
        buf[15] = hex_char((uint8_t)(pos >> 4)); buf[16] = hex_char(pos);
        buf[17] = '/'; buf[18] = hex_char((uint8_t)(len >> 4)); buf[19] = hex_char(len);
        buf[20] = '\0';
        ui_putf(0, 1, buf, GB_COLS, PAL_TEXT);
    }

    ui_puts(0, 2, "--------------------", PAL_ACCENT);

    /* Two columns of four slots, 8 total: rows 3..6. */
    for (i = 0; i < 8; i++) {
        uint8_t slot = i;
        uint8_t pat = (slot < g_song.order_length) ? g_song.order[slot] : 0;
        bool used = slot < g_song.order_length;

        col = (uint8_t)((i >= 4) ? 10 : 0);
        row = (uint8_t)(3 + (i % 4));

        buf[0] = hex_char((uint8_t)(slot >> 4));
        buf[1] = hex_char(slot);
        buf[2] = ':'; buf[3] = 'P';
        buf[4] = used ? hex_char((uint8_t)(pat >> 4)) : '-';
        buf[5] = used ? hex_char(pat) : '-';
        buf[6] = ' '; buf[7] = ' '; buf[8] = ' ';
        buf[9] = '\0';

        ui_putf(col, row, buf, 9, PAL_TEXT);
        if (slot == g_tracker_state.song_cursor) {
            uint8_t k;
            for (k = 0; k < 9; k++) ui_attr_tile((uint8_t)(col + k), row, PAL_CURSOR);
        }
    }

    /* Pattern preview for the selected slot. */
    {
        uint8_t slot = g_tracker_state.song_cursor;
        uint8_t pat = (slot < g_song.order_length) ? g_song.order[slot] : 0;
        uint8_t r;
        char cell[4];

        ui_puts(0, 8, "--------------------", PAL_ACCENT);
        ui_putf(0, 9, "PREVIEW", 7, PAL_ACCENT);

        for (r = 0; r < 4; r++) {
            int pos = 0;
            buf[pos++] = hex_char((uint8_t)(r >> 4));
            buf[pos++] = hex_char(r);
            buf[pos++] = '|';
            for (i = 0; i < 4; i++) {
                format_cell(g_song.patterns[pat].rows[r][i].note, cell);
                buf[pos++] = cell[0];
                buf[pos++] = cell[1];
                buf[pos++] = cell[2];
                buf[pos++] = '|';
            }
            buf[pos] = '\0';
            ui_putf(0, (uint8_t)(10 + r), buf, GB_VU_COL, PAL_TEXT);
        }
    }

    ui_puts(0, 16, "--------------------", PAL_ACCENT);
    ui_puts(0, 17, "A:PAT B:LEN ST:Play", PAL_TEXT);
}

/* ---- INST screen ---- */

static void format_inst_row(uint8_t row, char *out) {
    gb_inst_t *in = gb_sound_inst(g_tracker_state.patch_slot);
    int pos = 0;

    switch (row) {
        case GB_INST_ROW_CHANNEL: {
            static const char *const names[3] = {"PULSE", "WAVE", "NOISE"};
            const char *n = names[in->channel % 3];
            const char *p = "CHANNEL: ";
            while (*p) out[pos++] = *p++;
            while (*n) out[pos++] = *n++;
            break;
        }
        case GB_INST_ROW_DUTY: {
            uint8_t pc = gb_inst_duty_percent(in->duty);
            const char *p = "DUTY   : ";
            while (*p) out[pos++] = *p++;
            out[pos++] = (char)('0' + (pc / 10));
            out[pos++] = (char)('0' + (pc % 10));
            out[pos++] = '%';
            break;
        }
        case GB_INST_ROW_VOL: {
            const char *p = "ENV VOL: ";
            while (*p) out[pos++] = *p++;
            out[pos++] = hex_char((uint8_t)(in->volume >> 4));
            out[pos++] = hex_char(in->volume);
            break;
        }
        case GB_INST_ROW_DIR: {
            const char *p = "ENV DIR: ";
            const char *d = in->env_dir ? "UP" : "DOWN";
            while (*p) out[pos++] = *p++;
            while (*d) out[pos++] = *d++;
            break;
        }
        case GB_INST_ROW_ENVPACE: {
            const char *p = "ENV PAC: ";
            while (*p) out[pos++] = *p++;
            out[pos++] = hex_char(in->env_pace);
            break;
        }
        case GB_INST_ROW_SWEEP: {
            const char *p = "SWP TIM: ";
            while (*p) out[pos++] = *p++;
            out[pos++] = hex_char(in->sweep);
            break;
        }
        case GB_INST_ROW_WAVE: {
            const char *p = "WAVE   : ";
            while (*p) out[pos++] = *p++;
            out[pos++] = hex_char(in->wave);
            break;
        }
        case GB_INST_ROW_NOISE: {
            const char *p = "NOISE  : ";
            const char *d = in->noise ? "7BIT" : "15BIT";
            while (*p) out[pos++] = *p++;
            while (*d) out[pos++] = *d++;
            break;
        }
        default:
            break;
    }
    out[pos] = '\0';
}

static void draw_inst(void) {
    char buf[GB_COLS + 1];
    uint8_t i, row;
    static const char *const chan_names[3] = {"PULSE 1", "WAVE", "NOISE"};

    ui_puts(0, 0, "-- GB INST EDITOR --", PAL_ACCENT);

    /* "INST: 00 [PULSE 1]" */
    {
        gb_inst_t *in = gb_sound_inst(g_tracker_state.patch_slot);
        const char *n = chan_names[in->channel % 3];
        int pos = 0;
        const char *p = "INST: ";
        while (*p) buf[pos++] = *p++;
        buf[pos++] = hex_char((uint8_t)(g_tracker_state.patch_slot >> 4));
        buf[pos++] = hex_char(g_tracker_state.patch_slot);
        buf[pos++] = ' ';
        buf[pos++] = '[';
        while (*n) buf[pos++] = *n++;
        buf[pos++] = ']';
        buf[pos] = '\0';
        ui_putf(0, 1, buf, GB_COLS, PAL_TEXT);
    }

    ui_puts(0, 2, "--------------------", PAL_ACCENT);

    for (i = 0; i < GB_INST_NUM_PARAMS; i++) {
        row = (uint8_t)(3 + i);
        format_inst_row(i, buf);
        ui_putf(0, row, buf, GB_VU_COL, PAL_TEXT);
        if (i == g_tracker_state.patch_row) {
            ui_attr_row(row, PAL_CURSOR);
        }
    }

    /* Wave preview for wave instruments: the 32 samples as a bar graph. */
    {
        gb_inst_t *in = gb_sound_inst(g_tracker_state.patch_slot);
        uint8_t wave[16];
        uint8_t s;
        gb_inst_wave_data(in->wave, wave);
        for (s = 0; s < 16; s++) {
            uint8_t hi = (uint8_t)((wave[s] >> 4) & 0x0F);
            buf[s] = (hi > 7) ? '#' : ((hi > 3) ? '+' : '.');
        }
        buf[16] = '\0';
        ui_putf(0, 12, buf, 16, PAL_ACCENT);
    }

    ui_puts(0, 16, "--------------------", PAL_ACCENT);
    ui_puts(0, 17, "A:Audit B:Val SEL+<>", PAL_TEXT);
}

/* ---- frame driver ---- */

void ui_init(void) {
    int r, c;

    g_song.bpm = 120;
    g_song.speed = 6;
    g_song.order_length = 1;
    for (r = 0; r < RDJ_MAX_ORDER; r++) g_song.order[r] = 0;

    for (r = 0; r < RDJ_MAX_ROWS; r++) {
        for (c = 0; c < RDJ_MAX_CHANNELS; c++) {
            g_song.patterns[0].rows[r][c].note = RDJ_NOTE_EMPTY;
            g_song.patterns[0].rows[r][c].instrument = 0;
            g_song.patterns[0].rows[r][c].volume = 0;
            g_song.patterns[0].rows[r][c].fx_cmd = 0;
            g_song.patterns[0].rows[r][c].fx_val = 0;
        }
    }
    g_song.patterns[0].length = 32;
    g_song.patterns[0].speed = 6;

    /* Audition/edit notes through the Game Boy APU driver. */
    g_song.driver = &gb_sound_driver;

    input_mapper_init(&g_tracker_state);
    /* Route the shared third screen to this target's APU instrument editor. */
    {
        static const input_mapper_screen_ops_t ops = { gb_handle_inst };
        input_mapper_set_screen_ops(&ops);
    }

    s_tick_counter = 0;
    s_current_play_row = 0;
    s_prev_is_playing = 0;
    s_last_screen = 0xFF;
    s_last_cursor_row = 0;
    s_last_cursor_chan = 0;
    s_last_cursor_subcol = 0;
    s_last_start_row = 0;
    s_last_playing = 0;
    s_last_bpm = 0;
    s_last_song_cursor = 0xFF;
    s_last_inst_slot = 0xFF;
    s_last_inst_row = 0xFF;

#if defined(__PORT_sm83) || defined(__TARGET_gb) || defined(GBDK)
    font_init();
    font_color(3, 0);
    font_set(font_load(font_ibm));

    /* The tracker paints rows out of order, so the console must not scroll. */
    mode(M_NO_SCROLL);

    SHOW_BKG;
    DISPLAY_ON;

    /* Monochrome DMG: standard 4-shade contrast. */
    BGP_REG = 0xE4;

    if (_cpu == CGB_TYPE) {
        ui_palette_init();
    }
#endif
}

static void ui_trigger_row_notes(uint8_t row) {
    uint8_t ch;
    uint8_t pat = g_tracker_state.edit_pattern;
    for (ch = 0; ch < 4; ch++) {
        rdj_row_t *r = &g_song.patterns[pat].rows[row][ch];
        if (r->note != RDJ_NOTE_EMPTY && r->note != RDJ_NOTE_OFF) {
            gb_sound_driver.note_on(ch, r->note, r->instrument, r->volume);
        } else if (r->note == RDJ_NOTE_OFF) {
            gb_sound_driver.note_off(ch);
        }
    }
}

void ui_process_playback(void) {
    uint8_t speed, pat_len, ch;
    uint8_t pat = g_tracker_state.edit_pattern;

    if (g_tracker_state.is_playing) {
        if (!s_prev_is_playing) {
            s_prev_is_playing = 1;
            s_tick_counter = 0;
            s_current_play_row = g_tracker_state.cursor_row;
            ui_trigger_row_notes(s_current_play_row);
        } else {
            s_tick_counter++;
            speed = g_song.patterns[pat].speed ? g_song.patterns[pat].speed
                                              : (g_song.speed ? g_song.speed : 6);
            if (s_tick_counter >= speed) {
                s_tick_counter = 0;
                pat_len = g_song.patterns[pat].length ? g_song.patterns[pat].length
                                                     : RDJ_MAX_ROWS;
                s_current_play_row++;
                if (s_current_play_row >= pat_len) {
                    /* Advance the order list, wrapping at the order length. */
                    s_current_play_row = 0;
                    if (g_song.order_length > 1) {
                        uint8_t pos = (uint8_t)(g_tracker_state.play_order_pos + 1);
                        if (pos >= g_song.order_length) pos = 0;
                        g_tracker_state.play_order_pos = pos;
                        g_tracker_state.edit_pattern = g_song.order[pos];
                    }
                }
                g_tracker_state.cursor_row = s_current_play_row;
                ui_trigger_row_notes(s_current_play_row);
            }
        }
    } else {
        if (s_prev_is_playing) {
            s_prev_is_playing = 0;
            s_tick_counter = 0;
            for (ch = 0; ch < 4; ch++) {
                gb_sound_driver.note_off(ch);
            }
        }
    }
}

static uint16_t gb_read_controller(void) {
    uint16_t buttons = 0;
#if defined(__PORT_sm83) || defined(__TARGET_gb) || defined(GBDK)
    uint8_t joy = joypad();
    if (joy & J_A)      buttons |= BUTTON_A;
    if (joy & J_B)      buttons |= BUTTON_B;
    if (joy & J_START)  buttons |= BUTTON_START;
    if (joy & J_SELECT) buttons |= BUTTON_SELECT;
    if (joy & J_UP)     buttons |= BUTTON_UP;
    if (joy & J_DOWN)   buttons |= BUTTON_DOWN;
    if (joy & J_LEFT)   buttons |= BUTTON_LEFT;
    if (joy & J_RIGHT)  buttons |= BUTTON_RIGHT;
#endif
    return buttons;
}

/* [INST] screen editing, installed into the shared mapper so SELECT+direction
 * navigation keeps working while this screen is active. */
static void gb_handle_inst(input_tracker_state_t *state, rdj_song_t *song,
                           uint16_t pressed, uint8_t btn_a, uint8_t btn_b) {
    gb_inst_t *in = gb_sound_inst(state->patch_slot);
    (void)song;

    if (btn_a && !btn_b) {
        if (pressed & BUTTON_LEFT) {
            if (state->patch_slot > 0) state->patch_slot--;
            gb_sound_driver.note_on(0, (uint8_t)(state->octave * 12 + 12),
                                    state->patch_slot, 128);
        }
        if (pressed & BUTTON_RIGHT) {
            if (state->patch_slot < GB_INST_COUNT - 1) state->patch_slot++;
            gb_sound_driver.note_on(0, (uint8_t)(state->octave * 12 + 12),
                                    state->patch_slot, 128);
        }
        /* A tap auditions the instrument on its own channel. */
        if (!(pressed & (BUTTON_LEFT | BUTTON_RIGHT | BUTTON_UP | BUTTON_DOWN))) {
            gb_sound_driver.note_on(in->channel == GB_INST_CH_WAVE ? 2 :
                                    (in->channel == GB_INST_CH_NOISE ? 3 : 0),
                                    (uint8_t)(state->octave * 12 + 12),
                                    state->patch_slot, 128);
        }
    } else if (btn_b && !btn_a) {
        int delta = 0;
        if (pressed & BUTTON_UP) delta = 1;
        if (pressed & BUTTON_DOWN) delta = -1;
        if (delta != 0) {
            gb_inst_step(in, state->patch_row, delta);
            /* Audition the change immediately on the instrument's own channel. */
            gb_sound_driver.note_on(in->channel == GB_INST_CH_WAVE ? 2 :
                                    (in->channel == GB_INST_CH_NOISE ? 3 : 0),
                                    (uint8_t)(state->octave * 12 + 12),
                                    state->patch_slot, 128);
        }
    } else if (!btn_a && !btn_b) {
        if ((pressed & BUTTON_UP) && state->patch_row > 0) state->patch_row--;
        if ((pressed & BUTTON_DOWN) && state->patch_row < GB_INST_NUM_PARAMS - 1) state->patch_row++;
    }
}

void ui_update(void) {
    uint16_t joy = gb_read_controller();
    input_mapper_update(&g_tracker_state, &g_song, joy);
    ui_process_playback();
}

void ui_render(void) {
#if defined(__PORT_sm83) || defined(__TARGET_gb) || defined(GBDK)
    char buf[GB_COLS + 1];
    uint8_t screen = g_tracker_state.screen;
    uint8_t i;

    if (screen != s_last_screen) {
        s_last_screen = screen;
        cls();
        switch (screen) {
            case UI_SCREEN_SONG: draw_song(); break;
            case UI_SCREEN_INST:
                memcpy(&s_last_inst, gb_sound_inst(g_tracker_state.patch_slot),
                       sizeof(gb_inst_t));
                draw_inst();
                break;
            default:             draw_pat();  break;
        }
        s_last_cursor_row = g_tracker_state.cursor_row;
        s_last_cursor_chan = g_tracker_state.cursor_channel;
        s_last_cursor_subcol = g_tracker_state.cursor_subcol;
        s_last_edit_pattern = g_tracker_state.edit_pattern;
        for (i = 0; i < 4; i++) {
            s_last_row_notes[i] = g_song.patterns[g_tracker_state.edit_pattern]
                                        .rows[g_tracker_state.cursor_row][i].note;
        }
        s_last_playing = g_tracker_state.is_playing;
        s_last_bpm = g_song.bpm;
        s_last_song_cursor = g_tracker_state.song_cursor;
        s_last_inst_slot = g_tracker_state.patch_slot;
        s_last_inst_row = g_tracker_state.patch_row;
        return;
    }

    switch (screen) {
        case UI_SCREEN_SONG:
            if (g_tracker_state.song_cursor != s_last_song_cursor) {
                s_last_song_cursor = g_tracker_state.song_cursor;
                draw_song();
            }
            break;

        case UI_SCREEN_INST: {
            /* Repaint when the selection moves *or* any parameter value changed,
             * otherwise B+direction edits the instrument invisibly. */
            gb_inst_t *cur = gb_sound_inst(g_tracker_state.patch_slot);
            if (g_tracker_state.patch_slot != s_last_inst_slot ||
                g_tracker_state.patch_row != s_last_inst_row ||
                memcmp(cur, &s_last_inst, sizeof(gb_inst_t)) != 0) {
                s_last_inst_slot = g_tracker_state.patch_slot;
                s_last_inst_row = g_tracker_state.patch_row;
                memcpy(&s_last_inst, cur, sizeof(gb_inst_t));
                draw_inst();
            }
            break;
        }

        default: {
            uint8_t start_row;

            if (g_tracker_state.is_playing != s_last_playing ||
                g_song.bpm != s_last_bpm) {
                s_last_playing = g_tracker_state.is_playing;
                s_last_bpm = g_song.bpm;
                format_pat_status(buf);
                ui_putf(0, 0, buf, GB_COLS, PAL_TEXT);
            }

            start_row = g_tracker_state.cursor_row >= 6
                        ? (uint8_t)(g_tracker_state.cursor_row - 6) : 0;
            if (start_row > RDJ_MAX_ROWS - GB_GRID_ROWS) start_row = RDJ_MAX_ROWS - GB_GRID_ROWS;

            if (start_row != s_last_start_row) {
                /* The window scrolled: every row moved. */
                draw_pat();
            } else if (g_tracker_state.cursor_row != s_last_cursor_row ||
                       g_tracker_state.cursor_channel != s_last_cursor_chan ||
                       g_tracker_state.cursor_subcol != s_last_cursor_subcol ||
                       g_tracker_state.edit_pattern != s_last_edit_pattern ||
                       row_notes_changed()) {
                /* Only the old and new cursor rows changed. Repainting the whole
                 * grid here would cost more than a frame on the GB and starve
                 * the joypad read, which made START unresponsive during
                 * playback. */
                draw_pat_one_row(s_last_cursor_row, start_row);
                draw_pat_one_row(g_tracker_state.cursor_row, start_row);
                draw_pat_vu();
            }
            s_last_cursor_row = g_tracker_state.cursor_row;
            s_last_cursor_chan = g_tracker_state.cursor_channel;
            s_last_cursor_subcol = g_tracker_state.cursor_subcol;
            s_last_edit_pattern = g_tracker_state.edit_pattern;
            for (i = 0; i < 4; i++) {
                s_last_row_notes[i] = g_song.patterns[g_tracker_state.edit_pattern]
                                             .rows[g_tracker_state.cursor_row][i].note;
            }
            break;
        }
    }
#endif
}

input_tracker_state_t *ui_get_tracker_state(void) {
    return &g_tracker_state;
}

rdj_song_t *ui_get_song(void) {
    return &g_song;
}

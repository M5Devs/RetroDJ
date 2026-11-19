/* RetroDJ - Sega Genesis VDP Text-Mode Tracker UI */
/* GPLv3 - RetroDJ Project */

#include "ui.h"
#include "input_mapper.h"

#if !defined(SGDK_GCC) && !defined(RDJ_GENESIS_HARDWARE) && !defined(__m68k__)
#include <stdio.h>
#include <string.h>
#endif

#if defined(RDJ_GENESIS_HARDWARE) || defined(SGDK_GCC) || defined(__m68k__)
#include <genesis.h>
#endif

#include "core/rdj_driver.h"
#include "core/sequencer.h"
#include "core/rdj_ym2612.h"

static rdj_song_t g_song;
static input_tracker_state_t g_tracker_state;

static uint8_t s_tick_counter = 0;
static uint8_t s_current_play_row = 0;
static uint8_t s_prev_is_playing = 0;
static uint8_t s_play_order_pos = 0;

void get_note_name(uint8_t note, char *out) {
    if (note == RDJ_NOTE_EMPTY || note == 0) {
        out[0] = '-'; out[1] = '-'; out[2] = '-'; out[3] = '\0';
        return;
    }
    if (note == RDJ_NOTE_OFF) {
        out[0] = 'O'; out[1] = 'F'; out[2] = 'F'; out[3] = '\0';
        return;
    }
    {
        static const char *note_names[12] = {
            "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"
        };
        uint8_t note_idx = (note - 12) % 12;
        int octave = (note - 12) / 12;
        if (octave < 0) octave = 0;
        if (octave > 9) octave = 9;
        sprintf(out, "%s%d", note_names[note_idx], octave);
    }
}

#if defined(RDJ_GENESIS_HARDWARE) || defined(SGDK_GCC) || defined(__m68k__)

/* ---------------------------------------------------------------------------
 * 40 column x 28 row wide tracker layout
 *
 *   row 0        : centred title
 *   row 2        : status line (BPM / transport / octave / CPU bar)
 *   row 3        : divider
 *   row 4        : matrix header (cols 0..31) + HUD header (cols 32..39)
 *   rows 5..24   : 6-channel FM matrix | live mixer HUD
 *   row 26       : divider
 *   row 27       : control hints
 * ------------------------------------------------------------------------- */
#define UI_COLS        40
#define UI_ROWS        28

#define HUD_X          32                  /* cols 32..39: mixer HUD         */

#define HDR_TITLE_ROW  0
#define HDR_STATUS_ROW 2
#define HDR_DIV_ROW    3
#define MATRIX_HDR_ROW 4
#define GRID_ROW0      5
#define GRID_ROWS      20                  /* rows 5..24                     */
#define FOOT_DIV_ROW   26
#define FOOT_HELP_ROW  27

/* Palette entry indices (each VDP palette holds 16 colours) */
#define C_BACKDROP     0
#define C_HILITE       5
#define C_VU           6
#define C_INK          15

/* Tilemap palette selectors: bits 13-14 choose PAL0..PAL3 */
#define ATTR_WHITE     ((u16)(PAL0 << 13)) /* index 15 = crisp white         */
#define ATTR_CYAN      ((u16)(PAL1 << 13)) /* index 15 = electric cyan       */
#define ATTR_AMBER     ((u16)(PAL2 << 13)) /* index 15 = warm amber          */
#define ATTR_RED       ((u16)(PAL3 << 13)) /* index 15 = ruby red            */

/* User tiles: a solid amber block (cursor box) and a solid green block (VU) */
#define TILE_HILITE    (TILE_USER_INDEX + 0)
#define TILE_VU        (TILE_USER_INDEX + 1)

static u16 s_screen[UI_COLS * UI_ROWS];    /* per-frame BG_A tilemap image   */
static u16 s_frame = 0;
static uint8_t s_prev_cr = 0xFF, s_prev_cc = 0xFF;
static uint8_t s_prev_play = 0xFF, s_prev_pr = 0xFF;
static uint8_t s_prev_screen = 0xFF, s_prev_focus = 0xFF, s_prev_tab = 0xFF;
static uint8_t s_prev_songcur = 0xFF, s_prev_patchslot = 0xFF;
static uint8_t s_prev_patchrow = 0xFF, s_prev_patchop = 0xFF, s_prev_editpat = 0xFF;

static uint8_t s_grid_start = 0;           /* first pattern row on screen    */
static uint8_t s_hl_x = 0xFF;              /* BG_B highlight x, 0xFF = off   */
static uint8_t s_hl_y = 0;

static const u32 s_tile_hilite[8] = {
    0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555
};
static const u32 s_tile_vu[8] = {
    0x66666666, 0x66666666, 0x66666666, 0x66666666,
    0x66666666, 0x66666666, 0x66666666, 0x66666666
};

static char hex_char(uint8_t v) {
    v &= 0x0F;
    return (v < 10) ? (char)('0' + v) : (char)('A' + (v - 10));
}

/* ---- tilemap composition helpers (write into s_screen) ---- */
static void cell_put(int x, int y, char c, u16 attr) {
    if (x < 0 || x >= UI_COLS || y < 0 || y >= UI_ROWS) return;
    s_screen[(y * UI_COLS) + x] = (u16)(attr | (TILE_FONT_INDEX + (u16)(c - 32)));
}

static void tile_put(int x, int y, u16 tile, u16 attr) {
    if (x < 0 || x >= UI_COLS || y < 0 || y >= UI_ROWS) return;
    s_screen[(y * UI_COLS) + x] = (u16)(attr | tile);
}

static void str_put(int x, int y, const char *s, u16 attr) {
    while (*s) cell_put(x++, y, *s++, attr);
}

/* ---- palette / tile / plane setup ---- */
static void ui_palette_init(void) {
    u16 pal0[16], pal1[16], pal2[16], pal3[16];
    int i;

    for (i = 0; i < 16; i++) {
        pal0[i] = RGB3_3_3_TO_VDPCOLOR(1, 1, 3);   /* dark slate navy */
        pal1[i] = RGB3_3_3_TO_VDPCOLOR(1, 1, 3);
        pal2[i] = RGB3_3_3_TO_VDPCOLOR(1, 1, 3);
        pal3[i] = RGB3_3_3_TO_VDPCOLOR(1, 1, 3);
    }

    /* Palette 0 is the general plane palette: backdrop, the amber highlight
     * tile, the green VU tile and the default white ink. */
    pal0[C_BACKDROP] = RGB3_3_3_TO_VDPCOLOR(1, 1, 3);   /* 0x0204 dark navy */
    pal0[C_HILITE]   = RGB3_3_3_TO_VDPCOLOR(7, 4, 0);   /* warm amber       */
    pal0[C_VU]       = RGB3_3_3_TO_VDPCOLOR(0, 7, 1);   /* green level      */
    pal0[C_INK]      = RGB3_3_3_TO_VDPCOLOR(7, 7, 7);   /* crisp white      */

    pal1[C_INK]      = RGB3_3_3_TO_VDPCOLOR(0, 7, 7);   /* electric cyan    */
    pal2[C_INK]      = RGB3_3_3_TO_VDPCOLOR(7, 4, 0);   /* amber            */
    pal3[C_INK]      = RGB3_3_3_TO_VDPCOLOR(7, 0, 0);   /* ruby red         */

    PAL_setPalette(PAL0, pal0, CPU);
    PAL_setPalette(PAL1, pal1, CPU);
    PAL_setPalette(PAL2, pal2, CPU);
    PAL_setPalette(PAL3, pal3, CPU);

    VDP_setBackgroundColor(C_BACKDROP);
    VDP_setTextPalette(PAL0);
}

static void ui_load_custom_tiles(void) {
    VDP_loadTileData(s_tile_hilite, TILE_HILITE, 1, CPU);
    VDP_loadTileData(s_tile_vu, TILE_VU, 1, CPU);
}

static void ui_clear_bg_b(void) {
    int x, y;
    for (y = 0; y < UI_ROWS; y++) {
        for (x = 0; x < UI_COLS; x++) {
            VDP_setTileMapXY(BG_B, TILE_ATTR_FULL(PAL0, 0, 0, 0, TILE_FONT_INDEX), x, y);
        }
    }
    s_hl_x = 0xFF;
}

/* ---- live mixer data ---- */
static uint8_t hud_note(uint8_t ch) {
    int base = g_tracker_state.is_playing ? s_current_play_row : g_tracker_state.cursor_row;
    int d;
    uint8_t pat = g_tracker_state.is_playing
                  ? g_song.order[g_tracker_state.play_order_pos]
                  : g_tracker_state.edit_pattern;
    for (d = 0; d < 8; d++) {
        int r = base - d;
        while (r < 0) r += RDJ_MAX_ROWS;
        if (g_song.patterns[pat].rows[r][ch].note != RDJ_NOTE_EMPTY) {
            return g_song.patterns[pat].rows[r][ch].note;
        }
    }
    return RDJ_NOTE_EMPTY;
}

static uint8_t hud_level(uint8_t ch) {
    int base = g_tracker_state.is_playing ? s_current_play_row : g_tracker_state.cursor_row;
    int d;
    uint8_t pat = g_tracker_state.is_playing
                  ? g_song.order[g_tracker_state.play_order_pos]
                  : g_tracker_state.edit_pattern;
    for (d = 0; d < 8; d++) {
        int r = base - d;
        while (r < 0) r += RDJ_MAX_ROWS;
        {
            uint8_t n = g_song.patterns[pat].rows[r][ch].note;
            if (n == RDJ_NOTE_OFF) return 0;
            if (n != RDJ_NOTE_EMPTY) {
                int lv = 4 - (d / 2);
                return (uint8_t)(lv < 1 ? 1 : lv);
            }
        }
    }
    return 0;
}

/* ---- screen sections ---- */
static void ui_build_header(void) {
    char buf[16];
    int cpu, filled, i, x;

    switch (g_tracker_state.screen) {
        case UI_SCREEN_SONG:
            str_put(1, HDR_TITLE_ROW, "--- RetroDJ Genesis: Song Playlist ---", ATTR_CYAN);
            break;
        case UI_SCREEN_PATCH: {
            char t[41];
            sprintf(t, "--- YM2612 FM Patch Editor [INST %02X] ---",
                    g_tracker_state.patch_slot);
            str_put(0, HDR_TITLE_ROW, t, ATTR_CYAN);
            break;
        }
        default:
            str_put(6, HDR_TITLE_ROW, "--- RetroDJ Genesis v1.2 ---", ATTR_CYAN);
            break;
    }
    str_put(0, HDR_DIV_ROW, "========================================", ATTR_CYAN);

    /* BPM:130 [PLAY] OCT:4 CPU:[|||||||||] 42% */
    x = 0;
    str_put(x, HDR_STATUS_ROW, "BPM:", ATTR_WHITE);          x += 4;
    sprintf(buf, "%3d", g_song.bpm);
    str_put(x, HDR_STATUS_ROW, buf, ATTR_WHITE);             x += 3;
    cell_put(x++, HDR_STATUS_ROW, ' ', ATTR_WHITE);
    cell_put(x++, HDR_STATUS_ROW, '[', ATTR_WHITE);
    str_put(x, HDR_STATUS_ROW, g_tracker_state.is_playing ? "PLAY" : "STOP", ATTR_RED);
    x += 4;
    cell_put(x++, HDR_STATUS_ROW, ']', ATTR_WHITE);
    cell_put(x++, HDR_STATUS_ROW, ' ', ATTR_WHITE);
    str_put(x, HDR_STATUS_ROW, "OCT:", ATTR_WHITE);          x += 4;
    cell_put(x++, HDR_STATUS_ROW, (char)('0' + (g_tracker_state.octave % 10)), ATTR_WHITE);
    cell_put(x++, HDR_STATUS_ROW, ' ', ATTR_WHITE);
    str_put(x, HDR_STATUS_ROW, "CPU:[", ATTR_WHITE);         x += 5;

    cpu = (int)SYS_getCPULoad();
    if (cpu < 0) cpu = 0;
    if (cpu > 100) cpu = 100;
    filled = (cpu * 9) / 100;
    for (i = 0; i < 9; i++) {
        cell_put(x++, HDR_STATUS_ROW, (i < filled) ? '|' : '.', ATTR_WHITE);
    }
    cell_put(x++, HDR_STATUS_ROW, ']', ATTR_WHITE);
    sprintf(buf, "%3d%%", cpu);
    str_put(x, HDR_STATUS_ROW, buf, ATTR_WHITE);
}

/* ---- [PAT] pattern editor ---- */
static void ui_build_grid(void) {
    uint8_t cr = g_tracker_state.cursor_row;
    uint8_t start, i, c;
    char hdr[6];

    start = (cr >= (GRID_ROWS / 2)) ? (uint8_t)(cr - (GRID_ROWS / 2)) : 0;
    if (start > (RDJ_MAX_ROWS - GRID_ROWS)) start = (RDJ_MAX_ROWS - GRID_ROWS);
    s_grid_start = start;

    str_put(0, MATRIX_HDR_ROW, "R ", ATTR_CYAN);
    for (c = 0; c < 6; c++) {
        hdr[0] = 'F'; hdr[1] = 'M'; hdr[2] = (char)('1' + c);
        hdr[3] = ' '; hdr[4] = ' '; hdr[5] = '\0';
        str_put(2 + (c * 5), MATRIX_HDR_ROW, hdr, ATTR_CYAN);
    }

    for (i = 0; i < GRID_ROWS; i++) {
        uint8_t r = (uint8_t)(start + i);
        int y = GRID_ROW0 + i;
        int x = 0;
        bool is_cursor_row = (r == cr);

        cell_put(x++, y, hex_char(r >> 4), is_cursor_row ? ATTR_AMBER : ATTR_WHITE);
        cell_put(x++, y, hex_char(r),      is_cursor_row ? ATTR_AMBER : ATTR_WHITE);

        for (c = 0; c < 6; c++) {
            rdj_row_t *cell = &g_song.patterns[g_tracker_state.edit_pattern].rows[r][c];
            char nb[4];
            bool is_curr_cell = is_cursor_row && (g_tracker_state.cursor_channel == c);
            u16 attr = is_curr_cell ? ATTR_WHITE : (is_cursor_row ? ATTR_CYAN : ATTR_WHITE);

            get_note_name(cell->note, nb);
            cell_put(x++, y, nb[0], attr);
            cell_put(x++, y, nb[1], attr);
            cell_put(x++, y, nb[2], attr);
            cell_put(x++, y, hex_char(cell->instrument), attr);
            cell_put(x++, y, hex_char(cell->volume >> 4), attr);
        }
    }
}

/* Tab column shared by every screen: rows 5/6/7 hold [PAT]/[SONG]/[PATCH]. */
static void ui_build_tabs(void) {
    static const char *names[UI_NUM_SCREENS] = { "|[PAT]  ", "|[SONG] ", "|[PATCH]" };
    int t;
    for (t = 0; t < UI_NUM_SCREENS; t++) {
        u16 attr = ATTR_CYAN;
        if ((uint8_t)t == g_tracker_state.screen) attr = ATTR_AMBER;
        if (g_tracker_state.sidebar_focus && (uint8_t)t == g_tracker_state.sidebar_tab) {
            attr = ATTR_RED;   /* cursor-walk selection */
        }
        str_put(HUD_X, 5 + t, names[t], attr);
    }
}

static void ui_build_hud(void) {
    uint8_t c;
    int x = HUD_X;
    char line[12];

    str_put(x, MATRIX_HDR_ROW, "|HUD:MIX", ATTR_CYAN);
    ui_build_tabs();
    str_put(x, 8, "|-------", ATTR_CYAN);

    for (c = 0; c < 6; c++) {
        char nb[4];
        get_note_name(hud_note(c), nb);
        line[0] = '|';
        line[1] = (char)('1' + c);
        line[2] = ':';
        line[3] = nb[0]; line[4] = nb[1]; line[5] = nb[2];
        line[6] = ' '; line[7] = ' '; line[8] = '\0';
        str_put(x, 9 + c, line, ATTR_WHITE);
    }

    str_put(x, 15, "|-------", ATTR_CYAN);

    for (c = 0; c < 6; c++) {
        int y = 16 + c;
        uint8_t lv = hud_level(c);
        int b;
        cell_put(x, y, '|', ATTR_CYAN);
        cell_put(x + 1, y, (char)('1' + c), ATTR_CYAN);
        cell_put(x + 2, y, '|', ATTR_CYAN);
        for (b = 0; b < 4; b++) {
            if (b < (int)lv) tile_put(x + 3 + b, y, TILE_VU, ATTR_WHITE);
            else cell_put(x + 3 + b, y, ' ', ATTR_WHITE);
        }
        cell_put(x + 7, y, ' ', ATTR_WHITE);
    }

    str_put(x, 22, "|-------", ATTR_CYAN);

    {
        rdj_row_t *cell = &g_song.patterns[g_tracker_state.edit_pattern]
                                    .rows[g_tracker_state.cursor_row][g_tracker_state.cursor_channel];
        sprintf(line, "|I:%02X", cell->instrument);
        str_put(x, 23, line, ATTR_WHITE);
        sprintf(line, "|V:%02X", cell->volume);
        str_put(x, 24, line, ATTR_WHITE);
    }
}

/* ---- [SONG] playlist screen ---- */
static void ui_build_song(void) {
    uint8_t i;
    char line[24];
    int x, y;

    str_put(0, MATRIX_HDR_ROW, "SLOT PAT  SLOT PAT  SLOT PAT  SLOT PAT ", ATTR_CYAN);

    /* 16 slots (RDJ_MAX_PATTERNS) laid out as 4 columns x 4 rows. */
    for (i = 0; i < RDJ_MAX_PATTERNS; i++) {
        int col = i / 4;
        int row = i % 4;
        x = col * 10;
        y = GRID_ROW0 + row;
        {
            uint8_t sel = (i == g_tracker_state.song_cursor);
            uint8_t playing = g_tracker_state.is_playing &&
                              (i == g_tracker_state.play_order_pos);
            u16 attr = sel ? ATTR_AMBER : ATTR_WHITE;
            if (playing) attr = ATTR_RED;
            sprintf(line, "%02X:PAT%02X", i, g_song.order[i]);
            str_put(x, y, line, attr);
        }
    }

    str_put(0, 10, "----------------------------------------", ATTR_CYAN);

    sprintf(line, "ORDER LENGTH: %02X", g_song.order_length);
    str_put(0, 11, line, ATTR_WHITE);

    sprintf(line, "PLAY POS: %02X / %02X", g_tracker_state.play_order_pos,
            (uint8_t)(g_song.order_length ? g_song.order_length - 1 : 0));
    str_put(0, 12, line, g_tracker_state.is_playing ? ATTR_RED : ATTR_WHITE);

    sprintf(line, "EDIT PATTERN: %02X", g_tracker_state.edit_pattern);
    str_put(0, 13, line, ATTR_WHITE);

    str_put(0, 14, "PATTERN PREVIEW:", ATTR_CYAN);
    {
        uint8_t pat = g_song.order[g_tracker_state.song_cursor];
        for (i = 0; i < 10; i++) {
            rdj_row_t *r = &g_song.patterns[pat].rows[i][0];
            char nb[4];
            get_note_name(r->note, nb);
            sprintf(line, "%02X %s %02X %02X", i, nb, r->instrument, r->volume);
            str_put(0, 15 + i, line, ATTR_WHITE);
        }
    }
}

/* ---- [PATCH] FM synth editor ---- */
static const char *s_patch_row_names[UI_PATCH_NUM_ROWS] = {
    "ALGORITHM", "FEEDBACK ", "PAN      ",
    "DT/MUL   ", "TL       ", "AR       ", "DR       ",
    "SR       ", "SL/RR    ", "SSG      "
};

/* Operator parameter a given row edits, or -1 for a channel-global row. */
static int patch_row_is_op(uint8_t row) {
    return (row >= UI_PATCH_ROW_DTMUL);
}

static void patch_row_value(char *out, uint8_t row, uint8_t op) {
    ym2612_patch_t *p = ym2612_rdj_get_patch(g_tracker_state.patch_slot);
    if (!p) { out[0] = '?'; out[1] = '\0'; return; }
    switch (row) {
        case UI_PATCH_ROW_ALGO: sprintf(out, "%d", p->FB_ALG & 0x07); break;
        case UI_PATCH_ROW_FB:   sprintf(out, "%d", (p->FB_ALG >> 3) & 0x07); break;
        case UI_PATCH_ROW_PAN: {
            uint8_t pan = (uint8_t)((p->LR_AMS_PMS >> 6) & 0x03);
            out[0] = (pan & 0x02) ? 'L' : '-';
            out[1] = (pan & 0x01) ? 'R' : '-';
            out[2] = '\0';
            break;
        }
        case UI_PATCH_ROW_DTMUL:
            sprintf(out, "%d/%d", (p->ops[op].DT_MUL >> 4) & 0x07, p->ops[op].DT_MUL & 0x0F);
            break;
        case UI_PATCH_ROW_TL:   sprintf(out, "%03d", p->ops[op].TL); break;
        case UI_PATCH_ROW_AR:   sprintf(out, "%03d", p->ops[op].AR); break;
        case UI_PATCH_ROW_DR:   sprintf(out, "%03d", p->ops[op].DR); break;
        case UI_PATCH_ROW_SR:   sprintf(out, "%03d", p->ops[op].SR); break;
        case UI_PATCH_ROW_SLRR:
            sprintf(out, "%d/%d", (p->ops[op].SL_RR >> 4) & 0x0F, p->ops[op].SL_RR & 0x0F);
            break;
        case UI_PATCH_ROW_SSG:  sprintf(out, "%03d", p->ops[op].SSG_EG); break;
        default: out[0] = '\0'; break;
    }
}

static void ui_build_patch(void) {
    char val[8];
    uint8_t row;
    int y;

    str_put(0, MATRIX_HDR_ROW, "PARAM | OP1 | OP2 | OP3 | OP4 |", ATTR_CYAN);
    str_put(0, GRID_ROW0, "------+-----+-----+-----+-----+", ATTR_CYAN);

    for (row = 0; row < UI_PATCH_NUM_ROWS; row++) {
        y = GRID_ROW0 + 1 + row;
        {
            u16 rattr = (row == g_tracker_state.patch_row) ? ATTR_AMBER : ATTR_WHITE;
            str_put(0, y, s_patch_row_names[row], rattr);
        }
        if (patch_row_is_op(row)) {
            uint8_t op;
            for (op = 0; op < 4; op++) {
                patch_row_value(val, row, op);
                {
                    bool sel = (row == g_tracker_state.patch_row) &&
                               (op == g_tracker_state.patch_op);
                    u16 attr = sel ? ATTR_RED : ATTR_WHITE;
                    cell_put(7 + op * 6, y, '|', ATTR_CYAN);
                    str_put(9 + op * 6, y, val, attr);
                }
            }
        } else {
            patch_row_value(val, row, 0);
            str_put(10, y, val, (row == g_tracker_state.patch_row) ? ATTR_RED : ATTR_WHITE);
        }
    }

    str_put(0, 25, "----------------------------------------", ATTR_CYAN);
    {
        char line[24];
        sprintf(line, "SLOT %02X  OCT %d   A:Audition",
                g_tracker_state.patch_slot, g_tracker_state.octave % 10);
        str_put(0, FOOT_HELP_ROW, line, ATTR_WHITE);
    }
}

static void ui_build_footer(void) {
    str_put(0, FOOT_DIV_ROW, "========================================", ATTR_CYAN);
    switch (g_tracker_state.screen) {
        case UI_SCREEN_SONG:
            str_put(0, FOOT_HELP_ROW, "DIR:Slot A+PAT B:Len START:Play", ATTR_WHITE);
            break;
        case UI_SCREEN_PATCH:
            str_put(0, FOOT_HELP_ROW, "DIR:Sel B+DIR:Val A+DIR:Slot C:Aud", ATTR_WHITE);
            break;
        default:
            str_put(0, FOOT_HELP_ROW, "A+DIR:Note B+DIR:Val START:Play C:Audit", ATTR_WHITE);
            break;
    }
}

/* Amber box drawn on BG_B behind the active editing cell (PAT grid only). */
static void ui_update_highlight(void) {
    int x = 0, y = 0, w = 5;
    bool want = (g_tracker_state.screen == UI_SCREEN_PAT) &&
                !g_tracker_state.sidebar_focus;
    int i;

    if (want) {
        x = 2 + (g_tracker_state.cursor_channel * 5);
        y = GRID_ROW0 + (g_tracker_state.cursor_row - s_grid_start);
        if ((g_tracker_state.cursor_row < s_grid_start) ||
            (g_tracker_state.cursor_row >= (s_grid_start + GRID_ROWS))) {
            want = false;
        }
    }

    if (!want) {
        if (s_hl_x != 0xFF) {
            for (i = 0; i < 5; i++) {
                VDP_setTileMapXY(BG_B, TILE_ATTR_FULL(PAL0, 0, 0, 0, TILE_FONT_INDEX),
                                 s_hl_x + i, s_hl_y);
            }
            s_hl_x = 0xFF;
        }
        return;
    }

    if (s_hl_x == (uint8_t)x && s_hl_y == (uint8_t)y) return;

    if (s_hl_x != 0xFF) {
        for (i = 0; i < 5; i++) {
            VDP_setTileMapXY(BG_B, TILE_ATTR_FULL(PAL0, 0, 0, 0, TILE_FONT_INDEX),
                             s_hl_x + i, s_hl_y);
        }
    }
    for (i = 0; i < w; i++) {
        VDP_setTileMapXY(BG_B, TILE_ATTR_FULL(PAL0, 0, 0, 0, TILE_HILITE), x + i, y);
    }
    s_hl_x = (uint8_t)x;
    s_hl_y = (uint8_t)y;
}

/* Compose whichever screen is active. */
static void ui_build_screen(void) {
    ui_build_header();
    switch (g_tracker_state.screen) {
        case UI_SCREEN_SONG:  ui_build_song();  break;
        case UI_SCREEN_PATCH: ui_build_patch(); break;
        default:              ui_build_grid(); ui_build_hud(); break;
    }
    ui_build_footer();
}

#endif /* Genesis hardware */

static void ui_trigger_row_notes(uint8_t pattern, uint8_t row) {
    for (uint8_t ch = 0; ch < 6; ch++) {
        rdj_row_t *r = &g_song.patterns[pattern].rows[row][ch];
        if (r->note != RDJ_NOTE_EMPTY && r->note != RDJ_NOTE_OFF) {
            ym2612_rdj_note_on(ch, r->note, r->instrument);
        } else if (r->note == RDJ_NOTE_OFF) {
            ym2612_rdj_note_off(ch);
        }
        /* An empty cell (0x00 / '---') must NOT stop the channel; the previous
         * note sustains until an explicit note-off or a new note arrives. */
    }
}

void ui_process_playback(void) {
    if (g_tracker_state.is_playing) {
        uint8_t pat = g_song.order[s_play_order_pos];

        if (!s_prev_is_playing) {
            s_prev_is_playing = 1;
            s_tick_counter = 0;
            s_current_play_row = (g_tracker_state.screen == UI_SCREEN_PAT)
                                 ? g_tracker_state.cursor_row : 0;
            ui_trigger_row_notes(pat, s_current_play_row);
        } else {
            s_tick_counter++;
            {
                uint8_t speed = g_song.patterns[pat].speed ? g_song.patterns[pat].speed
                                                           : (g_song.speed ? g_song.speed : 6);
                if (s_tick_counter >= speed) {
                    s_tick_counter = 0;
                    {
                        uint8_t pat_len = g_song.patterns[pat].length ? g_song.patterns[pat].length
                                                                      : RDJ_MAX_ROWS;
                        s_current_play_row++;
                        if (s_current_play_row >= pat_len) {
                            /* Advance through the order list, wrapping at the end. */
                            s_current_play_row = 0;
                            s_play_order_pos++;
                            if (s_play_order_pos >= g_song.order_length) s_play_order_pos = 0;
                            pat = g_song.order[s_play_order_pos];
                        }
                        g_tracker_state.play_order_pos = s_play_order_pos;
                        if (g_tracker_state.screen == UI_SCREEN_PAT) {
                            g_tracker_state.cursor_row = s_current_play_row;
                        }
                        ui_trigger_row_notes(pat, s_current_play_row);
                    }
                }
            }
        }
    } else {
        if (s_prev_is_playing) {
            s_prev_is_playing = 0;
            s_tick_counter = 0;
            for (uint8_t ch = 0; ch < 6; ch++) {
                ym2612_rdj_note_off(ch);
            }
        }
    }
}

void ui_init(void) {
    int r, c, p;

    /* Initialize song and default pattern data */
    g_song.bpm = 130;
    g_song.speed = 6;
    g_song.order_length = 1;
    for (p = 0; p < RDJ_MAX_ORDER; p++) {
        g_song.order[p] = (uint8_t)(p < RDJ_MAX_PATTERNS ? p : 0);
    }

    for (p = 0; p < RDJ_MAX_PATTERNS; p++) {
        for (r = 0; r < RDJ_MAX_ROWS; r++) {
            for (c = 0; c < RDJ_MAX_CHANNELS; c++) {
                g_song.patterns[p].rows[r][c].note = RDJ_NOTE_EMPTY;
                g_song.patterns[p].rows[r][c].instrument = 0;
                g_song.patterns[p].rows[r][c].volume = 0;
                g_song.patterns[p].rows[r][c].fx_cmd = 0;
                g_song.patterns[p].rows[r][c].fx_val = 0;
            }
        }
        g_song.patterns[p].length = 64;
        g_song.patterns[p].speed = 6;
    }

    /* Audition/edit notes through the YM2612 driver. */
    g_song.driver = &ym2612_driver;

    input_mapper_init(&g_tracker_state);

    /* Route the [PATCH] editor at the real YM2612 driver. */
    {
        static const input_mapper_patch_ops_t patch_ops = {
            ym2612_rdj_get_patch,
            ym2612_rdj_apply_patch
        };
        input_mapper_set_patch_ops(&patch_ops);
    }

    s_tick_counter = 0;
    s_current_play_row = 0;
    s_prev_is_playing = 0;
    s_play_order_pos = 0;

#if defined(RDJ_GENESIS_HARDWARE) || defined(SGDK_GCC) || defined(__m68k__)
    VDP_setScreenWidth320();
    VDP_setTextPlane(BG_A);
    ui_palette_init();
    ui_load_custom_tiles();
    ui_clear_bg_b();
#endif

    /* Seed a few instrument slots with usable FM voices so the patch editor
     * has something to tweak out of the box. */
    {
        ym2612_patch_t bass_patch = {
            .ops = {
                { 0x31, 0x00, 0x1F, 0x05, 0x02, 0x14, 0x00 }, /* Op 1 */
                { 0x01, 0x24, 0x1F, 0x08, 0x05, 0x24, 0x00 }, /* Op 2 */
                { 0x01, 0x22, 0x1F, 0x06, 0x04, 0x24, 0x00 }, /* Op 3 */
                { 0x01, 0x00, 0x1F, 0x04, 0x02, 0x14, 0x00 }  /* Op 4 */
            },
            .FB_ALG = 0x04,
            .LR_AMS_PMS = 0xC0
        };
        ym2612_patch_t lead_patch = {
            .ops = {
                { 0x71, 0x18, 0x1F, 0x0C, 0x06, 0x24, 0x00 },
                { 0x31, 0x00, 0x1F, 0x0A, 0x04, 0x14, 0x00 },
                { 0x01, 0x28, 0x1F, 0x0E, 0x08, 0x24, 0x00 },
                { 0x01, 0x00, 0x1F, 0x08, 0x02, 0x14, 0x00 }
            },
            .FB_ALG = 0x22,
            .LR_AMS_PMS = 0xC0
        };
        ym2612_rdj_set_patch(0, &bass_patch);
        ym2612_rdj_set_patch(1, &lead_patch);
    }
}

void ui_update(void) {
#if defined(RDJ_GENESIS_HARDWARE) || defined(SGDK_GCC) || defined(__m68k__)
    uint16_t joy = JOY_readJoypad(JOY_1);
    input_mapper_update(&g_tracker_state, &g_song, joy);
#endif
    ui_process_playback();
}

void ui_render(void) {
#if defined(RDJ_GENESIS_HARDWARE) || defined(SGDK_GCC) || defined(__m68k__)
    int i;
    bool dirty;

    /* Full tilemap rebuilds cost more than one VBlank budget, so rebuild
     * immediately when the tracker state changes and otherwise only every
     * few frames (CPU meter / playback read-outs stay current enough). */
    dirty = (g_tracker_state.cursor_row != s_prev_cr) ||
            (g_tracker_state.cursor_channel != s_prev_cc) ||
            (g_tracker_state.is_playing != s_prev_play) ||
            (s_current_play_row != s_prev_pr) ||
            (g_tracker_state.screen != s_prev_screen) ||
            (g_tracker_state.sidebar_focus != s_prev_focus) ||
            (g_tracker_state.sidebar_tab != s_prev_tab) ||
            (g_tracker_state.song_cursor != s_prev_songcur) ||
            (g_tracker_state.patch_slot != s_prev_patchslot) ||
            (g_tracker_state.patch_row != s_prev_patchrow) ||
            (g_tracker_state.patch_op != s_prev_patchop) ||
            (g_tracker_state.edit_pattern != s_prev_editpat);
    s_prev_cr = g_tracker_state.cursor_row;
    s_prev_cc = g_tracker_state.cursor_channel;
    s_prev_play = g_tracker_state.is_playing;
    s_prev_pr = s_current_play_row;
    s_prev_screen = g_tracker_state.screen;
    s_prev_focus = g_tracker_state.sidebar_focus;
    s_prev_tab = g_tracker_state.sidebar_tab;
    s_prev_songcur = g_tracker_state.song_cursor;
    s_prev_patchslot = g_tracker_state.patch_slot;
    s_prev_patchrow = g_tracker_state.patch_row;
    s_prev_patchop = g_tracker_state.patch_op;
    s_prev_editpat = g_tracker_state.edit_pattern;

    s_frame++;
    if (!dirty && ((s_frame & 7) != 0)) { ui_update_highlight(); return; }

    for (i = 0; i < (UI_COLS * UI_ROWS); i++) {
        s_screen[i] = TILE_FONT_INDEX;    /* blank cell (ink off) */
    }

    ui_build_screen();

    /* DMA the whole tilemap during VBlank; s_screen is static so the queued
     * transfer stays valid until SYS_doVBlankProcess() flushes it. */
    VDP_setTileMapDataRectEx(BG_A, s_screen,
                             TILE_ATTR_FULL(PAL0, 0, 0, 0, 0),
                             0, 0, UI_COLS, UI_ROWS, UI_COLS, DMA_QUEUE);

    ui_update_highlight();
#endif
}

input_tracker_state_t *ui_get_tracker_state(void) {
    return &g_tracker_state;
}

rdj_song_t *ui_get_song(void) {
    return &g_song;
}

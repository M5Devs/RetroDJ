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

/* YM2612 patch structure definition */
typedef struct {
    uint8_t DT_MUL, TL, AR, DR, SR, SL_RR, SSG_EG;
} ym2612_op_test_t;

typedef struct {
    ym2612_op_test_t ops[4];
    uint8_t FB_ALG;
    uint8_t LR_AMS_PMS;
} ym2612_patch_test_t;

extern void ym2612_rdj_set_patch(uint8_t inst_slot, void *patch);
extern void ym2612_rdj_note_on(uint8_t channel, uint8_t note, uint8_t instrument);
extern void ym2612_rdj_note_off(uint8_t channel);

static rdj_song_t g_song;
static input_tracker_state_t g_tracker_state;

static uint8_t s_tick_counter = 0;
static uint8_t s_current_play_row = 0;
static uint8_t s_prev_is_playing = 0;

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
    for (d = 0; d < 8; d++) {
        int r = base - d;
        while (r < 0) r += RDJ_MAX_ROWS;
        if (g_song.patterns[0].rows[r][ch].note != RDJ_NOTE_EMPTY) {
            return g_song.patterns[0].rows[r][ch].note;
        }
    }
    return RDJ_NOTE_EMPTY;
}

static uint8_t hud_level(uint8_t ch) {
    int base = g_tracker_state.is_playing ? s_current_play_row : g_tracker_state.cursor_row;
    int d;
    for (d = 0; d < 8; d++) {
        int r = base - d;
        while (r < 0) r += RDJ_MAX_ROWS;
        {
            uint8_t n = g_song.patterns[0].rows[r][ch].note;
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
    char buf[12];
    int cpu, filled, i, x;

    str_put(6, HDR_TITLE_ROW, "--- RetroDJ Genesis v1.2 ---", ATTR_CYAN);
    str_put(0, HDR_DIV_ROW, "========================================", ATTR_CYAN);

    /* BPM:130 [PLAY] OCT:4 CPU:[||||||||||] 42% */
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
    /* Right-aligned 3-digit field plus '%' keeps the line within 40 columns
     * even when the load reads 100. */
    sprintf(buf, "%3d%%", cpu);
    str_put(x, HDR_STATUS_ROW, buf, ATTR_WHITE);
}

static void ui_build_grid(void) {
    uint8_t cr = g_tracker_state.cursor_row;
    uint8_t start, i, c;
    char hdr[6];

    start = (cr >= (GRID_ROWS / 2)) ? (uint8_t)(cr - (GRID_ROWS / 2)) : 0;
    if (start > (RDJ_MAX_ROWS - GRID_ROWS)) start = (RDJ_MAX_ROWS - GRID_ROWS);
    s_grid_start = start;

    /* header: "R FM1  FM2  FM3  FM4  FM5  FM6 " (cols 0..31) */
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
            rdj_row_t *cell = &g_song.patterns[0].rows[r][c];
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

static void ui_build_hud(void) {
    uint8_t c;
    int x = HUD_X;
    char line[12];

    str_put(x, MATRIX_HDR_ROW, "|HUD:MIX", ATTR_CYAN);

    /* mode tabs, PAT highlighted */
    str_put(x, 5, "|[PAT]  ", ATTR_AMBER);
    str_put(x, 6, "|[SONG] ", ATTR_CYAN);
    str_put(x, 7, "|[PATCH]", ATTR_CYAN);
    str_put(x, 8, "|-------", ATTR_CYAN);

    /* live channel note read-outs */
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

    /* VU meters */
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

    /* active cell detail */
    {
        rdj_row_t *cell = &g_song.patterns[0].rows[g_tracker_state.cursor_row]
                                                 [g_tracker_state.cursor_channel];
        sprintf(line, "|I:%02X", cell->instrument);
        str_put(x, 23, line, ATTR_WHITE);
        sprintf(line, "|V:%02X", cell->volume);
        str_put(x, 24, line, ATTR_WHITE);
    }
}

static void ui_build_footer(void) {
    str_put(0, FOOT_DIV_ROW, "========================================", ATTR_CYAN);
    str_put(0, FOOT_HELP_ROW, "A+DIR:Note B+DIR:Val START:Play C:Audit", ATTR_WHITE);
}

/* Amber box drawn on BG_B behind the active editing cell. */
static void ui_update_highlight(void) {
    int x = 2 + (g_tracker_state.cursor_channel * 5);
    int y = GRID_ROW0 + (g_tracker_state.cursor_row - s_grid_start);
    bool visible = (g_tracker_state.cursor_row >= s_grid_start) &&
                   (g_tracker_state.cursor_row < (s_grid_start + GRID_ROWS));
    int i;

    if (!visible) {
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
    for (i = 0; i < 5; i++) {
        VDP_setTileMapXY(BG_B, TILE_ATTR_FULL(PAL0, 0, 0, 0, TILE_HILITE), x + i, y);
    }
    s_hl_x = (uint8_t)x;
    s_hl_y = (uint8_t)y;
}

#endif /* Genesis hardware */

static void ui_trigger_row_notes(uint8_t row) {
    for (uint8_t ch = 0; ch < 6; ch++) {
        rdj_row_t *r = &g_song.patterns[0].rows[row][ch];
        if (r->note != RDJ_NOTE_EMPTY && r->note != RDJ_NOTE_OFF) {
            ym2612_rdj_note_on(ch, r->note, r->instrument);
        } else if (r->note == RDJ_NOTE_OFF) {
            ym2612_rdj_note_off(ch);
        }
        /* CRITICAL: If r->note == RDJ_NOTE_EMPTY (0x00 / '---'), DO NOT call note_off!
         * Let the previous note sustain naturally.
         */
    }
}

void ui_process_playback(void) {
    if (g_tracker_state.is_playing) {
        if (!s_prev_is_playing) {
            s_prev_is_playing = 1;
            s_tick_counter = 0;
            s_current_play_row = g_tracker_state.cursor_row;
            ui_trigger_row_notes(s_current_play_row);
        } else {
            s_tick_counter++;
            uint8_t speed = g_song.patterns[0].speed ? g_song.patterns[0].speed : (g_song.speed ? g_song.speed : 6);
            if (s_tick_counter >= speed) {
                s_tick_counter = 0;
                uint8_t pat_len = g_song.patterns[0].length ? g_song.patterns[0].length : RDJ_MAX_ROWS;
                s_current_play_row++;
                if (s_current_play_row >= pat_len) {
                    s_current_play_row = 0;
                }
                g_tracker_state.cursor_row = s_current_play_row;
                ui_trigger_row_notes(s_current_play_row);
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
    int r, c;

    /* Initialize song and default pattern data */
    g_song.bpm = 130;
    g_song.speed = 6;
    g_song.order_length = 1;
    g_song.order[0] = 0;

    for (r = 0; r < RDJ_MAX_ROWS; r++) {
        for (c = 0; c < RDJ_MAX_CHANNELS; c++) {
            g_song.patterns[0].rows[r][c].note = RDJ_NOTE_EMPTY;
            g_song.patterns[0].rows[r][c].instrument = 0;
            g_song.patterns[0].rows[r][c].volume = 0;
            g_song.patterns[0].rows[r][c].fx_cmd = 0;
            g_song.patterns[0].rows[r][c].fx_val = 0;
        }
    }
    g_song.patterns[0].length = 64;
    g_song.patterns[0].speed = 6;

    /* Audition/edit notes through the YM2612 driver. */
    g_song.driver = &ym2612_driver;

    input_mapper_init(&g_tracker_state);

    s_tick_counter = 0;
    s_current_play_row = 0;
    s_prev_is_playing = 0;

#if defined(RDJ_GENESIS_HARDWARE) || defined(SGDK_GCC) || defined(__m68k__)
    VDP_setScreenWidth320();
    VDP_setTextPlane(BG_A);
    ui_palette_init();
    ui_load_custom_tiles();
    ui_clear_bg_b();

    /* Prepare FM Synth Patch on slot 0 */
    {
        ym2612_patch_test_t bass_patch = {
            .ops = {
                { 0x31, 0x00, 0x1F, 0x05, 0x02, 0x14, 0x00 }, /* Op 1 */
                { 0x01, 0x24, 0x1F, 0x08, 0x05, 0x24, 0x00 }, /* Op 2 */
                { 0x01, 0x22, 0x1F, 0x06, 0x04, 0x24, 0x00 }, /* Op 3 */
                { 0x01, 0x00, 0x1F, 0x04, 0x02, 0x14, 0x00 }  /* Op 4 */
            },
            .FB_ALG = 0x04,
            .LR_AMS_PMS = 0xC0
        };
        ym2612_rdj_set_patch(0, &bass_patch);
    }
#endif
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
            (s_current_play_row != s_prev_pr);
    s_prev_cr = g_tracker_state.cursor_row;
    s_prev_cc = g_tracker_state.cursor_channel;
    s_prev_play = g_tracker_state.is_playing;
    s_prev_pr = s_current_play_row;

    s_frame++;
    if (!dirty && ((s_frame & 7) != 0)) { ui_update_highlight(); return; }

    for (i = 0; i < (UI_COLS * UI_ROWS); i++) {
        s_screen[i] = TILE_FONT_INDEX;    /* blank cell (ink off) */
    }

    ui_build_header();
    ui_build_grid();
    ui_build_hud();
    ui_build_footer();

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

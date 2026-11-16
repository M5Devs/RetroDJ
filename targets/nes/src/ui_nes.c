/* RetroDJ - NES Tracker UI & Controller Implementation */
/* GPLv3 - RetroDJ Project */

#include "ui_nes.h"
#include "core/rdj_driver.h"
#include "core/rdj_backend.h"

#include <stdio.h>
#include <string.h>

#define PPU_CTRL   (*((volatile uint8_t*)0x2000))
#define PPU_MASK   (*((volatile uint8_t*)0x2001))
#define PPU_STATUS (*((volatile uint8_t*)0x2002))
#define PPU_SCROLL (*((volatile uint8_t*)0x2005))
#define PPU_ADDR   (*((volatile uint8_t*)0x2006))
#define PPU_DATA   (*((volatile uint8_t*)0x2007))

static rdj_song_t g_song;
static input_tracker_state_t g_tracker_state;

/* Scratch byte shared with the inline-assembly controller shift loop. */
static uint8_t s_joy_bit = 0;

static uint8_t s_tick_counter = 0;
static uint8_t s_current_play_row = 0;
static uint8_t s_prev_is_playing = 0;

static uint8_t s_last_cursor_row = 0;
static uint8_t s_last_cursor_chan = 0;
static uint8_t s_last_cursor_subcol = 0;
static uint8_t s_last_status_bpm = 0;
static uint8_t s_last_status_oct = 0;
static uint8_t s_last_status_ch = 0;
static uint8_t s_last_status_playing = 0;

/* One visible row (or the status bar) of pending VRAM work. The string is
 * formatted during visible frame time; only the raw byte burst runs in VBlank. */
typedef struct {
    uint16_t addr;
    uint8_t  len;
    char     buf[33];
} nes_vram_write_t;

#define NES_MAX_STAGE 3
static nes_vram_write_t s_stage[NES_MAX_STAGE];
static uint8_t s_stage_count = 0;

/* Shadow copy of the 12 visible pattern rows so a row is re-pushed only when
 * its rendered text actually changes. s_shadow_start is the pattern row shown
 * at the top; when it drifts from the desired start_row the shadow is stale and
 * the grid is refilled one row per frame (a full 12-row burst would overrun
 * VBlank). */
static char    s_shadow[12][29];
static uint8_t s_shadow_start = 0xFF;
static uint8_t s_scroll_refresh_idx = 0;

#if defined(__NES__) || defined(__CC65__)
extern volatile uint8_t vblank_flag;
#pragma zpsym ("vblank_flag");
#else
static volatile uint8_t vblank_flag = 0;
#endif

void nes_vblank_wait(void) {
#if defined(__NES__) || defined(__CC65__)
    vblank_flag = 0;
    while (vblank_flag == 0) {
    }
#endif
}

uint16_t nes_read_controller(void) {
    uint16_t buttons = 0;
#if defined(__NES__) || defined(__CC65__)
    uint8_t raw = 0;
    uint8_t i;

    /* cc65 folds a C-level read of $4016 to zero when it follows the strobe
     * write, so the shift register has to be clocked in assembly. Read order
     * is A, B, SELECT, START, UP, DOWN, LEFT, RIGHT. */
    __asm__("lda #$01");
    __asm__("sta $4016");
    __asm__("lda #$00");
    __asm__("sta $4016");

    /* The pad shifts out A first, so shift each new bit in at the top and the
     * first button read ends up in bit 0 after all eight reads. */
    for (i = 0; i < 8; i++) {
        __asm__("lda $4016");
        __asm__("and #$01");
        __asm__("sta %v", s_joy_bit);
        raw = (uint8_t)((raw >> 1) | ((s_joy_bit & 1) << 7));
    }

    if (raw & 0x01) buttons |= BUTTON_A;      /* Bit 0: A */
    if (raw & 0x02) buttons |= BUTTON_B;      /* Bit 1: B */
    if (raw & 0x08) buttons |= BUTTON_START;  /* Bit 3: START */
    if (raw & 0x10) buttons |= BUTTON_UP;     /* Bit 4: UP */
    if (raw & 0x20) buttons |= BUTTON_DOWN;   /* Bit 5: DOWN */
    if (raw & 0x40) buttons |= BUTTON_LEFT;   /* Bit 6: LEFT */
    if (raw & 0x80) buttons |= BUTTON_RIGHT;  /* Bit 7: RIGHT */
#endif
    return buttons;
}

static void ppu_write_string(uint16_t ppu_addr, const char *str) {
#if defined(__NES__) || defined(__CC65__)
    __asm__("bit $2002"); /* Reset PPU address latch */
    PPU_ADDR = (uint8_t)(ppu_addr >> 8);
    PPU_ADDR = (uint8_t)(ppu_addr & 0xFF);
    while (*str) {
        PPU_DATA = (uint8_t)(*str++);
    }
#else
    (void)ppu_addr; (void)str;
#endif
}

static char hex_char(uint8_t val) {
    val &= 0x0F;
    return (val < 10) ? ('0' + val) : ('A' + (val - 10));
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
    note_idx = (note - 12) % 12;
    octave = (note - 12) / 12;
    if (octave < 0) octave = 0;
    if (octave > 9) octave = 9;

    out[0] = note_names[note_idx][0];
    out[1] = note_names[note_idx][1];
    out[2] = (char)('0' + octave);
    out[3] = '\0';
}

static void format_status_str(char *out) {
    uint16_t bpm = g_song.bpm;
    uint8_t oct = g_tracker_state.octave;
    uint8_t ch = g_tracker_state.cursor_channel + 1;
    bool playing = g_tracker_state.is_playing;

    out[0] = 'B'; out[1] = 'P'; out[2] = 'M'; out[3] = ':';
    out[4] = (char)('0' + ((bpm / 100) % 10));
    out[5] = (char)('0' + ((bpm / 10) % 10));
    out[6] = (char)('0' + (bpm % 10));
    out[7] = ' '; out[8] = '[';
    if (playing) {
        out[9] = 'P'; out[10] = 'L'; out[11] = 'A'; out[12] = 'Y';
    } else {
        out[9] = 'S'; out[10] = 'T'; out[11] = 'O'; out[12] = 'P';
    }
    out[13] = ']'; out[14] = ' ';
    out[15] = 'O'; out[16] = 'C'; out[17] = 'T'; out[18] = ':';
    out[19] = (char)('0' + (oct % 10));
    out[20] = ' ';
    out[21] = 'C'; out[22] = 'H'; out[23] = ':';
    out[24] = (char)('0' + (ch % 10));
    out[25] = '\0';
}

static void format_row_str(uint8_t r, char *out) {
    bool is_cursor_row = (r == g_tracker_state.cursor_row);
    char note_name[4];
    uint8_t c;
    int pos = 0;

    out[pos++] = hex_char(r >> 4);
    out[pos++] = hex_char(r);
    out[pos++] = is_cursor_row ? '>' : ' ';
    out[pos++] = '|';

    for (c = 0; c < 4; c++) {
        rdj_row_t *cell = &g_song.patterns[0].rows[r][c];
        bool is_curr_cell = is_cursor_row && (g_tracker_state.cursor_channel == c);
        char sub_note = (is_curr_cell && g_tracker_state.cursor_subcol == RDJ_SUBCOL_NOTE) ? '*' : ' ';
        char sub_inst = (is_curr_cell && g_tracker_state.cursor_subcol == RDJ_SUBCOL_INST) ? '*' : ' ';

        get_note_name(cell->note, note_name);

        out[pos++] = note_name[0];
        out[pos++] = note_name[1];
        out[pos++] = note_name[2];
        out[pos++] = sub_note;
        out[pos++] = hex_char(cell->instrument);
        out[pos++] = sub_inst;
    }
    out[pos] = '\0';
}

/* Queue one row's raw VRAM burst. The text is produced here (visible frame
 * time); the PPU burst happens later in ui_render() inside VBlank. */
static void nes_stage_push(uint16_t addr, const char *str) {
    nes_vram_write_t *w;
    uint8_t i;

    if (s_stage_count >= NES_MAX_STAGE) {
        return;
    }
    w = &s_stage[s_stage_count++];
    w->addr = addr;
    for (i = 0; i < 32 && str[i]; i++) {
        w->buf[i] = str[i];
    }
    w->len = i;
}

/* Re-push a visible row only when its text changed. shadow_idx is the slot in
 * the visible window (0..11). */
static void nes_stage_row_if_changed(uint8_t shadow_idx, uint8_t pattern_row) {
    char buf[32];

    format_row_str(pattern_row, buf);
    if (strncmp(s_shadow[shadow_idx], buf, 28) == 0) {
        return;
    }
    memcpy(s_shadow[shadow_idx], buf, 29);
    nes_stage_push((uint16_t)(0x20E2 + (shadow_idx * 32)), buf);
}

void ui_init(void) {
    int r, c, i, k;
    char init_str[32];

    g_song.bpm = 120;
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
    g_song.patterns[0].length = 32;
    g_song.patterns[0].speed = 6;

    /* Audition/edit notes through the 2A03 APU driver. */
    g_song.driver = &nes_2a03_driver;

    input_mapper_init(&g_tracker_state);

    s_tick_counter = 0;
    s_current_play_row = 0;
    s_prev_is_playing = 0;

    s_last_cursor_row = g_tracker_state.cursor_row;
    s_last_cursor_chan = g_tracker_state.cursor_channel;
    s_last_cursor_subcol = g_tracker_state.cursor_subcol;
    s_last_status_bpm = (uint8_t)g_song.bpm;
    s_last_status_oct = g_tracker_state.octave;
    s_last_status_ch = g_tracker_state.cursor_channel;
    s_last_status_playing = g_tracker_state.is_playing;
    s_stage_count = 0;
    s_shadow_start = 0xFF;
    s_scroll_refresh_idx = 0;
    memset(s_shadow, 0, sizeof(s_shadow));

#if defined(__NES__) || defined(__CC65__)
    /* Turn off PPU screen rendering before static layout setup */
    PPU_MASK = 0x00;

    /* Latch address $3F00 */
    __asm__("bit $2002");
    PPU_ADDR = 0x3F;
    PPU_ADDR = 0x00;

    /* Palette 0: Black BG ($0F), White Text ($30), Red ($16), Orange ($27) */
    PPU_DATA = 0x0F;
    PPU_DATA = 0x30;
    PPU_DATA = 0x16;
    PPU_DATA = 0x27;

    /* Fill remaining 12 background palette entries with $0F */
    for (k = 4; k < 16; k++) {
        PPU_DATA = 0x0F;
    }

    /* Clear Nametable 0 ($2000-$23BF) to spaces (0x20) */
    __asm__("bit $2002");
    PPU_ADDR = 0x20;
    PPU_ADDR = 0x00;
    for (k = 0; k < 960; k++) {
        PPU_DATA = ' ';
    }

    /* Write static UI frame and text directly to Nametable 0 */
    ppu_write_string(0x2022, "--- RetroDJ NES Tracker ---");

    format_status_str(init_str);
    ppu_write_string(0x2062, init_str);

    ppu_write_string(0x20A2, "ROW| SQ1   SQ2   TRI   NOI  ");
    ppu_write_string(0x20C2, "---|------------------------");

    for (i = 0; i < 12; i++) {
        format_row_str((uint8_t)i, init_str);
        ppu_write_string(0x20E2 + (i * 32), init_str);
        memcpy(s_shadow[i], init_str, 29);
    }
    s_shadow_start = 0;

    ppu_write_string(0x2380, "--------------------------------");
    ppu_write_string(0x23A0, "A+DIR:Note B+DIR:Val START:Play");

    /* Clear Attribute Table ($23C0-$23FF) to $00 (palette 0) */
    __asm__("bit $2002");
    PPU_ADDR = 0x23;
    PPU_ADDR = 0xC0;
    for (k = 0; k < 64; k++) {
        PPU_DATA = 0x00;
    }

    /* Reset PPU scroll latch cleanly */
    __asm__("bit $2002");
    PPU_SCROLL = 0x00;
    PPU_SCROLL = 0x00;

    /* Turn on PPU rendering: Enable BG & Sprites ($1E) */
    PPU_MASK = 0x1E;
#endif
}

static void ui_trigger_row_notes(uint8_t row) {
    uint8_t ch;
    for (ch = 0; ch < 4; ch++) {
        rdj_row_t *r = &g_song.patterns[0].rows[row][ch];
        if (r->note != RDJ_NOTE_EMPTY && r->note != RDJ_NOTE_OFF) {
            nes_2a03_driver.note_on(ch, r->note, r->instrument, r->volume ? r->volume : 128);
        } else if (r->note == RDJ_NOTE_OFF) {
            nes_2a03_driver.note_off(ch);
        }
    }
}

void ui_process_playback(void) {
    uint8_t speed, pat_len, ch;
    if (g_tracker_state.is_playing) {
        if (!s_prev_is_playing) {
            s_prev_is_playing = 1;
            s_tick_counter = 0;
            s_current_play_row = g_tracker_state.cursor_row;
            ui_trigger_row_notes(s_current_play_row);
        } else {
            s_tick_counter++;
            speed = g_song.patterns[0].speed ? g_song.patterns[0].speed : (g_song.speed ? g_song.speed : 6);
            if (s_tick_counter >= speed) {
                s_tick_counter = 0;
                pat_len = g_song.patterns[0].length ? g_song.patterns[0].length : RDJ_MAX_ROWS;
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
            for (ch = 0; ch < 4; ch++) {
                nes_2a03_driver.note_off(ch);
            }
        }
    }
}

void ui_update(void) {
    uint16_t joy = nes_read_controller();
    uint8_t start_row, i, r;

    input_mapper_update(&g_tracker_state, &g_song, joy);
    ui_process_playback();

#if defined(__NES__) || defined(__CC65__)
    /* All string formatting happens here, in visible frame time. ui_render()
     * then only has to move the staged bytes into VRAM during VBlank. */
    s_stage_count = 0;

    /* Status bar: re-format only when something it shows changed. */
    if (g_song.bpm != s_last_status_bpm ||
        g_tracker_state.octave != s_last_status_oct ||
        g_tracker_state.cursor_channel != s_last_status_ch ||
        g_tracker_state.is_playing != s_last_status_playing) {

        char status_str[32];
        format_status_str(status_str);
        nes_stage_push(0x2062, status_str);

        s_last_status_bpm = (uint8_t)g_song.bpm;
        s_last_status_oct = g_tracker_state.octave;
        s_last_status_ch = g_tracker_state.cursor_channel;
        s_last_status_playing = g_tracker_state.is_playing;
    }

    /* Visible window start for the 12-row grid. */
    start_row = g_tracker_state.cursor_row >= 6 ? g_tracker_state.cursor_row - 6 : 0;
    if (start_row > RDJ_MAX_ROWS - 12) start_row = RDJ_MAX_ROWS - 12;

    if (s_shadow_start != start_row) {
        /* The window scrolled. Refill one row per frame; s_shadow_start only
         * commits once all 12 slots are current again, so a burst never has to
         * push more than one row inside VBlank. */
        i = s_scroll_refresh_idx;
        r = start_row + i;
        format_row_str(r, s_shadow[i]);
        nes_stage_push((uint16_t)(0x20E2 + (i * 32)), s_shadow[i]);

        s_scroll_refresh_idx++;
        if (s_scroll_refresh_idx >= 12) {
            s_scroll_refresh_idx = 0;
            s_shadow_start = start_row;
        }
    } else {
        /* Steady state: push only the rows whose text actually changed (the
         * cursor moving or a cell being edited). */
        for (i = 0; i < 12; i++) {
            nes_stage_row_if_changed(i, (uint8_t)(start_row + i));
        }
    }

    s_last_cursor_row = g_tracker_state.cursor_row;
    s_last_cursor_chan = g_tracker_state.cursor_channel;
    s_last_cursor_subcol = g_tracker_state.cursor_subcol;
#endif
}

void ui_render(void) {
#if defined(__NES__) || defined(__CC65__)
    uint8_t stages = s_stage_count;
    uint8_t w, i;

    /* The cc65 byte loop is slow enough that a multi-row burst can spill past
     * the ~2270-cycle VBlank window. While rendering is on, a $2007 write that
     * lands in visible time makes the PPU advance its internal v register by 32
     * instead of 1, so the tail of the burst lands in the wrong nametable cells
     * (the left-column corruption). Blank the screen for the duration of the
     * transfer and restore it once the scroll latch has been reset. */
    if (stages) {
        PPU_MASK = 0x00;
    }

    for (w = 0; w < stages; w++) {
        nes_vram_write_t *s = &s_stage[w];
        __asm__("bit $2002");
        PPU_ADDR = (uint8_t)(s->addr >> 8);
        PPU_ADDR = (uint8_t)(s->addr & 0xFF);
        for (i = 0; i < s->len; i++) {
            PPU_DATA = (uint8_t)s->buf[i];
        }
    }
    s_stage_count = 0;

    /* Reset the scroll latch last: never write PPU_ADDR after this. */
    __asm__("bit $2002");
    PPU_SCROLL = 0x00;
    PPU_SCROLL = 0x00;

    if (stages) {
        PPU_MASK = 0x1E;
    }
#endif
}


input_tracker_state_t *ui_get_tracker_state(void) {
    return &g_tracker_state;
}

rdj_song_t *ui_get_song(void) {
    return &g_song;
}

/* RetroDJ - Sega Master System Tracker UI & Input Implementation */
/* GPLv3 - RetroDJ Project */

#include "ui_sms.h"

#if defined(__SMS__) || defined(__SDCC_z80)
#include "SMSlib.h"
#endif

#include <string.h>

static rdj_song_t g_song;
static input_tracker_state_t g_tracker_state;

static uint8_t s_tick_counter = 0;
static uint8_t s_current_play_row = 0;
static uint8_t s_prev_is_playing = 0;

/* Layout constants for the 32x24 tile screen. */
#define SMS_GRID_TOP   6
#define SMS_GRID_ROWS  12
#define SMS_ROW_WIDTH  29

static char s_shadow[SMS_GRID_ROWS][SMS_ROW_WIDTH];
static uint8_t s_shadow_start = 0xFF;
static uint8_t s_shadow_valid = 0;
static uint8_t s_last_status[26];
static uint8_t s_last_status_valid = 0;

void get_note_name(uint8_t note, char *out) {
    static const char note_names[12][2] = {
        {'C', '-'}, {'C', '#'}, {'D', '-'}, {'D', '#'},
        {'E', '-'}, {'F', '-'}, {'F', '#'}, {'G', '-'},
        {'G', '#'}, {'A', '-'}, {'A', '#'}, {'B', '-'}
    };
    uint8_t note_idx;
    int octave;

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

static char hex_char(uint8_t val) {
    val &= 0x0F;
    return (val < 10) ? (char)('0' + val) : (char)('A' + (val - 10));
}

static void format_status_str(char *out) {
    uint16_t bpm = g_song.bpm;
    uint8_t oct = g_tracker_state.octave;
    uint8_t ch = (uint8_t)(g_tracker_state.cursor_channel + 1);
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

    out[pos++] = hex_char((uint8_t)(r >> 4));
    out[pos++] = hex_char(r);
    out[pos++] = is_cursor_row ? '>' : ' ';
    out[pos++] = '|';

    /* One cell is 6 tiles (note + instrument, each with a cursor marker), so a
     * row is 4 + 4*6 = 28 tiles and fits the 32-column screen at x=2. */
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

#if defined(__SMS__) || defined(__SDCC_z80)
static void sms_draw(uint8_t x, uint8_t y, const char *str) {
    SMS_setNextTileatXY(x, y);
    SMS_print((const unsigned char *)str);
}
#endif

static void ui_trigger_row_notes(uint8_t row) {
    uint8_t ch;
    for (ch = 0; ch < 4; ch++) {
        rdj_row_t *r = &g_song.patterns[0].rows[row][ch];
        if (r->note != RDJ_NOTE_EMPTY && r->note != RDJ_NOTE_OFF) {
            uint8_t vol = r->volume ? r->volume : 128;
            sn76489_driver.note_on(ch, r->note, r->instrument, vol);
        } else if (r->note == RDJ_NOTE_OFF) {
            sn76489_driver.note_off(ch);
        }
        /* An empty cell must not silence the channel: the previous note is
         * meant to sustain until an explicit note-off. */
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
            uint8_t speed = g_song.patterns[0].speed ? g_song.patterns[0].speed
                                                     : (g_song.speed ? g_song.speed : 6);
            s_tick_counter++;
            if (s_tick_counter >= speed) {
                uint8_t pat_len;
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
            uint8_t ch;
            s_prev_is_playing = 0;
            s_tick_counter = 0;
            for (ch = 0; ch < 4; ch++) {
                sn76489_driver.note_off(ch);
            }
        }
    }
}

void ui_init(void) {
    int r, c;
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

    /* Audition/edit notes through the SN76489 PSG driver. */
    g_song.driver = &sn76489_driver;

    input_mapper_init(&g_tracker_state);

    s_tick_counter = 0;
    s_current_play_row = 0;
    s_prev_is_playing = 0;
    s_shadow_start = 0xFF;
    s_shadow_valid = 0;
    s_last_status_valid = 0;
    memset(s_shadow, 0, sizeof(s_shadow));

#if defined(__SMS__) || defined(__SDCC_z80)
    /* Clear the whole 16 KB VRAM to tile 0, which the 1bpp text renderer maps
     * to the blank space glyph. */
    SMS_VRAMmemsetW(0, 0x0000, 16384);

    /* Load the public-domain devkitSMS font, set the black/white palette and
     * turn the display on. */
    SMS_autoSetUpTextRenderer();

    sms_draw(3, 0, "--- RetroDJ SMS Tracker ---");

    format_status_str(init_str);
    sms_draw(2, 2, init_str);

    sms_draw(2, 4, "ROW|CH1   CH2   CH3   CH4");
    sms_draw(2, 5, "---|------------------------");

    for (r = 0; r < SMS_GRID_ROWS; r++) {
        format_row_str((uint8_t)r, init_str);
        sms_draw(2, (uint8_t)(SMS_GRID_TOP + r), init_str);
        memcpy(s_shadow[r], init_str, SMS_ROW_WIDTH);
    }
    s_shadow_start = 0;
    s_shadow_valid = 1;

    sms_draw(0, 21, "------------------------------");
    sms_draw(0, 22, "1+DIR:Note 2+DIR:Val PAUSE:Play");
#endif
}

void ui_update(void) {
#if defined(__SMS__) || defined(__SDCC_z80)
    uint16_t keys = SMS_getKeysStatus();
    uint16_t joy = 0;

    if (keys & PORT_A_KEY_UP)    joy |= BUTTON_UP;
    if (keys & PORT_A_KEY_DOWN)  joy |= BUTTON_DOWN;
    if (keys & PORT_A_KEY_LEFT)  joy |= BUTTON_LEFT;
    if (keys & PORT_A_KEY_RIGHT) joy |= BUTTON_RIGHT;
    /* The pad's Button 1 and Button 2 map to A and B. START comes from the
     * console PAUSE button: PORT_A_KEY_1 aliases PORT_A_KEY_START, so mapping
     * the pad's Button 1 to START would toggle playback on every note entry. */
    if (keys & PORT_A_KEY_1)     joy |= BUTTON_A;
    if (keys & PORT_A_KEY_2)     joy |= BUTTON_B;
#if defined(TARGET_GG)
    /* The Game Gear has no console PAUSE button; its START is a pad key. */
    if (keys & GG_KEY_START)     joy |= BUTTON_START;
#else
    if (SMS_queryPauseRequested()) {
        joy |= BUTTON_START;
        SMS_resetPauseRequest();
    }
#endif

    input_mapper_update(&g_tracker_state, &g_song, joy);
#endif
    ui_process_playback();
}

void ui_render(void) {
#if defined(__SMS__) || defined(__SDCC_z80)
    char buf[32];
    uint8_t start_row;
    uint8_t i;

    /* Status bar: only redrawn when its text changes. */
    format_status_str(buf);
    if (!s_last_status_valid || memcmp(s_last_status, buf, 26) != 0) {
        memcpy(s_last_status, buf, 26);
        s_last_status_valid = 1;
        sms_draw(2, 2, buf);
    }

    start_row = (g_tracker_state.cursor_row >= 6) ? (uint8_t)(g_tracker_state.cursor_row - 6) : 0;
    if (start_row > RDJ_MAX_ROWS - SMS_GRID_ROWS) {
        start_row = RDJ_MAX_ROWS - SMS_GRID_ROWS;
    }

    /* When the window scrolls, every visible row changes; otherwise only the
     * rows whose text actually differs are re-pushed. */
    if (!s_shadow_valid || start_row != s_shadow_start) {
        for (i = 0; i < SMS_GRID_ROWS; i++) {
            format_row_str((uint8_t)(start_row + i), buf);
            memcpy(s_shadow[i], buf, SMS_ROW_WIDTH);
            sms_draw(2, (uint8_t)(SMS_GRID_TOP + i), buf);
        }
        s_shadow_start = start_row;
        s_shadow_valid = 1;
        return;
    }

    for (i = 0; i < SMS_GRID_ROWS; i++) {
        format_row_str((uint8_t)(start_row + i), buf);
        if (memcmp(s_shadow[i], buf, SMS_ROW_WIDTH) != 0) {
            memcpy(s_shadow[i], buf, SMS_ROW_WIDTH);
            sms_draw(2, (uint8_t)(SMS_GRID_TOP + i), buf);
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

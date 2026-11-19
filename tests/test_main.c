/* RDJ Driver Integration Test */
/* GPLv3 - RetroDJ Project */

#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include "core/rdj_driver.h"
#include "core/sequencer.h"
#include "core/rdj_backend.h"
#include "core/rdj_ym2612.h"
#include "targets/genesis/src/input_mapper.h"
#include "targets/genesis/src/ui.h"
#include "drivers/gb_sound/rdj_gb_inst.h"
#include "drivers/gb_sound/gb_rdj.h"

extern void vgm_open(const char *filename);

/* Recording backend: captures APU and PSG writes so the NES volume mapping and
 * the SN76489 note encoding can be asserted without a hardware backend. */
static uint8_t test_apu[0x20];
static uint8_t test_gb_apu[0x40];
static uint8_t test_psg[64];
static uint8_t test_psg_count;
static uint8_t test_ym_port[64];
static uint8_t test_ym_reg[64];
static uint8_t test_ym_val[64];
static uint8_t test_ym_count;
static void rec_init(void) {
    for (int i = 0; i < 0x20; i++) test_apu[i] = 0;
    for (int i = 0; i < 0x40; i++) test_gb_apu[i] = 0;
    test_psg_count = 0;
    test_ym_count = 0;
}
static void rec_shutdown(void) {}
static void rec_write_ym2612(uint8_t port, uint8_t reg, uint8_t val) {
    if (test_ym_count < sizeof(test_ym_reg)) {
        test_ym_port[test_ym_count] = port;
        test_ym_reg[test_ym_count] = reg;
        test_ym_val[test_ym_count] = val;
        test_ym_count++;
    }
}
static void rec_write_apu(uint16_t addr, uint8_t val) {
    if (addr >= 0x4000 && addr <= 0x4015) test_apu[addr - 0x4000] = val;
    if (addr >= 0xFF10 && addr <= 0xFF3F) test_gb_apu[addr - 0xFF10] = val;
}
static void rec_write_psg(uint8_t val) {
    if (test_psg_count < sizeof(test_psg)) test_psg[test_psg_count++] = val;
}
static void rec_wait_samples(uint16_t samples) { (void)samples; }
static void rec_wait_frame(void) {}
static rdj_backend_t backend_recording = {
    "Recording Backend", rec_init, rec_shutdown,
    rec_write_ym2612, rec_write_apu, rec_write_psg,
    rec_wait_samples, rec_wait_frame
};

static void test_input_mapper(void) {
    printf("Testing Tracker UI & Input Mapper...\n");
    ui_init();

    input_tracker_state_t *state = ui_get_tracker_state();
    rdj_song_t *song = ui_get_song();

    assert(state != NULL);
    assert(song != NULL);

    /* Check initial state */
    assert(state->cursor_row == 0);
    assert(state->cursor_channel == 0);
    assert(state->cursor_subcol == RDJ_SUBCOL_NOTE);
    assert(state->is_playing == 0);

    /* 1. Test D-Pad navigation (DOWN) */
    input_mapper_update(state, song, BUTTON_DOWN);
    assert(state->cursor_row == 1);
    input_mapper_update(state, song, 0); /* Release button */

    /* 2. Test D-Pad navigation (RIGHT - sub-column move) */
    input_mapper_update(state, song, BUTTON_RIGHT);
    assert(state->cursor_subcol == RDJ_SUBCOL_INST);
    input_mapper_update(state, song, 0);

    /* 3. Test BUTTON_A + UP (Set initial note & Transpose +1 semitone) */
    state->cursor_subcol = RDJ_SUBCOL_NOTE;
    input_mapper_update(state, song, BUTTON_A | BUTTON_UP);
    /* Octave 4 default -> 4*12 + 12 = 60 (C-4) */
    uint8_t note_val = song->patterns[0].rows[1][0].note;
    assert(note_val == 60);
    input_mapper_update(state, song, BUTTON_A); /* Release UP while holding A */

    /* Transpose +1 semitone -> C#4 (61) */
    input_mapper_update(state, song, BUTTON_A | BUTTON_UP);
    assert(song->patterns[0].rows[1][0].note == 61);
    input_mapper_update(state, song, 0);

    /* 4. Test BUTTON_A + RIGHT (Transpose +1 octave = +12 semitones -> 73) */
    input_mapper_update(state, song, BUTTON_A | BUTTON_RIGHT);
    assert(song->patterns[0].rows[1][0].note == 73);
    input_mapper_update(state, song, 0);

    /* 5. Test BUTTON_B + UP on Instrument (increment inst) */
    state->cursor_subcol = RDJ_SUBCOL_INST;
    input_mapper_update(state, song, BUTTON_B | BUTTON_UP);
    assert(song->patterns[0].rows[1][0].instrument == 1);
    input_mapper_update(state, song, 0);

    /* 6. Test BUTTON_B + RIGHT on Volume (increment vol by 16) */
    state->cursor_subcol = RDJ_SUBCOL_VOL;
    input_mapper_update(state, song, BUTTON_B | BUTTON_RIGHT);
    assert(song->patterns[0].rows[1][0].volume == 16);
    input_mapper_update(state, song, 0);

    /* 7. Test BUTTON_B + BUTTON_A (Clear current cell) */
    input_mapper_update(state, song, BUTTON_B | BUTTON_A);
    assert(song->patterns[0].rows[1][0].note == RDJ_NOTE_EMPTY);
    assert(song->patterns[0].rows[1][0].instrument == 0);
    assert(song->patterns[0].rows[1][0].volume == 0);
    input_mapper_update(state, song, 0);

    /* 8. Test BUTTON_START (Toggle Play / Stop).
     * The toggle fires on release so that START held with a direction can cycle
     * screens without also starting playback. */
    assert(state->is_playing == 0);
    input_mapper_update(state, song, BUTTON_START);
    assert(state->is_playing == 0);      /* still held: no toggle yet */
    input_mapper_update(state, song, 0);
    assert(state->is_playing == 1);      /* released alone: toggled on */
    input_mapper_update(state, song, BUTTON_START);
    input_mapper_update(state, song, 0);
    assert(state->is_playing == 0);
    input_mapper_update(state, song, 0);

    printf("Input Mapper & Tracker UI tests passed successfully!\n");
}

static void test_sequencer_playback(void) {
    printf("Testing Polyphonic Multi-Channel Sequencer Playback & VBlank Ticker...\n");
    ui_init();

    input_tracker_state_t *state = ui_get_tracker_state();
    rdj_song_t *song = ui_get_song();

    assert(state != NULL);
    assert(song != NULL);

    /* Configure a 3-row test pattern with 4 polyphonic channels */
    song->patterns[0].length = 3;
    song->patterns[0].speed = 6;

    /* Row 0: Polyphonic chord across CH0..CH3 */
    song->patterns[0].rows[0][0].note = 60; /* C-4 */
    song->patterns[0].rows[0][1].note = 64; /* E-4 */
    song->patterns[0].rows[0][2].note = 67; /* G-4 */
    song->patterns[0].rows[0][3].note = 72; /* C-5 */

    /* Row 1: Note-OFF on CH0, Sustain CH1 & CH3, New note on CH2 */
    song->patterns[0].rows[1][0].note = RDJ_NOTE_OFF;
    song->patterns[0].rows[1][1].note = RDJ_NOTE_EMPTY; /* Sustain */
    song->patterns[0].rows[1][2].note = 71;            /* B-4 */
    song->patterns[0].rows[1][3].note = RDJ_NOTE_EMPTY; /* Sustain */

    /* Row 2: New bass note on CH0, Note-OFF on CH1..CH3 */
    song->patterns[0].rows[2][0].note = 48; /* C-3 */
    song->patterns[0].rows[2][1].note = RDJ_NOTE_OFF;
    song->patterns[0].rows[2][2].note = RDJ_NOTE_OFF;
    song->patterns[0].rows[2][3].note = RDJ_NOTE_OFF;

    /* Start playback */
    state->cursor_row = 0;
    state->is_playing = 1;

    /* Frame 0: Trigger Row 0 */
    ui_update();
    assert(state->cursor_row == 0);

    /* Frames 1..5: Advance ticks (5 frames, row remains 0) */
    for (int f = 0; f < 5; f++) {
        ui_update();
        assert(state->cursor_row == 0);
    }

    /* Frame 6 (Tick 6): Advance to Row 1 */
    ui_update();
    assert(state->cursor_row == 1);

    /* Frames 7..11: Row remains 1 */
    for (int f = 0; f < 5; f++) {
        ui_update();
        assert(state->cursor_row == 1);
    }

    /* Frame 12 (Tick 12): Advance to Row 2 */
    ui_update();
    assert(state->cursor_row == 2);

    /* Frames 13..17: Row remains 2 */
    for (int f = 0; f < 5; f++) {
        ui_update();
        assert(state->cursor_row == 2);
    }

    /* Frame 18 (Tick 18): Wrap back to Row 0 */
    ui_update();
    assert(state->cursor_row == 0);

    /* Stop playback */
    state->is_playing = 0;
    ui_update();
    assert(state->is_playing == 0);

    printf("Polyphonic Multi-Channel Sequencer Playback tests passed successfully!\n");
}

static void test_sn76489_driver(void) {
    printf("Testing SN76489 (Sega PSG) driver...\n");

    /* Capture every PSG byte the driver emits. */
    rdj_set_backend(&backend_recording);
    sn76489_driver.init(3579545);

    /* init() silences all four channels: 0x9F, 0xBF, 0xDF, 0xFF. */
    assert(test_psg_count == 4);
    assert(test_psg[0] == 0x9F);
    assert(test_psg[1] == 0xBF);
    assert(test_psg[2] == 0xDF);
    assert(test_psg[3] == 0xFF);

    /* note_on channel 0, MIDI 69 (A-4, 440 Hz): wavelength = 3579545 / (32*440)
     * = 254 = 0x0FE. Latch byte: 0x80 | ch<<5 | low nibble (0x0E) = 0x8E,
     * then data byte (0xFE >> 4) & 0x3F = 0x0F. Volume latch for vol 128 maps
     * to attenuation 0: 0x80 | 0x10 | 0 = 0x90. */
    test_psg_count = 0;
    sn76489_driver.note_on(0, 69, 0, 128);
    assert(test_psg_count == 3);
    assert(test_psg[0] == 0x8E); /* latch: ch0, freq, low nibble */
    assert(test_psg[1] == 0x0F); /* data: high 6 bits */
    assert(test_psg[2] == 0x90); /* latch: ch0, volume, attenuation 0 */

    /* Volume mapping: driver 0..128 -> attenuation 15..0. The top of the range
     * must clamp to 0 (loudest) instead of wrapping back to silence. */
    test_psg_count = 0;
    sn76489_driver.note_on(1, 60, 0, 64);   /* 64>>3 = 8 -> attenuation 7 */
    assert(test_psg[2] == (uint8_t)(0x80 | (1 << 5) | 0x10 | 7));
    sn76489_driver.note_on(1, 60, 0, 0);    /* 0 -> attenuation 15 (silent) */
    assert(test_psg[5] == (uint8_t)(0x80 | (1 << 5) | 0x10 | 15));

    /* note_off writes the channel's volume latch at full attenuation. */
    test_psg_count = 0;
    sn76489_driver.note_off(2);
    assert(test_psg_count == 1);
    assert(test_psg[0] == (uint8_t)(0x80 | (2 << 5) | 0x10 | 15));

    /* Channel 3 is the noise generator: 0xE0 | white(1)<<3 | note&7. */
    test_psg_count = 0;
    sn76489_driver.note_on(3, 3, 1, 128);   /* white noise, divider 3 */
    assert(test_psg_count == 2);
    assert(test_psg[0] == (uint8_t)(0xE0 | 0x08 | 3));
    assert(test_psg[1] == (uint8_t)(0x80 | (3 << 5) | 0x10 | 0));

    rdj_set_backend(&backend_null);
    printf("SN76489 driver tests passed successfully!\n");
}

/* Dual 3-button navigation: screen cycling, cursor walk into the sidebar and
 * the START-modifier consumption rule. */
static void test_screen_navigation(void) {
    printf("Testing Dual 3-Button Screen Navigation...\n");
    ui_init();

    input_tracker_state_t *state = ui_get_tracker_state();
    rdj_song_t *song = ui_get_song();

    /* Starts on the pattern editor. */
    assert(state->screen == UI_SCREEN_PAT);

    /* START + RIGHT cycles PAT -> SONG, and consumes START so no playback. */
    input_mapper_update(state, song, BUTTON_START);
    input_mapper_update(state, song, BUTTON_START | BUTTON_RIGHT);
    assert(state->screen == UI_SCREEN_SONG);
    input_mapper_update(state, song, BUTTON_START);
    input_mapper_update(state, song, 0);
    assert(state->is_playing == 0);   /* START consumed: no toggle */

    /* START + RIGHT again -> PATCH, then wraps back to PAT. */
    input_mapper_update(state, song, BUTTON_START);
    input_mapper_update(state, song, BUTTON_START | BUTTON_RIGHT);
    assert(state->screen == UI_SCREEN_PATCH);
    input_mapper_update(state, song, BUTTON_START);
    input_mapper_update(state, song, BUTTON_START | BUTTON_RIGHT);
    assert(state->screen == UI_SCREEN_PAT);
    input_mapper_update(state, song, 0);

    /* START + LEFT cycles backwards: PAT -> PATCH. */
    input_mapper_update(state, song, BUTTON_START);
    input_mapper_update(state, song, BUTTON_START | BUTTON_LEFT);
    assert(state->screen == UI_SCREEN_PATCH);
    input_mapper_update(state, song, 0);

    /* 6-button MODE cycles on its own: PATCH -> PAT. */
    input_mapper_update(state, song, BUTTON_MODE);
    assert(state->screen == UI_SCREEN_PAT);
    input_mapper_update(state, song, 0);

    /* Cursor walk: from FM6 (last sub-column of channel 5) RIGHT enters the
     * sidebar; UP/DOWN picks a tab and A opens it. */
    state->screen = UI_SCREEN_PAT;
    state->cursor_channel = RDJ_MAX_CHANNELS - 1;
    state->cursor_subcol = RDJ_NUM_SUBCOLS - 1;
    state->sidebar_focus = 0;
    input_mapper_update(state, song, 0);
    input_mapper_update(state, song, BUTTON_RIGHT);
    assert(state->sidebar_focus == 1);
    input_mapper_update(state, song, 0);

    /* Tab starts at PAT; DOWN -> SONG, DOWN -> PATCH. */
    assert(state->sidebar_tab == UI_TAB_PAT);
    input_mapper_update(state, song, BUTTON_DOWN);
    assert(state->sidebar_tab == UI_TAB_SONG);
    input_mapper_update(state, song, 0);
    input_mapper_update(state, song, BUTTON_DOWN);
    assert(state->sidebar_tab == UI_TAB_PATCH);
    input_mapper_update(state, song, 0);

    /* A enters the highlighted screen and leaves the sidebar. */
    input_mapper_update(state, song, BUTTON_A);
    assert(state->screen == UI_SCREEN_PATCH);
    assert(state->sidebar_focus == 0);
    input_mapper_update(state, song, 0);

    /* Walk in again and LEFT returns to the grid without switching screens. */
    state->screen = UI_SCREEN_PAT;
    state->sidebar_focus = 0;
    input_mapper_update(state, song, BUTTON_RIGHT);
    assert(state->sidebar_focus == 1);
    input_mapper_update(state, song, 0);
    input_mapper_update(state, song, BUTTON_LEFT);
    assert(state->sidebar_focus == 0);
    assert(state->screen == UI_SCREEN_PAT);
    input_mapper_update(state, song, 0);

    printf("Dual 3-Button Screen Navigation tests passed successfully!\n");
}

/* SONG screen: slot/pattern assignment, order length and the pattern preview
 * follow the selected slot. */
static void test_song_playlist(void) {
    printf("Testing Song Playlist Editing...\n");
    ui_init();

    input_tracker_state_t *state = ui_get_tracker_state();
    rdj_song_t *song = ui_get_song();

    state->screen = UI_SCREEN_SONG;
    state->song_cursor = 0;

    /* Default order is identity and length 1. */
    assert(song->order_length == 1);
    assert(song->order[0] == 0);

    /* A + UP assigns the next pattern to slot 0. */
    input_mapper_update(state, song, BUTTON_A);
    input_mapper_update(state, song, BUTTON_A | BUTTON_UP);
    assert(song->order[0] == 1);
    input_mapper_update(state, song, BUTTON_A);          /* release UP */
    input_mapper_update(state, song, BUTTON_A | BUTTON_UP);
    assert(song->order[0] == 2);
    input_mapper_update(state, song, 0);

    /* A + DOWN steps it back. */
    input_mapper_update(state, song, BUTTON_A);
    input_mapper_update(state, song, BUTTON_A | BUTTON_DOWN);
    assert(song->order[0] == 1);
    input_mapper_update(state, song, 0);

    /* B + UP extends the order length; the cursor can then move into slot 1. */
    input_mapper_update(state, song, BUTTON_B);
    input_mapper_update(state, song, BUTTON_B | BUTTON_UP);
    assert(song->order_length == 2);
    input_mapper_update(state, song, BUTTON_B);          /* release UP */
    input_mapper_update(state, song, BUTTON_B | BUTTON_UP);
    assert(song->order_length == 3);
    input_mapper_update(state, song, 0);
    input_mapper_update(state, song, BUTTON_DOWN);
    assert(state->song_cursor == 1);
    input_mapper_update(state, song, 0);

    /* B + DOWN shrinks it and clamps the cursor. */
    input_mapper_update(state, song, BUTTON_B);
    input_mapper_update(state, song, BUTTON_B | BUTTON_DOWN);
    assert(song->order_length == 2);
    input_mapper_update(state, song, BUTTON_B);          /* release DOWN */
    input_mapper_update(state, song, BUTTON_B | BUTTON_DOWN);
    assert(song->order_length == 1);
    assert(state->song_cursor == 0);
    input_mapper_update(state, song, 0);

    /* A + RIGHT opens the slot's pattern in the PAT editor. */
    state->song_cursor = 0;
    song->order[0] = 3;
    input_mapper_update(state, song, BUTTON_A);
    input_mapper_update(state, song, BUTTON_A | BUTTON_RIGHT);
    assert(state->screen == UI_SCREEN_PAT);
    assert(state->edit_pattern == 3);
    input_mapper_update(state, song, 0);

    printf("Song Playlist Editing tests passed successfully!\n");
}

/* Playback walks the order list: after the last row of a pattern it advances to
 * the next order slot (and wraps at the end of the song). */
static void test_song_order_playback(void) {
    printf("Testing Song Order Playback Chain...\n");
    ui_init();

    input_tracker_state_t *state = ui_get_tracker_state();
    rdj_song_t *song = ui_get_song();

    /* Two one-row patterns chained: order = [0, 1]. */
    song->order[0] = 0;
    song->order[1] = 1;
    song->order_length = 2;
    song->patterns[0].length = 1;
    song->patterns[1].length = 1;
    song->patterns[0].speed = 1;
    song->patterns[1].speed = 1;
    song->patterns[0].rows[0][0].note = 60;  /* C-4 */
    song->patterns[1].rows[0][0].note = 64;  /* E-4 */

    state->screen = UI_SCREEN_SONG;   /* playback position lives in the state */
    state->is_playing = 1;

    /* Frame 0: play order slot 0, row 0. */
    ui_update();
    assert(state->play_order_pos == 0);

    /* Frame 1: advance past the single row -> order slot 1. */
    ui_update();
    assert(state->play_order_pos == 1);

    /* Frame 2: advance -> wraps back to order slot 0. */
    ui_update();
    assert(state->play_order_pos == 0);

    state->is_playing = 0;
    ui_update();

    printf("Song Order Playback Chain tests passed successfully!\n");
}

/* PATCH editor: B + DIR edits the selected operator parameter in the live patch
 * bank and re-writes the YM2612 registers so the change is audible at once. */
static void test_patch_editor(void) {
    printf("Testing FM Patch Editor...\n");
    ui_init();

    input_tracker_state_t *state = ui_get_tracker_state();
    rdj_song_t *song = ui_get_song();

    ym2612_patch_t *p = ym2612_rdj_get_patch(0);
    assert(p != NULL);
    assert(ym2612_rdj_get_patch(YM2612_MAX_PATCHES) == NULL);

    /* The recording backend captures the register writes the editor emits. */
    rdj_set_backend(&backend_recording);
    ym2612_driver.init(7670454);

    state->screen = UI_SCREEN_PATCH;
    state->patch_slot = 0;
    state->patch_op = 0;

    /* Select ALGORITHM (row 0) and bump it: 4 -> 5. */
    state->patch_row = UI_PATCH_ROW_ALGO;
    {
        uint8_t before = p->FB_ALG & 0x07;
        input_mapper_update(state, song, BUTTON_B);
        input_mapper_update(state, song, BUTTON_B | BUTTON_UP);
        assert((p->FB_ALG & 0x07) == (uint8_t)(before + 1));
        input_mapper_update(state, song, 0);
    }

    /* Select OP2 TOTAL LEVEL (row 4, op 1) and lower it by one. */
    state->patch_row = UI_PATCH_ROW_TL;
    state->patch_op = 1;
    {
        uint8_t before = p->ops[1].TL;
        test_ym_count = 0;
        input_mapper_update(state, song, BUTTON_B);
        input_mapper_update(state, song, BUTTON_B | BUTTON_DOWN);
        assert(p->ops[1].TL == (uint8_t)(before - 1));
        input_mapper_update(state, song, 0);
        /* The live update must have written the operator's total level (0x40). */
        {
            int found = 0;
            for (int i = 0; i < test_ym_count; i++) {
                if (test_ym_reg[i] == 0x40) { found = 1; break; }
            }
            assert(found == 1);
        }
    }

    /* A + RIGHT switches instrument slot. */
    state->patch_slot = 0;
    input_mapper_update(state, song, BUTTON_A);
    input_mapper_update(state, song, BUTTON_A | BUTTON_RIGHT);
    assert(state->patch_slot == 1);
    input_mapper_update(state, song, 0);
    input_mapper_update(state, song, BUTTON_A);
    input_mapper_update(state, song, BUTTON_A | BUTTON_LEFT);
    assert(state->patch_slot == 0);
    input_mapper_update(state, song, 0);

    rdj_set_backend(&backend_null);
    printf("FM Patch Editor tests passed successfully!\n");
}


/* Game Boy instrument model: the [INST] screen edits exactly these fields, so
 * the clamps and the wave presets are asserted directly. */
static void test_gb_instrument_model(void) {
    gb_inst_t inst;
    uint8_t wave[16];
    int i;

    printf("Testing GB Instrument Model...\n");

    inst.channel = GB_INST_CH_PULSE;
    inst.duty = 2;
    inst.volume = 15;
    inst.env_dir = GB_INST_ENV_DOWN;
    inst.env_pace = 0;
    inst.sweep = 0;
    inst.wave = 0;
    inst.noise = 0;

    /* Duty walks 0..3 and saturates. */
    gb_inst_step(&inst, GB_INST_ROW_DUTY, 1);
    assert(inst.duty == 3);
    gb_inst_step(&inst, GB_INST_ROW_DUTY, 1);
    assert(inst.duty == 3);
    gb_inst_step(&inst, GB_INST_ROW_DUTY, -1);
    assert(inst.duty == 2);

    /* Volume saturates at both ends of the 4-bit range. */
    gb_inst_step(&inst, GB_INST_ROW_VOL, -100);
    assert(inst.volume == 0);
    gb_inst_step(&inst, GB_INST_ROW_VOL, 100);
    assert(inst.volume == 15);

    /* Envelope pace and sweep are 3-bit, the noise flag is 1-bit. */
    gb_inst_step(&inst, GB_INST_ROW_ENVPACE, 9);
    assert(inst.env_pace == 7);
    gb_inst_step(&inst, GB_INST_ROW_SWEEP, 9);
    assert(inst.sweep == 7);
    gb_inst_step(&inst, GB_INST_ROW_NOISE, 5);
    assert(inst.noise == 1);

    /* The channel row cycles the three APU channel types. */
    inst.channel = GB_INST_CH_PULSE;
    gb_inst_step(&inst, GB_INST_ROW_CHANNEL, 1);
    assert(inst.channel == GB_INST_CH_WAVE);
    gb_inst_step(&inst, GB_INST_ROW_CHANNEL, 1);
    assert(inst.channel == GB_INST_CH_NOISE);
    gb_inst_step(&inst, GB_INST_ROW_CHANNEL, 1);
    assert(inst.channel == GB_INST_CH_NOISE);

    /* Wave presets are distinct and the lookup wraps. */
    gb_inst_wave_data(0, wave);
    assert(wave[0] == 0x01 && wave[7] == 0xEF);
    {
        uint8_t other[16];
        uint8_t diff = 0;
        gb_inst_wave_data(2, other);
        for (i = 0; i < 16; i++) if (other[i] != wave[i]) diff = 1;
        assert(diff); /* preset 2 (saw) must differ from preset 0 (triangle) */
        gb_inst_wave_data(GB_WAVE_PRESET_COUNT, other); /* wraps to 0 */
        for (i = 0; i < 16; i++) assert(other[i] == wave[i]);
    }

    /* Duty percentages match the APU's four waveform shapes. */
    assert(gb_inst_duty_percent(0) == 12);
    assert(gb_inst_duty_percent(1) == 25);
    assert(gb_inst_duty_percent(2) == 50);
    assert(gb_inst_duty_percent(3) == 75);

    /* Every editable row has a non-zero range, else B+direction would no-op. */
    for (i = 0; i < GB_INST_NUM_PARAMS; i++) {
        assert(gb_inst_param_max[i] > 0);
    }

    printf("GB Instrument Model tests passed successfully!\n");
}

/* The driver must translate an instrument into real APU register writes: duty
 * in NRx1, the envelope in NRx2, the timer in NRx3/NRx4 and the trigger bit. */
static void test_gb_driver_registers(void) {
    gb_inst_t *in;

    printf("Testing GB APU Register Output...\n");

    rdj_set_backend(&backend_recording);
    rec_init();
    gb_sound_driver.init(0);

    /* Slot 0 is the historical default: 50% duty, full volume, no envelope. */
    in = gb_sound_inst(0);
    in->channel = GB_INST_CH_PULSE;
    in->duty = 2;
    in->volume = 15;
    in->env_dir = GB_INST_ENV_DOWN;
    in->env_pace = 0;
    in->sweep = 0;

    rec_init();
    gb_sound_driver.note_on(0, 48, 0, 0);

    /* NR11 holds duty in bits 7-6. */
    assert(((test_gb_apu[0x01] >> 6) & 0x03) == 2);
    /* NR12: initial volume 15, pace 0 means the level holds. */
    assert((test_gb_apu[0x02] >> 4) == 15);
    assert((test_gb_apu[0x02] & 0x07) == 0);
    /* NR14 must set the trigger bit. */
    assert(test_gb_apu[0x04] & 0x80);
    /* A note-off must clear the DAC (NR12 volume nibble) so the channel is
     * silent rather than stuck at its last amplitude. */
    gb_sound_driver.note_off(0);
    assert((test_gb_apu[0x02] & 0xF8) == 0);

    /* Duty is honoured from the instrument. */
    in->duty = 3;
    rec_init();
    gb_sound_driver.note_on(0, 48, 0, 0);
    assert(((test_gb_apu[0x01] >> 6) & 0x03) == 3);

    /* An envelope pace is encoded in NR12 bits 2-0 with direction in bit 3. */
    in->env_pace = 5;
    in->env_dir = GB_INST_ENV_UP;
    rec_init();
    gb_sound_driver.note_on(0, 48, 0, 0);
    assert((test_gb_apu[0x02] & 0x07) == 5);
    assert(test_gb_apu[0x02] & 0x08);

    /* CH2 uses the NR2x block. */
    rec_init();
    gb_sound_driver.note_on(1, 48, 0, 0);
    assert(((test_gb_apu[0x06] >> 6) & 0x03) == 3);
    assert(test_gb_apu[0x09] & 0x80);

    /* CH3 loads its wave preset into Wave RAM and sets the DAC enable bit. */
    in->channel = GB_INST_CH_WAVE;
    in->wave = 2;
    rec_init();
    gb_sound_driver.note_on(2, 48, 0, 0);
    assert(test_gb_apu[0x0A] & 0x80);          /* NR30: CH3 on */
    assert(test_gb_apu[0x0E] & 0x80);          /* NR34: trigger */
    {
        uint8_t expect[16];
        uint8_t i;
        gb_inst_wave_data(2, expect);
        for (i = 0; i < 16; i++) {
            assert(test_gb_apu[0x20 + i] == expect[i]);
        }
    }

    /* CH4 sets its trigger and honours the 7-bit polynomial flag. */
    in->channel = GB_INST_CH_NOISE;
    in->noise = 1;
    rec_init();
    gb_sound_driver.note_on(3, 48, 0, 0);
    assert(test_gb_apu[0x13] & 0x80);          /* NR44: trigger */
    assert(test_gb_apu[0x13 - 0x02] & 0x08);   /* NR43: 7-bit counter */

    rdj_set_backend(&backend_null);
    printf("GB APU Register Output tests passed successfully!\n");
}

/* The GB target installs its own third screen, so the shared cycle must reach
 * it and the mapper must not run the YM2612 patch editor in its place. */
static void test_gb_screen_hook(void) {
    input_tracker_state_t state;
    rdj_song_t song;
    int r, c;

    printf("Testing GB Screen Hook...\n");

    input_mapper_init(&state);
    assert(state.screen == UI_SCREEN_PAT);
    /* The GB's third screen occupies the shared third slot. */
    assert(UI_SCREEN_INST == UI_SCREEN_PATCH);

    /* The order list has its own capacity, which may differ from the pattern
     * count; every consumer must use RDJ_MAX_ORDER for it. */
    assert(RDJ_MAX_ORDER > 0);
    assert(RDJ_MAX_ORDER <= RDJ_MAX_PATTERNS || RDJ_MAX_ORDER > RDJ_MAX_PATTERNS);

    for (r = 0; r < RDJ_MAX_ROWS; r++)
        for (c = 0; c < RDJ_MAX_CHANNELS; c++)
            song.patterns[0].rows[r][c].note = RDJ_NOTE_EMPTY;
    song.order_length = 1;
    song.order[0] = 0;

    /* SELECT cycles the same way START does. */
    input_mapper_update(&state, &song, BUTTON_SELECT);
    input_mapper_update(&state, &song, BUTTON_SELECT | BUTTON_RIGHT);
    assert(state.screen == UI_SCREEN_SONG);
    input_mapper_update(&state, &song, BUTTON_SELECT);
    input_mapper_update(&state, &song, BUTTON_SELECT | BUTTON_RIGHT);
    assert(state.screen == UI_SCREEN_INST);
    input_mapper_update(&state, &song, BUTTON_SELECT);
    input_mapper_update(&state, &song, BUTTON_SELECT | BUTTON_RIGHT);
    assert(state.screen == UI_SCREEN_PAT);
    input_mapper_update(&state, &song, 0);

    /* SELECT+LEFT cycles backwards. */
    input_mapper_update(&state, &song, BUTTON_SELECT);
    input_mapper_update(&state, &song, BUTTON_SELECT | BUTTON_LEFT);
    assert(state.screen == UI_SCREEN_INST);
    input_mapper_update(&state, &song, 0);

    /* SELECT+direction must not edit a cell on the way past. */
    state.screen = UI_SCREEN_PAT;
    state.cursor_row = 0;
    state.cursor_channel = 0;
    state.cursor_subcol = RDJ_SUBCOL_NOTE;
    state.edit_pattern = 0;
    input_mapper_update(&state, &song, BUTTON_SELECT);
    input_mapper_update(&state, &song, BUTTON_SELECT | BUTTON_UP);
    input_mapper_update(&state, &song, 0);
    assert(song.patterns[0].rows[0][0].note == RDJ_NOTE_EMPTY);

    /* SELECT alone must not toggle playback (it is not START). */
    input_mapper_update(&state, &song, BUTTON_SELECT);
    input_mapper_update(&state, &song, 0);
    assert(state.is_playing == 0);

    printf("GB Screen Hook tests passed successfully!\n");
}

int main(void) {
    printf("Testing Backends...\n");
    rdj_set_backend(&backend_null);
    assert(rdj_active_backend == &backend_null);

    printf("Testing YM2612 Driver with Null Backend...\n");
    ym2612_driver.init(7670454);
    ym2612_driver.write_reg(0x002B, 0x00);
    ym2612_driver.note_on(0, 60, 0, 128);
    ym2612_driver.note_off(0);

    printf("Testing NES 2A03 volume mapping...\n");
    /* The audition path passes vol == 128, the top of the 0..128 driver range.
     * The 2A03 volume nibble is 0..15, so 128 must map to 15 rather than wrap
     * to 0 (which silenced every audition). */
    rdj_set_backend(&backend_recording);
    nes_2a03_driver.init(1789773);
    nes_2a03_driver.note_on(0, 60, 0, 128);
    assert(test_apu[0x00] == 0xBF); /* duty 2, constant volume, volume 15 */
    nes_2a03_driver.note_on(0, 60, 0, 64);
    assert((test_apu[0x00] & 0x0F) == 8);
    nes_2a03_driver.note_on(0, 60, 0, 0);
    assert((test_apu[0x00] & 0x0F) == 0);
    nes_2a03_driver.note_off(0);
    rdj_set_backend(&backend_null);

    printf("Testing NES 2A03 Driver with VGM Backend...\n");
    vgm_open("test_output.vgm");
    rdj_set_backend(&backend_vgm);
    nes_2a03_driver.init(1789773);
    nes_2a03_driver.note_on(0, 60, 0, 128);

    if (rdj_active_backend->wait_frame) {
        rdj_active_backend->wait_frame(); /* 735 samples */
    }
    if (rdj_active_backend->wait_samples) {
        rdj_active_backend->wait_samples(100); /* 100 samples */
    }

    nes_2a03_driver.note_off(0);

    /* Switch backend to flush/close VGM */
    rdj_set_backend(&backend_null);

    /* Verify VGM file header values */
    FILE *f = fopen("test_output.vgm", "rb");
    assert(f != NULL);

    fseek(f, 0x18, SEEK_SET);
    uint32_t total_samples = 0;
    size_t read_bytes = fread(&total_samples, sizeof(uint32_t), 1, f);
    assert(read_bytes == 1);
    assert(total_samples == 835); /* 735 + 100 */

    fseek(f, 0x04, SEEK_SET);
    uint32_t eof_offset = 0;
    read_bytes = fread(&eof_offset, sizeof(uint32_t), 1, f);
    assert(read_bytes == 1);
    assert(eof_offset > 0);
    fclose(f);

    printf("VGM wait & sample tracking verification passed! Total samples = %u, EOF offset = %u\n", total_samples, eof_offset);

    printf("Testing YM2612 Driver with Serial Backend...\n");
    rdj_set_backend(&backend_serial);
    ym2612_driver.note_on(1, 64, 0, 128);
    ym2612_driver.note_off(1);

    rdj_set_backend(&backend_null);

    test_input_mapper();
    test_sequencer_playback();
    test_sn76489_driver();
    test_screen_navigation();
    test_song_playlist();
    test_gb_instrument_model();
    test_gb_driver_registers();
    test_gb_screen_hook();
    test_song_order_playback();
    test_patch_editor();

    printf("All RetroDJ backend tests passed successfully!\n");
    return 0;
}

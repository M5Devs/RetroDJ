/* RDJ Driver Integration Test */
/* GPLv3 - RetroDJ Project */

#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include "core/rdj_driver.h"
#include "core/sequencer.h"
#include "core/rdj_backend.h"
#include "targets/genesis/src/input_mapper.h"
#include "targets/genesis/src/ui.h"

extern void vgm_open(const char *filename);

/* Recording backend: captures APU and PSG writes so the NES volume mapping and
 * the SN76489 note encoding can be asserted without a hardware backend. */
static uint8_t test_apu[0x20];
static uint8_t test_psg[64];
static uint8_t test_psg_count;
static void rec_init(void) {
    for (int i = 0; i < 0x20; i++) test_apu[i] = 0;
    test_psg_count = 0;
}
static void rec_shutdown(void) {}
static void rec_write_ym2612(uint8_t port, uint8_t reg, uint8_t val) {
    (void)port; (void)reg; (void)val;
}
static void rec_write_apu(uint16_t addr, uint8_t val) {
    if (addr >= 0x4000 && addr <= 0x4015) test_apu[addr - 0x4000] = val;
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

    /* 8. Test BUTTON_START (Toggle Play / Stop) */
    assert(state->is_playing == 0);
    input_mapper_update(state, song, BUTTON_START);
    assert(state->is_playing == 1);
    input_mapper_update(state, song, 0);
    input_mapper_update(state, song, BUTTON_START);
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

    printf("All RetroDJ backend tests passed successfully!\n");
    return 0;
}

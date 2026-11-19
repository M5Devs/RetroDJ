/* RDJ Game Boy APU Instrument Model */
/* GPLv3 - RetroDJ Project */

#include "core/rdj_gb_inst.h"

/* Ranges for the [INST] screen's editable rows, in GB_INST_ROW_* order. */
const uint8_t gb_inst_param_max[GB_INST_NUM_PARAMS] = {
    GB_INST_CH_NOISE,         /* channel  */
    3,                        /* duty     */
    15,                       /* volume   */
    1,                        /* env dir  */
    7,                        /* env pace */
    7,                        /* sweep    */
    GB_WAVE_PRESET_COUNT - 1, /* wave     */
    1                         /* noise    */
};

/* Wave RAM presets, each 32 4-bit samples packed two per byte. */
static const uint8_t s_wave_presets[GB_WAVE_PRESET_COUNT][16] = {
    /* 0: triangle */
    {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
     0xED, 0xCB, 0xA9, 0x87, 0x65, 0x43, 0x21, 0x00},
    /* 1: sine approximation */
    {0x89, 0xAC, 0xDE, 0xEF, 0xFE, 0xDC, 0xA9, 0x86,
     0x57, 0x34, 0x12, 0x01, 0x02, 0x34, 0x56, 0x78},
    /* 2: sawtooth */
    {0x01, 0x12, 0x23, 0x34, 0x45, 0x56, 0x67, 0x78,
     0x89, 0x9A, 0xAB, 0xBC, 0xCD, 0xDE, 0xEF, 0xFF},
    /* 3: 25% pulse */
    {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
     0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00}
};

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void gb_inst_step(gb_inst_t *inst, uint8_t row, int delta) {
    if (!inst) return;

    switch (row) {
        case GB_INST_ROW_CHANNEL:
            inst->channel = (uint8_t)clamp_int((int)inst->channel + delta, 0, GB_INST_CH_NOISE);
            break;
        case GB_INST_ROW_DUTY:
            inst->duty = (uint8_t)clamp_int((int)inst->duty + delta, 0, 3);
            break;
        case GB_INST_ROW_VOL:
            inst->volume = (uint8_t)clamp_int((int)inst->volume + delta, 0, 15);
            break;
        case GB_INST_ROW_DIR:
            inst->env_dir = (uint8_t)clamp_int((int)inst->env_dir + delta, 0, 1);
            break;
        case GB_INST_ROW_ENVPACE:
            inst->env_pace = (uint8_t)clamp_int((int)inst->env_pace + delta, 0, 7);
            break;
        case GB_INST_ROW_SWEEP:
            inst->sweep = (uint8_t)clamp_int((int)inst->sweep + delta, 0, 7);
            break;
        case GB_INST_ROW_WAVE:
            inst->wave = (uint8_t)clamp_int((int)inst->wave + delta, 0, GB_WAVE_PRESET_COUNT - 1);
            break;
        case GB_INST_ROW_NOISE:
            inst->noise = (uint8_t)clamp_int((int)inst->noise + delta, 0, 1);
            break;
        default:
            break;
    }
}

uint8_t gb_inst_duty_percent(uint8_t duty) {
    switch (duty & 0x03) {
        case 0:  return 12;
        case 1:  return 25;
        case 2:  return 50;
        default: return 75;
    }
}

void gb_inst_wave_data(uint8_t preset, uint8_t *out16) {
    uint8_t i;
    const uint8_t *src;
    if (!out16) return;
    src = s_wave_presets[preset % GB_WAVE_PRESET_COUNT];
    for (i = 0; i < 16; i++) out16[i] = src[i];
}

/*
 * PSX Verb Audio FX Plugin
 *
 * PlayStation 1 SPU reverb emulation with:
 * - Preset: 6 classic PSX reverb types
 * - Decay: Wall reflection feedback amount
 * - Mix: Dry/wet blend
 *
 * Structure (per channel, stereo linked):
 * Input -> [IIR lowpass] -> [Comb bank (4x)] -> [Allpass (2x)] -> Output
 *                                  ^                    |
 *                                  |____ feedback ______|
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "audio_fx_api_v1.h"

#define SAMPLE_RATE 44100

/* Work buffer size: 16384 samples per channel (power of 2 for easy masking) */
#define WORK_SIZE 16384
#define WORK_MASK (WORK_SIZE - 1)

/* Allpass coefficient */
#define APF_COEFF 0.5f

/* Plugin state */
static const host_api_v1_t *g_host = NULL;
static audio_fx_api_v1_t g_fx_api;

/* Parameters */
static int g_preset = 4;        /* 0-5: Room, Studio S/M/L, Hall, Space Echo */
static float g_decay = 0.7f;    /* Wall reflection feedback (0.0-1.0) */
static float g_mix = 0.35f;     /* Dry/wet mix */

/* Work buffers (circular, stereo) */
static float g_work_l[WORK_SIZE];
static float g_work_r[WORK_SIZE];
static int g_work_pos = 0;

/* IIR lowpass state */
static float g_iir_l = 0.0f;
static float g_iir_r = 0.0f;

/* Allpass filter states */
static float g_apf1_l[2048];
static float g_apf1_r[2048];
static float g_apf2_l[2048];
static float g_apf2_r[2048];

/* Current preset delay times */
static int g_comb1_delay = 4000;
static int g_comb2_delay = 4400;
static int g_comb3_delay = 4800;
static int g_comb4_delay = 5200;
static int g_apf1_delay = 1000;
static int g_apf2_delay = 900;
static float g_wall_coeff = 0.75f;

/*
 * Preset table: delay times in samples at 44.1kHz
 *
 * | Preset     | Comb1 | Comb2 | Comb3 | Comb4 | APF1 | APF2 | Wall |
 * |------------|-------|-------|-------|-------|------|------|------|
 * | Room       | 1500  | 1600  | 1700  | 1800  | 500  | 400  | 0.6  |
 * | Studio S   | 1200  | 1300  | 1400  | 1500  | 400  | 300  | 0.5  |
 * | Studio M   | 2000  | 2200  | 2400  | 2600  | 600  | 500  | 0.65 |
 * | Studio L   | 3000  | 3300  | 3600  | 3900  | 800  | 700  | 0.7  |
 * | Hall       | 4000  | 4400  | 4800  | 5200  | 1000 | 900  | 0.75 |
 * | Space Echo | 5500  | 6000  | 6500  | 7000  | 1200 | 1100 | 0.8  |
 */
typedef struct {
    int comb1, comb2, comb3, comb4;
    int apf1, apf2;
    float wall;
} preset_t;

static const preset_t g_presets[6] = {
    { 1500, 1600, 1700, 1800,  500,  400, 0.60f },  /* Room */
    { 1200, 1300, 1400, 1500,  400,  300, 0.50f },  /* Studio S */
    { 2000, 2200, 2400, 2600,  600,  500, 0.65f },  /* Studio M */
    { 3000, 3300, 3600, 3900,  800,  700, 0.70f },  /* Studio L */
    { 4000, 4400, 4800, 5200, 1000,  900, 0.75f },  /* Hall */
    { 5500, 6000, 6500, 7000, 1200, 1100, 0.80f },  /* Space Echo */
};

/* Logging helper */
static void fx_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[psxverb] %s", msg);
        g_host->log(buf);
    }
}

/* Apply preset */
static void apply_preset(int preset) {
    if (preset < 0) preset = 0;
    if (preset > 5) preset = 5;

    g_preset = preset;
    const preset_t *p = &g_presets[preset];

    g_comb1_delay = p->comb1;
    g_comb2_delay = p->comb2;
    g_comb3_delay = p->comb3;
    g_comb4_delay = p->comb4;
    g_apf1_delay = p->apf1;
    g_apf2_delay = p->apf2;
    g_wall_coeff = p->wall;

    char msg[128];
    snprintf(msg, sizeof(msg), "Preset %d: comb=%d/%d/%d/%d apf=%d/%d wall=%.2f",
             preset, g_comb1_delay, g_comb2_delay, g_comb3_delay, g_comb4_delay,
             g_apf1_delay, g_apf2_delay, g_wall_coeff);
    fx_log(msg);
}

/* Read from work buffer with wrap */
static inline float work_read(float *work, int pos, int delay) {
    return work[(pos - delay) & WORK_MASK];
}

/* === Audio FX API Implementation === */

static int fx_on_load(const char *module_dir, const char *config_json) {
    char msg[256];
    snprintf(msg, sizeof(msg), "PSX Verb loading from: %s", module_dir);
    fx_log(msg);

    /* Clear work buffers */
    memset(g_work_l, 0, sizeof(g_work_l));
    memset(g_work_r, 0, sizeof(g_work_r));
    g_work_pos = 0;

    /* Clear allpass buffers */
    memset(g_apf1_l, 0, sizeof(g_apf1_l));
    memset(g_apf1_r, 0, sizeof(g_apf1_r));
    memset(g_apf2_l, 0, sizeof(g_apf2_l));
    memset(g_apf2_r, 0, sizeof(g_apf2_r));

    /* Reset IIR state */
    g_iir_l = 0.0f;
    g_iir_r = 0.0f;

    /* Apply default preset */
    apply_preset(g_preset);

    fx_log("PSX Verb initialized");
    return 0;
}

static void fx_on_unload(void) {
    fx_log("PSX Verb unloading");
}

static void fx_process_block(int16_t *audio_inout, int frames) {
    /* Calculate effective decay (scales wall coefficient) */
    /* decay parameter maps 0.0-1.0 to 0.5-0.95 effective feedback */
    float effective_wall = g_wall_coeff * (0.5f + g_decay * 0.45f);

    for (int i = 0; i < frames; i++) {
        /* Convert input to float (-1.0 to 1.0) */
        float in_l = audio_inout[i * 2] / 32768.0f;
        float in_r = audio_inout[i * 2 + 1] / 32768.0f;

        /* === IIR Lowpass (input filtering for warmth) === */
        /* iir_out = iir_state + 0.5 * (input - iir_state) */
        g_iir_l = g_iir_l + 0.5f * (in_l - g_iir_l);
        g_iir_r = g_iir_r + 0.5f * (in_r - g_iir_r);

        float filtered_l = g_iir_l;
        float filtered_r = g_iir_r;

        /* === Comb Bank (4 parallel combs, summed) === */
        float comb_l = work_read(g_work_l, g_work_pos, g_comb1_delay) +
                       work_read(g_work_l, g_work_pos, g_comb2_delay) +
                       work_read(g_work_l, g_work_pos, g_comb3_delay) +
                       work_read(g_work_l, g_work_pos, g_comb4_delay);

        float comb_r = work_read(g_work_r, g_work_pos, g_comb1_delay) +
                       work_read(g_work_r, g_work_pos, g_comb2_delay) +
                       work_read(g_work_r, g_work_pos, g_comb3_delay) +
                       work_read(g_work_r, g_work_pos, g_comb4_delay);

        /* Scale comb sum (4 combs) */
        comb_l *= 0.25f;
        comb_r *= 0.25f;

        /* === Allpass Diffusers (2 in series) === */
        /* APF: out = -in + delayed + delayed*coeff fed back */

        /* APF1 */
        int apf1_idx = g_work_pos & 2047;  /* 2048-sample APF buffer mask */
        float apf1_delayed_l = g_apf1_l[(apf1_idx - g_apf1_delay) & 2047];
        float apf1_delayed_r = g_apf1_r[(apf1_idx - g_apf1_delay) & 2047];

        float apf1_out_l = -comb_l + apf1_delayed_l;
        float apf1_out_r = -comb_r + apf1_delayed_r;

        g_apf1_l[apf1_idx] = comb_l + apf1_delayed_l * APF_COEFF;
        g_apf1_r[apf1_idx] = comb_r + apf1_delayed_r * APF_COEFF;

        /* APF2 */
        float apf2_delayed_l = g_apf2_l[(apf1_idx - g_apf2_delay) & 2047];
        float apf2_delayed_r = g_apf2_r[(apf1_idx - g_apf2_delay) & 2047];

        float apf2_out_l = -apf1_out_l + apf2_delayed_l;
        float apf2_out_r = -apf1_out_r + apf2_delayed_r;

        g_apf2_l[apf1_idx] = apf1_out_l + apf2_delayed_l * APF_COEFF;
        g_apf2_r[apf1_idx] = apf1_out_r + apf2_delayed_r * APF_COEFF;

        /* Wet signal is APF2 output */
        float wet_l = apf2_out_l;
        float wet_r = apf2_out_r;

        /* === Wall Reflection (feedback to work buffer) === */
        /* Feedback = output * wall_coeff * decay, written back to work buffer */
        g_work_l[g_work_pos & WORK_MASK] = filtered_l + wet_l * effective_wall;
        g_work_r[g_work_pos & WORK_MASK] = filtered_r + wet_r * effective_wall;

        /* Advance work buffer position */
        g_work_pos++;

        /* === Mix dry and wet === */
        float out_l = in_l * (1.0f - g_mix) + wet_l * g_mix;
        float out_r = in_r * (1.0f - g_mix) + wet_r * g_mix;

        /* Clamp output and convert back to int16 */
        if (out_l > 1.0f) out_l = 1.0f;
        if (out_l < -1.0f) out_l = -1.0f;
        if (out_r > 1.0f) out_r = 1.0f;
        if (out_r < -1.0f) out_r = -1.0f;

        audio_inout[i * 2] = (int16_t)(out_l * 32767.0f);
        audio_inout[i * 2 + 1] = (int16_t)(out_r * 32767.0f);
    }
}

static void fx_set_param(const char *key, const char *val) {
    if (strcmp(key, "preset") == 0) {
        int v = atoi(val);
        apply_preset(v);
    } else if (strcmp(key, "decay") == 0) {
        float v = atof(val);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        g_decay = v;
    } else if (strcmp(key, "mix") == 0) {
        float v = atof(val);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        g_mix = v;
    }
}

static int fx_get_param(const char *key, char *buf, int buf_len) {
    if (strcmp(key, "preset") == 0) {
        return snprintf(buf, buf_len, "%d", g_preset);
    } else if (strcmp(key, "decay") == 0) {
        return snprintf(buf, buf_len, "%.2f", g_decay);
    } else if (strcmp(key, "mix") == 0) {
        return snprintf(buf, buf_len, "%.2f", g_mix);
    } else if (strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "PSX Verb");
    }
    return -1;
}

/* === Entry Point === */

audio_fx_api_v1_t* move_audio_fx_init_v1(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_fx_api, 0, sizeof(g_fx_api));
    g_fx_api.api_version = AUDIO_FX_API_VERSION;
    g_fx_api.on_load = fx_on_load;
    g_fx_api.on_unload = fx_on_unload;
    g_fx_api.process_block = fx_process_block;
    g_fx_api.set_param = fx_set_param;
    g_fx_api.get_param = fx_get_param;

    fx_log("PSX Verb plugin initialized");

    return &g_fx_api;
}

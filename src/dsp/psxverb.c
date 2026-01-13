/*
 * PSX Verb Audio FX Plugin - Authentic PlayStation 1 SPU Reverb
 *
 * EXACT port from CVCHothouse/PSXVerb reference implementation.
 * All preset values use authentic SPU register hex values.
 *
 * PSX SPU Reverb operates at 22.05kHz internally (half of 44.1kHz).
 * This implementation uses:
 * - Halfband 39-tap FIR for 2:1 decimation/interpolation
 * - WorkArea circular int16 buffer emulating SPU RAM with saturating writes
 * - Authentic PSX SPU register values for 6 presets (exact hex from psx-spx)
 * - Full PSX algorithm: Same/Diff reflections -> Comb -> APF1 -> APF2
 *
 * Parameters:
 * - preset: 0-5 (Room, Studio S/M/L, Hall, Space Echo)
 * - decay: Wall reflection feedback scaling (0.0-1.0)
 * - mix: Dry/wet blend (0.0-1.0)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "audio_fx_api_v1.h"

#define SAMPLE_RATE 44100
#define PSX_INTERNAL_RATE 22050  /* PSX SPU runs at half sample rate */

/* ============================================================================
 * HALFBAND 39-TAP FIR FILTER
 * Exact port from Halfband39.h
 * ============================================================================ */

#define HB_TAPS 39
#define HB_STATE_SIZE 64  /* Power of 2 >= 39 */
#define HB_STATE_MASK 63  /* 0x3F for fast wrap */

/* Halfband FIR coefficients - EXACT from reference Halfband39.h */
static const float g_hb_coeffs[HB_TAPS] = {
    -0.000275135f,  /* 0 */
     0.0f,          /* 1 */
    -0.001467466f,  /* 2 */
     0.0f,          /* 3 */
    -0.004356503f,  /* 4 */
     0.0f,          /* 5 */
    -0.009765625f,  /* 6 */
     0.0f,          /* 7 */
    -0.018493652f,  /* 8 */
     0.0f,          /* 9 */
    -0.031494141f,  /* 10 */
     0.0f,          /* 11 */
    -0.050598145f,  /* 12 */
     0.0f,          /* 13 */
    -0.079833984f,  /* 14 */
     0.0f,          /* 15 */
    -0.130859375f,  /* 16 */
     0.0f,          /* 17 */
    -0.281494141f,  /* 18 */
     0.632812500f,  /* 19 - CENTER TAP */
    -0.281494141f,  /* 20 */
     0.0f,          /* 21 */
    -0.130859375f,  /* 22 */
     0.0f,          /* 23 */
    -0.079833984f,  /* 24 */
     0.0f,          /* 25 */
    -0.050598145f,  /* 26 */
     0.0f,          /* 27 */
    -0.031494141f,  /* 28 */
     0.0f,          /* 29 */
    -0.018493652f,  /* 30 */
     0.0f,          /* 31 */
    -0.009765625f,  /* 32 */
     0.0f,          /* 33 */
    -0.004356503f,  /* 34 */
     0.0f,          /* 35 */
    -0.001467466f,  /* 36 */
     0.0f,          /* 37 */
    -0.000275135f,  /* 38 */
};

/* Polyphase Phase 0: coeffs at even indices [0, 2, 4, ..., 38] */
static const float g_hb_phase0[20] = {
    -0.000275135f, -0.001467466f, -0.004356503f, -0.009765625f,
    -0.018493652f, -0.031494141f, -0.050598145f, -0.079833984f,
    -0.130859375f, -0.281494141f,  0.632812500f, -0.281494141f,
    -0.130859375f, -0.079833984f, -0.050598145f, -0.031494141f,
    -0.018493652f, -0.009765625f, -0.004356503f, -0.001467466f
};

/* Polyphase Phase 1: coeffs at odd indices (all zeros for halfband) */
static const float g_hb_phase1[19] = {
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f
};

typedef struct {
    float state[HB_STATE_SIZE];
    int pos;
} halfband_t;

static void halfband_init(halfband_t *hb) {
    memset(hb->state, 0, sizeof(hb->state));
    hb->pos = 0;
}

/* Decimate: 44.1kHz -> 22.05kHz (2 samples in, 1 out)
 * Exact port from Halfband39::Decimate */
static float halfband_decimate(halfband_t *hb, float x0, float x1) {
    /* Push two new samples */
    hb->state[hb->pos] = x0;
    hb->pos = (hb->pos + 1) & HB_STATE_MASK;  /* wrap at 64 */
    hb->state[hb->pos] = x1;
    hb->pos = (hb->pos + 1) & HB_STATE_MASK;

    /* Convolve with ALL 39 taps (CRITICAL: must process all taps to prevent aliasing) */
    float sum = 0.0f;
    int idx = hb->pos;
    for (int i = 0; i < HB_TAPS; ++i) {
        idx = (idx - 1) & HB_STATE_MASK;
        sum += g_hb_coeffs[i] * hb->state[idx];
    }
    return sum;
}

/* Interpolate: 22.05kHz -> 44.1kHz (1 sample in, 2 out)
 * Exact port from Halfband39::Interpolate */
static void halfband_interpolate(halfband_t *hb, float in_tick, float *out_s0, float *out_s1) {
    /* Push upsampled input (zero-stuff: [in, 0]) */
    hb->state[hb->pos] = in_tick;
    hb->pos = (hb->pos + 1) & HB_STATE_MASK;

    /* Phase 0: even coefficients (produces sample 0) */
    float sum0 = 0.0f;
    int idx = hb->pos;
    for (int i = 0; i < 20; ++i) {  /* 20 even taps */
        idx = (idx - 1) & HB_STATE_MASK;
        sum0 += g_hb_phase0[i] * hb->state[idx];
    }

    /* Phase 1: odd coefficients (produces sample 1) */
    hb->state[hb->pos] = 0.0f;  /* zero-stuffed sample */
    hb->pos = (hb->pos + 1) & HB_STATE_MASK;

    float sum1 = 0.0f;
    idx = hb->pos;
    for (int i = 0; i < 19; ++i) {  /* 19 odd taps */
        idx = (idx - 1) & HB_STATE_MASK;
        sum1 += g_hb_phase1[i] * hb->state[idx];
    }

    *out_s0 = sum0 * 2.0f;  /* Compensate for zero-stuffing */
    *out_s1 = sum1 * 2.0f;
}

/* ============================================================================
 * WORK AREA - SPU RAM EMULATION
 * Exact port from WorkArea.h
 * ============================================================================ */

#define WORK_MAX_SIZE 65536  /* Maximum work area size */

/* Conversion constants - exact from WorkArea.h */
static const float kInt16ToFloat = 1.0f / 32768.0f;
static const float kFloatToInt16 = 32768.0f;

typedef struct {
    int16_t *buf;
    uint32_t size_mask;  /* size - 1, for fast wrap with & */
    uint32_t base;       /* Current base position (advances each tick) */
} workarea_t;

static int16_t g_work_buffer[WORK_MAX_SIZE];

static void workarea_init(workarea_t *wa, uint32_t size_pow2) {
    wa->buf = g_work_buffer;
    wa->size_mask = size_pow2 - 1;
    wa->base = 0;
    memset(wa->buf, 0, size_pow2 * sizeof(int16_t));
}

/* Read at relative offset from current base (for IIR: base - 1)
 * Exact port from WorkArea::ReadRelative */
static inline float workarea_read_relative(const workarea_t *wa, int32_t offset) {
    uint32_t idx = (wa->base + offset) & wa->size_mask;
    return (float)wa->buf[idx] * kInt16ToFloat;
}

/* Write at relative offset from current base with 16-bit saturation
 * Exact port from WorkArea::WriteRelative */
static inline void workarea_write_relative(workarea_t *wa, int32_t offset, float value) {
    int32_t val_int = (int32_t)(value * kFloatToInt16);
    /* Saturate to int16 range [-32768, 32767] */
    if (val_int > 32767) val_int = 32767;
    if (val_int < -32768) val_int = -32768;
    uint32_t idx = (wa->base + offset) & wa->size_mask;
    wa->buf[idx] = (int16_t)val_int;
}

/* Advance base pointer (called once per tick)
 * Exact port from WorkArea::Advance */
static inline void workarea_advance(workarea_t *wa, uint32_t n) {
    wa->base = (wa->base + n) & wa->size_mask;
}

/* ============================================================================
 * PSX PRESETS - EXACT SPU Register Values from PsxPreset.h
 * All coefficients stored as int16_t hex values, converted at runtime
 * ============================================================================ */

typedef struct {
    /* All-pass filter parameters */
    uint16_t dAPF1, dAPF2;           /* APF displacement offsets */
    int16_t vIIR;                    /* Reflection volume 1 (IIR feedback) */
    int16_t vCOMB1, vCOMB2, vCOMB3, vCOMB4;  /* Comb filter volumes */
    int16_t vWALL;                   /* Reflection volume 2 (wall reflection) */
    int16_t vAPF1, vAPF2;            /* APF volumes */

    /* Same-side reflection addresses and offsets */
    uint16_t mLSAME, mRSAME;         /* Memory addresses for same-side reflections */
    uint16_t dLSAME, dRSAME;         /* Displacement offsets for same-side */

    /* Different-side reflection addresses and offsets (cross-channel) */
    uint16_t mLDIFF, mRDIFF;         /* Memory addresses for different-side */
    uint16_t dLDIFF, dRDIFF;         /* Displacement offsets for different-side */

    /* Comb filter addresses */
    uint16_t mLCOMB1, mRCOMB1;
    uint16_t mLCOMB2, mRCOMB2;
    uint16_t mLCOMB3, mRCOMB3;
    uint16_t mLCOMB4, mRCOMB4;

    /* All-pass filter addresses */
    uint16_t mLAPF1, mRAPF1;
    uint16_t mLAPF2, mRAPF2;

    /* Input volumes */
    int16_t vLIN, vRIN;

    /* Output volumes */
    int16_t vLOUT, vROUT;

    /* Work area size (in bytes) */
    uint32_t work_size;

    const char *name;
} psx_preset_t;

/* PSX SPU Reverb Presets - EXACT hex values from PsxPreset.h */
static const psx_preset_t g_presets[6] = {
    /* Room - exact from PsxPresets::kRoom */
    {
        .dAPF1 = 0x007D, .dAPF2 = 0x005B,
        .vIIR = 0x6D80, .vCOMB1 = 0x54B8, .vCOMB2 = (int16_t)0xBED0,
        .vCOMB3 = 0x0000, .vCOMB4 = 0x0000,
        .vWALL = (int16_t)0xBA80,
        .vAPF1 = 0x5800, .vAPF2 = 0x5300,
        .mLSAME = 0x04D6, .mRSAME = 0x0333,
        .dLSAME = 0x0334, .dRSAME = 0x01B5,
        .mLDIFF = 0x0000, .mRDIFF = 0x0000,
        .dLDIFF = 0x0000, .dRDIFF = 0x0000,
        .mLCOMB1 = 0x03F0, .mRCOMB1 = 0x0227,
        .mLCOMB2 = 0x0374, .mRCOMB2 = 0x01EF,
        .mLCOMB3 = 0x0000, .mRCOMB3 = 0x0000,
        .mLCOMB4 = 0x0000, .mRCOMB4 = 0x0000,
        .mLAPF1 = 0x01B4, .mRAPF1 = 0x0136,
        .mLAPF2 = 0x00B8, .mRAPF2 = 0x005C,
        .vLIN = (int16_t)0x8000, .vRIN = (int16_t)0x8000,
        .vLOUT = (int16_t)0x8000, .vROUT = (int16_t)0x8000,
        .work_size = 0x26C0,
        .name = "Room"
    },
    /* Studio Small - exact from PsxPresets::kStudioSmall */
    {
        .dAPF1 = 0x0033, .dAPF2 = 0x0025,
        .vIIR = 0x70F0, .vCOMB1 = 0x4FA8, .vCOMB2 = (int16_t)0xBCE0,
        .vCOMB3 = 0x4410, .vCOMB4 = (int16_t)0xC0F0,
        .vWALL = (int16_t)0x9C00,
        .vAPF1 = 0x5280, .vAPF2 = 0x4EC0,
        .mLSAME = 0x03E4, .mRSAME = 0x031B,
        .dLSAME = 0x031C, .dRSAME = 0x025D,
        .mLDIFF = 0x025C, .mRDIFF = 0x018E,
        .dLDIFF = 0x018F, .dRDIFF = 0x00B5,
        .mLCOMB1 = 0x03A4, .mRCOMB1 = 0x02AF,
        .mLCOMB2 = 0x0372, .mRCOMB2 = 0x0266,
        .mLCOMB3 = 0x022F, .mRCOMB3 = 0x0135,
        .mLCOMB4 = 0x01D2, .mRCOMB4 = 0x00B7,
        .mLAPF1 = 0x00B4, .mRAPF1 = 0x0080,
        .mLAPF2 = 0x004C, .mRAPF2 = 0x0026,
        .vLIN = (int16_t)0x8000, .vRIN = (int16_t)0x8000,
        .vLOUT = (int16_t)0x8000, .vROUT = (int16_t)0x8000,
        .work_size = 0x1F40,
        .name = "Studio S"
    },
    /* Studio Medium - exact from PsxPresets::kStudioMedium */
    {
        .dAPF1 = 0x00B1, .dAPF2 = 0x007F,
        .vIIR = 0x70F0, .vCOMB1 = 0x4FA8, .vCOMB2 = (int16_t)0xBCE0,
        .vCOMB3 = 0x4510, .vCOMB4 = (int16_t)0xBEF0,
        .vWALL = (int16_t)0xB4C0,
        .vAPF1 = 0x5280, .vAPF2 = 0x4EC0,
        .mLSAME = 0x0904, .mRSAME = 0x076B,
        .dLSAME = 0x076C, .dRSAME = 0x05ED,
        .mLDIFF = 0x05EC, .mRDIFF = 0x042E,
        .dLDIFF = 0x042F, .dRDIFF = 0x0265,
        .mLCOMB1 = 0x0824, .mRCOMB1 = 0x065F,
        .mLCOMB2 = 0x07A2, .mRCOMB2 = 0x0616,
        .mLCOMB3 = 0x050F, .mRCOMB3 = 0x0305,
        .mLCOMB4 = 0x0462, .mRCOMB4 = 0x02B7,
        .mLAPF1 = 0x0264, .mRAPF1 = 0x01B2,
        .mLAPF2 = 0x0100, .mRAPF2 = 0x0080,
        .vLIN = (int16_t)0x8000, .vRIN = (int16_t)0x8000,
        .vLOUT = (int16_t)0x8000, .vROUT = (int16_t)0x8000,
        .work_size = 0x4840,
        .name = "Studio M"
    },
    /* Studio Large - exact from PsxPresets::kStudioLarge */
    {
        .dAPF1 = 0x00E3, .dAPF2 = 0x00A9,
        .vIIR = 0x6F60, .vCOMB1 = 0x4FA8, .vCOMB2 = (int16_t)0xBCE0,
        .vCOMB3 = 0x4510, .vCOMB4 = (int16_t)0xBEF0,
        .vWALL = (int16_t)0xA680,
        .vAPF1 = 0x5680, .vAPF2 = 0x52C0,
        .mLSAME = 0x0DFB, .mRSAME = 0x0B58,
        .dLSAME = 0x0B59, .dRSAME = 0x08DA,
        .mLDIFF = 0x08D9, .mRDIFF = 0x05E9,
        .dLDIFF = 0x05EA, .dRDIFF = 0x031D,
        .mLCOMB1 = 0x0D09, .mRCOMB1 = 0x0A3C,
        .mLCOMB2 = 0x0BD9, .mRCOMB2 = 0x0973,
        .mLCOMB3 = 0x07EC, .mRCOMB3 = 0x04B0,
        .mLCOMB4 = 0x06EF, .mRCOMB4 = 0x03D2,
        .mLAPF1 = 0x031C, .mRAPF1 = 0x0238,
        .mLAPF2 = 0x0154, .mRAPF2 = 0x00AA,
        .vLIN = (int16_t)0x8000, .vRIN = (int16_t)0x8000,
        .vLOUT = (int16_t)0x8000, .vROUT = (int16_t)0x8000,
        .work_size = 0x6FE0,
        .name = "Studio L"
    },
    /* Hall - exact from PsxPresets::kHall */
    {
        .dAPF1 = 0x01A5, .dAPF2 = 0x0139,
        .vIIR = 0x6000, .vCOMB1 = 0x5000, .vCOMB2 = 0x4C00,
        .vCOMB3 = (int16_t)0xB800, .vCOMB4 = (int16_t)0xBC00,
        .vWALL = (int16_t)0xC000,
        .vAPF1 = 0x6000, .vAPF2 = 0x5C00,
        .mLSAME = 0x15BA, .mRSAME = 0x11BB,
        .dLSAME = 0x11C0, .dRSAME = 0x0DC3,
        .mLDIFF = 0x0DC0, .mRDIFF = 0x09C1,
        .dLDIFF = 0x09C2, .dRDIFF = 0x05C1,
        .mLCOMB1 = 0x14C2, .mRCOMB1 = 0x10BD,
        .mLCOMB2 = 0x11BC, .mRCOMB2 = 0x0DC1,
        .mLCOMB3 = 0x0BC4, .mRCOMB3 = 0x07C1,
        .mLCOMB4 = 0x0A00, .mRCOMB4 = 0x06CD,
        .mLAPF1 = 0x05C0, .mRAPF1 = 0x041A,
        .mLAPF2 = 0x0274, .mRAPF2 = 0x013A,
        .vLIN = (int16_t)0x8000, .vRIN = (int16_t)0x8000,
        .vLOUT = (int16_t)0x8000, .vROUT = (int16_t)0x8000,
        .work_size = 0xADE0,
        .name = "Hall"
    },
    /* Space Echo - exact from PsxPresets::kSpaceEcho */
    {
        .dAPF1 = 0x033D, .dAPF2 = 0x0231,
        .vIIR = 0x7E00, .vCOMB1 = 0x5000, .vCOMB2 = (int16_t)0xB400,
        .vCOMB3 = (int16_t)0xB000, .vCOMB4 = 0x4C00,
        .vWALL = (int16_t)0xB000,
        .vAPF1 = 0x6000, .vAPF2 = 0x5400,
        .mLSAME = 0x1ED6, .mRSAME = 0x1A31,
        .dLSAME = 0x1A32, .dRSAME = 0x15EF,
        .mLDIFF = 0x15EE, .mRDIFF = 0x1055,
        .dLDIFF = 0x1056, .dRDIFF = 0x0AE1,
        .mLCOMB1 = 0x1D14, .mRCOMB1 = 0x183B,
        .mLCOMB2 = 0x1BC2, .mRCOMB2 = 0x16B2,
        .mLCOMB3 = 0x1334, .mRCOMB3 = 0x0F2D,
        .mLCOMB4 = 0x11F6, .mRCOMB4 = 0x0C5D,
        .mLAPF1 = 0x0AE0, .mRAPF1 = 0x07A2,
        .mLAPF2 = 0x0464, .mRAPF2 = 0x0232,
        .vLIN = (int16_t)0x8000, .vRIN = (int16_t)0x8000,
        .vLOUT = (int16_t)0x8000, .vROUT = (int16_t)0x8000,
        .work_size = 0xF6C0,
        .name = "Space Echo"
    }
};

/* ============================================================================
 * SCALED PRESET - Runtime float values converted from int16
 * Matches PsxReverb.h ScaledPreset struct
 * ============================================================================ */

typedef struct {
    uint16_t dAPF1, dAPF2;
    uint16_t dLSAME, dRSAME, dLDIFF, dRDIFF;
    uint16_t mLSAME, mRSAME, mLDIFF, mRDIFF;
    uint16_t mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2;
    uint16_t mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4;
    uint16_t mLAPF1, mRAPF1, mLAPF2, mRAPF2;
    float vIIR_f, vCOMB1_f, vCOMB2_f, vCOMB3_f, vCOMB4_f;
    float vWALL_f, vAPF1_f, vAPF2_f, vLIN_f, vRIN_f;
    float vLOUT_f, vROUT_f;
} scaled_preset_t;

/* ============================================================================
 * PLUGIN STATE
 * ============================================================================ */

static const host_api_v1_t *g_host = NULL;
static audio_fx_api_v1_t g_fx_api;

/* Parameters */
static int g_preset_idx = 4;    /* Default: Hall */
static float g_decay = 0.8f;    /* Wall reflection feedback scaling (0-1) */
static float g_mix = 0.35f;     /* Dry/wet mix */
static float g_input_gain = 0.5f;   /* Input gain: 0=0x, 0.5=1x, 1.0=2x */
static float g_reverb_level = 0.5f; /* Reverb level: 0=0x, 0.5=2x, 1.0=4x */

/* Scaled presets (runtime float values) */
static scaled_preset_t g_current;
static scaled_preset_t g_base;  /* Unscaled base for decay modulation */

/* DSP state */
static workarea_t g_work;
static halfband_t g_down_l, g_down_r;
static halfband_t g_up_l, g_up_r;

/* Logging helper */
static void fx_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[psxverb] %s", msg);
        g_host->log(buf);
    }
}

/* Next power of 2 - exact from PsxReverb.h */
static uint32_t next_pow2(uint32_t v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

/* Convert SPU signed 16-bit coefficient to float [-1.0, 1.0)
 * Exact from PsxPreset.h CoeffToFloat */
static inline float coeff_to_float(int16_t coeff) {
    return (float)coeff / 32768.0f;
}

/* Clamp float value to [min, max] - matches std::clamp */
static inline float clamp_f(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Absolute value for float */
static inline float abs_f(float v) {
    return v < 0.0f ? -v : v;
}

/* Max of two floats */
static inline float max_f(float a, float b) {
    return a > b ? a : b;
}

/* Scale preset from source to runtime values
 * Matches PsxReverb::ScalePreset */
static void scale_preset(const psx_preset_t *src, scaled_preset_t *dst) {
    /* Copy delay offsets (no scaling needed at 44.1kHz) */
    dst->dAPF1 = src->dAPF1;
    dst->dAPF2 = src->dAPF2;
    dst->dLSAME = src->dLSAME;
    dst->dRSAME = src->dRSAME;
    dst->dLDIFF = src->dLDIFF;
    dst->dRDIFF = src->dRDIFF;

    /* Copy memory addresses */
    dst->mLSAME = src->mLSAME;
    dst->mRSAME = src->mRSAME;
    dst->mLDIFF = src->mLDIFF;
    dst->mRDIFF = src->mRDIFF;
    dst->mLCOMB1 = src->mLCOMB1;
    dst->mRCOMB1 = src->mRCOMB1;
    dst->mLCOMB2 = src->mLCOMB2;
    dst->mRCOMB2 = src->mRCOMB2;
    dst->mLCOMB3 = src->mLCOMB3;
    dst->mRCOMB3 = src->mRCOMB3;
    dst->mLCOMB4 = src->mLCOMB4;
    dst->mRCOMB4 = src->mRCOMB4;
    dst->mLAPF1 = src->mLAPF1;
    dst->mRAPF1 = src->mRAPF1;
    dst->mLAPF2 = src->mLAPF2;
    dst->mRAPF2 = src->mRAPF2;

    /* Convert coefficients to float using CoeffToFloat */
    dst->vIIR_f = coeff_to_float(src->vIIR);
    dst->vCOMB1_f = coeff_to_float(src->vCOMB1);
    dst->vCOMB2_f = coeff_to_float(src->vCOMB2);
    dst->vCOMB3_f = coeff_to_float(src->vCOMB3);
    dst->vCOMB4_f = coeff_to_float(src->vCOMB4);
    dst->vWALL_f = coeff_to_float(src->vWALL);
    dst->vAPF1_f = coeff_to_float(src->vAPF1);
    dst->vAPF2_f = coeff_to_float(src->vAPF2);
    dst->vLIN_f = coeff_to_float(src->vLIN);
    dst->vRIN_f = coeff_to_float(src->vRIN);
    dst->vLOUT_f = coeff_to_float(src->vLOUT);
    dst->vROUT_f = coeff_to_float(src->vROUT);
}

/* Runtime decay control: expects normalized 0..1; maps to 0.5x..maxScale with midpoint ~1x
 * Exact port from PsxReverb::SetDecayTime in shared_dsp/PsxReverb.h */
static void update_decay(float norm_decay) {
    const float base = max_f(1e-5f, abs_f(g_base.vWALL_f));
    const float max_scale = clamp_f(0.99f / base, 0.5f, 10.0f);
    const float min_scale = 0.5f;
    const float mid_scale = 1.0f;
    const float mid_norm = 0.5f;
    float wall_scale = 1.0f;

    if (norm_decay <= mid_norm) {
        const float t = norm_decay / mid_norm;  /* 0..1 */
        wall_scale = min_scale + t * (mid_scale - min_scale);
    } else {
        const float t = (norm_decay - mid_norm) / (1.0f - mid_norm);  /* 0..1 */
        wall_scale = mid_scale + t * (max_scale - mid_scale);
    }

    float target = g_base.vWALL_f * wall_scale;
    /* Clamp to stable range to avoid runaway feedback */
    g_current.vWALL_f = clamp_f(target, -0.995f, 0.995f);
}

/* Runtime input gain control (0.0 to 1.0 range)
 * Exact port from PsxReverb::SetInputGain */
static void update_input_gain(float gain) {
    /* Scale vLIN/vRIN (input volume)
     * Range: 0x (gain=0) -> 2x (gain=1.0)
     * 50% = 1x = unity/authentic */
    float gain_scale = gain * 2.0f;  /* 0x to 2x */
    g_current.vLIN_f = g_base.vLIN_f * gain_scale;
    g_current.vRIN_f = g_base.vRIN_f * gain_scale;
}

/* Runtime reverb output level control (0.0 to 1.0 range)
 * Exact port from PsxReverb::SetReverbLevel */
static void update_reverb_level(float level) {
    /* Scale vLOUT/vROUT (output volume)
     * Range: 0x (level=0) -> 4x (level=1.0)
     * 50% = 2x = unity gain for most presets */
    float level_scale = level * 4.0f;  /* 0x to 4x */
    g_current.vLOUT_f = g_base.vLOUT_f * level_scale;
    g_current.vROUT_f = g_base.vROUT_f * level_scale;
}

/* Apply preset */
static void apply_preset(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 5) idx = 5;
    g_preset_idx = idx;

    const psx_preset_t *src = &g_presets[idx];

    /* Scale preset to runtime values and store base */
    scale_preset(src, &g_base);
    g_current = g_base;

    /* Initialize work area with appropriate size */
    uint32_t work_size = next_pow2(src->work_size / sizeof(int16_t));
    if (work_size > WORK_MAX_SIZE) work_size = WORK_MAX_SIZE;
    workarea_init(&g_work, work_size);

    /* Apply current parameter settings */
    update_decay(g_decay);
    update_input_gain(g_input_gain);
    update_reverb_level(g_reverb_level);

    char msg[128];
    snprintf(msg, sizeof(msg), "Preset %d: %s (work=%u)", idx, src->name, work_size);
    fx_log(msg);
}

/* ============================================================================
 * AUDIO FX API IMPLEMENTATION
 * ============================================================================ */

static int fx_on_load(const char *module_dir, const char *config_json) {
    char msg[256];
    snprintf(msg, sizeof(msg), "PSX Verb (exact port) loading from: %s", module_dir);
    fx_log(msg);

    /* Initialize halfband filters */
    halfband_init(&g_down_l);
    halfband_init(&g_down_r);
    halfband_init(&g_up_l);
    halfband_init(&g_up_r);

    /* Apply default preset */
    apply_preset(g_preset_idx);

    fx_log("PSX Verb initialized (exact port from CVCHothouse/PSXVerb)");
    return 0;
}

static void fx_on_unload(void) {
    fx_log("PSX Verb unloading");
}

/* Process block - EXACT port from PsxReverb::ProcessBlock */
static void fx_process_block(int16_t *audio_inout, int frames) {
    const scaled_preset_t *p = &g_current;

    /* Process pairs of samples (2:1 decimation to internal rate) */
    for (int i = 0; i < frames; i += 2) {
        if (i + 1 >= frames) break;

        /* Get input samples and convert to float */
        float in_l0 = audio_inout[i * 2] / 32768.0f;
        float in_r0 = audio_inout[i * 2 + 1] / 32768.0f;
        float in_l1 = audio_inout[(i + 1) * 2] / 32768.0f;
        float in_r1 = audio_inout[(i + 1) * 2 + 1] / 32768.0f;

        /* Decimate both channels 44.1kHz -> 22.05kHz */
        float Lin = halfband_decimate(&g_down_l, in_l0, in_l1) * p->vLIN_f;
        float Rin = halfband_decimate(&g_down_r, in_r0, in_r1) * p->vRIN_f;

        /* === PSX SPU Reverb Algorithm (exact from PsxReverb::ProcessBlock) === */

        /* Same-side reflection (L->L, R->R) */
        float lsame_fb = workarea_read_relative(&g_work, p->dLSAME);
        float lsame_iir = workarea_read_relative(&g_work, p->mLSAME - 1);
        float lsame_out = (Lin + lsame_fb * p->vWALL_f - lsame_iir) * p->vIIR_f + lsame_iir;
        workarea_write_relative(&g_work, p->mLSAME, lsame_out);

        float rsame_fb = workarea_read_relative(&g_work, p->dRSAME);
        float rsame_iir = workarea_read_relative(&g_work, p->mRSAME - 1);
        float rsame_out = (Rin + rsame_fb * p->vWALL_f - rsame_iir) * p->vIIR_f + rsame_iir;
        workarea_write_relative(&g_work, p->mRSAME, rsame_out);

        /* Different-side reflection (cross-channel: L->R, R->L) */
        float ldiff_fb = workarea_read_relative(&g_work, p->dRDIFF);
        float ldiff_iir = workarea_read_relative(&g_work, p->mLDIFF - 1);
        float ldiff_out = (Lin + ldiff_fb * p->vWALL_f - ldiff_iir) * p->vIIR_f + ldiff_iir;
        workarea_write_relative(&g_work, p->mLDIFF, ldiff_out);

        float rdiff_fb = workarea_read_relative(&g_work, p->dLDIFF);
        float rdiff_iir = workarea_read_relative(&g_work, p->mRDIFF - 1);
        float rdiff_out = (Rin + rdiff_fb * p->vWALL_f - rdiff_iir) * p->vIIR_f + rdiff_iir;
        workarea_write_relative(&g_work, p->mRDIFF, rdiff_out);

        /* Early echo (comb filter bank) */
        float Lout = p->vCOMB1_f * workarea_read_relative(&g_work, p->mLCOMB1) +
                     p->vCOMB2_f * workarea_read_relative(&g_work, p->mLCOMB2) +
                     p->vCOMB3_f * workarea_read_relative(&g_work, p->mLCOMB3) +
                     p->vCOMB4_f * workarea_read_relative(&g_work, p->mLCOMB4);

        float Rout = p->vCOMB1_f * workarea_read_relative(&g_work, p->mRCOMB1) +
                     p->vCOMB2_f * workarea_read_relative(&g_work, p->mRCOMB2) +
                     p->vCOMB3_f * workarea_read_relative(&g_work, p->mRCOMB3) +
                     p->vCOMB4_f * workarea_read_relative(&g_work, p->mRCOMB4);

        /* Late reverb: All-pass filter 1 */
        float lapf1_del = workarea_read_relative(&g_work, p->mLAPF1 - p->dAPF1);
        Lout -= p->vAPF1_f * lapf1_del;
        workarea_write_relative(&g_work, p->mLAPF1, Lout);
        Lout = Lout * p->vAPF1_f + lapf1_del;

        float rapf1_del = workarea_read_relative(&g_work, p->mRAPF1 - p->dAPF1);
        Rout -= p->vAPF1_f * rapf1_del;
        workarea_write_relative(&g_work, p->mRAPF1, Rout);
        Rout = Rout * p->vAPF1_f + rapf1_del;

        /* Late reverb: All-pass filter 2 */
        float lapf2_del = workarea_read_relative(&g_work, p->mLAPF2 - p->dAPF2);
        Lout -= p->vAPF2_f * lapf2_del;
        workarea_write_relative(&g_work, p->mLAPF2, Lout);
        Lout = Lout * p->vAPF2_f + lapf2_del;

        float rapf2_del = workarea_read_relative(&g_work, p->mRAPF2 - p->dAPF2);
        Rout -= p->vAPF2_f * rapf2_del;
        workarea_write_relative(&g_work, p->mRAPF2, Rout);
        Rout = Rout * p->vAPF2_f + rapf2_del;

        /* Advance buffer (called once per tick) */
        workarea_advance(&g_work, 1);

        /* Upsample back to 44.1kHz and apply output volume */
        float out_l0, out_l1, out_r0, out_r1;
        halfband_interpolate(&g_up_l, Lout * p->vLOUT_f, &out_l0, &out_l1);
        halfband_interpolate(&g_up_r, Rout * p->vROUT_f, &out_r0, &out_r1);

        /* Mix dry and wet */
        float dry_mix = 1.0f - g_mix;
        float wet_mix = g_mix;

        float final_l0 = in_l0 * dry_mix + out_l0 * wet_mix;
        float final_r0 = in_r0 * dry_mix + out_r0 * wet_mix;
        float final_l1 = in_l1 * dry_mix + out_l1 * wet_mix;
        float final_r1 = in_r1 * dry_mix + out_r1 * wet_mix;

        /* Clamp and convert back to int16 */
        if (final_l0 > 1.0f) final_l0 = 1.0f;
        if (final_l0 < -1.0f) final_l0 = -1.0f;
        if (final_r0 > 1.0f) final_r0 = 1.0f;
        if (final_r0 < -1.0f) final_r0 = -1.0f;
        if (final_l1 > 1.0f) final_l1 = 1.0f;
        if (final_l1 < -1.0f) final_l1 = -1.0f;
        if (final_r1 > 1.0f) final_r1 = 1.0f;
        if (final_r1 < -1.0f) final_r1 = -1.0f;

        audio_inout[i * 2] = (int16_t)(final_l0 * 32767.0f);
        audio_inout[i * 2 + 1] = (int16_t)(final_r0 * 32767.0f);
        audio_inout[(i + 1) * 2] = (int16_t)(final_l1 * 32767.0f);
        audio_inout[(i + 1) * 2 + 1] = (int16_t)(final_r1 * 32767.0f);
    }
}

static void fx_set_param(const char *key, const char *val) {
    if (strcmp(key, "preset") == 0) {
        int v = atoi(val);
        apply_preset(v);
    } else if (strcmp(key, "decay") == 0) {
        float v = (float)atof(val);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        g_decay = v;
        update_decay(g_decay);
    } else if (strcmp(key, "mix") == 0) {
        float v = (float)atof(val);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        g_mix = v;
    } else if (strcmp(key, "input_gain") == 0) {
        float v = (float)atof(val);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        g_input_gain = v;
        update_input_gain(g_input_gain);
    } else if (strcmp(key, "level") == 0) {
        float v = (float)atof(val);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        g_reverb_level = v;
        update_reverb_level(g_reverb_level);
    }
}

static int fx_get_param(const char *key, char *buf, int buf_len) {
    if (strcmp(key, "preset") == 0) {
        return snprintf(buf, buf_len, "%d", g_preset_idx);
    } else if (strcmp(key, "decay") == 0) {
        return snprintf(buf, buf_len, "%.2f", (double)g_decay);
    } else if (strcmp(key, "mix") == 0) {
        return snprintf(buf, buf_len, "%.2f", (double)g_mix);
    } else if (strcmp(key, "input_gain") == 0) {
        return snprintf(buf, buf_len, "%.2f", (double)g_input_gain);
    } else if (strcmp(key, "level") == 0) {
        return snprintf(buf, buf_len, "%.2f", (double)g_reverb_level);
    } else if (strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "PSX Verb");
    }
    return -1;
}

/* ============================================================================
 * ENTRY POINT
 * ============================================================================ */

audio_fx_api_v1_t* move_audio_fx_init_v1(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_fx_api, 0, sizeof(g_fx_api));
    g_fx_api.api_version = AUDIO_FX_API_VERSION;
    g_fx_api.on_load = fx_on_load;
    g_fx_api.on_unload = fx_on_unload;
    g_fx_api.process_block = fx_process_block;
    g_fx_api.set_param = fx_set_param;
    g_fx_api.get_param = fx_get_param;

    fx_log("PSX Verb plugin initialized (exact port from CVCHothouse/PSXVerb)");

    return &g_fx_api;
}

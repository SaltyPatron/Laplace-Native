/* Tensor data types, deterministic float/block-quant decoders, zero-copy Safetensors/GGUF readers,
 * and HF-to-GGML canonical tensor naming. */
#include "laplace/tensor.h"
#include "internal.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t u16_le(const uint8_t *p){
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t u32_le(const uint8_t *p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t u64_le(const uint8_t *p){
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static int16_t i16_le(const uint8_t *p){ return (int16_t)u16_le(p); }
static int32_t i32_le(const uint8_t *p){ return (int32_t)u32_le(p); }
static int64_t i64_le(const uint8_t *p){ return (int64_t)u64_le(p); }
static float f32_le(const uint8_t *p){ uint32_t u = u32_le(p); float f; memcpy(&f, &u, 4); return f; }
static double f64_le(const uint8_t *p){ uint64_t u = u64_le(p); double d; memcpy(&d, &u, 8); return d; }

float lp_f16_to_f32(uint16_t h){
    uint32_t sign = ((uint32_t)h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x03FFu;
    uint32_t u;
    if (exp == 0) {
        if (mant == 0) {
            u = sign;
        } else {
            uint32_t e = 127u - 14u;
            while ((mant & 0x0400u) == 0) { mant <<= 1; e--; }
            mant &= 0x03FFu;
            u = sign | (e << 23) | (mant << 13);
        }
    } else if (exp == 31u) {
        if (mant == 0) u = sign | 0x7F800000u;
        else u = sign | 0x7FC00000u | (mant << 13);
    } else {
        u = sign | ((exp + (127u - 15u)) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

uint16_t lp_f32_to_f16(float v){
    uint32_t u;
    memcpy(&u, &v, sizeof u);
    uint32_t sign = (u >> 16) & 0x8000u;
    uint32_t mag = u & 0x7FFFFFFFu;
    if (mag > 0x7F800000u) return (uint16_t)(sign | 0x7E00u | ((mag >> 13) & 0x03FFu));
    if (mag >= 0x47800000u) return (uint16_t)(sign | 0x7C00u);
    if (mag < 0x33000000u) return (uint16_t)sign;
    if (mag < 0x38800000u) {
        uint32_t mant = (mag & 0x007FFFFFu) | 0x00800000u;
        uint32_t shift = 126u - (mag >> 23);
        uint32_t rem = (shift < 32u) ? (mant & ((1u << shift) - 1u)) : mant;
        uint32_t half = 1u << (shift - 1u);
        uint32_t q = mant >> shift;
        if (rem > half || (rem == half && (q & 1u))) q++;
        return (uint16_t)(sign | q);
    }
    uint32_t round_bias = 0x00000FFFu + ((u >> 13) & 1u);
    uint32_t rounded = mag + round_bias;
    if (rounded >= 0x47800000u) return (uint16_t)(sign | 0x7C00u);
    int32_t exp = (int32_t)(rounded >> 23) - 127 + 15;
    uint32_t mant = (rounded >> 13) & 0x03FFu;
    return (uint16_t)(sign | ((uint32_t)exp << 10) | mant);
}

float lp_bf16_to_f32(uint16_t b){
    uint32_t u = (uint32_t)b << 16;
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

uint16_t lp_f32_to_bf16(float v){
    uint32_t u;
    memcpy(&u, &v, sizeof u);
    if ((u & 0x7FFFFFFFu) > 0x7F800000u) return (uint16_t)((u >> 16) | 0x0040u);
    uint32_t bias = 0x7FFFu + ((u >> 16) & 1u);
    return (uint16_t)((u + bias) >> 16);
}

void lp_f16_to_f32_scalar(const uint16_t *in, size_t n, float *out){
    for (size_t i = 0; i < n; i++) out[i] = lp_f16_to_f32(in[i]);
}

void lp_bf16_to_f32_scalar(const uint16_t *in, size_t n, float *out){
    for (size_t i = 0; i < n; i++) out[i] = lp_bf16_to_f32(in[i]);
}

static float f8_e5m2_to_f32(uint8_t b){
    uint32_t sign = ((uint32_t)b & 0x80u) << 24;
    uint32_t exp = (b >> 2) & 0x1Fu;
    uint32_t mant = b & 0x03u;
    uint32_t u;
    if (exp == 0) {
        if (!mant) u = sign;
        else {
            uint32_t e = 127u - 14u;
            while ((mant & 0x04u) == 0) { mant <<= 1; e--; }
            mant &= 0x03u;
            u = sign | (e << 23) | (mant << 21);
        }
    } else if (exp == 31u) {
        u = mant ? (sign | 0x7FC00000u | (mant << 21)) : (sign | 0x7F800000u);
    } else {
        u = sign | ((exp + (127u - 15u)) << 23) | (mant << 21);
    }
    float f; memcpy(&f, &u, sizeof f); return f;
}

static float f8_e4m3_to_f32(uint8_t b){
    uint32_t sign = ((uint32_t)b & 0x80u) << 24;
    uint32_t exp = (b >> 3) & 0x0Fu;
    uint32_t mant = b & 0x07u;
    uint32_t u;
    if (exp == 15u && mant == 7u) {
        u = sign | 0x7FC00000u;
    } else if (exp == 0) {
        if (!mant) u = sign;
        else {
            uint32_t e = 127u - 6u;
            while ((mant & 0x08u) == 0) { mant <<= 1; e--; }
            mant &= 0x07u;
            u = sign | (e << 23) | (mant << 20);
        }
    } else {
        u = sign | ((exp + (127u - 7u)) << 23) | (mant << 20);
    }
    float f; memcpy(&f, &u, sizeof f); return f;
}

lp_dtype lp_dtype_from_name(const char *name){
    if (!name) return LP_DTYPE_UNKNOWN;
    if (!strcmp(name, "F32")) return LP_DTYPE_F32;
    if (!strcmp(name, "F16")) return LP_DTYPE_F16;
    if (!strcmp(name, "BF16")) return LP_DTYPE_BF16;
    if (!strcmp(name, "F64")) return LP_DTYPE_F64;
    if (!strcmp(name, "I8")) return LP_DTYPE_I8;
    if (!strcmp(name, "I16")) return LP_DTYPE_I16;
    if (!strcmp(name, "I32")) return LP_DTYPE_I32;
    if (!strcmp(name, "I64")) return LP_DTYPE_I64;
    if (!strcmp(name, "U8")) return LP_DTYPE_U8;
    if (!strcmp(name, "U16")) return LP_DTYPE_U16;
    if (!strcmp(name, "U32")) return LP_DTYPE_U32;
    if (!strcmp(name, "U64")) return LP_DTYPE_U64;
    if (!strcmp(name, "BOOL")) return LP_DTYPE_BOOL;
    if (!strcmp(name, "F8_E5M2")) return LP_DTYPE_F8_E5M2;
    if (!strcmp(name, "F8_E4M3")) return LP_DTYPE_F8_E4M3;
    if (!strcmp(name, "Q4_0")) return LP_DTYPE_Q4_0;
    if (!strcmp(name, "Q4_1")) return LP_DTYPE_Q4_1;
    if (!strcmp(name, "Q5_0")) return LP_DTYPE_Q5_0;
    if (!strcmp(name, "Q5_1")) return LP_DTYPE_Q5_1;
    if (!strcmp(name, "Q8_0")) return LP_DTYPE_Q8_0;
    if (!strcmp(name, "Q8_1")) return LP_DTYPE_Q8_1;
    if (!strcmp(name, "Q2_K")) return LP_DTYPE_Q2_K;
    if (!strcmp(name, "Q3_K")) return LP_DTYPE_Q3_K;
    if (!strcmp(name, "Q4_K")) return LP_DTYPE_Q4_K;
    if (!strcmp(name, "Q5_K")) return LP_DTYPE_Q5_K;
    if (!strcmp(name, "Q6_K")) return LP_DTYPE_Q6_K;
    if (!strcmp(name, "Q8_K")) return LP_DTYPE_Q8_K;
    return LP_DTYPE_UNKNOWN;
}

const char *lp_dtype_name(lp_dtype dt){
    switch (dt) {
    case LP_DTYPE_F32: return "F32";
    case LP_DTYPE_F16: return "F16";
    case LP_DTYPE_BF16: return "BF16";
    case LP_DTYPE_F64: return "F64";
    case LP_DTYPE_I8: return "I8";
    case LP_DTYPE_I16: return "I16";
    case LP_DTYPE_I32: return "I32";
    case LP_DTYPE_I64: return "I64";
    case LP_DTYPE_U8: return "U8";
    case LP_DTYPE_U16: return "U16";
    case LP_DTYPE_U32: return "U32";
    case LP_DTYPE_U64: return "U64";
    case LP_DTYPE_BOOL: return "BOOL";
    case LP_DTYPE_F8_E5M2: return "F8_E5M2";
    case LP_DTYPE_F8_E4M3: return "F8_E4M3";
    case LP_DTYPE_Q4_0: return "Q4_0";
    case LP_DTYPE_Q4_1: return "Q4_1";
    case LP_DTYPE_Q5_0: return "Q5_0";
    case LP_DTYPE_Q5_1: return "Q5_1";
    case LP_DTYPE_Q8_0: return "Q8_0";
    case LP_DTYPE_Q8_1: return "Q8_1";
    case LP_DTYPE_Q2_K: return "Q2_K";
    case LP_DTYPE_Q3_K: return "Q3_K";
    case LP_DTYPE_Q4_K: return "Q4_K";
    case LP_DTYPE_Q5_K: return "Q5_K";
    case LP_DTYPE_Q6_K: return "Q6_K";
    case LP_DTYPE_Q8_K: return "Q8_K";
    default: return "UNKNOWN";
    }
}

lp_dtype lp_dtype_from_ggml(uint32_t ggml_type){
    switch (ggml_type) {
    case 0: return LP_DTYPE_F32;
    case 1: return LP_DTYPE_F16;
    case 2: return LP_DTYPE_Q4_0;
    case 3: return LP_DTYPE_Q4_1;
    case 6: return LP_DTYPE_Q5_0;
    case 7: return LP_DTYPE_Q5_1;
    case 8: return LP_DTYPE_Q8_0;
    case 9: return LP_DTYPE_Q8_1;
    case 10: return LP_DTYPE_Q2_K;
    case 11: return LP_DTYPE_Q3_K;
    case 12: return LP_DTYPE_Q4_K;
    case 13: return LP_DTYPE_Q5_K;
    case 14: return LP_DTYPE_Q6_K;
    case 15: return LP_DTYPE_Q8_K;
    case 24: return LP_DTYPE_I8;
    case 25: return LP_DTYPE_I16;
    case 26: return LP_DTYPE_I32;
    case 27: return LP_DTYPE_I64;
    case 28: return LP_DTYPE_F64;
    case 30: return LP_DTYPE_BF16;
    default: return LP_DTYPE_UNKNOWN;
    }
}

int32_t lp_dtype_to_ggml(lp_dtype dt){
    switch (dt) {
    case LP_DTYPE_F32: return 0;
    case LP_DTYPE_F16: return 1;
    case LP_DTYPE_Q4_0: return 2;
    case LP_DTYPE_Q4_1: return 3;
    case LP_DTYPE_Q5_0: return 6;
    case LP_DTYPE_Q5_1: return 7;
    case LP_DTYPE_Q8_0: return 8;
    case LP_DTYPE_Q8_1: return 9;
    case LP_DTYPE_Q2_K: return 10;
    case LP_DTYPE_Q3_K: return 11;
    case LP_DTYPE_Q4_K: return 12;
    case LP_DTYPE_Q5_K: return 13;
    case LP_DTYPE_Q6_K: return 14;
    case LP_DTYPE_Q8_K: return 15;
    case LP_DTYPE_I8: return 24;
    case LP_DTYPE_I16: return 25;
    case LP_DTYPE_I32: return 26;
    case LP_DTYPE_I64: return 27;
    case LP_DTYPE_F64: return 28;
    case LP_DTYPE_BF16: return 30;
    default: return -1;
    }
}

size_t lp_dtype_block_elements(lp_dtype dt){
    switch (dt) {
    case LP_DTYPE_F32: case LP_DTYPE_F16: case LP_DTYPE_BF16: case LP_DTYPE_F64:
    case LP_DTYPE_I8: case LP_DTYPE_I16: case LP_DTYPE_I32: case LP_DTYPE_I64:
    case LP_DTYPE_U8: case LP_DTYPE_U16: case LP_DTYPE_U32: case LP_DTYPE_U64:
    case LP_DTYPE_BOOL: case LP_DTYPE_F8_E5M2: case LP_DTYPE_F8_E4M3:
        return 1u;
    case LP_DTYPE_Q4_0: case LP_DTYPE_Q4_1: case LP_DTYPE_Q5_0: case LP_DTYPE_Q5_1:
    case LP_DTYPE_Q8_0: case LP_DTYPE_Q8_1:
        return 32u;
    case LP_DTYPE_Q2_K: case LP_DTYPE_Q3_K: case LP_DTYPE_Q4_K: case LP_DTYPE_Q5_K:
    case LP_DTYPE_Q6_K: case LP_DTYPE_Q8_K:
        return 256u;
    default:
        return 0;
    }
}

size_t lp_dtype_block_bytes(lp_dtype dt){
    switch (dt) {
    case LP_DTYPE_F64: case LP_DTYPE_I64: case LP_DTYPE_U64: return 8u;
    case LP_DTYPE_F32: case LP_DTYPE_I32: case LP_DTYPE_U32: return 4u;
    case LP_DTYPE_F16: case LP_DTYPE_BF16: case LP_DTYPE_I16: case LP_DTYPE_U16: return 2u;
    case LP_DTYPE_I8: case LP_DTYPE_U8: case LP_DTYPE_BOOL:
    case LP_DTYPE_F8_E5M2: case LP_DTYPE_F8_E4M3: return 1u;
    case LP_DTYPE_Q4_0: return 18u;
    case LP_DTYPE_Q4_1: return 20u;
    case LP_DTYPE_Q5_0: return 22u;
    case LP_DTYPE_Q5_1: return 24u;
    case LP_DTYPE_Q8_0: return 34u;
    case LP_DTYPE_Q8_1: return 36u;
    case LP_DTYPE_Q2_K: return 84u;
    case LP_DTYPE_Q3_K: return 110u;
    case LP_DTYPE_Q4_K: return 144u;
    case LP_DTYPE_Q5_K: return 176u;
    case LP_DTYPE_Q6_K: return 210u;
    case LP_DTYPE_Q8_K: return 292u;
    default: return 0;
    }
}

size_t lp_tensor_nbytes(lp_dtype dt, size_t n_elements){
    size_t bel = lp_dtype_block_elements(dt);
    size_t bsz = lp_dtype_block_bytes(dt);
    if (!bel || !bsz) return 0;
    if (n_elements % bel != 0) return 0;
    size_t nb = n_elements / bel;
    if (nb > SIZE_MAX / bsz) return 0;
    return nb * bsz;
}

static void dequant_q4_0(const uint8_t *blk, float *y){
    float d = lp_f16_to_f32(u16_le(blk));
    const uint8_t *qs = blk + 2;
    for (int j = 0; j < 16; j++) {
        int x0 = (int)(qs[j] & 0x0Fu) - 8;
        int x1 = (int)(qs[j] >> 4) - 8;
        y[j] = (float)x0 * d;
        y[j + 16] = (float)x1 * d;
    }
}

static void dequant_q4_1(const uint8_t *blk, float *y){
    float d = lp_f16_to_f32(u16_le(blk));
    float m = lp_f16_to_f32(u16_le(blk + 2));
    const uint8_t *qs = blk + 4;
    for (int j = 0; j < 16; j++) {
        y[j] = (float)(qs[j] & 0x0Fu) * d + m;
        y[j + 16] = (float)(qs[j] >> 4) * d + m;
    }
}

static void dequant_q5_0(const uint8_t *blk, float *y){
    float d = lp_f16_to_f32(u16_le(blk));
    uint32_t qh = u32_le(blk + 2);
    const uint8_t *qs = blk + 6;
    for (int j = 0; j < 16; j++) {
        int x0 = (int)((qs[j] & 0x0Fu) | (((qh >> j) & 1u) << 4)) - 16;
        int x1 = (int)((qs[j] >> 4) | (((qh >> (j + 16)) & 1u) << 4)) - 16;
        y[j] = (float)x0 * d;
        y[j + 16] = (float)x1 * d;
    }
}

static void dequant_q5_1(const uint8_t *blk, float *y){
    float d = lp_f16_to_f32(u16_le(blk));
    float m = lp_f16_to_f32(u16_le(blk + 2));
    uint32_t qh = u32_le(blk + 4);
    const uint8_t *qs = blk + 8;
    for (int j = 0; j < 16; j++) {
        uint32_t x0 = (qs[j] & 0x0Fu) | (((qh >> j) & 1u) << 4);
        uint32_t x1 = (qs[j] >> 4) | (((qh >> (j + 16)) & 1u) << 4);
        y[j] = (float)x0 * d + m;
        y[j + 16] = (float)x1 * d + m;
    }
}

static void dequant_q8_0(const uint8_t *blk, float *y){
    float d = lp_f16_to_f32(u16_le(blk));
    const int8_t *qs = (const int8_t *)(blk + 2);
    for (int j = 0; j < 32; j++) y[j] = (float)qs[j] * d;
}

static void dequant_q8_1(const uint8_t *blk, float *y){
    float d = lp_f16_to_f32(u16_le(blk));
    const int8_t *qs = (const int8_t *)(blk + 4);
    for (int j = 0; j < 32; j++) y[j] = (float)qs[j] * d;
}

static void get_scale_min_k4(int j, const uint8_t *q, uint8_t *d, uint8_t *m){
    if (j < 4) {
        *d = q[j] & 63u;
        *m = q[j + 4] & 63u;
    } else {
        *d = (uint8_t)((q[j + 4] & 0x0Fu) | ((q[j - 4] >> 6) << 4));
        *m = (uint8_t)((q[j + 4] >> 4) | ((q[j] >> 6) << 4));
    }
}

static void dequant_q2_k(const uint8_t *blk, float *y){
    const uint8_t *scales = blk;
    const uint8_t *qs = blk + 16;
    float d = lp_f16_to_f32(u16_le(blk + 80));
    float dmin = lp_f16_to_f32(u16_le(blk + 82));
    int is = 0;
    for (int n = 0; n < 256; n += 128) {
        const uint8_t *q = qs + (n == 128 ? 32 : 0);
        int shift = 0;
        for (int j = 0; j < 4; j++) {
            uint8_t sc = scales[is++];
            float dl = d * (float)(sc & 0x0Fu);
            float ml = dmin * (float)(sc >> 4);
            for (int l = 0; l < 16; l++) *y++ = dl * (float)((int)((q[l] >> shift) & 3u)) - ml;
            sc = scales[is++];
            dl = d * (float)(sc & 0x0Fu);
            ml = dmin * (float)(sc >> 4);
            for (int l = 0; l < 16; l++) *y++ = dl * (float)((int)((q[l + 16] >> shift) & 3u)) - ml;
            shift += 2;
        }
    }
}

static void dequant_q3_k(const uint8_t *blk, float *y){
    const uint8_t *hmask = blk;
    const uint8_t *qs = blk + 32;
    const uint8_t *sc_raw = blk + 96;
    float d_all = lp_f16_to_f32(u16_le(blk + 108));
    uint32_t a0 = u32_le(sc_raw), a1 = u32_le(sc_raw + 4), a2 = u32_le(sc_raw + 8);
    uint32_t aux[4];
    aux[0] = (a0 & 0x0F0F0F0Fu) | ((a2 & 0x03030303u) << 4);
    aux[1] = (a1 & 0x0F0F0F0Fu) | (((a2 >> 2) & 0x03030303u) << 4);
    aux[2] = ((a0 >> 4) & 0x0F0F0F0Fu) | (((a2 >> 4) & 0x03030303u) << 4);
    aux[3] = ((a1 >> 4) & 0x0F0F0F0Fu) | (((a2 >> 6) & 0x03030303u) << 4);
    int8_t scales[16];
    for (int i = 0; i < 4; i++) {
        for (int b = 0; b < 4; b++) {
            uint8_t sb = (uint8_t)((aux[i] >> (8 * b)) & 0xFFu);
            scales[4 * i + b] = (int8_t)sb;
        }
    }
    int is = 0;
    uint8_t m = 1;
    const uint8_t *q = qs;
    for (int n = 0; n < 256; n += 128) {
        int shift = 0;
        for (int j = 0; j < 4; j++) {
            float dl = d_all * (float)(scales[is++] - 32);
            for (int l = 0; l < 16; l++)
                *y++ = dl * (float)(((int)(q[l] >> shift) & 3) - ((hmask[l] & m) ? 0 : 4));
            dl = d_all * (float)(scales[is++] - 32);
            for (int l = 0; l < 16; l++)
                *y++ = dl * (float)(((int)(q[l + 16] >> shift) & 3) - ((hmask[l + 16] & m) ? 0 : 4));
            shift += 2;
            m <<= 1;
        }
        q += 32;
    }
}

static void dequant_q4_k(const uint8_t *blk, float *y){
    float d = lp_f16_to_f32(u16_le(blk));
    float dmin = lp_f16_to_f32(u16_le(blk + 2));
    const uint8_t *scales = blk + 4;
    const uint8_t *qs = blk + 16;
    int is = 0;
    const uint8_t *q = qs;
    for (int j = 0; j < 256; j += 64) {
        uint8_t sc, m;
        get_scale_min_k4(is + 0, scales, &sc, &m);
        float d1 = d * (float)sc, m1 = dmin * (float)m;
        get_scale_min_k4(is + 1, scales, &sc, &m);
        float d2 = d * (float)sc, m2 = dmin * (float)m;
        for (int l = 0; l < 32; l++) *y++ = d1 * (float)(q[l] & 0x0Fu) - m1;
        for (int l = 0; l < 32; l++) *y++ = d2 * (float)(q[l] >> 4) - m2;
        q += 32;
        is += 2;
    }
}

static void dequant_q5_k(const uint8_t *blk, float *y){
    float d = lp_f16_to_f32(u16_le(blk));
    float dmin = lp_f16_to_f32(u16_le(blk + 2));
    const uint8_t *scales = blk + 4;
    const uint8_t *qh = blk + 16;
    const uint8_t *qs = blk + 48;
    int is = 0;
    uint8_t u1 = 1u, u2 = 2u;
    const uint8_t *q = qs;
    for (int j = 0; j < 256; j += 64) {
        uint8_t sc, m;
        get_scale_min_k4(is + 0, scales, &sc, &m);
        float d1 = d * (float)sc, m1 = dmin * (float)m;
        get_scale_min_k4(is + 1, scales, &sc, &m);
        float d2 = d * (float)sc, m2 = dmin * (float)m;
        for (int l = 0; l < 32; l++) *y++ = d1 * (float)((q[l] & 0x0Fu) + ((qh[l] & u1) ? 16u : 0u)) - m1;
        for (int l = 0; l < 32; l++) *y++ = d2 * (float)((q[l] >> 4) + ((qh[l] & u2) ? 16u : 0u)) - m2;
        q += 32;
        is += 2;
        u1 <<= 2;
        u2 <<= 2;
    }
}

static void dequant_q6_k(const uint8_t *blk, float *y){
    const uint8_t *ql = blk;
    const uint8_t *qh = blk + 128;
    const int8_t *sc = (const int8_t *)(blk + 192);
    float d = lp_f16_to_f32(u16_le(blk + 208));
    for (int n = 0; n < 256; n += 128) {
        for (int l = 0; l < 32; l++) {
            int is = l / 16;
            int8_t q1 = (int8_t)((ql[l] & 0x0Fu) | (((qh[l] >> 0) & 3u) << 4)) - 32;
            int8_t q2 = (int8_t)((ql[l + 32] & 0x0Fu) | (((qh[l] >> 2) & 3u) << 4)) - 32;
            int8_t q3 = (int8_t)((ql[l] >> 4) | (((qh[l] >> 4) & 3u) << 4)) - 32;
            int8_t q4 = (int8_t)((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3u) << 4)) - 32;
            y[l + 0] = d * (float)sc[is + 0] * (float)q1;
            y[l + 32] = d * (float)sc[is + 2] * (float)q2;
            y[l + 64] = d * (float)sc[is + 4] * (float)q3;
            y[l + 96] = d * (float)sc[is + 6] * (float)q4;
        }
        y += 128;
        ql += 64;
        qh += 32;
        sc += 8;
    }
}

static void dequant_q8_k(const uint8_t *blk, float *y){
    float d = f32_le(blk);
    const int8_t *qs = (const int8_t *)(blk + 4);
    for (int j = 0; j < 256; j++) y[j] = (float)qs[j] * d;
}

int lp_tensor_decode_f32(const void *raw, size_t n_elements, lp_dtype dt, float *out){
    if (n_elements == 0) return (!raw || !out) ? -1 : 0;
    if (!raw || !out) return -1;
    size_t bel = lp_dtype_block_elements(dt);
    size_t bsz = lp_dtype_block_bytes(dt);
    if (!bel || !bsz) return -2;
    if (n_elements % bel != 0) return -1;
    const uint8_t *p = (const uint8_t *)raw;

    switch (dt) {
    case LP_DTYPE_F32:
        for (size_t i = 0; i < n_elements; i++) out[i] = f32_le(p + i * 4u);
        return 0;
    case LP_DTYPE_F64:
        for (size_t i = 0; i < n_elements; i++) out[i] = (float)f64_le(p + i * 8u);
        return 0;
    case LP_DTYPE_F16:
        if (((uintptr_t)p & 1u) == 0) lp_kernels()->f16_to_f32((const uint16_t *)p, n_elements, out);
        else for (size_t i = 0; i < n_elements; i++) out[i] = lp_f16_to_f32(u16_le(p + i * 2u));
        return 0;
    case LP_DTYPE_BF16:
        if (((uintptr_t)p & 1u) == 0) lp_kernels()->bf16_to_f32((const uint16_t *)p, n_elements, out);
        else for (size_t i = 0; i < n_elements; i++) out[i] = lp_bf16_to_f32(u16_le(p + i * 2u));
        return 0;
    case LP_DTYPE_I8:
        for (size_t i = 0; i < n_elements; i++) out[i] = (float)(int8_t)p[i];
        return 0;
    case LP_DTYPE_I16:
        for (size_t i = 0; i < n_elements; i++) out[i] = (float)i16_le(p + i * 2u);
        return 0;
    case LP_DTYPE_I32:
        for (size_t i = 0; i < n_elements; i++) out[i] = (float)i32_le(p + i * 4u);
        return 0;
    case LP_DTYPE_I64:
        for (size_t i = 0; i < n_elements; i++) out[i] = (float)i64_le(p + i * 8u);
        return 0;
    case LP_DTYPE_U8:
        for (size_t i = 0; i < n_elements; i++) out[i] = (float)p[i];
        return 0;
    case LP_DTYPE_U16:
        for (size_t i = 0; i < n_elements; i++) out[i] = (float)u16_le(p + i * 2u);
        return 0;
    case LP_DTYPE_U32:
        for (size_t i = 0; i < n_elements; i++) out[i] = (float)u32_le(p + i * 4u);
        return 0;
    case LP_DTYPE_U64:
        for (size_t i = 0; i < n_elements; i++) out[i] = (float)u64_le(p + i * 8u);
        return 0;
    case LP_DTYPE_BOOL:
        for (size_t i = 0; i < n_elements; i++) out[i] = p[i] ? 1.0f : 0.0f;
        return 0;
    case LP_DTYPE_F8_E5M2:
        for (size_t i = 0; i < n_elements; i++) out[i] = f8_e5m2_to_f32(p[i]);
        return 0;
    case LP_DTYPE_F8_E4M3:
        for (size_t i = 0; i < n_elements; i++) out[i] = f8_e4m3_to_f32(p[i]);
        return 0;
    case LP_DTYPE_Q4_0:
        for (size_t b = 0, nb = n_elements / 32u; b < nb; b++) dequant_q4_0(p + b * 18u, out + b * 32u);
        return 0;
    case LP_DTYPE_Q4_1:
        for (size_t b = 0, nb = n_elements / 32u; b < nb; b++) dequant_q4_1(p + b * 20u, out + b * 32u);
        return 0;
    case LP_DTYPE_Q5_0:
        for (size_t b = 0, nb = n_elements / 32u; b < nb; b++) dequant_q5_0(p + b * 22u, out + b * 32u);
        return 0;
    case LP_DTYPE_Q5_1:
        for (size_t b = 0, nb = n_elements / 32u; b < nb; b++) dequant_q5_1(p + b * 24u, out + b * 32u);
        return 0;
    case LP_DTYPE_Q8_0:
        for (size_t b = 0, nb = n_elements / 32u; b < nb; b++) dequant_q8_0(p + b * 34u, out + b * 32u);
        return 0;
    case LP_DTYPE_Q8_1:
        for (size_t b = 0, nb = n_elements / 32u; b < nb; b++) dequant_q8_1(p + b * 36u, out + b * 32u);
        return 0;
    case LP_DTYPE_Q2_K:
        for (size_t b = 0, nb = n_elements / 256u; b < nb; b++) dequant_q2_k(p + b * 84u, out + b * 256u);
        return 0;
    case LP_DTYPE_Q3_K:
        for (size_t b = 0, nb = n_elements / 256u; b < nb; b++) dequant_q3_k(p + b * 110u, out + b * 256u);
        return 0;
    case LP_DTYPE_Q4_K:
        for (size_t b = 0, nb = n_elements / 256u; b < nb; b++) dequant_q4_k(p + b * 144u, out + b * 256u);
        return 0;
    case LP_DTYPE_Q5_K:
        for (size_t b = 0, nb = n_elements / 256u; b < nb; b++) dequant_q5_k(p + b * 176u, out + b * 256u);
        return 0;
    case LP_DTYPE_Q6_K:
        for (size_t b = 0, nb = n_elements / 256u; b < nb; b++) dequant_q6_k(p + b * 210u, out + b * 256u);
        return 0;
    case LP_DTYPE_Q8_K:
        for (size_t b = 0, nb = n_elements / 256u; b < nb; b++) dequant_q8_k(p + b * 292u, out + b * 256u);
        return 0;
    default:
        return -2;
    }
}

int lp_gguf_hf_to_ggml_name(const char *hf_name, char *out, size_t cap){
    if (!hf_name || !out || cap == 0) return -1;
    const char *exact = NULL;
    if (!strcmp(hf_name, "model.embed_tokens.weight")) exact = "token_embd.weight";
    else if (!strcmp(hf_name, "model.norm.weight")) exact = "output_norm.weight";
    else if (!strcmp(hf_name, "lm_head.weight")) exact = "output.weight";
    if (exact) {
        size_t n = strlen(exact);
        if (n + 1 > cap) return -1;
        memcpy(out, exact, n + 1);
        return (int)n;
    }
    static const char prefix[] = "model.layers.";
    const size_t plen = sizeof(prefix) - 1;
    if (!strncmp(hf_name, prefix, plen)) {
        const char *dot = strchr(hf_name + plen, '.');
        if (dot && dot > hf_name + plen) {
            size_t ilen = (size_t)(dot - (hf_name + plen));
            const char *rest = dot + 1;
            static const struct { const char *hf, *ggml; } map[] = {
                { "self_attn.q_proj.weight", "attn_q.weight" },
                { "self_attn.k_proj.weight", "attn_k.weight" },
                { "self_attn.v_proj.weight", "attn_v.weight" },
                { "self_attn.o_proj.weight", "attn_output.weight" },
                { "mlp.gate_proj.weight", "ffn_gate.weight" },
                { "mlp.up_proj.weight", "ffn_up.weight" },
                { "mlp.down_proj.weight", "ffn_down.weight" },
                { "input_layernorm.weight", "attn_norm.weight" },
                { "post_attention_layernorm.weight", "ffn_norm.weight" }
            };
            const char *mapped = rest;
            for (size_t i = 0; i < sizeof map / sizeof *map; i++) {
                if (!strcmp(rest, map[i].hf)) { mapped = map[i].ggml; break; }
            }
            size_t mlen = strlen(mapped);
            size_t need = 4u + ilen + 1u + mlen;
            if (need + 1 > cap) return -1;
            memcpy(out, "blk.", 4);
            memcpy(out + 4, hf_name + plen, ilen);
            out[4 + ilen] = '.';
            memcpy(out + 4 + ilen + 1, mapped, mlen + 1);
            return (int)need;
        }
    }
    size_t n = strlen(hf_name);
    if (n + 1 > cap) return -1;
    memcpy(out, hf_name, n + 1);
    return (int)n;
}

/* ---- Safetensors reader --------------------------------------------------- */

typedef struct {
    char *name;
    size_t name_len;
    lp_dtype dtype;
    uint32_t ndim;
    uint64_t dims[LP_TENSOR_MAX_DIMS];
    uint64_t data_start;
    uint64_t data_end;
} StTensor;

struct lp_safetensors {
    uint64_t data_offset;
    bool has_metadata;
    char *metadata_json;
    size_t metadata_len;
    StTensor *tensors;
    size_t count;
};

typedef struct { const char *p, *end; } StReader;

static void st_ws(StReader *r){
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')) r->p++;
}
static bool st_ch(StReader *r, char c){
    st_ws(r);
    if (r->p < r->end && *r->p == c) { r->p++; return true; }
    return false;
}
static bool st_peek(StReader *r, char c){
    st_ws(r);
    return r->p < r->end && *r->p == c;
}
static int hex_val(char c){
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool st_str(StReader *r, lp_buf *out){
    st_ws(r);
    if (r->p >= r->end || *r->p != '"') return false;
    r->p++;
    out->n = 0;
    while (r->p < r->end && *r->p != '"') {
        unsigned char c = (unsigned char)*r->p++;
        if (c == '\\') {
            if (r->p >= r->end) return false;
            char esc = *r->p++;
            switch (esc) {
            case '"': lp_buf_byte(out, '"'); break;
            case '\\': lp_buf_byte(out, '\\'); break;
            case '/': lp_buf_byte(out, '/'); break;
            case 'b': lp_buf_byte(out, '\b'); break;
            case 'f': lp_buf_byte(out, '\f'); break;
            case 'n': lp_buf_byte(out, '\n'); break;
            case 'r': lp_buf_byte(out, '\r'); break;
            case 't': lp_buf_byte(out, '\t'); break;
            case 'u': {
                if (r->end - r->p < 4) return false;
                uint32_t cp = 0;
                for (int k = 0; k < 4; k++) {
                    int h = hex_val(r->p[k]);
                    if (h < 0) return false;
                    cp = (cp << 4) | (uint32_t)h;
                }
                r->p += 4;
                if (cp < 0x80u) {
                    lp_buf_byte(out, (uint8_t)cp);
                } else if (cp < 0x800u) {
                    lp_buf_byte(out, (uint8_t)(0xC0u | (cp >> 6)));
                    lp_buf_byte(out, (uint8_t)(0x80u | (cp & 0x3Fu)));
                } else {
                    lp_buf_byte(out, (uint8_t)(0xE0u | (cp >> 12)));
                    lp_buf_byte(out, (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu)));
                    lp_buf_byte(out, (uint8_t)(0x80u | (cp & 0x3Fu)));
                }
                break;
            }
            default: return false;
            }
        } else {
            lp_buf_byte(out, c);
        }
    }
    if (r->p >= r->end) return false;
    r->p++;
    lp_buf_byte(out, 0);
    out->n--;
    return true;
}
static bool st_u64(StReader *r, uint64_t *out){
    st_ws(r);
    if (r->p >= r->end) return false;
    if (*r->p == '+') r->p++;
    if (r->p >= r->end || *r->p < '0' || *r->p > '9') return false;
    uint64_t v = 0;
    while (r->p < r->end && *r->p >= '0' && *r->p <= '9') {
        uint64_t d = (uint64_t)(*r->p - '0');
        if (v > (UINT64_MAX - d) / 10u) return false;
        v = v * 10u + d;
        r->p++;
    }
    if (r->p < r->end && (*r->p == '.' || *r->p == 'e' || *r->p == 'E')) return false;
    *out = v;
    return true;
}
static bool st_skip(StReader *r, lp_buf *scratch){
    st_ws(r);
    if (r->p >= r->end) return false;
    if (*r->p == '"') return st_str(r, scratch);
    if (*r->p == '{' || *r->p == '[') {
        char open = *r->p, close = open == '{' ? '}' : ']';
        int depth = 0;
        while (r->p < r->end) {
            if (*r->p == '"') { if (!st_str(r, scratch)) return false; continue; }
            if (*r->p == open) depth++;
            else if (*r->p == close && --depth == 0) { r->p++; return true; }
            r->p++;
        }
        return false;
    }
    const char *start = r->p;
    while (r->p < r->end && *r->p != ',' && *r->p != '}' && *r->p != ']'
           && *r->p != ' ' && *r->p != '\n' && *r->p != '\r' && *r->p != '\t') r->p++;
    return r->p > start;
}

static int st_tensor_cmp(const void *a, const void *b){
    const StTensor *x = a, *y = b;
    if (x->data_start != y->data_start) return x->data_start < y->data_start ? -1 : 1;
    return 0;
}

lp_safetensors *lp_safetensors_open(const void *bytes, size_t len){
    if (!bytes || len < 8u) { errno = EINVAL; return NULL; }
    const uint8_t *raw = (const uint8_t *)bytes;
    uint64_t jlen = u64_le(raw);
    if (jlen == 0 || jlen > 256ull * 1024ull * 1024ull || (uint64_t)len < 8ull + jlen) {
        errno = EINVAL; return NULL;
    }
    StReader r = { (const char *)raw + 8, (const char *)raw + 8 + (size_t)jlen };
    if (!st_ch(&r, '{')) { errno = EINVAL; return NULL; }

    lp_safetensors *st = lp_zalloc(sizeof *st);
    st->data_offset = 8ull + jlen;
    lp_buf key = {0}, val = {0};
    size_t cap = 0;

    if (!st_peek(&r, '}')) {
        for (;;) {
            if (!st_str(&r, &key) || !st_ch(&r, ':')) goto fail;
            if (key.n == 12 && !strcmp((const char *)key.b, "__metadata__")) {
                st_ws(&r);
                const char *m0 = r.p;
                if (!st_skip(&r, &val)) goto fail;
                size_t mlen = (size_t)(r.p - m0);
                lp_free(st->metadata_json);
                st->metadata_json = lp_alloc(mlen + 1);
                memcpy(st->metadata_json, m0, mlen);
                st->metadata_json[mlen] = 0;
                st->metadata_len = mlen;
                st->has_metadata = true;
            } else {
                if (!st_ch(&r, '{')) goto fail;
                StTensor t; memset(&t, 0, sizeof t);
                t.dtype = LP_DTYPE_UNKNOWN;
                t.name = lp_alloc(key.n + 1);
                memcpy(t.name, key.b, key.n + 1);
                t.name_len = key.n;
                bool have_dt = false, have_sh = false, have_off = false;
                if (!st_peek(&r, '}')) {
                    for (;;) {
                        if (!st_str(&r, &val) || !st_ch(&r, ':')) { lp_free(t.name); goto fail; }
                        if (!strcmp((const char *)val.b, "dtype")) {
                            if (!st_str(&r, &val)) { lp_free(t.name); goto fail; }
                            t.dtype = lp_dtype_from_name((const char *)val.b);
                            have_dt = true;
                        } else if (!strcmp((const char *)val.b, "shape")) {
                            if (!st_ch(&r, '[')) { lp_free(t.name); goto fail; }
                            if (!st_peek(&r, ']')) {
                                for (;;) {
                                    uint64_t d;
                                    if (!st_u64(&r, &d) || t.ndim >= LP_TENSOR_MAX_DIMS) { lp_free(t.name); goto fail; }
                                    t.dims[t.ndim++] = d;
                                    if (st_ch(&r, ',')) continue;
                                    break;
                                }
                            }
                            if (!st_ch(&r, ']')) { lp_free(t.name); goto fail; }
                            have_sh = true;
                        } else if (!strcmp((const char *)val.b, "data_offsets")) {
                            if (!st_ch(&r, '[') || !st_u64(&r, &t.data_start) || !st_ch(&r, ',')
                                || !st_u64(&r, &t.data_end) || !st_ch(&r, ']')) {
                                lp_free(t.name); goto fail;
                            }
                            have_off = true;
                        } else {
                            if (!st_skip(&r, &val)) { lp_free(t.name); goto fail; }
                        }
                        if (st_ch(&r, ',')) continue;
                        break;
                    }
                }
                if (!st_ch(&r, '}') || !have_dt || !have_sh || !have_off || t.data_end < t.data_start) {
                    lp_free(t.name); goto fail;
                }
                if (st->count == cap) lp_reserve((void **)&st->tensors, &cap, st->count + 1, sizeof *st->tensors);
                st->tensors[st->count++] = t;
            }
            if (st_ch(&r, ',')) continue;
            break;
        }
    }
    if (!st_ch(&r, '}')) goto fail;
    st_ws(&r);
    if (r.p != r.end) goto fail;

    lp_buf_free(&key);
    lp_buf_free(&val);
    if (st->count > 1) lp_sort(st->tensors, st->count, sizeof *st->tensors, st_tensor_cmp);
    return st;

fail:
    lp_buf_free(&key);
    lp_buf_free(&val);
    lp_safetensors_close(st);
    errno = EINVAL;
    return NULL;
}

void lp_safetensors_close(lp_safetensors *st){
    if (!st) return;
    for (size_t i = 0; i < st->count; i++) lp_free(st->tensors[i].name);
    lp_free(st->tensors);
    lp_free(st->metadata_json);
    lp_free(st);
}
uint64_t lp_safetensors_data_offset(const lp_safetensors *st){ return st ? st->data_offset : 0; }
size_t lp_safetensors_count(const lp_safetensors *st){ return st ? st->count : 0; }
bool lp_safetensors_has_metadata(const lp_safetensors *st){ return st && st->has_metadata; }
const char *lp_safetensors_metadata_json(const lp_safetensors *st, size_t *len){
    if (!st || !st->metadata_json) { if (len) *len = 0; return NULL; }
    if (len) *len = st->metadata_len;
    return st->metadata_json;
}
const char *lp_safetensors_name(const lp_safetensors *st, size_t index, size_t *len){
    if (!st || index >= st->count) { if (len) *len = 0; return NULL; }
    if (len) *len = st->tensors[index].name_len;
    return st->tensors[index].name;
}
lp_dtype lp_safetensors_dtype(const lp_safetensors *st, size_t index){
    return (st && index < st->count) ? st->tensors[index].dtype : LP_DTYPE_UNKNOWN;
}
uint32_t lp_safetensors_dims(const lp_safetensors *st, size_t index, uint64_t *dims_out, uint32_t cap){
    if (!st || index >= st->count) return 0;
    uint32_t n = st->tensors[index].ndim;
    if (dims_out) for (uint32_t i = 0; i < n && i < cap; i++) dims_out[i] = st->tensors[index].dims[i];
    return n;
}
bool lp_safetensors_span(const lp_safetensors *st, size_t index, uint64_t *data_start, uint64_t *data_end){
    if (!st || index >= st->count) return false;
    if (data_start) *data_start = st->tensors[index].data_start;
    if (data_end) *data_end = st->tensors[index].data_end;
    return true;
}
int64_t lp_safetensors_find(const lp_safetensors *st, const char *name){
    if (!st || !name) return -1;
    for (size_t i = 0; i < st->count; i++) if (!strcmp(st->tensors[i].name, name)) return (int64_t)i;
    return -1;
}

/* ---- GGUF v2/v3 zero-copy reader ----------------------------------------- */

typedef struct {
    char *key;
    size_t key_len;
    uint32_t type;
    const uint8_t *val;
    uint32_t arr_type;
    size_t arr_len;
    const uint8_t **arr_elems; /* populated for ARRAY strings or variable-sized elements */
    size_t str_len;
} GgufKV;

typedef struct {
    char *name;
    size_t name_len;
    uint32_t ndim;
    uint64_t dims[LP_TENSOR_MAX_DIMS];
    lp_dtype dtype;
    uint64_t offset;
    size_t bytes;
} GgufTensor;

struct lp_gguf {
    const uint8_t *base;
    size_t len;
    uint32_t version;
    uint32_t alignment;
    uint64_t data_offset;
    GgufKV *kvs;
    size_t kv_count;
    GgufTensor *tensors;
    size_t tensor_count;
};

static bool g_need(size_t pos, size_t len, size_t k){
    return pos <= len && k <= len - pos;
}
static bool g_str(const uint8_t *base, size_t len, size_t *pos, const char **out, size_t *slen){
    if (!g_need(*pos, len, 8)) return false;
    uint64_t n64 = u64_le(base + *pos);
    *pos += 8;
    if (n64 > len - *pos) return false;
    *out = (const char *)(base + *pos);
    *slen = (size_t)n64;
    *pos += (size_t)n64;
    return true;
}
static size_t g_scalar_size(uint32_t t){
    switch (t) {
    case LP_GGUF_TYPE_UINT8: case LP_GGUF_TYPE_INT8: case LP_GGUF_TYPE_BOOL: return 1u;
    case LP_GGUF_TYPE_UINT16: case LP_GGUF_TYPE_INT16: return 2u;
    case LP_GGUF_TYPE_UINT32: case LP_GGUF_TYPE_INT32: case LP_GGUF_TYPE_FLOAT32: return 4u;
    case LP_GGUF_TYPE_UINT64: case LP_GGUF_TYPE_INT64: case LP_GGUF_TYPE_FLOAT64: return 8u;
    default: return 0;
    }
}
static bool g_skip_val(const uint8_t *base, size_t len, size_t *pos, uint32_t t, int depth){
    if (depth > 32) return false;
    size_t sz = g_scalar_size(t);
    if (sz) {
        if (!g_need(*pos, len, sz)) return false;
        *pos += sz;
        return true;
    }
    if (t == LP_GGUF_TYPE_STRING) {
        const char *s; size_t sn;
        return g_str(base, len, pos, &s, &sn);
    }
    if (t == LP_GGUF_TYPE_ARRAY) {
        if (!g_need(*pos, len, 12)) return false;
        uint32_t et = u32_le(base + *pos);
        uint64_t cnt = u64_le(base + *pos + 4);
        *pos += 12;
        size_t esz = g_scalar_size(et);
        if (esz) {
            if (cnt > (len - *pos) / esz) return false;
            *pos += (size_t)cnt * esz;
            return true;
        }
        for (uint64_t i = 0; i < cnt; i++) if (!g_skip_val(base, len, pos, et, depth + 1)) return false;
        return true;
    }
    return false;
}

lp_gguf *lp_gguf_open(const void *bytes, size_t len){
    if (!bytes || len < 24u) { errno = EINVAL; return NULL; }
    const uint8_t *raw = (const uint8_t *)bytes;
    if (u32_le(raw) != 0x46554747u) { errno = EINVAL; return NULL; } /* "GGUF" */
    uint32_t ver = u32_le(raw + 4);
    if (ver < 2u || ver > 3u) { errno = EINVAL; return NULL; }
    uint64_t tc64 = u64_le(raw + 8);
    uint64_t kc64 = u64_le(raw + 16);
    if (tc64 > len / 8u || kc64 > len / 8u) { errno = EINVAL; return NULL; }

    lp_gguf *g = lp_zalloc(sizeof *g);
    g->base = raw;
    g->len = len;
    g->version = ver;
    g->alignment = 32u;
    g->kv_count = (size_t)kc64;
    g->tensor_count = (size_t)tc64;
    if (g->kv_count) g->kvs = lp_zalloc(g->kv_count * sizeof *g->kvs);
    if (g->tensor_count) g->tensors = lp_zalloc(g->tensor_count * sizeof *g->tensors);

    size_t pos = 24u;
    for (size_t i = 0; i < g->kv_count; i++) {
        const char *kptr; size_t klen;
        if (!g_str(raw, len, &pos, &kptr, &klen) || !g_need(pos, len, 4)) goto fail;
        uint32_t vt = u32_le(raw + pos);
        pos += 4;
        GgufKV *kv = &g->kvs[i];
        kv->key = lp_alloc(klen + 1);
        memcpy(kv->key, kptr, klen);
        kv->key[klen] = 0;
        kv->key_len = klen;
        kv->type = vt;
        kv->val = raw + pos;
        if (vt == LP_GGUF_TYPE_STRING) {
            const char *sptr; size_t slen;
            if (!g_str(raw, len, &pos, &sptr, &slen)) goto fail;
            kv->val = (const uint8_t *)sptr;
            kv->str_len = slen;
        } else if (vt == LP_GGUF_TYPE_ARRAY) {
            if (!g_need(pos, len, 12)) goto fail;
            uint32_t et = u32_le(raw + pos);
            uint64_t cnt64 = u64_le(raw + pos + 4);
            pos += 12;
            kv->arr_type = et;
            size_t esz = g_scalar_size(et);
            if (esz) {
                if (cnt64 > (len - pos) / esz) goto fail;
                kv->arr_len = (size_t)cnt64;
                kv->val = raw + pos;
                pos += (size_t)cnt64 * esz;
            } else if (et == LP_GGUF_TYPE_STRING) {
                if (cnt64 > (len - pos) / 8u) goto fail;
                kv->arr_len = (size_t)cnt64;
                kv->arr_elems = kv->arr_len ? lp_alloc(kv->arr_len * sizeof(uint8_t *)) : NULL;
                for (size_t j = 0; j < kv->arr_len; j++) {
                    kv->arr_elems[j] = raw + pos;
                    const char *sptr; size_t slen;
                    if (!g_str(raw, len, &pos, &sptr, &slen)) goto fail;
                }
            } else {
                kv->arr_len = (size_t)cnt64;
                for (uint64_t j = 0; j < cnt64; j++) if (!g_skip_val(raw, len, &pos, et, 1)) goto fail;
            }
        } else {
            if (!g_skip_val(raw, len, &pos, vt, 0)) goto fail;
            if (!strcmp(kv->key, "general.alignment")) {
                uint64_t a = 0;
                if (vt == LP_GGUF_TYPE_UINT32) a = u32_le(kv->val);
                else if (vt == LP_GGUF_TYPE_INT32 && i32_le(kv->val) > 0) a = (uint64_t)i32_le(kv->val);
                else if (vt == LP_GGUF_TYPE_UINT64) a = u64_le(kv->val);
                if (a >= 1 && a <= 65536u && (a & (a - 1u)) == 0) g->alignment = (uint32_t)a;
            }
        }
    }

    for (size_t i = 0; i < g->tensor_count; i++) {
        const char *nptr; size_t nlen;
        if (!g_str(raw, len, &pos, &nptr, &nlen) || !g_need(pos, len, 4)) goto fail;
        uint32_t ndim = u32_le(raw + pos);
        pos += 4;
        if (ndim > LP_TENSOR_MAX_DIMS || !g_need(pos, len, (size_t)ndim * 8u + 12u)) goto fail;
        GgufTensor *t = &g->tensors[i];
        t->name = lp_alloc(nlen + 1);
        memcpy(t->name, nptr, nlen);
        t->name[nlen] = 0;
        t->name_len = nlen;
        t->ndim = ndim;
        size_t nel = 1u;
        for (uint32_t d = 0; d < ndim; d++) {
            t->dims[d] = u64_le(raw + pos);
            pos += 8;
            if (t->dims[d] > SIZE_MAX / (nel ? nel : 1u)) nel = 0;
            else nel *= (size_t)t->dims[d];
        }
        uint32_t gtype = u32_le(raw + pos);
        pos += 4;
        t->dtype = lp_dtype_from_ggml(gtype);
        t->offset = u64_le(raw + pos);
        pos += 8;
        t->bytes = (ndim == 0) ? 0 : lp_tensor_nbytes(t->dtype, nel);
    }

    size_t align = g->alignment;
    g->data_offset = (uint64_t)((pos + align - 1u) & ~(align - 1u));
    return g;

fail:
    lp_gguf_close(g);
    errno = EINVAL;
    return NULL;
}

void lp_gguf_close(lp_gguf *g){
    if (!g) return;
    for (size_t i = 0; i < g->kv_count; i++) {
        lp_free(g->kvs[i].key);
        lp_free(g->kvs[i].arr_elems);
    }
    for (size_t i = 0; i < g->tensor_count; i++) lp_free(g->tensors[i].name);
    lp_free(g->kvs);
    lp_free(g->tensors);
    lp_free(g);
}

uint32_t lp_gguf_version(const lp_gguf *g){ return g ? g->version : 0; }
uint32_t lp_gguf_alignment(const lp_gguf *g){ return g ? g->alignment : 0; }
uint64_t lp_gguf_data_offset(const lp_gguf *g){ return g ? g->data_offset : 0; }
size_t lp_gguf_kv_count(const lp_gguf *g){ return g ? g->kv_count : 0; }

int64_t lp_gguf_find_kv(const lp_gguf *g, const char *key){
    if (!g || !key) return -1;
    for (size_t i = 0; i < g->kv_count; i++) if (!strcmp(g->kvs[i].key, key)) return (int64_t)i;
    return -1;
}
const char *lp_gguf_kv_key(const lp_gguf *g, size_t index, size_t *len){
    if (!g || index >= g->kv_count) { if (len) *len = 0; return NULL; }
    if (len) *len = g->kvs[index].key_len;
    return g->kvs[index].key;
}
uint32_t lp_gguf_kv_type(const lp_gguf *g, size_t index){
    return (g && index < g->kv_count) ? g->kvs[index].type : 0xFFFFFFFFu;
}
bool lp_gguf_kv_u64(const lp_gguf *g, size_t index, uint64_t *out){
    if (!g || !out || index >= g->kv_count) return false;
    const GgufKV *kv = &g->kvs[index];
    switch (kv->type) {
    case LP_GGUF_TYPE_UINT8: *out = kv->val[0]; return true;
    case LP_GGUF_TYPE_UINT16: *out = u16_le(kv->val); return true;
    case LP_GGUF_TYPE_UINT32: *out = u32_le(kv->val); return true;
    case LP_GGUF_TYPE_UINT64: *out = u64_le(kv->val); return true;
    case LP_GGUF_TYPE_INT8: if ((int8_t)kv->val[0] < 0) return false; *out = (uint64_t)(int8_t)kv->val[0]; return true;
    case LP_GGUF_TYPE_INT16: if (i16_le(kv->val) < 0) return false; *out = (uint64_t)i16_le(kv->val); return true;
    case LP_GGUF_TYPE_INT32: if (i32_le(kv->val) < 0) return false; *out = (uint64_t)i32_le(kv->val); return true;
    case LP_GGUF_TYPE_INT64: if (i64_le(kv->val) < 0) return false; *out = (uint64_t)i64_le(kv->val); return true;
    default: return false;
    }
}
bool lp_gguf_kv_i64(const lp_gguf *g, size_t index, int64_t *out){
    if (!g || !out || index >= g->kv_count) return false;
    const GgufKV *kv = &g->kvs[index];
    switch (kv->type) {
    case LP_GGUF_TYPE_INT8: *out = (int8_t)kv->val[0]; return true;
    case LP_GGUF_TYPE_INT16: *out = i16_le(kv->val); return true;
    case LP_GGUF_TYPE_INT32: *out = i32_le(kv->val); return true;
    case LP_GGUF_TYPE_INT64: *out = i64_le(kv->val); return true;
    case LP_GGUF_TYPE_UINT8: *out = kv->val[0]; return true;
    case LP_GGUF_TYPE_UINT16: *out = u16_le(kv->val); return true;
    case LP_GGUF_TYPE_UINT32: *out = (int64_t)u32_le(kv->val); return true;
    case LP_GGUF_TYPE_UINT64: {
        uint64_t u = u64_le(kv->val);
        if (u > (uint64_t)INT64_MAX) return false;
        *out = (int64_t)u;
        return true;
    }
    default: return false;
    }
}
bool lp_gguf_kv_f64(const lp_gguf *g, size_t index, double *out){
    if (!g || !out || index >= g->kv_count) return false;
    const GgufKV *kv = &g->kvs[index];
    if (kv->type == LP_GGUF_TYPE_FLOAT32) { *out = (double)f32_le(kv->val); return true; }
    if (kv->type == LP_GGUF_TYPE_FLOAT64) { *out = f64_le(kv->val); return true; }
    return false;
}
bool lp_gguf_kv_bool(const lp_gguf *g, size_t index, bool *out){
    if (!g || !out || index >= g->kv_count) return false;
    if (g->kvs[index].type != LP_GGUF_TYPE_BOOL) return false;
    *out = g->kvs[index].val[0] != 0;
    return true;
}
const char *lp_gguf_kv_str(const lp_gguf *g, size_t index, size_t *len){
    if (!g || index >= g->kv_count || g->kvs[index].type != LP_GGUF_TYPE_STRING) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = g->kvs[index].str_len;
    return (const char *)g->kvs[index].val;
}
size_t lp_gguf_kv_array_len(const lp_gguf *g, size_t index){
    if (!g || index >= g->kv_count || g->kvs[index].type != LP_GGUF_TYPE_ARRAY) return 0;
    return g->kvs[index].arr_len;
}
uint32_t lp_gguf_kv_array_type(const lp_gguf *g, size_t index){
    if (!g || index >= g->kv_count || g->kvs[index].type != LP_GGUF_TYPE_ARRAY) return 0xFFFFFFFFu;
    return g->kvs[index].arr_type;
}
const char *lp_gguf_kv_array_str(const lp_gguf *g, size_t index, size_t elem, size_t *len){
    if (!g || index >= g->kv_count) { if (len) *len = 0; return NULL; }
    const GgufKV *kv = &g->kvs[index];
    if (kv->type != LP_GGUF_TYPE_ARRAY || kv->arr_type != LP_GGUF_TYPE_STRING
        || !kv->arr_elems || elem >= kv->arr_len) {
        if (len) *len = 0;
        return NULL;
    }
    const uint8_t *p = kv->arr_elems[elem];
    if (len) *len = (size_t)u64_le(p);
    return (const char *)(p + 8);
}
bool lp_gguf_kv_array_f32(const lp_gguf *g, size_t index, size_t elem, float *out){
    if (!g || !out || index >= g->kv_count) return false;
    const GgufKV *kv = &g->kvs[index];
    if (kv->type != LP_GGUF_TYPE_ARRAY || kv->arr_type != LP_GGUF_TYPE_FLOAT32 || elem >= kv->arr_len) return false;
    *out = f32_le(kv->val + elem * 4u);
    return true;
}
bool lp_gguf_kv_array_i32(const lp_gguf *g, size_t index, size_t elem, int32_t *out){
    if (!g || !out || index >= g->kv_count) return false;
    const GgufKV *kv = &g->kvs[index];
    if (kv->type != LP_GGUF_TYPE_ARRAY || kv->arr_type != LP_GGUF_TYPE_INT32 || elem >= kv->arr_len) return false;
    *out = i32_le(kv->val + elem * 4u);
    return true;
}

size_t lp_gguf_tensor_count(const lp_gguf *g){ return g ? g->tensor_count : 0; }
int64_t lp_gguf_find_tensor(const lp_gguf *g, const char *name){
    if (!g || !name) return -1;
    for (size_t i = 0; i < g->tensor_count; i++) if (!strcmp(g->tensors[i].name, name)) return (int64_t)i;
    return -1;
}
const char *lp_gguf_tensor_name(const lp_gguf *g, size_t index, size_t *len){
    if (!g || index >= g->tensor_count) { if (len) *len = 0; return NULL; }
    if (len) *len = g->tensors[index].name_len;
    return g->tensors[index].name;
}
lp_dtype lp_gguf_tensor_dtype(const lp_gguf *g, size_t index){
    return (g && index < g->tensor_count) ? g->tensors[index].dtype : LP_DTYPE_UNKNOWN;
}
uint32_t lp_gguf_tensor_dims(const lp_gguf *g, size_t index, uint64_t *dims_out, uint32_t cap){
    if (!g || index >= g->tensor_count) return 0;
    uint32_t n = g->tensors[index].ndim;
    if (dims_out) for (uint32_t i = 0; i < n && i < cap; i++) dims_out[i] = g->tensors[index].dims[i];
    return n;
}
uint64_t lp_gguf_tensor_offset(const lp_gguf *g, size_t index){
    return (g && index < g->tensor_count) ? g->tensors[index].offset : 0;
}
size_t lp_gguf_tensor_bytes(const lp_gguf *g, size_t index){
    return (g && index < g->tensor_count) ? g->tensors[index].bytes : 0;
}

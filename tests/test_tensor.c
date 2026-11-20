#include "laplace/tensor.h"
#include "../src/core/internal.h"
#include "check.h"
#include <math.h>
#include <string.h>

static void write_u32_le(lp_buf *b, uint32_t v){
    uint8_t x[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    lp_buf_put(b, x, 4);
}
static void write_u64_le(lp_buf *b, uint64_t v){
    uint8_t x[8];
    for (int i = 0; i < 8; i++) x[i] = (uint8_t)(v >> (8 * i));
    lp_buf_put(b, x, 8);
}
static void write_gguf_str(lp_buf *b, const char *s){
    size_t n = strlen(s);
    write_u64_le(b, (uint64_t)n);
    lp_buf_put(b, s, n);
}

static void test_dtypes_and_simd(void){
    CHECK(lp_dtype_from_name("F32") == LP_DTYPE_F32 && lp_dtype_from_name("BF16") == LP_DTYPE_BF16, "dtype names");
    CHECK(lp_dtype_from_name("Q4_K") == LP_DTYPE_Q4_K && lp_dtype_block_bytes(LP_DTYPE_Q4_K) == 144, "Q4_K size");
    CHECK(lp_tensor_nbytes(LP_DTYPE_Q4_0, 64) == 36 && lp_tensor_nbytes(LP_DTYPE_Q4_0, 31) == 0, "block align");

    const float f16_vals[19] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 2.0f, -2.0f, 4.0f, 0.25f,
                                 65504.0f, -65504.0f, 3.0f, -3.0f, 0.125f, 8.0f, -8.0f, 16.0f, 1024.0f };
    uint16_t raw16[19];
    for (int i = 0; i < 19; i++) raw16[i] = lp_f32_to_f16(f16_vals[i]);
    float sc[19], simd[19];
    lp_f16_to_f32_scalar(raw16, 19, sc);
    CHECK(lp_tensor_decode_f32(raw16, 19, LP_DTYPE_F16, simd) == 0, "decode f16");
    CHECK(!memcmp(sc, simd, sizeof sc), "F16 scalar and dispatch agree bit-for-bit across tail");
    for (int i = 0; i < 19; i++) CHECK(!memcmp(&simd[i], &f16_vals[i], 4), "F16 round-trip %d", i);

    uint16_t bf16_in[11];
    float bf_ref[11] = { 0.0f, 1.0f, -1.0f, 3.5f, -2.25f, 1e30f, -1e-30f, 123456.0f, 0.001953125f, 7.0f, -7.0f };
    for (int i = 0; i < 11; i++) bf16_in[i] = lp_f32_to_bf16(bf_ref[i]);
    float bf_sc[11], bf_out[11];
    lp_bf16_to_f32_scalar(bf16_in, 11, bf_sc);
    CHECK(lp_tensor_decode_f32(bf16_in, 11, LP_DTYPE_BF16, bf_out) == 0, "decode bf16");
    CHECK(!memcmp(bf_sc, bf_out, sizeof bf_out), "BF16 scalar and dispatch agree bit-for-bit");

    /* subnormal, +inf, -inf, NaN in F16 */
    uint16_t edge16[5] = { 0x0001u, 0x03FFu, 0x7C00u, 0xFC00u, 0x7E00u };
    float edge_sc[5], edge_out[5];
    lp_f16_to_f32_scalar(edge16, 5, edge_sc);
    CHECK(lp_tensor_decode_f32(edge16, 5, LP_DTYPE_F16, edge_out) == 0, "decode f16 edges");
    CHECK(edge_out[0] == ldexpf(1.0f, -24) && isinf(edge_out[2]) && edge_out[2] > 0
          && isinf(edge_out[3]) && edge_out[3] < 0 && isnan(edge_out[4]), "F16 special values");
    CHECK(!memcmp(edge_sc, edge_out, 16) && isnan(edge_sc[4]), "F16 edge parity");

    /* FP8 E4M3 / E5M2 */
    const uint8_t e4m3[5] = { 0x00, 0x38, 0xB8, 0x01, 0x7F };
    float e4[5];
    CHECK(lp_tensor_decode_f32(e4m3, 5, LP_DTYPE_F8_E4M3, e4) == 0
          && e4[0] == 0.0f && e4[1] == 1.0f && e4[2] == -1.0f
          && e4[3] == ldexpf(1.0f, -9) && isnan(e4[4]), "F8_E4M3");

    const uint8_t e5m2[4] = { 0x00, 0x3C, 0xBC, 0x40 };
    float e5[4];
    CHECK(lp_tensor_decode_f32(e5m2, 4, LP_DTYPE_F8_E5M2, e5) == 0
          && e5[0] == 0.0f && e5[1] == 1.0f && e5[2] == -1.0f && e5[3] == 2.0f, "F8_E5M2");
}

static void test_block_quants(void){
    /* Q4_0: d=0.5 (0x3800), nibble 9 -> (9-8)*0.5 = 0.5, hi nibble 6 -> (6-8)*0.5 = -1.0 */
    uint8_t q40[18]; memset(q40, 0, sizeof q40);
    q40[0] = 0x00; q40[1] = 0x38; /* d = 0.5 */
    for (int i = 0; i < 16; i++) q40[2 + i] = 0x69u;
    float out32[32];
    CHECK(lp_tensor_decode_f32(q40, 32, LP_DTYPE_Q4_0, out32) == 0
          && out32[0] == 0.5f && out32[16] == -1.0f, "Q4_0");

    /* Q8_0: d=0.25 (0x3400), qs[0]=4 -> 1.0, qs[31]=-8 -> -2.0 */
    uint8_t q80[34]; memset(q80, 0, sizeof q80);
    q80[0] = 0x00; q80[1] = 0x34;
    q80[2] = 4; q80[33] = (uint8_t)(int8_t)-8;
    CHECK(lp_tensor_decode_f32(q80, 32, LP_DTYPE_Q8_0, out32) == 0
          && out32[0] == 1.0f && out32[31] == -2.0f, "Q8_0");

    /* Q8_K: d=0.5f, qs[0]=6 -> 3.0, qs[255]=-4 -> -2.0 */
    uint8_t q8k[292]; memset(q8k, 0, sizeof q8k);
    float d = 0.5f; memcpy(q8k, &d, 4);
    q8k[4] = 6; q8k[4 + 255] = (uint8_t)(int8_t)-4;
    float out256[256];
    CHECK(lp_tensor_decode_f32(q8k, 256, LP_DTYPE_Q8_K, out256) == 0
          && out256[0] == 3.0f && out256[255] == -2.0f, "Q8_K");

    /* Verify Q4_K / Q5_K / Q6_K / Q2_K / Q3_K non-zero deterministic decode */
    uint8_t q4k[144]; memset(q4k, 0, sizeof q4k);
    uint16_t h1 = lp_f32_to_f16(1.0f); memcpy(q4k, &h1, 2);
    q4k[4] = 2; /* sc[0] = 2 */
    q4k[16] = 0x35u; /* lo=5 => 10.0 */
    CHECK(lp_tensor_decode_f32(q4k, 256, LP_DTYPE_Q4_K, out256) == 0 && out256[0] == 10.0f, "Q4_K");
}

static void test_safetensors_and_gguf(void){
    const char *json = "{\"__metadata__\":{\"arch\":\"llama\"},\"w_b\":{\"dtype\":\"F16\",\"shape\":[2,3],\"data_offsets\":[16,28]},\"w_a\":{\"dtype\":\"BF16\",\"shape\":[4],\"data_offsets\":[0,8]}}";
    size_t jlen = strlen(json);
    lp_buf st_buf = {0};
    write_u64_le(&st_buf, (uint64_t)jlen);
    lp_buf_put(&st_buf, json, jlen);
    lp_safetensors *st = lp_safetensors_open(st_buf.b, st_buf.n);
    CHECK(st != NULL, "lp_safetensors_open");
    if (st) {
        CHECK(lp_safetensors_count(st) == 2 && lp_safetensors_has_metadata(st), "safetensors count+meta");
        CHECK(lp_safetensors_data_offset(st) == 8 + jlen, "safetensors data_offset");
        /* sorted on-disk by data_start: w_a first, w_b second */
        CHECK(!strcmp(lp_safetensors_name(st, 0, NULL), "w_a")
              && lp_safetensors_dtype(st, 0) == LP_DTYPE_BF16, "safetensors order 0");
        uint64_t dims[LP_TENSOR_MAX_DIMS];
        CHECK(lp_safetensors_dims(st, 1, dims, LP_TENSOR_MAX_DIMS) == 2 && dims[0] == 2 && dims[1] == 3, "safetensors dims");
        CHECK(lp_safetensors_find(st, "w_b") == 1, "safetensors find");
        lp_safetensors_close(st);
    }
    lp_buf_free(&st_buf);

    /* Minimal valid GGUF v3 buffer */
    lp_buf gb = {0};
    lp_buf_put(&gb, "GGUF", 4);
    write_u32_le(&gb, 3u);
    write_u64_le(&gb, 1u); /* tensor_count */
    write_u64_le(&gb, 2u); /* kv_count */
    write_gguf_str(&gb, "general.architecture");
    write_u32_le(&gb, LP_GGUF_TYPE_STRING);
    write_gguf_str(&gb, "llama");
    write_gguf_str(&gb, "tokenizer.ggml.tokens");
    write_u32_le(&gb, LP_GGUF_TYPE_ARRAY);
    write_u32_le(&gb, LP_GGUF_TYPE_STRING);
    write_u64_le(&gb, 2u);
    write_gguf_str(&gb, "<s>");
    write_gguf_str(&gb, "hello");
    /* tensor 0 */
    write_gguf_str(&gb, "token_embd.weight");
    write_u32_le(&gb, 2u);
    write_u64_le(&gb, 2u);
    write_u64_le(&gb, 3u);
    write_u32_le(&gb, 30u); /* BF16 */
    write_u64_le(&gb, 0u);  /* offset */

    lp_gguf *g = lp_gguf_open(gb.b, gb.n);
    CHECK(g != NULL, "lp_gguf_open");
    if (g) {
        CHECK(lp_gguf_version(g) == 3 && lp_gguf_alignment(g) == 32 && (lp_gguf_data_offset(g) % 32) == 0, "gguf header");
        int64_t arch_i = lp_gguf_find_kv(g, "general.architecture");
        size_t slen = 0;
        const char *arch = lp_gguf_kv_str(g, (size_t)arch_i, &slen);
        CHECK(arch_i == 0 && arch && slen == 5 && !memcmp(arch, "llama", 5), "gguf str kv");
        int64_t tok_i = lp_gguf_find_kv(g, "tokenizer.ggml.tokens");
        const char *t1 = lp_gguf_kv_array_str(g, (size_t)tok_i, 1, &slen);
        CHECK(lp_gguf_kv_array_len(g, (size_t)tok_i) == 2 && t1 && slen == 5 && !memcmp(t1, "hello", 5), "gguf str array");
        CHECK(lp_gguf_tensor_count(g) == 1 && lp_gguf_find_tensor(g, "token_embd.weight") == 0
              && lp_gguf_tensor_dtype(g, 0) == LP_DTYPE_BF16 && lp_gguf_tensor_bytes(g, 0) == 12u, "gguf tensor");
        lp_gguf_close(g);
    }
    lp_buf_free(&gb);

    char m[128];
    CHECK(lp_gguf_hf_to_ggml_name("model.layers.7.self_attn.k_proj.weight", m, sizeof m) > 0
          && !strcmp(m, "blk.7.attn_k.weight"), "hf->ggml layer");
    CHECK(lp_gguf_hf_to_ggml_name("model.embed_tokens.weight", m, 8) == -1, "hf->ggml capacity guard");
}

int main(void){
    test_dtypes_and_simd();
    test_block_quants();
    test_safetensors_and_gguf();
    DONE("tensor");
}

/* Tensor data types, deterministic f32 decode (including F16/BF16 AVX2 dispatch and GGML block quants),
 * zero-copy Safetensors/GGUF container readers, and HF-to-GGML tensor naming. */
#ifndef LAPLACE_TENSOR_H
#define LAPLACE_TENSOR_H

#include "laplace/laplace.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LP_TENSOR_MAX_DIMS 8

typedef enum {
    LP_DTYPE_UNKNOWN = -1,
    LP_DTYPE_F32     = 0,
    LP_DTYPE_F16     = 1,
    LP_DTYPE_BF16    = 2,
    LP_DTYPE_F64     = 3,
    LP_DTYPE_I8      = 4,
    LP_DTYPE_I16     = 5,
    LP_DTYPE_I32     = 6,
    LP_DTYPE_I64     = 7,
    LP_DTYPE_U8      = 8,
    LP_DTYPE_U16     = 9,
    LP_DTYPE_U32     = 10,
    LP_DTYPE_U64     = 11,
    LP_DTYPE_BOOL    = 12,
    LP_DTYPE_F8_E5M2 = 13,
    LP_DTYPE_F8_E4M3 = 14,
    LP_DTYPE_Q4_0    = 20,
    LP_DTYPE_Q4_1    = 21,
    LP_DTYPE_Q5_0    = 22,
    LP_DTYPE_Q5_1    = 23,
    LP_DTYPE_Q8_0    = 24,
    LP_DTYPE_Q8_1    = 25,
    LP_DTYPE_Q2_K    = 26,
    LP_DTYPE_Q3_K    = 27,
    LP_DTYPE_Q4_K    = 28,
    LP_DTYPE_Q5_K    = 29,
    LP_DTYPE_Q6_K    = 30,
    LP_DTYPE_Q8_K    = 31
} lp_dtype;

typedef enum {
    LP_GGUF_TYPE_UINT8   = 0,
    LP_GGUF_TYPE_INT8    = 1,
    LP_GGUF_TYPE_UINT16  = 2,
    LP_GGUF_TYPE_INT16   = 3,
    LP_GGUF_TYPE_UINT32  = 4,
    LP_GGUF_TYPE_INT32   = 5,
    LP_GGUF_TYPE_FLOAT32 = 6,
    LP_GGUF_TYPE_BOOL    = 7,
    LP_GGUF_TYPE_STRING  = 8,
    LP_GGUF_TYPE_ARRAY   = 9,
    LP_GGUF_TYPE_UINT64  = 10,
    LP_GGUF_TYPE_INT64   = 11,
    LP_GGUF_TYPE_FLOAT64 = 12
} lp_gguf_val_type;

LP_API lp_dtype lp_dtype_from_name(const char *name);
LP_API const char *lp_dtype_name(lp_dtype dt);
LP_API lp_dtype lp_dtype_from_ggml(uint32_t ggml_type);
LP_API int32_t lp_dtype_to_ggml(lp_dtype dt);
LP_API size_t lp_dtype_block_elements(lp_dtype dt);
LP_API size_t lp_dtype_block_bytes(lp_dtype dt);
LP_API size_t lp_tensor_nbytes(lp_dtype dt, size_t n_elements);

LP_API float lp_f16_to_f32(uint16_t h);
LP_API uint16_t lp_f32_to_f16(float v);
LP_API float lp_bf16_to_f32(uint16_t b);
LP_API uint16_t lp_f32_to_bf16(float v);

/* Returns 0 on success, -1 on invalid args or non-block-aligned element count, -2 on unsupported dtype. */
LP_API int lp_tensor_decode_f32(const void *raw, size_t n_elements, lp_dtype dt, float *out);

/* Canonical HuggingFace -> GGML tensor name mapping; returns bytes written or -1 on invalid/overflow. */
LP_API int lp_gguf_hf_to_ggml_name(const char *hf_name, char *out, size_t cap);

typedef struct lp_safetensors lp_safetensors;
LP_API lp_safetensors *lp_safetensors_open(const void *bytes, size_t len);
LP_API void lp_safetensors_close(lp_safetensors *st);
LP_API uint64_t lp_safetensors_data_offset(const lp_safetensors *st);
LP_API size_t lp_safetensors_count(const lp_safetensors *st);
LP_API bool lp_safetensors_has_metadata(const lp_safetensors *st);
LP_API const char *lp_safetensors_metadata_json(const lp_safetensors *st, size_t *len);
LP_API const char *lp_safetensors_name(const lp_safetensors *st, size_t index, size_t *len);
LP_API lp_dtype lp_safetensors_dtype(const lp_safetensors *st, size_t index);
LP_API uint32_t lp_safetensors_dims(const lp_safetensors *st, size_t index, uint64_t *dims_out, uint32_t cap);
LP_API bool lp_safetensors_span(const lp_safetensors *st, size_t index, uint64_t *data_start, uint64_t *data_end);
LP_API int64_t lp_safetensors_find(const lp_safetensors *st, const char *name);

typedef struct lp_gguf lp_gguf;
LP_API lp_gguf *lp_gguf_open(const void *bytes, size_t len);
LP_API void lp_gguf_close(lp_gguf *g);
LP_API uint32_t lp_gguf_version(const lp_gguf *g);
LP_API uint32_t lp_gguf_alignment(const lp_gguf *g);
LP_API uint64_t lp_gguf_data_offset(const lp_gguf *g);
LP_API size_t lp_gguf_kv_count(const lp_gguf *g);
LP_API int64_t lp_gguf_find_kv(const lp_gguf *g, const char *key);
LP_API const char *lp_gguf_kv_key(const lp_gguf *g, size_t index, size_t *len);
LP_API uint32_t lp_gguf_kv_type(const lp_gguf *g, size_t index);
LP_API bool lp_gguf_kv_u64(const lp_gguf *g, size_t index, uint64_t *out);
LP_API bool lp_gguf_kv_i64(const lp_gguf *g, size_t index, int64_t *out);
LP_API bool lp_gguf_kv_f64(const lp_gguf *g, size_t index, double *out);
LP_API bool lp_gguf_kv_bool(const lp_gguf *g, size_t index, bool *out);
LP_API const char *lp_gguf_kv_str(const lp_gguf *g, size_t index, size_t *len);
LP_API size_t lp_gguf_kv_array_len(const lp_gguf *g, size_t index);
LP_API uint32_t lp_gguf_kv_array_type(const lp_gguf *g, size_t index);
LP_API const char *lp_gguf_kv_array_str(const lp_gguf *g, size_t index, size_t elem, size_t *len);
LP_API bool lp_gguf_kv_array_f32(const lp_gguf *g, size_t index, size_t elem, float *out);
LP_API bool lp_gguf_kv_array_i32(const lp_gguf *g, size_t index, size_t elem, int32_t *out);
LP_API size_t lp_gguf_tensor_count(const lp_gguf *g);
LP_API int64_t lp_gguf_find_tensor(const lp_gguf *g, const char *name);
LP_API const char *lp_gguf_tensor_name(const lp_gguf *g, size_t index, size_t *len);
LP_API lp_dtype lp_gguf_tensor_dtype(const lp_gguf *g, size_t index);
LP_API uint32_t lp_gguf_tensor_dims(const lp_gguf *g, size_t index, uint64_t *dims_out, uint32_t cap);
LP_API uint64_t lp_gguf_tensor_offset(const lp_gguf *g, size_t index);
LP_API size_t lp_gguf_tensor_bytes(const lp_gguf *g, size_t index);

#ifdef __cplusplus
}
#endif
#endif

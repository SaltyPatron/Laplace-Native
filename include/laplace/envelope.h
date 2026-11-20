/* Compute envelopes and exact work-unit counters (#1709): deterministic budgets and accounting across
 * Native, Postgres, Engine, and .NET without wall-clock guesswork or silent truncation. */
#ifndef LAPLACE_ENVELOPE_H
#define LAPLACE_ENVELOPE_H

#include "laplace/laplace.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LP_ENVELOPE_FAST = 0,
    LP_ENVELOPE_STANDARD = 1,
    LP_ENVELOPE_DEEP = 2,
    LP_ENVELOPE_UNLIMITED = 3
} lp_envelope_preset_id;

typedef enum {
    LP_LIMIT_NONE = 0,
    LP_LIMIT_WALL_US = 1,
    LP_LIMIT_DB_PROBES = 2,
    LP_LIMIT_ROWS_SCANNED = 3,
    LP_LIMIT_BYTES_ALLOC = 4,
    LP_LIMIT_HOPS = 5,
    LP_LIMIT_TOKENS_OUT = 6
} lp_limit_reason;

/* Fixed-width resource limits; 0 means unbounded for that dimension. */
typedef struct {
    uint64_t max_wall_us;
    uint64_t max_db_probes;
    uint64_t max_rows_scanned;
    uint64_t max_bytes_alloc;
    uint32_t max_hops;
    uint32_t max_tokens_out;
    int32_t  preset;
    int32_t  reserved;
} lp_envelope;

/* Exact work counters accumulated over an execution span. */
typedef struct {
    uint64_t wall_us;
    uint64_t db_probes;
    uint64_t rows_scanned;
    uint64_t bytes_alloc;
    uint32_t hops;
    uint32_t tokens_out;
    uint32_t truncated;         /* 0 or 1 */
    int32_t  exhausted_reason;  /* lp_limit_reason */
} lp_work_counters;

/* Microsecond cost weights for estimating or billing deterministic work units. */
typedef struct {
    double us_per_db_probe;
    double us_per_row;
    double us_per_kb;
    double us_per_hop;
    double us_per_token;
} lp_work_weights;

LP_API lp_envelope lp_envelope_preset(int32_t preset);
LP_API lp_work_weights lp_work_weights_default(void);
LP_API uint64_t lp_envelope_estimate_us(const lp_envelope *env, const lp_work_counters *planned,
                                        const lp_work_weights *w);
LP_API uint64_t lp_work_actual_us(const lp_work_counters *c, const lp_work_weights *w);
LP_API bool lp_work_check(const lp_envelope *env, const lp_work_counters *c);
LP_API int32_t lp_work_exceeded(const lp_envelope *env, const lp_work_counters *c);
LP_API void lp_work_merge(lp_work_counters *dst, const lp_work_counters *src);

#ifdef __cplusplus
}
#endif
#endif

#include "laplace/envelope.h"
#include <math.h>
#include <string.h>

static uint64_t sat_add64(uint64_t a, uint64_t b){
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}
static uint32_t sat_add32(uint32_t a, uint32_t b){
    return a > UINT32_MAX - b ? UINT32_MAX : a + b;
}

lp_envelope lp_envelope_preset(int32_t preset){
    switch (preset) {
    case LP_ENVELOPE_FAST:
        return (lp_envelope){
            .max_wall_us = 100000ull,
            .max_db_probes = 8ull,
            .max_rows_scanned = 4096ull,
            .max_bytes_alloc = 16ull * 1024ull * 1024ull,
            .max_hops = 2u,
            .max_tokens_out = 256u,
            .preset = LP_ENVELOPE_FAST
        };
    case LP_ENVELOPE_STANDARD:
        return (lp_envelope){
            .max_wall_us = 1000000ull,
            .max_db_probes = 32ull,
            .max_rows_scanned = 65536ull,
            .max_bytes_alloc = 64ull * 1024ull * 1024ull,
            .max_hops = 4u,
            .max_tokens_out = 1024u,
            .preset = LP_ENVELOPE_STANDARD
        };
    case LP_ENVELOPE_DEEP:
        return (lp_envelope){
            .max_wall_us = 2000000ull,
            .max_db_probes = 256ull,
            .max_rows_scanned = 1048576ull,
            .max_bytes_alloc = 512ull * 1024ull * 1024ull,
            .max_hops = 8u,
            .max_tokens_out = 8192u,
            .preset = LP_ENVELOPE_DEEP
        };
    case LP_ENVELOPE_UNLIMITED:
    default:
        return (lp_envelope){ .preset = LP_ENVELOPE_UNLIMITED };
    }
}

lp_work_weights lp_work_weights_default(void){
    return (lp_work_weights){
        .us_per_db_probe = 150.0,
        .us_per_row = 0.25,
        .us_per_kb = 0.5,
        .us_per_hop = 200.0,
        .us_per_token = 5.0
    };
}

static uint64_t weighted_us(const lp_work_counters *c, const lp_work_weights *w){
    if (!c) return 0;
    lp_work_weights def = lp_work_weights_default();
    const lp_work_weights *kw = w ? w : &def;
    double kb = (double)c->bytes_alloc / 1024.0;
    double total = (double)c->db_probes * kw->us_per_db_probe
                 + (double)c->rows_scanned * kw->us_per_row
                 + kb * kw->us_per_kb
                 + (double)c->hops * kw->us_per_hop
                 + (double)c->tokens_out * kw->us_per_token;
    if (!isfinite(total) || total <= 0.0) return 0;
    if (total >= (double)UINT64_MAX) return UINT64_MAX;
    return (uint64_t)total;
}

uint64_t lp_envelope_estimate_us(const lp_envelope *env, const lp_work_counters *planned,
                                 const lp_work_weights *w){
    if (planned) {
        uint64_t est = weighted_us(planned, w);
        if (env && env->max_wall_us && est > env->max_wall_us) return env->max_wall_us;
        return est;
    }
    if (!env) return 0;
    lp_work_counters cap = {
        .wall_us = env->max_wall_us,
        .db_probes = env->max_db_probes,
        .rows_scanned = env->max_rows_scanned,
        .bytes_alloc = env->max_bytes_alloc,
        .hops = env->max_hops,
        .tokens_out = env->max_tokens_out
    };
    uint64_t est = weighted_us(&cap, w);
    if (env->max_wall_us && (!est || est > env->max_wall_us)) return env->max_wall_us;
    return est;
}

uint64_t lp_work_actual_us(const lp_work_counters *c, const lp_work_weights *w){
    if (!c) return 0;
    if (c->wall_us) return c->wall_us;
    return weighted_us(c, w);
}

int32_t lp_work_exceeded(const lp_envelope *env, const lp_work_counters *c){
    if (!env || !c) return LP_LIMIT_NONE;
    if (env->max_wall_us && c->wall_us > env->max_wall_us) return LP_LIMIT_WALL_US;
    if (env->max_db_probes && c->db_probes > env->max_db_probes) return LP_LIMIT_DB_PROBES;
    if (env->max_rows_scanned && c->rows_scanned > env->max_rows_scanned) return LP_LIMIT_ROWS_SCANNED;
    if (env->max_bytes_alloc && c->bytes_alloc > env->max_bytes_alloc) return LP_LIMIT_BYTES_ALLOC;
    if (env->max_hops && c->hops > env->max_hops) return LP_LIMIT_HOPS;
    if (env->max_tokens_out && c->tokens_out > env->max_tokens_out) return LP_LIMIT_TOKENS_OUT;
    return LP_LIMIT_NONE;
}

bool lp_work_check(const lp_envelope *env, const lp_work_counters *c){
    return lp_work_exceeded(env, c) == LP_LIMIT_NONE;
}

void lp_work_merge(lp_work_counters *dst, const lp_work_counters *src){
    if (!dst || !src) return;
    dst->wall_us = sat_add64(dst->wall_us, src->wall_us);
    dst->db_probes = sat_add64(dst->db_probes, src->db_probes);
    dst->rows_scanned = sat_add64(dst->rows_scanned, src->rows_scanned);
    dst->bytes_alloc = sat_add64(dst->bytes_alloc, src->bytes_alloc);
    dst->hops = sat_add32(dst->hops, src->hops);
    dst->tokens_out = sat_add32(dst->tokens_out, src->tokens_out);
    if (src->truncated) dst->truncated = 1u;
    if (!dst->exhausted_reason && src->exhausted_reason) dst->exhausted_reason = src->exhausted_reason;
}

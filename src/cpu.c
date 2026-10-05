/* CPU feature detection and the dispatch level. Every ISA the CPU has is used; LAPLACE_ISA may lower the level for
 * testing (scalar, sse2, avx2, avxvnni, avx512, avx512vnni), never raise it past what the CPU supports. avx2 is
 * x86-64-v3 alone (hart-server's Broadwell-E); avx512 is x86-64-v4 without either VNNI (Skylake-SP). */
#include "laplace/laplace.h"
#include "internal.h"
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#include <intrin.h>
static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]){ int v[4]; __cpuidex(v, (int)leaf, (int)sub); memcpy(r, v, sizeof v); }
#else
#include <cpuid.h>
static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]){ __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]); }
#endif

static uint64_t xgetbv0(void){ uint32_t a, d; __asm__ volatile("xgetbv" : "=a"(a), "=d"(d) : "c"(0)); return ((uint64_t)d << 32) | a; }

uint32_t lp_cpu_features(void){
    static uint32_t cached = 0xFFFFFFFFu;
    if (cached != 0xFFFFFFFFu) return cached;
    uint32_t r[4], f = 0;
    cpuid(0, 0, r); uint32_t max = r[0];
    if (max < 1) return cached = 0;
    cpuid(1, 0, r); uint32_t c = r[2], d = r[3];
    if (d & (1u << 26)) f |= LP_CPU_SSE2;
    if (c & (1u << 19)) f |= LP_CPU_SSE41;
    bool osxsave = c & (1u << 27), fma = c & (1u << 12);
    uint64_t xcr0 = osxsave ? xgetbv0() : 0;
    bool ymm = (xcr0 & 0x6) == 0x6, zmm = (xcr0 & 0xE6) == 0xE6, tiles = (xcr0 & 0x60000) == 0x60000;
    uint32_t b7 = 0, c7 = 0, d7 = 0, a71 = 0;
    if (max >= 7) {
        cpuid(7, 0, r); b7 = r[1]; c7 = r[2]; d7 = r[3];
        cpuid(7, 1, r); a71 = r[0];
    }
    if (ymm && fma && (b7 & (1u << 5)) && (b7 & (1u << 8))) f |= LP_CPU_AVX2;                 /* AVX2, BMI2 */
    if (zmm && (b7 & (1u << 16)) && (b7 & (1u << 30)) && (b7 & (1u << 31)) && (b7 & (1u << 17)))
        f |= LP_CPU_AVX512;                                                                   /* F, BW, VL, DQ */
    if ((f & LP_CPU_AVX512) && (c7 & (1u << 11))) f |= LP_CPU_VNNI512;
    if (ymm && (a71 & (1u << 4))) f |= LP_CPU_AVXVNNI;
    if (tiles && (d7 & (1u << 24))) f |= LP_CPU_AMX;
    return cached = f;
}

uint32_t lp_cpu_active(void){
    static uint32_t cached = 0xFFFFFFFFu;
    if (cached != 0xFFFFFFFFu) return cached;
    uint32_t f = lp_cpu_features();
    const char *want = getenv("LAPLACE_ISA");
    if (want) {
        uint32_t cap = !strcmp(want, "scalar") ? 0
                     : !strcmp(want, "sse2")   ? (LP_CPU_SSE2 | LP_CPU_SSE41)
                     : !strcmp(want, "avx2")   ? (LP_CPU_SSE2 | LP_CPU_SSE41 | LP_CPU_AVX2)
                     : !strcmp(want, "avxvnni") ? (LP_CPU_SSE2 | LP_CPU_SSE41 | LP_CPU_AVX2 | LP_CPU_AVXVNNI)
                     : !strcmp(want, "avx512") ? (LP_CPU_SSE2 | LP_CPU_SSE41 | LP_CPU_AVX2 | LP_CPU_AVX512)
                     : 0xFFFFFFFFu;                                                         /* avx512vnni: all */
        f &= cap;
    }
    return cached = f;
}

const char *lp_cpu_describe(uint32_t f){
    static _Thread_local char buf[128]; buf[0] = 0;
    static const struct { uint32_t bit; const char *name; } names[] = {
        { LP_CPU_SSE2, "sse2" }, { LP_CPU_SSE41, "sse4.1" }, { LP_CPU_AVX2, "avx2" }, { LP_CPU_AVX512, "avx512" },
        { LP_CPU_VNNI512, "avx512-vnni" }, { LP_CPU_AVXVNNI, "avx-vnni" }, { LP_CPU_AMX, "amx" } };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (f & names[i].bit) { if (buf[0]) strcat(buf, " "); strcat(buf, names[i].name); }
    if (!buf[0]) strcpy(buf, "scalar");
    return buf;
}

/* The kernels, chosen once for the process: the widest each has that the active level allows. */
static lp_kernel_set kernels; static int kernels_set; static lp_lock kernels_mu = LP_LOCK_INIT;
static void kernels_pick(void){
    uint32_t f = lp_cpu_active();
    kernels.scan = lp_scan_scalar; kernels.row_d2 = lp_row_d2_scalar; kernels.interleave = lp_hilbert4_interleave_scalar;
    kernels.half_chord = lp_half_chord_scalar;
#if defined(LP_HAVE_AVX2)
    if (f & LP_CPU_AVX2) { kernels.scan = lp_scan_avx2; kernels.row_d2 = lp_row_d2_avx2; kernels.interleave = lp_hilbert4_interleave_bmi2;   /* x86-64-v3 includes BMI2 */
                           kernels.half_chord = lp_half_chord_avx2; }
#endif
#if defined(LP_HAVE_AVX512)
    if (f & LP_CPU_AVX512) { kernels.scan = lp_scan_avx512; kernels.row_d2 = lp_row_d2_avx512; }
#endif
    (void)f;
}
const lp_kernel_set *lp_kernels(void){
    if (!__atomic_load_n(&kernels_set, __ATOMIC_ACQUIRE)) {
        lp_lock_take(&kernels_mu);
        if (!kernels_set) { kernels_pick(); __atomic_store_n(&kernels_set, 1, __ATOMIC_RELEASE); }
        lp_lock_give(&kernels_mu);
    }
    return &kernels;
}

/* CPU feature detection and the dispatch level. Every ISA the CPU has is used; LAPLACE_ISA may lower the level for
 * testing (scalar, sse2, avx2, avx512), never raise it past what the CPU supports. */
#include "laplace/laplace.h"
#include "internal.h"
#include <cpuid.h>
#include <stdlib.h>
#include <string.h>

static uint64_t xgetbv0(void){ uint32_t a, d; __asm__ volatile("xgetbv" : "=a"(a), "=d"(d) : "c"(0)); return ((uint64_t)d << 32) | a; }

uint32_t lp_cpu_features(void){
    static uint32_t cached = 0xFFFFFFFFu;
    if (cached != 0xFFFFFFFFu) return cached;
    uint32_t a, b, c, d, f = 0;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return cached = 0;
    if (d & (1u << 26)) f |= LP_CPU_SSE2;
    if (c & (1u << 19)) f |= LP_CPU_SSE41;
    bool osxsave = c & (1u << 27), fma = c & (1u << 12);
    uint64_t xcr0 = osxsave ? xgetbv0() : 0;
    bool ymm = (xcr0 & 0x6) == 0x6, zmm = (xcr0 & 0xE6) == 0xE6, tiles = (xcr0 & 0x60000) == 0x60000;
    uint32_t b7 = 0, c7 = 0, d7 = 0, a71 = 0;
    if (__get_cpuid_max(0, NULL) >= 7) {
        __cpuid_count(7, 0, a, b7, c7, d7);
        __cpuid_count(7, 1, a71, b, c, d);
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
                     : !strcmp(want, "avx2")   ? (LP_CPU_SSE2 | LP_CPU_SSE41 | LP_CPU_AVX2 | LP_CPU_AVXVNNI)
                     : 0xFFFFFFFFu;
        f &= cap;
    }
    return cached = f;
}

const char *lp_cpu_describe(uint32_t f){
    static char buf[128]; buf[0] = 0;
    static const struct { uint32_t bit; const char *name; } names[] = {
        { LP_CPU_SSE2, "sse2" }, { LP_CPU_SSE41, "sse4.1" }, { LP_CPU_AVX2, "avx2" }, { LP_CPU_AVX512, "avx512" },
        { LP_CPU_VNNI512, "avx512-vnni" }, { LP_CPU_AVXVNNI, "avx-vnni" }, { LP_CPU_AMX, "amx" } };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (f & names[i].bit) { if (buf[0]) strcat(buf, " "); strcat(buf, names[i].name); }
    if (!buf[0]) strcpy(buf, "scalar");
    return buf;
}

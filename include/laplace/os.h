/* The platform layer: everything Laplace asks of the operating system beyond standard C, behind one
 * contract. Paths are UTF-8 with '/' separators on every platform.
 * Note: NOT part of the C# P/Invoke ABI (tools/csharp/api.c) and NOT linked into Laplace::laplace_static,
 * so PostgreSQL extensions never see Windows POSIX compatibility macros or headers. */
#ifndef LAPLACE_OS_H
#define LAPLACE_OS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#define OS_DLL_SUFFIX ".dll"
#else
#define OS_DLL_SUFFIX ".so"
#endif

/* A loaded library and its symbols (dlopen/dlsym; LoadLibrary/GetProcAddress). */
void *os_dl_open(const char *path);
void *os_dl_sym(void *lib, const char *symbol);
const char *os_dl_error(void);                        /* why the last os_dl_open failed */

/* A list of paths or names, owned by the list. */
typedef struct { char **item; size_t n; } os_list;
void os_list_free(os_list *);

/* Every existing path a pattern names (glob(3) with flags 0): '*', '?' and '[...]' within one path component, a
 * leading '.' matched only by a leading '.', sorted byte-wise. No match is an empty list. */
void os_glob(const char *pattern, os_list *out);

/* The names in a directory, '.' and '..' included, unsorted; 0, or -1 when it cannot be read (errno set). */
int os_list_dir(const char *dir, os_list *out);

/* Whether anything exists at a path, and whether what is there is a directory. */
int os_exists(const char *path);
int os_is_dir(const char *path);

/* A file moved over another (rename(2): the destination replaced; MoveFileEx with MOVEFILE_REPLACE_EXISTING, where
 * rename refuses an existing destination). 0, or -1 with errno set. */
int os_rename(const char *from, const char *to);

/* fnmatch(pattern, string, 0): 0 when the string matches. '*' and '?' match '/' too; '\' escapes. */
int os_fnmatch(const char *pattern, const char *string);

/* A path normalized to '/' separators. In place; a no-op on POSIX. */
void os_slashes(char *path);

/* Address space reserved and committed lazily as pages are touched (mmap anonymous, MAP_NORESERVE; VirtualAlloc
 * reserve + on-touch commit handler): zero-filled, NULL on failure. */
void *os_reserve(size_t bytes);
/* Pages of os_reserve memory handed back: they read as zero when touched again (madvise MADV_DONTNEED). */
void os_discard(void *p, size_t bytes);

/* Explicit virtual memory lifecycle (without VEH auto-commit). os_vm_discard decommits backing pages;
 * caller must call os_vm_commit again before reading or writing discarded pages. */
void *os_vm_reserve(size_t bytes);
bool os_vm_commit(void *p, size_t bytes);
void os_vm_release(void *p, size_t bytes);
void os_vm_discard(void *p, size_t bytes);

/* Register a destructor fn(arg) to run in LIFO order when the current thread exits (and at process exit on
 * the main thread). Returns 0 on success, -1 with errno set on error. */
int os_at_thread_exit(void (*fn)(void *), void *arg);

/* Temporary binary read/write file removed automatically on close or process termination. Avoids CRT tmpfile()
 * failures in C:\ on Windows. */
FILE *os_tmpfile(void);

/* 64-bit file positioning and size without disturbing the cursor. */
int os_fseek64(FILE *f, int64_t offset, int origin);
int64_t os_ftell64(FILE *f);
int64_t os_fsize(FILE *f);

/* Reliable line buffering (unbuffered on non-TTY pipes on Windows where MSVC treats _IOLBF as full-buffered). */
void os_stdio_linebuf(FILE *f);

/* Monotonic clock in nanoseconds. */
uint64_t os_monotonic_ns(void);

/* UTF-8 path of the current executable with '/' separators. Returns length in bytes (excluding NUL) or -1
 * with errno set on failure or truncation. */
int os_self_exe(char *buf, size_t cap);

/* A program run to its end with its standard output and error in a log file (truncated) and, when var is non-NULL,
 * one variable set in its environment: its exit status (128 on signal/NT-exception), or -1 if it could not be
 * started (errno set). */
int os_run_logged(const char *exe, char *const argv[], const char *log, const char *var, const char *value);

/* argv[0] run to its end, or stopped after timeout_s seconds (0: no limit, its process group with it on POSIX), its
 * standard output and error read back into *out (malloc'd, NUL-terminated, at most cap - 1 bytes): 1, and *code its
 * exit status (124 when stopped, 128 when it died), or 0 when it could not be started. */
int os_run_captured(char *const argv[], int timeout_s, size_t cap, char **out, int *code);

/* Monotonic clock in seconds and failing-fast realloc shared by native tools and readers. */
double now(void);
void *xrealloc(void *p, size_t n);

#ifdef _WIN32
/* The POSIX calls native tools use, on Windows: same names and semantics, so callers do not change. */
#include <string.h>
#include <io.h>
#include <time.h>
#include <sys/types.h>
typedef struct { void *opaque; } pthread_mutex_t;             /* an SRWLOCK: one pointer, zero-initialised */
#define PTHREAD_MUTEX_INITIALIZER { 0 }
int pthread_mutex_init(pthread_mutex_t *, const void *attr);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_mutex_destroy(pthread_mutex_t *);
void *memmem(const void *hay, size_t hn, const void *needle, size_t nn);
char *strndup(const char *s, size_t n);
typedef long long ssize_t;
ssize_t getline(char **line, size_t *cap, FILE *f);
#define strtok_r strtok_s
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#define strdup _strdup
#define read _read
#define close _close
#define isatty _isatty
#define fileno _fileno
#define CLOCK_MONOTONIC 1
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
int clock_gettime(int clock, struct timespec *ts);
/* glibc's rand_r, step for step, so a seed gives the same sequence on both systems. */
int rand_r(unsigned int *seed);
/* glibc's qsort_r argument order (comparator takes the context last); a stable merge sort. */
void qsort_r(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *, void *), void *arg);
/* statx(2): what Windows records of a file, under Linux's field names; a field Windows lacks is left out of
 * stx_mask (the caller already treats a missing field as absent). */
struct statx_timestamp { int64_t tv_sec; uint32_t tv_nsec; int32_t reserved; };
struct statx { uint32_t stx_mask, stx_blksize; uint64_t stx_attributes; uint32_t stx_nlink, stx_uid, stx_gid; uint16_t stx_mode;
               uint64_t stx_ino, stx_size, stx_blocks, stx_attributes_mask;
               struct statx_timestamp stx_atime, stx_btime, stx_ctime, stx_mtime;
               uint32_t stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor; };
#define AT_FDCWD -100
#define STATX_TYPE 0x1u
#define STATX_MODE 0x2u
#define STATX_NLINK 0x4u
#define STATX_UID 0x8u
#define STATX_GID 0x10u
#define STATX_ATIME 0x20u
#define STATX_MTIME 0x40u
#define STATX_CTIME 0x80u
#define STATX_INO 0x100u
#define STATX_SIZE 0x200u
#define STATX_BLOCKS 0x400u
#define STATX_BASIC_STATS 0x7ffu
#define STATX_BTIME 0x800u
int statx(int dirfd, const char *path, int flags, unsigned int mask, struct statx *x);
/* printf's "'" flag (thousands grouping under en_US.UTF-8): the Windows C runtime has none, so the three
 * printf forms group the digits themselves, as glibc does. */
#include <stdarg.h>
int os_vsnprintf(char *out, size_t cap, const char *fmt, va_list ap);
int os_snprintf(char *out, size_t cap, const char *fmt, ...);
int os_fprintf(FILE *f, const char *fmt, ...);
int os_printf(const char *fmt, ...);
#define snprintf os_snprintf
#define fprintf os_fprintf
#define printf os_printf
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdlib.h>
#include <strings.h>
#endif

#ifdef __cplusplus
}
#endif
#endif

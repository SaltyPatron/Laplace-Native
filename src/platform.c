/* The platform layer (platform.h): POSIX and Windows behind one contract. */
#include "platform.h"
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

_Static_assert(sizeof(lp_lock) == sizeof(SRWLOCK), "lp_lock holds an SRWLOCK");

const void *lp_map_file(const char *path, size_t *size){
    wchar_t w[4096];                                   /* paths are UTF-8, as on every other platform */
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, (int)(sizeof w / sizeof *w))) return NULL;
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER n; HANDLE m = NULL; const void *p = NULL;
    if (GetFileSizeEx(f, &n) && n.QuadPart > 0 && (m = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL)))
        p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (m) CloseHandle(m);                             /* the view keeps the mapping alive */
    CloseHandle(f);
    if (p) *size = (size_t)n.QuadPart;
    return p;
}
void lp_unmap_file(const void *p, size_t size){ (void)size; if (p) UnmapViewOfFile(p); }

void lp_lock_take(lp_lock *l){ AcquireSRWLockExclusive((PSRWLOCK)l); }
void lp_lock_give(lp_lock *l){ ReleaseSRWLockExclusive((PSRWLOCK)l); }

/* POSIX getline: the line with its newline into *line, grown as needed; its length, or -1 at the end or on error. */
ptrdiff_t lp_getline(char **line, size_t *cap, FILE *f){
    size_t n = 0; int c;
    if (!*line || !*cap) { *cap = 256; if (!(*line = malloc(*cap))) return -1; }
    while ((c = fgetc(f)) != EOF) {
        if (n + 2 > *cap) { char *g = realloc(*line, *cap * 2); if (!g) return -1; *line = g; *cap *= 2; }
        (*line)[n++] = (char)c;
        if (c == '\n') break;
    }
    if (!n) return -1;
    (*line)[n] = 0; return (ptrdiff_t)n;
}

#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

const void *lp_map_file(const char *path, size_t *size){
    int fd = open(path, O_RDONLY); struct stat st; if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return NULL; }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0); close(fd);
    if (p == MAP_FAILED) return NULL;
    *size = (size_t)st.st_size; return p;
}
void lp_unmap_file(const void *p, size_t size){ if (p) munmap((void *)p, size); }

void lp_lock_take(lp_lock *l){ pthread_mutex_lock(l); }
void lp_lock_give(lp_lock *l){ pthread_mutex_unlock(l); }
#endif

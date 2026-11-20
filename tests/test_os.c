/* Cross-platform tests for Laplace::os (including process execution, VM reserve/commit/discard,
 * thread-exit callbacks, 64-bit temp files, POSIX regex/glob/statx/qsort_r shims). */
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include "laplace/os.h"
#include "check.h"
#include <errno.h>
#include <locale.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

static volatile int exit_order[4];
static volatile int exit_count;
static void exit_cb_a(void *p){ (void)p; exit_order[exit_count++] = 1; }
static void exit_cb_b(void *p){ (void)p; exit_order[exit_count++] = 2; }

#ifdef _WIN32
static DWORD WINAPI worker_fn(LPVOID unused){
    (void)unused;
    os_at_thread_exit(exit_cb_a, NULL);
    os_at_thread_exit(exit_cb_b, NULL);
    return 0;
}
#else
static void *worker_fn(void *unused){
    (void)unused;
    os_at_thread_exit(exit_cb_a, NULL);
    os_at_thread_exit(exit_cb_b, NULL);
    return NULL;
}
#endif

static int cmp_desc_ctx(const void *a, const void *b, void *ctx){
    int sign = *(const int *)ctx;
    int x = *(const int *)a, y = *(const int *)b;
    return sign * ((x > y) - (x < y));
}

int main(int argc, char **argv){
    if (argc > 1 && !strcmp(argv[1], "child-exit")) { printf("hello\n"); return atoi(argv[2]); }
    if (argc > 1 && !strcmp(argv[1], "child-env")) { printf("%s\n", getenv("LAPLACE_OS_TEST") ? getenv("LAPLACE_OS_TEST") : "(unset)"); return 0; }
    if (argc > 1 && !strcmp(argv[1], "child-sleep")) {
#ifdef _WIN32
        Sleep(10000);
#else
        sleep(10);
#endif
        return 0;
    }

    char exe[4096];
    int elen = os_self_exe(exe, sizeof exe);
    CHECK(elen > 0 && exe[elen] == 0 && strchr(exe, '\\') == NULL && os_exists(exe) && !os_is_dir(exe),
          "os_self_exe: %s", elen > 0 ? exe : "(err)");

    char tmp_path[4096];
#ifdef _WIN32
    const char *tdir = getenv("TEMP"); if (!tdir || !*tdir) tdir = ".";
    snprintf(tmp_path, sizeof tmp_path, "%s/laplace-os-test-%lu.log", tdir, (unsigned long)GetCurrentProcessId());
#else
    snprintf(tmp_path, sizeof tmp_path, "/tmp/laplace-os-test-%d.log", (int)getpid());
#endif
    os_slashes(tmp_path);

    {
        char *av[] = { exe, "child-exit", "7", NULL };
        int r = os_run_logged(exe, av, tmp_path, "LAPLACE_OS_TEST", "ok");
        CHECK(r == 7, "os_run_logged exit code %d", r);
        remove(tmp_path);
    }
    {
        char *av[] = { exe, "child-env", NULL };
        int r = os_run_logged(exe, av, tmp_path, "LAPLACE_OS_TEST", "alpha");
        FILE *f = fopen(tmp_path, "rb"); char line[64] = ""; if (f) { fgets(line, sizeof line, f); fclose(f); }
        CHECK(r == 0 && strncmp(line, "alpha", 5) == 0 && getenv("LAPLACE_OS_TEST") == NULL,
              "os_run_logged isolated env: rc=%d line=%s", r, line);
        remove(tmp_path);
    }
    {
        char *av[] = { "/nonexistent/laplace/missing_bin", NULL };
        errno = 0;
        int r = os_run_logged(av[0], av, tmp_path, NULL, NULL);
        CHECK(r == -1 && errno == ENOENT, "os_run_logged missing binary: rc=%d errno=%d", r, errno);
        remove(tmp_path);
    }
    {
        char *av[] = { exe, "child-exit", "5", NULL };
        char *out = NULL; int code = -1;
        int ok = os_run_captured(av, 0, 4096, &out, &code);
        CHECK(ok && code == 5 && out && strncmp(out, "hello", 5) == 0, "os_run_captured ok");
        free(out);
    }
    {
        char *av[] = { exe, "child-sleep", NULL };
        char *out = NULL; int code = -1;
        int ok = os_run_captured(av, 1, 4096, &out, &code);
        CHECK(ok && code == 124 && out && strstr(out, "stopped after 1 s"), "os_run_captured timeout");
        free(out);
    }

    /* VM reserve + commit + discard + release */
    void *vm = os_vm_reserve(1024 * 1024);
    CHECK(vm != NULL, "os_vm_reserve");
    if (vm) {
        CHECK(os_vm_commit(vm, 65536), "os_vm_commit");
        memset(vm, 0x5A, 65536);
        CHECK(((uint8_t *)vm)[123] == 0x5A, "committed page writable");
        os_vm_discard(vm, 65536);
        CHECK(os_vm_commit(vm, 65536), "os_vm_commit after discard");
        CHECK(((uint8_t *)vm)[123] == 0, "re-committed page is zero");
        os_vm_release(vm, 1024 * 1024);
    }

    /* tmpfile + 64-bit stdio helpers + monotonic clock */
    FILE *tf = os_tmpfile();
    CHECK(tf != NULL, "os_tmpfile");
    if (tf) {
        CHECK(fwrite("abcdef", 1, 6, tf) == 6, "tmpfile write");
        CHECK(os_fsize(tf) == 6 && os_ftell64(tf) == 6, "os_fsize keeps position");
        CHECK(os_fseek64(tf, 2, SEEK_SET) == 0 && os_ftell64(tf) == 2 && fgetc(tf) == 'c', "os_fseek64");
        os_stdio_linebuf(tf);
        fclose(tf);
    }
    uint64_t t0 = os_monotonic_ns();
    double d0 = now();
    CHECK(t0 > 0 && d0 > 0.0, "monotonic clock");

    /* thread-exit LIFO destructor */
    exit_count = 0;
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, worker_fn, NULL, 0, NULL);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
#else
    pthread_t th; pthread_create(&th, NULL, worker_fn, NULL); pthread_join(th, NULL);
#endif
    CHECK(exit_count == 2 && exit_order[0] == 2 && exit_order[1] == 1, "os_at_thread_exit LIFO");

    /* regex, qsort_r, grouping snprintf */
    regex_t re;
    CHECK(regcomp(&re, "^lap-[0-9]+$", REG_EXTENDED | REG_NOSUB) == 0, "regcomp");
    CHECK(regexec(&re, "lap-42", 0, NULL, 0) == 0 && regexec(&re, "bad", 0, NULL, 0) != 0, "regexec");
    regfree(&re);

    int vals[4] = { 1, 4, 2, 3 }, sign = -1;
    qsort_r(vals, 4, sizeof(int), cmp_desc_ctx, &sign);
    CHECK(vals[0] == 4 && vals[3] == 1, "qsort_r");

    setlocale(LC_NUMERIC, "en_US.UTF-8");
    char gbuf[64];
    snprintf(gbuf, sizeof gbuf, "%'d", 1234567);
    CHECK(!strcmp(gbuf, "1,234,567"), "grouped printf: %s", gbuf);

    CHECK(os_fnmatch("a*c?", "abbbcZ") == 0 && os_fnmatch("a*c?", "ab") != 0, "os_fnmatch");
    DONE("os");
}

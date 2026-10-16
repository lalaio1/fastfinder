/*
 * FastFinder 2.1.0
 * ================
 * Blazing-fast, cross-platform (Windows / Linux) recursive keyword scanner.
 *
 * Recursively walks a directory tree (or scans a single file), searches every
 * text file for a keyword, and writes every matching line to an output file.
 *
 * Design goals
 * ------------
 *   - Saturate disk I/O:
 *       * zero-copy memory-mapped I/O for data that is already in the page
 *         cache (mmap / MapViewOfFile); plain large sequential reads when the
 *         data still has to come from disk (measured faster than faulting).
 *         The choice is made per range by sampling page residency;
 *       * one worker thread per CPU core by default (-t to tune);
 *       * big files are split into line-aligned segments and scanned in
 *         parallel by several workers (a single 10 GB dump uses every core);
 *       * scanning starts immediately while the directory tree is still
 *         being walked (producer / consumer pipeline).
 *   - Go fast on the CPU side:
 *       * single fused SIMD pass that finds keyword candidates (first two
 *         bytes compared vector-wide) and counts lines with popcount;
 *         SSE2 baseline plus runtime-dispatched AVX2 on x86 (picked
 *         automatically when the portable build runs on a modern CPU);
 *       * matches are expanded to lines and batched per thread, flushed with
 *         one lock instead of locking once per matched line.
 *   - Never freeze the machine:
 *       * hard-bounded work queue: memory stays roughly constant no matter
 *         how many files exist (backpressure, no unbounded growth);
 *       * worker count defaults to the number of CPU cores;
 *       * -l / --low-priority lowers CPU and I/O priority on both platforms;
 *       * a SIGBUS handler recovers cleanly if a mapped file is truncated
 *         while it is being scanned (e.g. a download in progress).
 *   - Be correct on the edges:
 *       * lines split across read-buffer and segment boundaries are handled;
 *       * files without a trailing newline are handled;
 *       * NUL bytes / binary-ish content are handled (byte-wise search);
 *       * CRLF files keep their original bytes;
 *       * symlinks are not followed (same as grep -r / ripgrep defaults),
 *         so there are no duplicate reads or directory loops.
 *
 * Build (see Makefile / README.md):
 *   Linux (portable: SSE2 + runtime AVX2 dispatch):
 *     gcc -O3 -std=c11 -pthread -o fastfinder fastfinder.c
 *   Linux (tuned for this exact CPU):
 *     gcc -O3 -std=c11 -pthread -march=native -o fastfinder fastfinder.c
 *   Windows (mingw-w64):
 *     x86_64-w64-mingw32-gcc -O3 -std=c11 -static -o fastfinder.exe fastfinder.c
 *
 * License: Apache-2.0 (see LICENSE)
 */

#if defined(_WIN32)
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0601        /* Windows 7+ (condition variables) */
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef __USE_MINGW_ANSI_STDIO
    #define __USE_MINGW_ANSI_STDIO 1   /* proper printf/snprintf on mingw */
  #endif
#else
  #ifndef _POSIX_C_SOURCE
    #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
    #define _DEFAULT_SOURCE 1          /* nice(), syscall() on glibc */
  #endif
  #ifndef _FILE_OFFSET_BITS
    #define _FILE_OFFSET_BITS 64       /* 64-bit file offsets even on 32-bit */
  #endif
#endif

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <process.h>
  #include <windows.h>
#else
  #include <dirent.h>
  #include <fcntl.h>
  #include <pthread.h>
  #include <setjmp.h>
  #include <signal.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <time.h>
  #include <unistd.h>
  #if defined(__linux__)
    #include <sys/syscall.h>
  #endif
  #ifndef DT_UNKNOWN
    #define DT_UNKNOWN 0
    #define DT_FIFO 1
    #define DT_CHR 2
    #define DT_DIR 4
    #define DT_BLK 6
    #define DT_REG 8
    #define DT_LNK 10
    #define DT_SOCK 12
  #endif
#endif

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

#define FF_VERSION       "2.1.0"
#define FF_DEFAULT_EXT   ".txt"
#define FF_QUEUE_CAP     4096          /* pending work units (bounded = bounded RAM) */
#define FF_MAX_THREADS   128

#ifndef FF_READ_BUF
  #define FF_READ_BUF    (4u * 1024u * 1024u)   /* per-worker read buffer  */
#endif
#ifndef FF_MATCH_BUF
  #define FF_MATCH_BUF   (1u << 20)             /* per-worker match batch  */
#endif
#ifndef FF_MMAP_MIN
  #define FF_MMAP_MIN    (1u << 20)             /* 1 MiB: map files this big */
#endif
#ifndef FF_SPLIT_MIN
  #define FF_SPLIT_MIN   (8u << 20)             /* 8 MiB: split files this big */
#endif
#ifndef FF_SEG_TARGET
  #define FF_SEG_TARGET  (16u << 20)            /* target segment size     */
#endif
#ifndef FF_DIRECT_EMIT
  #define FF_DIRECT_EMIT (16u << 20)            /* huge line: bypass buffer */
#endif

/* 64-bit address space (needed for large mappings). */
#if defined(_WIN64) || defined(__LP64__) || defined(_LP64) || \
    defined(__x86_64__) || defined(__aarch64__) || defined(__ppc64__)
  #define FF_ADDRESS_64 1
#endif

/* ------------------------------------------------------------------ */
/* SIMD selection (fused keyword scan)                                */
/* ------------------------------------------------------------------ */
/*
 * x86 gets an SSE2 baseline (always present on x86-64). When the binary is
 * not compiled with -mavx2, an AVX2 version of the inner loop is also built
 * and picked at runtime when the CPU supports it (GCC only). Builds made
 * with -mavx2 / -march=native use AVX2 everywhere and need no dispatch.
 */
#if defined(FF_SCALAR_ONLY)
  #define FF_SIMD          0
  #define FF_X86_SSE2      0
  #define FF_AVX2_DISPATCH 0
#elif defined(__AVX2__)
  #include <immintrin.h>
  #define FF_SIMD          1
  #define FF_X86_SSE2      1
  #define FF_AVX2_DISPATCH 0
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
  #include <immintrin.h>
  #define FF_SIMD          1
  #define FF_X86_SSE2      1
  #if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    #define FF_AVX2_DISPATCH 1
  #else
    #define FF_AVX2_DISPATCH 0
  #endif
#else
  #define FF_SIMD          0
  #define FF_X86_SSE2      0
  #define FF_AVX2_DISPATCH 0
#endif

static inline int ff_popcount32(uint32_t x) {
#if defined(__GNUC__)
    return __builtin_popcount(x);
#else
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    return (int)((x * 0x01010101u) >> 24);
#endif
}

static inline unsigned ff_ctz32(uint32_t x) {
#if defined(__GNUC__)
    return (unsigned)__builtin_ctz(x);
#else
    unsigned n = 0;
    while ((x & 1u) == 0) { x >>= 1; n++; }
    return n;
#endif
}

/* ------------------------------------------------------------------ */
/* Small portable helpers                                             */
/* ------------------------------------------------------------------ */

static void die_oom(void) {
    fprintf(stderr, "fastfinder: out of memory\n");
    exit(1);
}

static char *ff_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (!p) die_oom();
    memcpy(p, s, n);
    return p;
}

static int ascii_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

#if defined(_WIN32)
/* ASCII case-insensitive compare (portable, no _stricmp/strcasecmp). */
static int ff_stricmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = ascii_lower((unsigned char)*a);
        int cb = ascii_lower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return ascii_lower((unsigned char)*a) - ascii_lower((unsigned char)*b);
}
#endif

static const char *path_basename(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
#if defined(_WIN32)
        if (*p == '/' || *p == '\\') base = p + 1;
#else
        if (*p == '/') base = p + 1;
#endif
    }
    return base;
}

static char *path_join(const char *dir, const char *name) {
    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    int sep = (dl > 0 && dir[dl - 1] != '/' && dir[dl - 1] != '\\') ? 1 : 0;
    char *p = (char *)malloc(dl + (size_t)sep + nl + 1);
    if (!p) die_oom();
    memcpy(p, dir, dl);
    size_t o = dl;
#if defined(_WIN32)
    if (sep) p[o++] = '\\';
#else
    if (sep) p[o++] = '/';
#endif
    memcpy(p + o, name, nl + 1);
    return p;
}

/* ------------------------------------------------------------------ */
/* Options                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *keyword;
    char       *keyword_folded;   /* lowercased keyword (only with -i) */
    size_t      keyword_len;
    int         ignore_case;
    int         all_files;
    int         show_file;
    int         no_mmap;          /* --no-mmap: never use mapped I/O   */
    int         no_split;         /* --no-split: one worker per file   */
    int         low_priority;     /* -l: reduced CPU/IO priority       */
} Options;

static int ext_ok(const char *name, const Options *opt) {
    if (opt->all_files) return 1;
    const char *ext = FF_DEFAULT_EXT;
    size_t el = strlen(ext);
    size_t nl = strlen(name);
    if (nl < el) return 0;
    const char *p = name + (nl - el);
    for (size_t i = 0; i < el; i++) {
        if (ascii_lower((unsigned char)p[i]) != (unsigned char)ext[i]) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Sync primitives (CRITICAL_SECTION+CONDITION_VARIABLE / pthreads)   */
/* ------------------------------------------------------------------ */

typedef struct {
#if defined(_WIN32)
    CRITICAL_SECTION   cs;
    CONDITION_VARIABLE cv;
#else
    pthread_mutex_t cs;
    pthread_cond_t  cv;
#endif
} Sync;

static void sync_init(Sync *s) {
#if defined(_WIN32)
    InitializeCriticalSection(&s->cs);
    InitializeConditionVariable(&s->cv);
#else
    pthread_mutex_init(&s->cs, NULL);
    pthread_cond_init(&s->cv, NULL);
#endif
}

static void sync_destroy(Sync *s) {
#if defined(_WIN32)
    DeleteCriticalSection(&s->cs);
#else
    pthread_mutex_destroy(&s->cs);
    pthread_cond_destroy(&s->cv);
#endif
}

static void sync_lock(Sync *s) {
#if defined(_WIN32)
    EnterCriticalSection(&s->cs);
#else
    pthread_mutex_lock(&s->cs);
#endif
}

static void sync_unlock(Sync *s) {
#if defined(_WIN32)
    LeaveCriticalSection(&s->cs);
#else
    pthread_mutex_unlock(&s->cs);
#endif
}

/* Must be called with the lock held. */
static void sync_wait(Sync *s) {
#if defined(_WIN32)
    SleepConditionVariableCS(&s->cv, &s->cs, INFINITE);
#else
    pthread_cond_wait(&s->cv, &s->cs);
#endif
}

/* Must be called with the lock held. */
static void sync_wake(Sync *s) {
#if defined(_WIN32)
    WakeConditionVariable(&s->cv);
#else
    pthread_cond_signal(&s->cv);
#endif
}

/* Must be called with the lock held. */
static void sync_wake_all(Sync *s) {
#if defined(_WIN32)
    WakeAllConditionVariable(&s->cv);
#else
    pthread_cond_broadcast(&s->cv);
#endif
}

/* ------------------------------------------------------------------ */
/* Bounded work queue                                                 */
/* ------------------------------------------------------------------ */

#define FF_WHOLE UINT64_MAX            /* unit scans the whole file */

typedef struct WorkUnit {
    char    *path;
    uint64_t start;                /* absolute offset of first byte    */
    uint64_t end;                  /* one past last byte, or FF_WHOLE  */
    int      count_file;           /* 1 => counts as one scanned file  */
} WorkUnit;

typedef struct {
    WorkUnit **items;
    size_t     cap;
    size_t     head;
    size_t     count;
    int        done;
    Sync       sync;
} FileQueue;

static void queue_init(FileQueue *q, size_t cap) {
    q->items = (WorkUnit **)malloc(cap * sizeof(WorkUnit *));
    if (!q->items) die_oom();
    q->cap = cap;
    q->head = 0;
    q->count = 0;
    q->done = 0;
    sync_init(&q->sync);
}

static void queue_destroy(FileQueue *q) {
    for (size_t i = 0; i < q->count; i++) {
        WorkUnit *u = q->items[(q->head + i) % q->cap];
        free(u->path);
        free(u);
    }
    free(q->items);
    sync_destroy(&q->sync);
}

/* Takes ownership of the unit. Blocks while the queue is full (backpressure).
 * Only the single producer thread uses this version. */
static void queue_put(FileQueue *q, WorkUnit *u) {
    sync_lock(&q->sync);
    while (q->count == q->cap) sync_wait(&q->sync);
    q->items[(q->head + q->count) % q->cap] = u;
    q->count++;
    sync_wake(&q->sync);
    sync_unlock(&q->sync);
}

/* Never blocks. Used by workers to hand off split segments. If the queue is
 * full the caller keeps the unit and scans it itself (no deadlock possible:
 * every blocked producer still makes progress on a unit it holds). */
static int queue_try_put(FileQueue *q, WorkUnit *u) {
    sync_lock(&q->sync);
    int ok = 0;
    if (q->count < q->cap && !q->done) {
        q->items[(q->head + q->count) % q->cap] = u;
        q->count++;
        sync_wake(&q->sync);
        ok = 1;
    }
    sync_unlock(&q->sync);
    return ok;
}

/* Returns NULL only when the queue is empty AND the producer is done. */
static WorkUnit *queue_get(FileQueue *q) {
    sync_lock(&q->sync);
    while (q->count == 0 && !q->done) sync_wait(&q->sync);
    WorkUnit *u = NULL;
    if (q->count > 0) {
        u = q->items[q->head];
        q->head = (q->head + 1) % q->cap;
        q->count--;
        sync_wake(&q->sync);   /* producers may be waiting for room */
    }
    sync_unlock(&q->sync);
    return u;
}

static void queue_finish(FileQueue *q) {
    sync_lock(&q->sync);
    q->done = 1;
    sync_wake_all(&q->sync);
    sync_unlock(&q->sync);
}

static WorkUnit *ff_work_unit(char *path) {
    WorkUnit *u = (WorkUnit *)malloc(sizeof(WorkUnit));
    if (!u) die_oom();
    u->path = path;
    u->start = 0;
    u->end = FF_WHOLE;
    u->count_file = 1;
    return u;
}

/* ------------------------------------------------------------------ */
/* Simple growable stack of strings (directory walk, iterative)       */
/* ------------------------------------------------------------------ */

typedef struct {
    char **items;
    size_t len;
    size_t cap;
} PathStack;

static void stack_push(PathStack *s, char *path) {
    if (s->len == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 64;
        char **ni = (char **)realloc(s->items, nc * sizeof(char *));
        if (!ni) die_oom();
        s->items = ni;
        s->cap = nc;
    }
    s->items[s->len++] = path;
}

static char *stack_pop(PathStack *s) {
    return s->len ? s->items[--s->len] : NULL;
}

/* ------------------------------------------------------------------ */
/* Raw file I/O (no stdio buffering on the input path)                */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)
typedef HANDLE FileHandle;
#define FF_INVALID_HANDLE INVALID_HANDLE_VALUE

static FileHandle file_open(const char *path) {
    return CreateFileA(path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
}

/* Positional read (works with a plain synchronous handle). */
static ptrdiff_t file_read_at(FileHandle h, void *buf, size_t n, uint64_t offset) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return -1;
    DWORD want = (n > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (DWORD)n;
    DWORD got = 0;
    if (!ReadFile(h, buf, want, &got, NULL)) return -1;
    return (ptrdiff_t)got;   /* 0 means EOF */
}

static int file_size(FileHandle h, uint64_t *out) {
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li)) return -1;
    *out = (uint64_t)li.QuadPart;
    return 0;
}

static void file_close(FileHandle h) { CloseHandle(h); }

/* Map [start, end) of the file, read-only, zero-copy. */
typedef struct {
    void       *base;
    size_t      len;
    const char *data;
} Mapped;

static int map_range(FileHandle h, uint64_t start, uint64_t end, Mapped *m) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint64_t gran = si.dwAllocationGranularity ? (uint64_t)si.dwAllocationGranularity
                                               : 65536u;
    uint64_t base = start & ~(gran - 1);
    uint64_t len = end - base;
#if SIZE_MAX < UINT64_MAX
    if (len == 0 || len > (uint64_t)SIZE_MAX) return -1;
#else
    if (len == 0) return -1;
#endif
    HANDLE map = CreateFileMappingA(h, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!map) return -1;
    void *view = MapViewOfFile(map, FILE_MAP_READ,
                               (DWORD)(base >> 32), (DWORD)(base & 0xFFFFFFFFu),
                               (SIZE_T)len);
    CloseHandle(map);
    if (!view) return -1;
    m->base = view;
    m->len = (size_t)len;
    m->data = (const char *)view + (size_t)(start - base);
    return 0;
}

static void unmap_any(void *base, size_t len) {
    (void)len;
    UnmapViewOfFile(base);
}

/*
 * True when the sampled pages are resident in memory: mmap (zero copy) is
 * then a clear win. When the data still has to come from disk, plain large
 * reads stream more efficiently than faulting page by page.
 *
 * Uses QueryWorkingSetEx, resolved dynamically (K32QueryWorkingSetEx from
 * kernel32, psapi.dll fallback) so no import library is required.
 */
typedef struct {
    PVOID     VirtualAddress;
    ULONG_PTR Attributes;
} FfWsExEntry;

typedef BOOL (WINAPI *ff_qwse_fn)(HANDLE, PVOID, DWORD);

static int range_looks_cached(const Mapped *m) {
    static ff_qwse_fn fn;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        if (k32) fn = (ff_qwse_fn)(void *)GetProcAddress(k32, "K32QueryWorkingSetEx");
        if (!fn) {
            HMODULE psapi = LoadLibraryA("psapi.dll");
            if (psapi) fn = (ff_qwse_fn)(void *)GetProcAddress(psapi, "QueryWorkingSetEx");
        }
    }
    if (!fn) return 1;   /* cannot tell: mmap is a good default */

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    size_t page = si.dwPageSize ? (size_t)si.dwPageSize : 4096u;
    size_t win = 64 * 1024;
    if (win > m->len) win = m->len;
    size_t npages = win / page;
    if (npages == 0) npages = 1;
    if (npages > 64) npages = 64;

    FfWsExEntry info[5 * 64];
    DWORD count = 0;
    const char *base = (const char *)m->base;
    size_t range = (m->len > win) ? (m->len - win) : 0;
    for (int i = 0; i < 5; i++) {
        size_t off = (range * (size_t)i) / 4u;
        off &= ~(page - 1);
        for (size_t p = 0; p < npages; p++) {
            info[count].VirtualAddress =
                (PVOID)(uintptr_t)(const void *)(base + off + p * page);
            count++;
        }
    }
    if (!fn(GetCurrentProcess(), info, count * (DWORD)sizeof(FfWsExEntry))) return 1;
    DWORD resident = 0;
    for (DWORD i = 0; i < count; i++) {
        if (info[i].Attributes & 1) resident++;   /* bit 0 = Valid */
    }
    return resident * 2 >= count;   /* >= 50% of the sampled pages resident */
}

#else   /* POSIX */

typedef int FileHandle;
#define FF_INVALID_HANDLE (-1)

static FileHandle file_open(const char *path) {
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    return open(path, flags);
}

static ptrdiff_t file_read_at(FileHandle fd, void *buf, size_t n, uint64_t offset) {
    for (;;) {
        ssize_t r = pread(fd, buf, n, (off_t)offset);
        if (r >= 0) return (ptrdiff_t)r;
        if (errno != EINTR) return -1;
    }
}

static int file_size(FileHandle fd, uint64_t *out) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    if (!S_ISREG(st.st_mode)) return -1;
    *out = (uint64_t)st.st_size;
    return 0;
}

static void file_close(FileHandle fd) { close(fd); }

/* Map [start, end) of the file, read-only, zero-copy. */
typedef struct {
    void       *base;
    size_t      len;
    const char *data;
} Mapped;

static int map_range(FileHandle fd, uint64_t start, uint64_t end, Mapped *m) {
    long psz = sysconf(_SC_PAGE_SIZE);
    uint64_t gran = (psz > 0) ? (uint64_t)psz : 4096u;
    uint64_t base = start & ~(gran - 1);
    uint64_t len = end - base;
#if SIZE_MAX < UINT64_MAX
    if (len == 0 || len > (uint64_t)SIZE_MAX) return -1;
#else
    if (len == 0) return -1;
#endif
    void *view = mmap(NULL, (size_t)len, PROT_READ, MAP_PRIVATE, fd, (off_t)base);
    if (view == MAP_FAILED) return -1;
    m->base = view;
    m->len = (size_t)len;
    m->data = (const char *)view + (size_t)(start - base);
    return 0;
}

static void unmap_any(void *base, size_t len) {
    munmap(base, len);
}

/*
 * True when the sampled pages are resident in memory: mmap (zero copy) is
 * then a clear win. When the data still has to come from disk, plain large
 * reads stream more efficiently than faulting page by page.
 */
static int range_looks_cached(const Mapped *m) {
    long ps = sysconf(_SC_PAGE_SIZE);
    size_t page = (ps > 0) ? (size_t)ps : 4096u;
    size_t win = 64 * 1024;
    if (win > m->len) win = m->len;
    size_t npages = win / page;
    if (npages == 0) { win = page; npages = 1; }
    if (npages > 256) npages = 256;

    unsigned char vec[256];
    unsigned resident = 0, total = 0;
    const char *base = (const char *)m->base;
    size_t range = (m->len > win) ? (m->len - win) : 0;
    for (int i = 0; i < 5; i++) {
        size_t off = (range * (size_t)i) / 4u;
        off &= ~(page - 1);
        if (mincore((void *)(uintptr_t)(const void *)(base + off), win, vec) != 0) return 1;
        for (size_t p = 0; p < npages; p++) {
            total++;
            if (vec[p] & 1) resident++;
        }
    }
    return resident * 2 >= total;   /* >= 50% of the sampled pages resident */
}

/* If a mapped file is truncated while we scan it, the kernel raises SIGBUS.
 * Recover per-thread via siglongjmp and stop scanning that unit cleanly. */
static _Thread_local sigjmp_buf g_mmap_jmp;
static _Thread_local volatile sig_atomic_t g_mmap_guard = 0;

static void ff_sigbus_handler(int sig) {
    if (g_mmap_guard) {
        g_mmap_guard = 0;
        siglongjmp(g_mmap_jmp, 1);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

#endif

/* ------------------------------------------------------------------ */
/* Keyword matching primitives                                        */
/* ------------------------------------------------------------------ */

/* Exact (or case-folded) full-keyword test at p. */
static inline int kw_match_at(const Options *opt, const char *p) {
    const size_t kl = opt->keyword_len;
    if (!opt->ignore_case) return memcmp(p, opt->keyword, kl) == 0;
    const unsigned char *fold = (const unsigned char *)opt->keyword_folded;
    for (size_t i = 0; i < kl; i++) {
        if ((unsigned char)ascii_lower((unsigned char)p[i]) != fold[i]) return 0;
    }
    return 1;
}

#if FF_SIMD
/* Cheap first-byte filter (exact when case-sensitive, superset otherwise). */
static inline int kw_first_ok(const Options *opt, unsigned char c) {
    unsigned char k0 = (unsigned char)opt->keyword[0];
    if (!opt->ignore_case) return c == k0;
    return (unsigned char)(c | 0x20) == (unsigned char)(k0 | 0x20);
}
#else
/* Scalar fallback: hunt the first byte with memchr, verify with memcmp. */
static const char *find_keyword(const Options *opt, const char *hay, size_t hay_len) {
    const size_t kl = opt->keyword_len;
    if (hay_len < kl) return NULL;

    const char *kw = opt->keyword;

    if (!opt->ignore_case) {
        const char *limit = hay + hay_len - kl;   /* last legal start */
        const char *p = hay;
        while (p <= limit) {
            const char *hit = (const char *)memchr(p, kw[0], (size_t)(limit - p) + 1);
            if (!hit) return NULL;
            if (kl == 1 || memcmp(hit + 1, kw + 1, kl - 1) == 0) return hit;
            p = hit + 1;
        }
        return NULL;
    }

    /* Case-insensitive (ASCII) fallback. */
    const unsigned char *fold = (const unsigned char *)opt->keyword_folded;
    const size_t last = hay_len - kl;
    for (size_t i = 0; i <= last; i++) {
        if ((unsigned char)ascii_lower((unsigned char)hay[i]) != fold[0]) continue;
        size_t j = 1;
        while (j < kl && (unsigned char)ascii_lower((unsigned char)hay[i + j]) == fold[j]) j++;
        if (j == kl) return hay + i;
    }
    return NULL;
}
#endif

/* ------------------------------------------------------------------ */
/* Workers                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned long long files;
    unsigned long long bytes;
    unsigned long long lines_read;
    unsigned long long lines_matched;
    unsigned long long failed;
} WorkerStats;

typedef struct {
    FileQueue  *queue;
    Options    *opt;
    FILE       *out;
    Sync       *out_lock;

    char       *io_buf;          /* FF_READ_BUF            */
    char       *match_buf;       /* FF_MATCH_BUF, growable */
    size_t      match_cap;
    size_t      match_len;

    const char *current_path;    /* file being scanned (-f prefix) */
    int         write_error;
    WorkerStats stats;
} Worker;

static int g_workers = 1;        /* total workers (split decision) */

#if defined(_WIN32)
  #define FF_THREAD_RET  unsigned
  #define FF_THREAD_CALL __stdcall
#else
  #define FF_THREAD_RET  void *
  #define FF_THREAD_CALL
#endif

/* Write the batch of matched lines while holding the output lock once. */
static void worker_flush(Worker *w) {
    if (w->match_len == 0) return;
    sync_lock(w->out_lock);
    size_t written = fwrite(w->match_buf, 1, w->match_len, w->out);
    sync_unlock(w->out_lock);
    if (written != w->match_len) w->write_error = 1;
    w->match_len = 0;
}

static void worker_emit_line(Worker *w, const char *line, size_t len) {
    size_t prefix = w->opt->show_file ? strlen(w->current_path) + 1 : 0;
    size_t need = prefix + len + 1;

    if (need > (size_t)FF_DIRECT_EMIT) {
        /* Pathological giant line: write it straight out instead of growing
         * the batch buffer by hundreds of megabytes. */
        worker_flush(w);
        int ok = 1;
        sync_lock(w->out_lock);
        if (prefix) {
            ok = ok && fwrite(w->current_path, 1, prefix - 1, w->out) == prefix - 1;
            ok = ok && fwrite(":", 1, 1, w->out) == 1;
        }
        ok = ok && fwrite(line, 1, len, w->out) == len;
        ok = ok && fwrite("\n", 1, 1, w->out) == 1;
        sync_unlock(w->out_lock);
        if (!ok) w->write_error = 1;
        w->stats.lines_matched++;
        return;
    }

    if (need > w->match_cap) {
        worker_flush(w);
        size_t nc = w->match_cap;
        while (nc < need) nc *= 2;
        char *nb = (char *)realloc(w->match_buf, nc);
        if (!nb) die_oom();
        w->match_buf = nb;
        w->match_cap = nc;
    } else if (w->match_len + need > w->match_cap) {
        worker_flush(w);
    }

    char *p = w->match_buf + w->match_len;
    if (prefix) {
        memcpy(p, w->current_path, prefix - 1);
        p += prefix - 1;
        *p++ = ':';
    }
    memcpy(p, line, len);
    p += len;
    *p++ = '\n';
    w->match_len = (size_t)(p - w->match_buf);
    w->stats.lines_matched++;
}

/* Emit the line that contains position p of `buf` (once per line). */
static void handle_hit(Worker *w, const char *buf, size_t len, size_t p,
                       size_t *next_line) {
    if (p < *next_line) return;          /* same line already emitted */

    size_t ls = p;
    while (ls > 0 && buf[ls - 1] != '\n') ls--;

    const char *nl = (const char *)memchr(buf + p, '\n', len - p);
    size_t le = nl ? (size_t)(nl - buf) : len;

    if (le > ls) worker_emit_line(w, buf + ls, le - ls);
    *next_line = (le < len) ? le + 1 : len;
}

#if FF_AVX2_DISPATCH
static int g_have_avx2 = 0;   /* set once at startup */
#endif

#if FF_X86_SSE2 && !defined(__AVX2__)
/*
 * Fused SSE2 block pass: counts newlines, finds keyword candidates by
 * comparing the first two keyword bytes vector-wide, and expands hits to
 * lines (once per line). Returns the first offset not covered by a block.
 */
static size_t scan_blocks_sse2(Worker *w, const char *buf, size_t len,
                               size_t kw_limit,
                               unsigned long long *newlines, size_t *next_line) {
    const Options *opt = w->opt;
    const size_t kl = opt->keyword_len;

    const __m128i v_nl = _mm_set1_epi8('\n');
    const unsigned char c0 = (unsigned char)opt->keyword[0];
    const unsigned char n0 = opt->ignore_case ? (unsigned char)(c0 | 0x20) : c0;
    const unsigned char n0b = (unsigned char)(n0 ^ 0x20);
    const __m128i v_b0  = _mm_set1_epi8((char)n0);
    const __m128i v_b0b = _mm_set1_epi8((char)n0b);
    const unsigned char c1 = (kl >= 2) ? (unsigned char)opt->keyword[1] : 0;
    const unsigned char n1 = opt->ignore_case ? (unsigned char)(c1 | 0x20) : c1;
    const unsigned char n1b = (unsigned char)(n1 ^ 0x20);
    const __m128i v_b1  = _mm_set1_epi8((char)n1);
    const __m128i v_b1b = _mm_set1_epi8((char)n1b);

    unsigned long long nl = 0;
    size_t pos = 0;
    while (pos + 16 + 1 <= len) {
        __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(buf + pos));
        nl += (unsigned)ff_popcount32((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(v, v_nl)));

        __m128i m = _mm_cmpeq_epi8(v, v_b0);
        if (opt->ignore_case) m = _mm_or_si128(m, _mm_cmpeq_epi8(v, v_b0b));

        uint32_t mask;
        if (kl >= 2) {
            __m128i v2 = _mm_loadu_si128((const __m128i *)(const void *)(buf + pos + 1));
            __m128i m2 = _mm_cmpeq_epi8(v2, v_b1);
            if (opt->ignore_case) m2 = _mm_or_si128(m2, _mm_cmpeq_epi8(v2, v_b1b));
            mask = (uint32_t)_mm_movemask_epi8(_mm_and_si128(m, m2));
        } else {
            mask = (uint32_t)_mm_movemask_epi8(m);
        }

        if (mask) {
            if (pos + 16 > kw_limit) {
                size_t room = (kw_limit > pos) ? (kw_limit - pos) : 0;
                mask &= (1u << room) - 1u;   /* room < 16 */
            }
            while (mask) {
                unsigned bit = ff_ctz32(mask);
                mask &= mask - 1u;
                size_t p = pos + bit;
                if (p < *next_line) continue;
                if ((kl > 2 || opt->ignore_case) && !kw_match_at(opt, buf + p)) continue;
                handle_hit(w, buf, len, p, next_line);
            }
        }
        pos += 16;
    }
    *newlines += nl;
    return pos;
}
#endif

#if defined(__AVX2__) || FF_AVX2_DISPATCH
/* Same as scan_blocks_sse2, 32 bytes per iteration. */
#if FF_AVX2_DISPATCH
__attribute__((target("avx2")))
#endif
static size_t scan_blocks_avx2(Worker *w, const char *buf, size_t len,
                               size_t kw_limit,
                               unsigned long long *newlines, size_t *next_line) {
    const Options *opt = w->opt;
    const size_t kl = opt->keyword_len;

    const __m256i v_nl = _mm256_set1_epi8('\n');
    const unsigned char c0 = (unsigned char)opt->keyword[0];
    const unsigned char n0 = opt->ignore_case ? (unsigned char)(c0 | 0x20) : c0;
    const unsigned char n0b = (unsigned char)(n0 ^ 0x20);
    const __m256i v_b0  = _mm256_set1_epi8((char)n0);
    const __m256i v_b0b = _mm256_set1_epi8((char)n0b);
    const unsigned char c1 = (kl >= 2) ? (unsigned char)opt->keyword[1] : 0;
    const unsigned char n1 = opt->ignore_case ? (unsigned char)(c1 | 0x20) : c1;
    const unsigned char n1b = (unsigned char)(n1 ^ 0x20);
    const __m256i v_b1  = _mm256_set1_epi8((char)n1);
    const __m256i v_b1b = _mm256_set1_epi8((char)n1b);

    unsigned long long nl = 0;
    size_t pos = 0;
    while (pos + 32 + 1 <= len) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(buf + pos));
        nl += (unsigned)ff_popcount32((uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, v_nl)));

        __m256i m = _mm256_cmpeq_epi8(v, v_b0);
        if (opt->ignore_case) m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, v_b0b));

        uint32_t mask;
        if (kl >= 2) {
            __m256i v2 = _mm256_loadu_si256((const __m256i *)(const void *)(buf + pos + 1));
            __m256i m2 = _mm256_cmpeq_epi8(v2, v_b1);
            if (opt->ignore_case) m2 = _mm256_or_si256(m2, _mm256_cmpeq_epi8(v2, v_b1b));
            mask = (uint32_t)_mm256_movemask_epi8(_mm256_and_si256(m, m2));
        } else {
            mask = (uint32_t)_mm256_movemask_epi8(m);
        }

        if (mask) {
            if (pos + 32 > kw_limit) {
                size_t room = (kw_limit > pos) ? (kw_limit - pos) : 0;
                mask &= (1u << room) - 1u;   /* room < 32 */
            }
            while (mask) {
                unsigned bit = ff_ctz32(mask);
                mask &= mask - 1u;
                size_t p = pos + bit;
                if (p < *next_line) continue;
                if ((kl > 2 || opt->ignore_case) && !kw_match_at(opt, buf + p)) continue;
                handle_hit(w, buf, len, p, next_line);
            }
        }
        pos += 32;
    }
    *newlines += nl;
    return pos;
}
#endif

/*
 * Processes a chunk of complete lines (the last byte is '\n', except for the
 * final call at EOF where a trailing partial line may be present).
 *
 * One fused SIMD pass per chunk:
 *   - every line is counted with popcount(newline mask);
 *   - keyword candidates are found by comparing the first two bytes of the
 *     keyword vector-wide (16 bytes SSE2 / 32 bytes AVX2);
 *   - every hit is expanded to its line and emitted exactly once per line.
 */
static void scan_region(Worker *w, const char *buf, size_t len) {
    if (len == 0) return;

    const Options *opt = w->opt;
    const size_t kl = opt->keyword_len;

    unsigned long long newlines = 0;
    size_t next_line = 0;

#if FF_SIMD
    const size_t kw_limit = (len >= kl) ? (len - kl + 1) : 0;
    size_t pos;
  #if defined(__AVX2__)
    pos = scan_blocks_avx2(w, buf, len, kw_limit, &newlines, &next_line);
  #elif FF_AVX2_DISPATCH
    if (g_have_avx2) pos = scan_blocks_avx2(w, buf, len, kw_limit, &newlines, &next_line);
    else             pos = scan_blocks_sse2(w, buf, len, kw_limit, &newlines, &next_line);
  #else
    pos = scan_blocks_sse2(w, buf, len, kw_limit, &newlines, &next_line);
  #endif

    /* Tail: fewer than one vector of bytes left (scalar). */
    for (size_t q = pos; q < len; q++) {
        if (buf[q] == '\n') newlines++;
    }
    if (kw_limit > pos) {
        for (size_t q = pos; q < kw_limit; q++) {
            if (!kw_first_ok(opt, (unsigned char)buf[q])) continue;
            if ((kl > 1 || opt->ignore_case) && !kw_match_at(opt, buf + q)) continue;
            handle_hit(w, buf, len, q, &next_line);
        }
    }
#else
    /* Portable scalar fallback. */
    {
        const char *p = buf, *end = buf + len;
        while (p < end) {
            const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
            if (!nl) break;
            newlines++;
            p = nl + 1;
        }
    }
    {
        size_t pos = 0;
        while (pos + kl <= len) {
            const char *hit = find_keyword(opt, buf + pos, len - pos);
            if (!hit) break;
            size_t p = (size_t)(hit - buf);
            handle_hit(w, buf, len, p, &next_line);
            pos = (next_line > p) ? next_line : p + 1;
        }
    }
#endif

    w->stats.lines_read += newlines + ((buf[len - 1] != '\n') ? 1u : 0u);
}

/* Sequential-read path over [start, end). */
static void read_range(Worker *w, FileHandle h, uint64_t start, uint64_t end) {
    char *buf = w->io_buf;
    const size_t cap = (size_t)FF_READ_BUF;
    uint64_t pos = start;
    size_t carry = 0;

    while (pos < end) {
        size_t want = (size_t)(end - pos);
        if (want > cap - carry) want = cap - carry;
        ptrdiff_t got = file_read_at(h, buf + carry, want, pos);
        if (got < 0) { w->stats.failed++; break; }
        if (got == 0) break;                    /* truncated under us */
        pos += (uint64_t)got;
        w->stats.bytes += (uint64_t)got;

        size_t total = carry + (size_t)got;

        /* End of region = just after the last newline (only complete lines
         * are processed; the rest is carried over to the next read). */
        size_t region = 0;
        for (size_t i = total; i > 0; i--) {
            if (buf[i - 1] == '\n') { region = i; break; }
        }

        if (region == 0 && total >= cap) {
            /* Pathological single line larger than the read buffer: process
             * the full buffer as one line rather than buffering forever. */
            scan_region(w, buf, total);
            carry = 0;
            continue;
        }

        if (region > 0) scan_region(w, buf, region);

        carry = total - region;
        if (carry > 0 && region > 0) {
            memmove(buf, buf + region, carry);
        }
    }

    if (carry > 0) scan_region(w, buf, carry);  /* trailing partial line */
}

/* Scan [start, end) of an open file: zero-copy mapping when worthwhile. */
static void scan_range(Worker *w, FileHandle h, uint64_t start, uint64_t end) {
    if (start >= end) return;
    uint64_t span64 = end - start;

    int can_map = !w->opt->no_mmap && span64 >= (uint64_t)FF_MMAP_MIN;
#if !defined(FF_ADDRESS_64)
    if (span64 > ((uint64_t)1u << 29)) can_map = 0;
#endif
#if SIZE_MAX < UINT64_MAX
    if (span64 > (uint64_t)SIZE_MAX) can_map = 0;
#endif

    if (can_map) {
        size_t span = (size_t)span64;
        Mapped m;
        if (map_range(h, start, end, &m) == 0) {
            int cached = range_looks_cached(&m);
#ifdef FF_DEBUG_CACHE
            fprintf(stderr, "cache: cached=%d span=%llu file=%s\n", cached,
                    (unsigned long long)span64, w->current_path);
#endif
            if (!cached) {
                /* Data is not in cache: streaming reads beat page-fault I/O. */
                unmap_any(m.base, m.len);
                read_range(w, h, start, end);
                return;
            }
            void *volatile vbase = m.base;
            size_t volatile vlen = m.len;
            const char *data = m.data;
#if defined(POSIX_MADV_SEQUENTIAL)
            (void)posix_madvise(m.base, m.len, POSIX_MADV_SEQUENTIAL);
#endif
            w->stats.bytes += span64;
#if !defined(_WIN32)
            if (sigsetjmp(g_mmap_jmp, 1) == 0) {
                g_mmap_guard = 1;
                scan_region(w, data, span);
                g_mmap_guard = 0;
            } else {
                /* File was truncated while mapped: stop this unit cleanly. */
                g_mmap_guard = 0;
                w->stats.failed++;
            }
#else
            scan_region(w, data, span);
#endif
            unmap_any((void *)vbase, (size_t)vlen);
            return;
        }
    }

    read_range(w, h, start, end);
}

/* First line start at or after `off` (i.e. the byte after the first '\n'
 * found at or after `off`). Returns `size` when there is none. */
static uint64_t align_line_start(FileHandle h, uint64_t off, uint64_t size) {
    if (off == 0) return 0;
    if (off >= size) return size;
    unsigned char win[64 * 1024];
    uint64_t pos = off;
    while (pos < size) {
        size_t want = (size_t)(size - pos);
        if (want > sizeof win) want = sizeof win;
        ptrdiff_t got = file_read_at(h, win, want, pos);
        if (got <= 0) return size;
        const unsigned char *nl =
            (const unsigned char *)memchr(win, '\n', (size_t)got);
        if (nl) return pos + (uint64_t)(nl - win) + 1;
        pos += (uint64_t)got;
    }
    return size;
}

/* Hand off one line-aligned segment; scan it locally if the queue is full. */
static void worker_push_segment(Worker *w, FileHandle h, const char *path,
                                uint64_t start, uint64_t end) {
    WorkUnit *u = (WorkUnit *)malloc(sizeof(WorkUnit));
    if (!u) die_oom();
    u->path = ff_strdup(path);
    u->start = start;
    u->end = end;
    u->count_file = 0;
    if (!queue_try_put(w->queue, u)) {
        scan_range(w, h, start, end);
        free(u->path);
        free(u);
    }
}

/* Split a big file into line-aligned segments so several workers can scan
 * it in parallel (the win for single huge dumps). */
static void worker_split_file(Worker *w, FileHandle h, const char *path,
                              uint64_t size) {
    uint64_t k = size / (uint64_t)FF_SEG_TARGET;
    uint64_t cap = (uint64_t)g_workers * 8u;
    if (k < 2) k = 2;
    if (cap < 2) cap = 2;
    if (k > cap) k = cap;
    if (k > 1024) k = 1024;

    uint64_t prev = 0;
    for (uint64_t i = 1; i < k; i++) {
        uint64_t nominal = (size / k) * i;
        uint64_t b = align_line_start(h, nominal, size);
        if (b > size) b = size;
        if (b > prev) {
            worker_push_segment(w, h, path, prev, b);
            prev = b;
        }
        if (prev >= size) break;
    }
    if (prev < size) worker_push_segment(w, h, path, prev, size);
}

static void worker_process_unit(Worker *w, WorkUnit *u) {
    w->current_path = u->path;

    FileHandle h = file_open(u->path);
    if (h == FF_INVALID_HANDLE) {
        w->stats.failed++;
        if (u->count_file) w->stats.files++;
        goto out;
    }

#if defined(POSIX_FADV_SEQUENTIAL)
    (void)posix_fadvise(h, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

    uint64_t size = 0;
    if (file_size(h, &size) != 0) {
        size = (u->end != FF_WHOLE) ? u->end : 0;
    }

    if (u->count_file && g_workers > 1 && !w->opt->no_split &&
        size >= (uint64_t)FF_SPLIT_MIN) {
        w->stats.files++;
        worker_split_file(w, h, u->path, size);
        file_close(h);
        goto out;
    }

    uint64_t start = (u->start <= size) ? u->start : size;
    uint64_t end = (u->end == FF_WHOLE) ? size : u->end;
    if (end > size) end = size;

    scan_range(w, h, start, end);
    file_close(h);
    if (u->count_file) w->stats.files++;

out:
    free(u->path);
    free(u);
    w->current_path = NULL;
}

static FF_THREAD_RET FF_THREAD_CALL worker_entry(void *arg) {
    Worker *w = (Worker *)arg;

#if defined(_WIN32)
    if (w->opt->low_priority) {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    }
#endif

    WorkUnit *u;
    while ((u = queue_get(w->queue)) != NULL) {
        worker_process_unit(w, u);
    }
    worker_flush(w);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

/* ------------------------------------------------------------------ */
/* Thread creation / join                                             */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)
typedef HANDLE ff_thread_t;

static int ff_thread_create(ff_thread_t *t,
                            FF_THREAD_RET (FF_THREAD_CALL *fn)(void *),
                            void *arg) {
    unsigned tid = 0;
    uintptr_t h = _beginthreadex(NULL, 0, fn, arg, 0, &tid);
    if (h == 0) return -1;
    *t = (HANDLE)h;
    return 0;
}

static void ff_thread_join(ff_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
#else
typedef pthread_t ff_thread_t;

static int ff_thread_create(ff_thread_t *t, void *(*fn)(void *), void *arg) {
    return pthread_create(t, NULL, fn, arg);
}

static void ff_thread_join(ff_thread_t t) {
    pthread_join(t, NULL);
}
#endif

/* ------------------------------------------------------------------ */
/* Output file protection (never scan the file we are writing to)     */
/* ------------------------------------------------------------------ */

static const char *g_out_name = NULL;           /* basename of output file */

#if defined(_WIN32)
static char *g_out_full = NULL;                 /* canonical output path */

static const char *win_strip_prefix(const char *p) {
    if (strncmp(p, "\\\\?\\UNC\\", 8) == 0) return p + 8;
    if (strncmp(p, "\\\\?\\", 4) == 0) return p + 4;
    return p;
}

static int is_output_file(const char *path) {
    if (!g_out_full || !g_out_name) return 0;
    if (ff_stricmp(path_basename(path), g_out_name) != 0) return 0;
    return ff_stricmp(win_strip_prefix(path), win_strip_prefix(g_out_full)) == 0;
}
#else
static dev_t g_out_dev;
static ino_t g_out_ino;
static int   g_out_id_ok = 0;

/* Compare by device + inode: catches hardlinks and symlinked paths. */
static int is_output_file(const char *path) {
    if (!g_out_id_ok || !g_out_name) return 0;
    if (strcmp(path_basename(path), g_out_name) != 0) return 0;
    struct stat sb;
    if (stat(path, &sb) != 0) return 0;
    return sb.st_dev == g_out_dev && sb.st_ino == g_out_ino;
}
#endif

/* ------------------------------------------------------------------ */
/* Directory walk (producer)                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned long long dirs;
    unsigned long long dir_errors;
    unsigned long long queued;
} ProducerStats;

static void enqueue_candidate(FileQueue *q, const char *dir, const char *name,
                              const Options *opt, ProducerStats *st) {
    if (!ext_ok(name, opt)) return;
    char *full = path_join(dir, name);
    if (is_output_file(full)) { free(full); return; }
    st->queued++;
    queue_put(q, ff_work_unit(full));
}

#if defined(_WIN32)

static char *win_normalize_path(const char *path) {
    DWORD need = GetFullPathNameA(path, 0, NULL, NULL);
    char *abs = NULL;
    if (need == 0) {
        abs = ff_strdup(path);
    } else {
        abs = (char *)malloc((size_t)need + 1);
        if (!abs) die_oom();
        if (GetFullPathNameA(path, need + 1, abs, NULL) == 0) {
            free(abs);
            abs = ff_strdup(path);
        }
    }

    for (char *p = abs; *p; p++) {
        if (*p == '/') *p = '\\';
    }

    /* Prefix with \\?\ so paths longer than MAX_PATH keep working. */
    size_t len = strlen(abs);
    if (len >= 240) {
        int unc = (abs[0] == '\\' && abs[1] == '\\');
        const char *prefix = unc ? "\\\\?\\UNC\\" : "\\\\?\\";
        const char *body = unc ? abs + 2 : abs;
        size_t pl = strlen(prefix), bl = strlen(body);
        char *lp = (char *)malloc(pl + bl + 1);
        if (!lp) die_oom();
        memcpy(lp, prefix, pl);
        memcpy(lp + pl, body, bl + 1);
        free(abs);
        abs = lp;
    }
    return abs;
}

static void scan_path(const char *root, FileQueue *q, const Options *opt,
                      ProducerStats *st) {
    DWORD attrs = GetFileAttributesA(root);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "fastfinder: cannot access '%s' (error %lu)\n",
                root, (unsigned long)GetLastError());
        exit(2);
    }

    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!is_output_file(root)) {
            st->queued++;
            queue_put(q, ff_work_unit(ff_strdup(root)));
        }
        return;
    }

    PathStack stack;
    memset(&stack, 0, sizeof stack);
    stack_push(&stack, ff_strdup(root));

    char *dir;
    while ((dir = stack_pop(&stack)) != NULL) {
        st->dirs++;

        char *pattern = path_join(dir, "*");
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        free(pattern);
        if (h == INVALID_HANDLE_VALUE) { st->dir_errors++; free(dir); continue; }

        do {
            const char *name = fd.cFileName;
            if (name[0] == '.' &&
                (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                stack_push(&stack, path_join(dir, name));
            } else {
                enqueue_candidate(q, dir, name, opt, st);
            }
        } while (FindNextFileA(h, &fd));

        FindClose(h);
        free(dir);
    }
    free(stack.items);
}

#else   /* POSIX */

static void scan_path(const char *root, FileQueue *q, const Options *opt,
                      ProducerStats *st) {
    struct stat sb;
    if (stat(root, &sb) != 0) {
        fprintf(stderr, "fastfinder: cannot access '%s': %s\n", root, strerror(errno));
        exit(2);
    }

    if (S_ISREG(sb.st_mode)) {
        if (!is_output_file(root)) {
            st->queued++;
            queue_put(q, ff_work_unit(ff_strdup(root)));
        }
        return;
    }
    if (!S_ISDIR(sb.st_mode)) {
        fprintf(stderr, "fastfinder: '%s' is neither a regular file nor a directory\n", root);
        exit(2);
    }

    PathStack stack;
    memset(&stack, 0, sizeof stack);
    stack_push(&stack, ff_strdup(root));

    char *dir;
    while ((dir = stack_pop(&stack)) != NULL) {
        st->dirs++;
        DIR *d = opendir(dir);
        if (!d) { st->dir_errors++; free(dir); continue; }

        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *name = e->d_name;
            if (name[0] == '.' &&
                (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;

            unsigned char t;
#if defined(_DIRENT_HAVE_D_TYPE)
            t = (unsigned char)e->d_type;
#else
            t = DT_UNKNOWN;
#endif

            if (t == DT_DIR) {
                stack_push(&stack, path_join(dir, name));
            } else if (t == DT_REG) {
                enqueue_candidate(q, dir, name, opt, st);
            } else if (t == DT_LNK) {
                /* Symlinks are not followed (grep -r / ripgrep default):
                 * avoids duplicate content and directory loops. */
                continue;
            } else if (t == DT_UNKNOWN) {
                char *full = path_join(dir, name);
                struct stat ls;
                if (lstat(full, &ls) == 0) {
                    if (S_ISDIR(ls.st_mode)) {
                        stack_push(&stack, full);
                    } else if (S_ISREG(ls.st_mode)) {
                        if (!is_output_file(full) && ext_ok(name, opt)) {
                            st->queued++;
                            queue_put(q, ff_work_unit(full));
                        } else {
                            free(full);
                        }
                    } else {
                        free(full);
                    }
                } else {
                    free(full);
                }
            }
            /* Symlinks, FIFOs, sockets and devices are skipped. */
        }

        closedir(d);
        free(dir);
    }
    free(stack.items);
}

#endif

/* ------------------------------------------------------------------ */
/* Misc platform bits                                                 */
/* ------------------------------------------------------------------ */

static int detect_cpu_count(void) {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
    return n > 0 ? n : 4;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 4;
#endif
}

static double now_seconds(void) {
#if defined(_WIN32)
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static void format_size(double v, char *out, size_t outlen) {
    static const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    int u = 0;
    while (v >= 1000.0 && u < 4) { v /= 1000.0; u++; }
    snprintf(out, outlen, "%.2f %s", v, units[u]);
}

static void print_help(FILE *f) {
    fprintf(f,
        "FastFinder " FF_VERSION " - fast multi-threaded keyword search for text files\n"
        "\n"
        "Usage:\n"
        "  fastfinder [options] <file|directory> <keyword> <output>\n"
        "\n"
        "Options:\n"
        "  -t, --threads N     Worker threads (default: CPU cores, max 128)\n"
        "  -i, --ignore-case   Case-insensitive matching (ASCII)\n"
        "  -a, --all-files     Search every file (default: only *.txt)\n"
        "  -f, --show-file     Prefix every match with the file path\n"
        "  -l, --low-priority  Lower CPU/IO priority (friendlier to the system)\n"
        "      --no-mmap       Disable memory-mapped I/O (use plain reads)\n"
        "      --no-split      Disable parallel splitting of big files\n"
        "  -h, --help          Show this help\n"
        "  -v, --version       Show version\n"
        "\n"
        "Examples:\n"
        "  fastfinder /home/user/dump \"password\" results.txt\n"
        "  fastfinder -i -a -t 12 C:\\Users\\user\\Downloads \"api_key\" leaks.txt\n");
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    Options opt;
    memset(&opt, 0, sizeof opt);

    const char *positional[3];
    int npos = 0;
    long threads = 0;
    int no_flags = 0;

    /* ---- argument parsing ---- */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!no_flags && strcmp(a, "--") == 0) { no_flags = 1; continue; }

        if (!no_flags && a[0] == '-' && a[1] != '\0') {
            if (strcmp(a, "-t") == 0 || strcmp(a, "--threads") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "fastfinder: missing value for %s\n\n", a);
                    print_help(stderr);
                    return 1;
                }
                char *end = NULL;
                long v = strtol(argv[++i], &end, 10);
                if (!end || end == argv[i] || *end != '\0' || v < 1 || v > FF_MAX_THREADS) {
                    fprintf(stderr, "fastfinder: invalid thread count (1-%d)\n", FF_MAX_THREADS);
                    return 1;
                }
                threads = v;
            } else if (strncmp(a, "--threads=", 10) == 0) {
                char *end = NULL;
                long v = strtol(a + 10, &end, 10);
                if (!end || end == a + 10 || *end != '\0' || v < 1 || v > FF_MAX_THREADS) {
                    fprintf(stderr, "fastfinder: invalid thread count (1-%d)\n", FF_MAX_THREADS);
                    return 1;
                }
                threads = v;
            } else if (strcmp(a, "-i") == 0 || strcmp(a, "--ignore-case") == 0) {
                opt.ignore_case = 1;
            } else if (strcmp(a, "-a") == 0 || strcmp(a, "--all-files") == 0) {
                opt.all_files = 1;
            } else if (strcmp(a, "-f") == 0 || strcmp(a, "--show-file") == 0) {
                opt.show_file = 1;
            } else if (strcmp(a, "-l") == 0 || strcmp(a, "--low-priority") == 0) {
                opt.low_priority = 1;
            } else if (strcmp(a, "--no-mmap") == 0) {
                opt.no_mmap = 1;
            } else if (strcmp(a, "--no-split") == 0) {
                opt.no_split = 1;
            } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
                print_help(stdout);
                return 0;
            } else if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
                printf("FastFinder %s\n", FF_VERSION);
                return 0;
            } else {
                fprintf(stderr, "fastfinder: unknown option '%s'\n\n", a);
                print_help(stderr);
                return 1;
            }
        } else {
            if (npos == 3) {
                fprintf(stderr, "fastfinder: too many arguments\n\n");
                print_help(stderr);
                return 1;
            }
            positional[npos++] = a;
        }
    }

    if (npos != 3) {
        print_help(stderr);
        return 1;
    }

    opt.keyword = positional[1];
    opt.keyword_len = strlen(opt.keyword);
    if (opt.keyword_len == 0) {
        fprintf(stderr, "fastfinder: keyword must not be empty\n");
        return 1;
    }
    if (strchr(opt.keyword, '\n') != NULL) {
        fprintf(stderr, "fastfinder: keyword must not contain a newline\n");
        return 1;
    }
    if (opt.keyword_len + 1 > (size_t)FF_READ_BUF) {
        fprintf(stderr, "fastfinder: keyword is too long\n");
        return 1;
    }
    if (opt.ignore_case) {
        char *folded = (char *)malloc(opt.keyword_len + 1);
        if (!folded) die_oom();
        for (size_t i = 0; i < opt.keyword_len; i++) {
            folded[i] = (char)ascii_lower((unsigned char)opt.keyword[i]);
        }
        folded[opt.keyword_len] = '\0';
        opt.keyword_folded = folded;
    }

    /* ---- normalize input root / output path ---- */
    char *root;
#if defined(_WIN32)
    root = win_normalize_path(positional[0]);
    g_out_full = win_normalize_path(positional[2]);
    g_out_name = path_basename(g_out_full);
#else
    root = ff_strdup(positional[0]);
    g_out_name = path_basename(positional[2]);
#endif

    FILE *out = fopen(positional[2], "wb");
    if (!out) {
        fprintf(stderr, "fastfinder: cannot create output file '%s': %s\n",
                positional[2], strerror(errno));
        return 1;
    }
    static char out_stdio_buf[1 << 20];
    setvbuf(out, out_stdio_buf, _IOFBF, sizeof out_stdio_buf);

#if !defined(_WIN32)
    {
        struct stat ost;
        if (fstat(fileno(out), &ost) == 0) {
            g_out_dev = ost.st_dev;
            g_out_ino = ost.st_ino;
            g_out_id_ok = 1;
        }
    }
    /* Recover gracefully if a mapped file is truncated while being scanned. */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = ff_sigbus_handler;
        sigemptyset(&sa.sa_mask);
        (void)sigaction(SIGBUS, &sa, NULL);
    }
#endif

    /* ---- optional low priority ---- */
    if (opt.low_priority) {
#if defined(_WIN32)
        (void)SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#else
        (void)nice(10);
#if defined(__linux__) && defined(SYS_ioprio_set)
        /* IOPRIO_WHO_PROCESS, own pid, class BE(2) level 7 */
        (void)syscall(SYS_ioprio_set, 1, 0, (int)((2 << 13) | 7));
#endif
#endif
    }

    /* ---- runtime CPU dispatch ---- */
#if FF_AVX2_DISPATCH
    __builtin_cpu_init();
    g_have_avx2 = __builtin_cpu_supports("avx2") ? 1 : 0;
#endif

    /* ---- workers ---- */
    int num_threads = (int)(threads > 0 ? threads : (long)detect_cpu_count());
    if (num_threads > FF_MAX_THREADS) num_threads = FF_MAX_THREADS;
    if (num_threads < 1) num_threads = 1;
    g_workers = num_threads;

    FileQueue queue;
    queue_init(&queue, FF_QUEUE_CAP);

    Sync out_lock;
    sync_init(&out_lock);

    Worker *workers = (Worker *)calloc((size_t)num_threads, sizeof(Worker));
    ff_thread_t *thread_handles = (ff_thread_t *)calloc((size_t)num_threads, sizeof(ff_thread_t));
    if (!workers || !thread_handles) die_oom();

    for (int i = 0; i < num_threads; i++) {
        Worker *w = &workers[i];
        w->queue = &queue;
        w->opt = &opt;
        w->out = out;
        w->out_lock = &out_lock;
        w->io_buf = (char *)malloc(FF_READ_BUF);
        w->match_buf = (char *)malloc(FF_MATCH_BUF);
        if (!w->io_buf || !w->match_buf) die_oom();
        w->match_cap = FF_MATCH_BUF;
        w->match_len = 0;
    }

    fprintf(stderr,
            "fastfinder: searching for '%s' in %s (%d threads%s%s)\n",
            opt.keyword, root, num_threads,
            opt.ignore_case ? ", case-insensitive" : "",
            opt.low_priority ? ", low priority" : "");

    int spawned = 0;
    for (int i = 0; i < num_threads; i++) {
        if (ff_thread_create(&thread_handles[i], worker_entry, &workers[i]) != 0) {
            fprintf(stderr, "fastfinder: warning: could not create worker thread %d\n", i);
            break;
        }
        spawned++;
    }
    if (spawned == 0) {
        fprintf(stderr, "fastfinder: failed to create any worker thread\n");
        queue_destroy(&queue);
        sync_destroy(&out_lock);
        fclose(out);
        return 1;
    }

    /* ---- produce (walk the tree) ---- */
    double t0 = now_seconds();
    ProducerStats pst;
    memset(&pst, 0, sizeof pst);
    scan_path(root, &queue, &opt, &pst);

    queue_finish(&queue);
    for (int i = 0; i < spawned; i++) {
        ff_thread_join(thread_handles[i]);
    }
    double elapsed = now_seconds() - t0;

    /* ---- aggregate ---- */
    unsigned long long files = 0, bytes = 0, lines_read = 0, matched = 0, failed = 0;
    int write_error = 0;
    for (int i = 0; i < spawned; i++) {
        files      += workers[i].stats.files;
        bytes      += workers[i].stats.bytes;
        lines_read += workers[i].stats.lines_read;
        matched    += workers[i].stats.lines_matched;
        failed     += workers[i].stats.failed;
        write_error |= workers[i].write_error;
    }
    if (fclose(out) != 0) write_error = 1;

    /* ---- report ---- */
    char data_sz[32], rate_sz[32];
    format_size((double)bytes, data_sz, sizeof data_sz);
    format_size(elapsed > 0.0 ? (double)bytes / elapsed : 0.0, rate_sz, sizeof rate_sz);

#if defined(__AVX2__)
    const char *simd_name = "AVX2";
#elif FF_AVX2_DISPATCH
    const char *simd_name = g_have_avx2 ? "AVX2" : "SSE2";
#elif FF_X86_SSE2
    const char *simd_name = "SSE2";
#else
    const char *simd_name = "scalar";
#endif

    printf("\nFastFinder %s\n", FF_VERSION);
    printf("  Root          : %s\n", root);
    printf("  Keyword       : %s%s\n", opt.keyword,
           opt.ignore_case ? "  (case-insensitive)" : "");
    printf("  Threads       : %d\n", spawned);
    printf("  Engine        : %s SIMD, %s%s\n", simd_name,
           opt.no_mmap ? "plain reads" : "mmap/reads",
           opt.no_split ? ", no split" : ", auto-split");
    printf("  Directories   : %llu", pst.dirs);
    if (pst.dir_errors) printf(" (%llu unreadable)", pst.dir_errors);
    printf("\n");
    printf("  Files scanned : %llu", files);
    if (failed) printf(" (%llu unreadable)", failed);
    printf("\n");
    printf("  Data read     : %s in %.2f s (%s/s)\n", data_sz, elapsed, rate_sz);
    printf("  Lines read    : %llu\n", lines_read);
    printf("  Lines matched : %llu\n", matched);
    printf("  Output        : %s\n", positional[2]);

    if (write_error) {
        fprintf(stderr, "fastfinder: error while writing the output file\n");
    }

    /* ---- cleanup ---- */
    for (int i = 0; i < num_threads; i++) {
        free(workers[i].io_buf);
        free(workers[i].match_buf);
    }
    free(workers);
    free(thread_handles);
    queue_destroy(&queue);
    sync_destroy(&out_lock);
    free(opt.keyword_folded);
    free(root);
#if defined(_WIN32)
    free(g_out_full);
#endif

    return write_error ? 1 : 0;
}

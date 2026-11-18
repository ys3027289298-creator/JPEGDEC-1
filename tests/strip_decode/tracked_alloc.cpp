//
// Heap instrumentation: intercepts malloc/calloc/realloc/free via the
// linker's --wrap mechanism and tracks live/peak bytes. Every allocation
// gets a 16-byte header holding the requested size so free() can account
// for it in O(1).
//
// Global operator new/delete are also overridden: --wrap only rewrites
// references from the objects being linked, so libstdc++'s internal
// operator new would otherwise reach libc malloc directly and C++ heap
// allocations (e.g. the JPEGDEC decoder object) would go uncounted.
//
// Link with: -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
//
#include "tracked_alloc.h"
#include <new>
#include <string.h>

extern "C" {
extern void *__real_malloc(size_t);
extern void *__real_realloc(void *, size_t);
extern void  __real_free(void *);
}

static size_t ta_live_bytes = 0;
static size_t ta_peak_bytes = 0;

void ta_reset(void)
{
    ta_live_bytes = 0;
    ta_peak_bytes = 0;
}
size_t ta_live(void) { return ta_live_bytes; }
size_t ta_peak(void) { return ta_peak_bytes; }

static void ta_add(size_t n)
{
    ta_live_bytes += n;
    if (ta_live_bytes > ta_peak_bytes) ta_peak_bytes = ta_live_bytes;
}
static void ta_sub(size_t n)
{
    ta_live_bytes = (n > ta_live_bytes) ? 0 : ta_live_bytes - n;
}

extern "C" void *__wrap_malloc(size_t n)
{
    size_t *p = (size_t *)__real_malloc(n + 16);
    if (!p) return 0;
    *p = n;
    ta_add(n);
    return p + 2; // 16-byte header keeps the returned pointer 16-aligned
}

extern "C" void __wrap_free(void *q)
{
    size_t *p;
    if (!q) return;
    p = ((size_t *)q) - 2;
    ta_sub(*p);
    __real_free(p);
}

extern "C" void *__wrap_calloc(size_t n, size_t sz)
{
    size_t total = n * sz;
    void *p = __wrap_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

extern "C" void *__wrap_realloc(void *q, size_t n)
{
    size_t *p, *np;
    size_t old;
    if (!q) return __wrap_malloc(n);
    p = ((size_t *)q) - 2;
    old = *p;
    np = (size_t *)__real_realloc(p, n + 16);
    if (!np) return 0;
    *np = n;
    ta_sub(old);
    ta_add(n);
    return np + 2;
}

// Route C++ heap allocations through the tracked allocator as well.
void *operator new(size_t n) { return __wrap_malloc(n); }
void *operator new[](size_t n) { return __wrap_malloc(n); }
void operator delete(void *p) noexcept { __wrap_free(p); }
void operator delete[](void *p) noexcept { __wrap_free(p); }
void operator delete(void *p, size_t) noexcept { __wrap_free(p); }
void operator delete[](void *p, size_t) noexcept { __wrap_free(p); }

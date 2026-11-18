//
// Heap instrumentation API - see tracked_alloc.c
//
#ifndef TRACKED_ALLOC_H
#define TRACKED_ALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void   ta_reset(void); // zero live/peak counters
size_t ta_live(void);  // bytes currently allocated through the wrapped allocator
size_t ta_peak(void);  // high-water mark of live bytes since the last reset

#ifdef __cplusplus
}
#endif

#endif // TRACKED_ALLOC_H

#pragma once
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned char* base;
    size_t cap, off;
} Arena;

static inline void arena_init(Arena* a, void* mem, size_t cap)
{
    a->base = (unsigned char*)mem;
    a->cap = cap;
    a->off = 0;
}

static inline void* arena_alloc(Arena* a, size_t n, size_t align)
{
    size_t a0 = align ? align : alignof(max_align_t);
    if (a0 == 0 || (a0 & (a0 - 1)) != 0)
        return NULL; // require power-of-two alignment

    size_t p = a->off;
    if (a0 > 1) {
        size_t mask = a0 - 1;
        size_t rem = p & mask;
        if (rem) {
            size_t add = a0 - rem;
            if (SIZE_MAX - p < add)
                return NULL; // overflow
            p += add;
        }
    }

    if (p > a->cap)            // NEW: guard aligned offset
        return NULL;
    if (n > a->cap - p)        // capacity check without wrap
        return NULL;

    void* ptr = a->base + p;
    a->off = p + n;
    return ptr;
}

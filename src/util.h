#pragma once

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

// Arena allocator
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

    if (p > a->cap)
        return NULL;
    if (n > a->cap - p)        // capacity check without wrap
        return NULL;

    void* ptr = a->base + p;
    a->off = p + n;
    return ptr;
}

// Safe alignment utility
static inline size_t safe_align(size_t x, size_t align)
{
    if (align <= 1)
        return x;
    size_t rem = x % align;
    if (rem == 0)
        return x;
    size_t add = align - rem;
    if (SIZE_MAX - x < add)
        return SIZE_MAX;
    return x + add;
}

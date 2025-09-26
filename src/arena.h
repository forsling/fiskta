#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdalign.h>

typedef struct { unsigned char *base; size_t cap, off; } Arena;

static inline size_t a_align(size_t x, size_t a){ return (x + (a-1)) & ~(a-1); }
static inline void arena_init(Arena* a, void* mem, size_t cap){ a->base=(unsigned char*)mem; a->cap=cap; a->off=0; }
static inline void* arena_alloc(Arena* a, size_t n, size_t align){
    size_t p = a_align(a->off, align ? align : alignof(max_align_t));
    if (p + n > a->cap) return NULL;
    void* ptr = a->base + p; a->off = p + n; return ptr;
}

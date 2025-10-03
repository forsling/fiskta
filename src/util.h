#pragma once

#include <stddef.h>
#include <stdint.h>

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

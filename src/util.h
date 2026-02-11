#pragma once

#include "fiskta_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned char* base;
    size_t cap;
    size_t off;
} Arena;

void arena_init(Arena* a, void* mem, size_t cap);
void* arena_alloc(Arena* a, size_t n, size_t align);
size_t safe_align(size_t x, size_t align);
int add_overflow(size_t a, size_t b, size_t* out);
int mul_overflow(size_t a, size_t b, size_t* out);
void sleep_msec(int msec);

// Clamp value to range [lo, hi]
static inline i64 clamp64(i64 x, i64 lo, i64 hi)
{
    if (x < lo) {
        return lo;
    }
    if (x > hi) {
        return hi;
    }
    return x;
}

bool string_eq(FisktaString a, FisktaString b);
bool string_eq_cstr(FisktaString s, const char* literal);
char string_first(FisktaString s);
FisktaString string_from_cstr(const char* s);

FisktaString parse_string_to_bytes(FisktaString str, char* str_pool, size_t* str_pool_off, size_t str_pool_cap, enum FisktaErr* err_out, i32* cursor_marks_out, i32* cursor_offsets_out);
FisktaString parse_hex_to_bytes(FisktaString hex_str, char* str_pool, size_t* str_pool_off, size_t str_pool_cap, enum FisktaErr* err_out);

// Parser-specific string helpers
bool string_try_parse_unsigned(FisktaString s, u64* out, FisktaUnit* unit);
bool string_try_parse_signed(FisktaString s, i64* out, FisktaUnit* unit);
bool string_copy_to_buffer(FisktaString src, char* dst, size_t dst_cap);
bool string_is_valid_label(FisktaString s);
bool string_char_in_set(char c, const char* set);

// String escape processing
size_t calculate_escaped_string_length(FisktaString str);

// Token handling optimizations
void convert_tokens_to_strings(char** tokens, i32 token_count, FisktaString* out);

// Tokenize whitespace-separated operations string into FisktaString array
//
// Caller provides scratch buffer. Returned Strings point into that buffer.
//
// Returns: Number of tokens parsed, or -1 if buffer capacity exceeded
i32 tokenize_ops_string(const char* s, FisktaString* out, i32 max_tokens, char* scratch_buf, size_t scratch_cap);

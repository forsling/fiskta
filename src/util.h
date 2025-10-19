#pragma once

#include "fiskta.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PRINT_CURSOR_SENTINEL '\x01'

typedef struct {
    unsigned char* base;
    size_t cap;
    size_t off;
} Arena;

void arena_init(Arena* a, void* mem, size_t cap);
void* arena_alloc(Arena* a, size_t n, size_t align);
size_t safe_align(size_t x, size_t align);
int add_overflow(size_t a, size_t b, size_t* out);
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

bool string_eq(String a, String b);
bool string_eq_cstr(String s, const char* literal);
char string_first(String s);
String string_from_cstr(const char* s);

String parse_string_to_bytes(String str, char* str_pool, size_t* str_pool_off, size_t str_pool_cap, enum Err* err_out, i32* cursor_marks_out);
String parse_hex_to_bytes(String hex_str, char* str_pool, size_t* str_pool_off, size_t str_pool_cap, enum Err* err_out);

// Parser-specific String helpers
bool string_try_parse_unsigned(String s, u64* out, Unit* unit);
bool string_try_parse_signed(String s, i64* out, Unit* unit);
bool string_copy_to_buffer(String src, char* dst, size_t dst_cap);
bool string_is_valid_label(String s);
bool string_char_in_set(char c, const char* set);

// String escape processing
size_t calculate_escaped_string_length(String str);

// Token handling optimizations
void convert_tokens_to_strings(char** tokens, i32 token_count, String* out);
i32 tokenize_ops_string(const char* s, String* out, i32 max_tokens);

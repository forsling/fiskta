#pragma once

#include "fiskta.h"
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
void sleep_msec(int msec);

bool string_eq(String a, String b);
bool string_eq_cstr(String s, const char* literal);
char string_first(String s);
String string_from_cstr(const char* s);

String parse_string_to_bytes(String str, char* str_pool, size_t* str_pool_off, size_t str_pool_cap, enum Err* err_out);
String parse_hex_to_bytes(String hex_str, char* str_pool, size_t* str_pool_off, size_t str_pool_cap, enum Err* err_out);

// Parser-specific String helpers
bool string_try_parse_unsigned(String s, u64* out, Unit* unit);
bool string_try_parse_signed(String s, i64* out, Unit* unit);
bool string_copy_to_buffer(String src, char* dst, size_t dst_cap);
bool string_is_valid_label(String s);
bool string_char_in_set(char c, const char* set);

// Token handling optimizations
void convert_tokens_to_strings(char** tokens, i32 token_count, String* out);
bool string_eq_keyword(String s, const String* keyword);
i32 split_ops_string_optimized(const char* s, String* out, i32 max_tokens);

// Precomputed keyword Strings for fast comparison
extern const String KW_THEN;
extern const String KW_OR;
extern const String KW_TO;
extern const String KW_AT;
extern const String KW_LEN;
extern const String KW_FIND;
extern const String KW_FIND_RE;
extern const String KW_FIND_BIN;
extern const String KW_SKIP;
extern const String KW_TAKE;
extern const String KW_UNTIL;
extern const String KW_UNTIL_RE;
extern const String KW_UNTIL_BIN;
extern const String KW_LABEL;
extern const String KW_GOTO;
extern const String KW_VIEW;
extern const String KW_CLEAR;
extern const String KW_PRINT;
extern const String KW_ECHO;
extern const String KW_FAIL;
extern const String KW_CURSOR;
extern const String KW_BOF;
extern const String KW_EOF;
extern const String KW_MATCH_START;
extern const String KW_MATCH_END;
extern const String KW_LINE_START;
extern const String KW_LINE_END;

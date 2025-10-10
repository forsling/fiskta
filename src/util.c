#define _POSIX_C_SOURCE 199309L
#include "util.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdalign.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

void arena_init(Arena* a, void* mem, size_t cap)
{
    if (!a) {
        return;
    }
    a->base = (unsigned char*)mem;
    a->cap = cap;
    a->off = 0;
}

void* arena_alloc(Arena* a, size_t n, size_t align)
{
    if (!a) {
        return NULL;
    }

    size_t a0 = align ? align : alignof(max_align_t);
    if (a0 == 0 || (a0 & (a0 - 1)) != 0) {
        return NULL;
    }

    size_t p = a->off;
    if (a0 > 1) {
        size_t mask = a0 - 1;
        size_t rem = p & mask;
        if (rem) {
            size_t add = a0 - rem;
            if (SIZE_MAX - p < add) {
                return NULL;
            }
            p += add;
        }
    }

    if (p > a->cap || n > a->cap - p) {
        return NULL;
    }

    void* ptr = a->base + p;
    a->off = p + n;
    return ptr;
}

size_t safe_align(size_t x, size_t align)
{
    if (align <= 1) {
        return x;
    }
    size_t rem = x % align;
    if (rem == 0) {
        return x;
    }
    size_t add = align - rem;
    if (SIZE_MAX - x < add) {
        return SIZE_MAX;
    }
    return x + add;
}

int add_overflow(size_t a, size_t b, size_t* out)
{
    if (SIZE_MAX - a < b) {
        return 1;
    }
    if (out) {
        *out = a + b;
    }
    return 0;
}

void sleep_msec(int msec)
{
    if (msec <= 0) {
        return;
    }

#ifdef _WIN32
    Sleep((DWORD)msec);
#else
    struct timespec req;
    req.tv_sec = msec / 1000;
    req.tv_nsec = (long)(msec % 1000) * 1000000L;
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
    }
#endif
}

bool string_eq(String a, String b)
{
    if (a.len != b.len) {
        return false;
    }
    if (a.len == 0) {
        return true;
    }
    if (!a.bytes || !b.bytes) {
        return false;
    }
    return memcmp(a.bytes, b.bytes, (size_t)a.len) == 0;
}

bool string_eq_cstr(String s, const char* literal)
{
    if (!literal) {
        return s.len == 0;
    }
    return string_eq(s, string_from_cstr(literal));
}

char string_first(String s)
{
    if (!s.bytes || s.len <= 0) {
        return '\0';
}
    return s.bytes[0];
}

String string_from_cstr(const char* s)
{
    if (!s) {
        return (String) { NULL, 0 };
    }
    size_t len = strlen(s);
    if (len > INT32_MAX) {
        len = INT32_MAX;
    }
    return (String) { s, (i32)len };
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

String parse_hex_to_bytes(String hex_str, char* str_pool, size_t* str_pool_off, size_t str_pool_cap, enum Err* err_out)
{
    String out = { 0 };
    if (err_out) {
        *err_out = E_OK;
    }

    if (hex_str.len < 0) {
        goto bad_hex;
    }

    size_t hex_digit_count = 0;
    for (i32 i = 0; i < hex_str.len; ++i) {
        unsigned char c = (unsigned char)hex_str.bytes[i];
        if (isspace(c)) {
            continue;
        }
        if (hex_value((char)c) < 0) {
            goto bad_hex;
        }
        hex_digit_count++;
    }

    if (hex_digit_count == 0 || (hex_digit_count % 2) != 0) {
        goto bad_hex;
    }

    size_t byte_count = hex_digit_count / 2;
    if (*str_pool_off + byte_count > str_pool_cap) {
        if (err_out) {
            *err_out = E_OOM;
        }
        return out;
    }

    char* dst = str_pool + *str_pool_off;
    size_t dst_pos = 0;
    int pending_nibble = -1;

    for (i32 i = 0; i < hex_str.len; ++i) {
        unsigned char c = (unsigned char)hex_str.bytes[i];
        if (isspace(c)) {
            continue;
        }

        int nibble = hex_value((char)c);
        if (nibble < 0) {
            goto bad_hex;
        }

        if (pending_nibble < 0) {
            pending_nibble = nibble;
        } else {
            dst[dst_pos++] = (char)((pending_nibble << 4) | nibble);
            pending_nibble = -1;
        }
    }

    *str_pool_off += byte_count;
    out.bytes = dst;
    out.len = (i32)byte_count;
    return out;

bad_hex:
    if (err_out) {
        *err_out = E_BAD_HEX;
    }
    return out;
}

String parse_string_to_bytes(String str, char* str_pool, size_t* str_pool_off, size_t str_pool_cap, enum Err* err_out)
{
    String out = { 0 };
    if (err_out) {
        *err_out = E_OK;
    }

    if (str.len < 0) {
        return out;
    }

    size_t src_len = (size_t)str.len;
    size_t dst_len = 0;

    for (size_t i = 0; i < src_len; i++) {
        if (str.bytes[i] == '\\' && i + 1 < src_len) {
            char esc = str.bytes[i + 1];
            if (esc == 'n' || esc == 't' || esc == 'r' || esc == '0' || esc == '\\') {
                dst_len++;
                i++;
                continue;
            }
            if (esc == 'x') {
                if (i + 3 >= src_len) {
                    goto parse_err;
                }
                int hi = hex_value(str.bytes[i + 2]);
                int lo = hex_value(str.bytes[i + 3]);
                if (hi < 0 || lo < 0) {
                    goto parse_err;
                }
                dst_len++;
                i += 3;
                continue;
            }
            dst_len++;
            continue;
        }
        dst_len++;
    }

    if (*str_pool_off + dst_len > str_pool_cap) {
        if (err_out) {
            *err_out = E_OOM;
        }
        return out;
    }

    char* dst = str_pool + *str_pool_off;
    size_t dst_pos = 0;

    for (size_t i = 0; i < src_len; i++) {
        if (str.bytes[i] == '\\' && i + 1 < src_len) {
            char esc = str.bytes[i + 1];
            if (esc == 'n') {
                dst[dst_pos++] = '\n';
                i++;
                continue;
            }
            if (esc == 't') {
                dst[dst_pos++] = '\t';
                i++;
                continue;
            }
            if (esc == 'r') {
                dst[dst_pos++] = '\r';
                i++;
                continue;
            }
            if (esc == '0') {
                dst[dst_pos++] = '\0';
                i++;
                continue;
            }
            if (esc == '\\') {
                dst[dst_pos++] = '\\';
                i++;
                continue;
            }
            if (esc == 'x') {
                if (i + 3 >= src_len) {
                    goto parse_err;
                }
                int hi = hex_value(str.bytes[i + 2]);
                int lo = hex_value(str.bytes[i + 3]);
                if (hi < 0 || lo < 0) {
                    goto parse_err;
                }
                dst[dst_pos++] = (char)((hi << 4) | lo);
                i += 3;
                continue;
            }
            dst[dst_pos++] = '\\';
            continue;
        }
        dst[dst_pos++] = str.bytes[i];
    }

    *str_pool_off += dst_len;
    out.bytes = dst;
    out.len = (i32)dst_len;
    return out;

parse_err:
    if (err_out) {
        *err_out = E_PARSE;
    }
    return (String) { 0 };
}

// Parser-specific String helpers

bool string_char_in_set(char c, const char* set)
{
    if (!set) {
        return false;
    }
    for (const char* p = set; *p; p++) {
        if (*p == c) {
            return true;
        }
    }
    return false;
}

bool string_is_valid_label(String s)
{
    if (s.len <= 0 || s.len > 16 || !s.bytes) {
        return false;
    }

    // First character must be A-Z
    if (s.bytes[0] < 'A' || s.bytes[0] > 'Z') {
        return false;
    }

    // Remaining characters must be A-Z, 0-9, _, or -
    for (i32 i = 1; i < s.len; i++) {
        char c = s.bytes[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }

    return true;
}

bool string_copy_to_buffer(String src, char* dst, size_t dst_cap)
{
    if (!dst || dst_cap == 0 || !src.bytes) {
        return false;
    }

    size_t copy_len = (size_t)src.len;
    if (copy_len >= dst_cap) {
        return false; // Would truncate
    }

    memcpy(dst, src.bytes, copy_len);
    dst[copy_len] = '\0';
    return true;
}

static bool parse_unit_suffix(String s, i32* unit_start, Unit* unit)
{
    if (s.len == 0) {
        return false;
    }

    char last = s.bytes[s.len - 1];
    switch (last) {
    case 'b':
        *unit = UNIT_BYTES;
        break;
    case 'l':
        *unit = UNIT_LINES;
        break;
    case 'c':
        *unit = UNIT_CHARS;
        break;
    default:
        return false;
    }

    *unit_start = s.len - 1;
    return true;
}

static bool parse_number_part(String s, i32 unit_start, u64* out)
{
    if (unit_start <= 0) {
        return false;
    }

    u64 result = 0;
    for (i32 i = 0; i < unit_start; i++) {
        char c = s.bytes[i];
        if (c < '0' || c > '9') {
            return false;
        }

        u64 digit = (u64)(c - '0');
        if (result > (UINT64_MAX - digit) / 10) {
            return false; // Overflow
        }

        result = result * 10 + digit;
    }

    *out = result;
    return true;
}

bool string_try_parse_unsigned(String s, u64* out, Unit* unit)
{
    if (!s.bytes || s.len <= 0 || !out || !unit) {
        return false;
    }

    i32 unit_start;
    if (!parse_unit_suffix(s, &unit_start, unit)) {
        return false;
    }

    return parse_number_part(s, unit_start, out);
}

bool string_try_parse_signed(String s, i64* out, Unit* unit)
{
    if (!s.bytes || s.len <= 0 || !out || !unit) {
        return false;
    }

    i32 start = 0;
    bool negative = false;

    // Check for sign
    if (s.bytes[0] == '+') {
        start = 1;
    } else if (s.bytes[0] == '-') {
        start = 1;
        negative = true;
    }

    if (start >= s.len) {
        return false;
    }

    // Create substring without sign
    String unsigned_part = { s.bytes + start, s.len - start };

    u64 unsigned_val;
    if (!string_try_parse_unsigned(unsigned_part, &unsigned_val, unit)) {
        return false;
    }

    if (negative) {
        if (unsigned_val > (u64)INT64_MAX + 1) {
            return false; // Would overflow
        }
        *out = -(i64)unsigned_val;
    } else {
        if (unsigned_val > (u64)INT64_MAX) {
            return false; // Would overflow
        }
        *out = (i64)unsigned_val;
    }

    return true;
}


void convert_tokens_to_strings(char** tokens, i32 token_count, String* out)
{
    for (i32 i = 0; i < token_count; i++) {
        out[i] = string_from_cstr(tokens[i]);
    }
}


i32 split_ops_string_optimized(const char* s, String* out, i32 max_tokens)
{
    static char buf[4096];
    size_t boff = 0;
    i32 ntok = 0;
    i32 token_start = 0;

    enum { S_WS,
        S_TOKEN,
        S_SQ,
        S_DQ } st
        = S_WS;
    const char* p = s;

    while (*p && ntok < max_tokens) {
        unsigned char c = (unsigned char)*p;

        if (st == S_WS) {
            if (c == ' ' || c == '\t') {
                p++;
                continue;
            }
            if (c == '\'' || c == '"') {
                if (boff >= sizeof buf - 1) {
                    return -1;
                }
                token_start = (i32)boff;
                st = (c == '\'') ? S_SQ : S_DQ;
                p++;
                continue;
            }
            // Start token
            if (boff >= sizeof buf - 1) {
                return -1;
            }
            token_start = (i32)boff;
            st = S_TOKEN;
            continue;
        }
        if (st == S_TOKEN) {
            if (c == ' ' || c == '\t') {
                // End token
                out[ntok].bytes = buf + token_start;
                out[ntok].len = (i32)boff - token_start;
                ntok++;
                st = S_WS;
                p++;
                continue;
            }
            if (c == '\'') {
                st = S_SQ;
                p++;
                continue;
            }
            if (c == '"') {
                st = S_DQ;
                p++;
                continue;
            }
            // Regular character
            if (boff >= sizeof buf - 1) {
                return -1;
            }
            buf[boff++] = (char)c;
            p++;
            continue;
        }
        if (st == S_SQ) {
            if (c == '\'') {
                st = S_TOKEN;
                p++;
                continue;
            }
            if (boff >= sizeof buf - 1) {
                return -1;
            }
            buf[boff++] = (char)c;
            p++;
            continue;
        }
        if (st == S_DQ) {
            if (c == '"') {
                st = S_TOKEN;
                p++;
                continue;
            }
            if (c == '\\' && p[1]) {
                unsigned char esc = (unsigned char)p[1];
                if (esc == '"' || esc == '\\') {
                    if (boff >= sizeof buf - 1) {
                        return -1;
                    }
                    buf[boff++] = (char)esc;
                    p += 2;
                    continue;
                }
            }
            if (boff >= sizeof buf - 1) {
                return -1;
            }
            buf[boff++] = (char)c;
            p++;
            continue;
        }
    }

    // Handle final token
    if (st == S_TOKEN || st == S_SQ || st == S_DQ) {
        if (ntok < max_tokens) {
            out[ntok].bytes = buf + token_start;
            out[ntok].len = (i32)boff - token_start;
            ntok++;
        }
    }

    return ntok;
}

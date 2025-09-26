// iosearch.c
#include "iosearch.h"
#include <stdlib.h>
#include <string.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

// Forward declarations for Boyer-Moore-Horspool algorithm
static enum Err bmh_forward(const unsigned char* text, size_t text_len,
    const unsigned char* needle, size_t nlen, i64* ms, i64* me);

// UTF-8 helper functions
static inline int is_cont_byte(unsigned char b){ return (b & 0xC0) == 0x80; }
static inline int utf8_len_from_lead(unsigned char b){
    if ((b & 0x80) == 0x00) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 0; // invalid lead
}

enum Err io_open(File* io, const char* path)
{
    memset(io, 0, sizeof(*io));

    if (strcmp(path, "-") == 0) {
        // Spool stdin to temp file
        io->f = tmpfile();
        if (!io->f)
            return E_IO;

#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif

        // Copy stdin to temp file in chunks
        unsigned char buf[256 * 1024]; // 256 KiB chunks
        size_t total_read = 0;

        while (1) {
            size_t n = fread(buf, 1, sizeof(buf), stdin);
            if (n == 0) {
                if (ferror(stdin)) { fclose(io->f); return E_IO; }
                break;
            }

            size_t written = fwrite(buf, 1, n, io->f);
            if (written != n) {
                fclose(io->f);
                return E_IO;
            }
            total_read += n;
        }

        io->size = (i64)total_read;
        if (fflush(io->f) != 0) { fclose(io->f); return E_IO; }
        rewind(io->f);
    } else {
        io->f = fopen(path, "rb");
        if (!io->f)
            return E_IO;

        // Get file size
        if (fseeko(io->f, 0, SEEK_END) != 0) {
            fclose(io->f);
            return E_IO;
        }
        io->size = ftello(io->f);
        if (io->size < 0) {
            fclose(io->f);
            return E_IO;
        }
        rewind(io->f);
    }

    // Allocate search buffer
    size_t buf_size = FW_WIN > (BK_BLK + OVERLAP_MAX) ? FW_WIN : (BK_BLK + OVERLAP_MAX);
    io->buf = malloc(buf_size);
    if (!io->buf) {
        fclose(io->f);
        return E_OOM;
    }
    io->buf_cap = buf_size;

    return E_OK;
}

void io_close(File* io)
{
    if (io->f) {
        fclose(io->f);
        io->f = NULL;
    }
    if (io->buf) {
        free(io->buf);
        io->buf = NULL;
    }
    io->size = 0;
    io->buf_cap = 0;
}

enum Err io_emit(File* io, i64 start, i64 end, FILE* out)
{
    if (start >= end) return E_OK;          // empty OK (no-op)
    if (start < 0 || end > io->size) return E_IO; // outside file is an error

    if (fseeko(io->f, start, SEEK_SET) != 0) {
        return E_IO;
    }

    i64 remaining = end - start;
    while (remaining > 0) {
        size_t chunk_size = remaining > io->buf_cap ? io->buf_cap : (size_t)remaining;
        size_t n = fread(io->buf, 1, chunk_size, io->f);
        if (n == 0) {
            if (ferror(io->f)) return E_IO;
            break; // shouldn't happen with bounded ranges, but be defensive
        }

        size_t written = fwrite(io->buf, 1, n, out);
        if (written != n) return E_IO;

        remaining -= n;
    }

    return E_OK;
}

enum Err io_line_start(File* io, i64 pos, i64* out)
{
    if (pos <= 0) {
        *out = 0;
        return E_OK;
    }

    i64 search_end = pos;
    i64 block = FW_WIN;
    while (search_end > 0) {
        i64 block_lo = search_end - block;
        if (block_lo < 0) block_lo = 0;
        if (fseeko(io->f, block_lo, SEEK_SET) != 0) return E_IO;
        size_t n = fread(io->buf, 1, (size_t)(search_end - block_lo), io->f);
        for (i64 i = (i64)n - 1; i >= 0; --i) {
            if (io->buf[i] == '\n') {
                i64 line_start = block_lo + i + 1;
                // Check if this is a CRLF line ending (previous char is \r)
                if (i > 0 && io->buf[i - 1] == '\r') {
                    // This is a CRLF line ending, line starts after the \n
                    line_start = block_lo + i + 1;
                }
                *out = line_start;
                return E_OK;
            }
        }
        if (block_lo == 0) {
            *out = 0;
            return E_OK;
        }
        search_end = block_lo;
    }
    *out = 0;
    return E_OK;
}

enum Err io_line_end(File* io, i64 pos, i64* out)
{
    if (pos < 0) {
        *out = 0;
        return E_OK;
    }
    if (pos >= io->size) {
        *out = io->size;
        return E_OK;
    }

    i64 search_start = pos;
    i64 block = FW_WIN;
    while (search_start < io->size) {
        i64 block_hi = search_start + block;
        if (block_hi > io->size) block_hi = io->size;
        if (fseeko(io->f, search_start, SEEK_SET) != 0) return E_IO;
        size_t n = fread(io->buf, 1, (size_t)(block_hi - search_start), io->f);
        for (size_t i = 0; i < n; ++i) {
            if (io->buf[i] == '\n') {
                i64 line_end = search_start + (i64)i + 1;
                // Check if this is a CRLF line ending (previous char is \r)
                if (i > 0 && io->buf[i - 1] == '\r') {
                    // This is a CRLF line ending, line ends after the \n
                    line_end = search_start + (i64)i + 1;
                }
                *out = line_end;
                return E_OK;
            }
        }
        if (block_hi == io->size) {
            *out = io->size;
            return E_OK;
        }
        search_start = block_hi;
    }
    *out = io->size;
    return E_OK;
}

enum Err io_step_lines_from(File* io, i64 start_line_start, int delta, i64* out_line_start)
{
    if (start_line_start < 0 || start_line_start > io->size) {
        return E_LOC_RESOLVE;
    }

    i64 current = start_line_start;

    if (delta > 0) {
        // Move forward by delta lines
        for (int i = 0; i < delta; i++) {
            i64 line_end;
            enum Err err = io_line_end(io, current, &line_end);
            if (err != E_OK)
                return err;

            if (line_end >= io->size) {
                *out_line_start = io->size;
                return E_OK;
            }
            current = line_end;
        }
    } else if (delta < 0) {
        // Move backward by |delta| lines
        for (int i = 0; i < -delta; i++) {
            if (current == 0) {
                *out_line_start = 0;
                return E_OK;
            }

            i64 line_start;
            enum Err err = io_line_start(io, current - 1, &line_start);
            if (err != E_OK)
                return err;
            current = line_start;
        }
    }

    *out_line_start = current;
    return E_OK;
}

enum Err io_find_window(File* io, i64 win_lo, i64 win_hi,
    const unsigned char* needle, size_t nlen,
    enum Dir dir, i64* ms, i64* me)
{
    if (nlen == 0)
        return E_BAD_NEEDLE;
    if (win_lo >= win_hi || win_lo < 0 || win_hi > io->size) {
        return E_NO_MATCH;
    }

    // Clamp window to file bounds
    win_lo = clamp64(win_lo, 0, io->size);
    win_hi = clamp64(win_hi, 0, io->size);

    if (win_lo >= win_hi) {
        return E_NO_MATCH;
    }

    if (dir == DIR_FWD) {
        // Forward search: chunked scan with overlap
        size_t overlap = nlen > 0 ? nlen - 1 : 0;
        if (overlap < OVERLAP_MIN) overlap = OVERLAP_MIN;
        if (overlap > OVERLAP_MAX) overlap = OVERLAP_MAX;

        i64 pos = win_lo;
        while (pos < win_hi) {
            i64 block_lo = pos;
            i64 block_hi = block_lo + io->buf_cap;
            if (block_hi > win_hi) block_hi = win_hi;

            if (fseeko(io->f, block_lo, SEEK_SET) != 0) return E_IO;
            size_t n = fread(io->buf, 1, (size_t)(block_hi - block_lo), io->f);
            if (n == 0) break;

            i64 local_ms, local_me;
            enum Err err = bmh_forward(io->buf, n, needle, nlen, &local_ms, &local_me);
            if (err == E_OK) {
                *ms = block_lo + local_ms;
                *me = block_lo + local_me;
                return E_OK;
            }

            if (block_hi == win_hi) break;
            pos = block_hi - (i64)overlap; // retain overlap for boundary matches
            if (pos <= block_lo) pos = block_hi; // guard
        }
        return E_NO_MATCH;

    } else {
        // Backward search: scan blocks backwards
        i64 best_ms = -1;
        i64 best_me = -1;

        // Calculate overlap
        size_t overlap = nlen > 0 ? nlen - 1 : 0;
        if (overlap < OVERLAP_MIN)
            overlap = OVERLAP_MIN;
        if (overlap > OVERLAP_MAX)
            overlap = OVERLAP_MAX;

        // Scan backwards in blocks
        for (i64 pos = win_hi; pos > win_lo; pos -= (BK_BLK - overlap)) {
            i64 block_hi = pos;
            i64 block_lo = block_hi - BK_BLK;
            if (block_lo < win_lo)
                block_lo = win_lo;

            i64 block_size = block_hi - block_lo;
            if (block_size <= 0)
                break;

            if (fseeko(io->f, block_lo, SEEK_SET) != 0) {
                return E_IO;
            }

            size_t n = fread(io->buf, 1, block_size, io->f);
            if (n == 0)
                break;

            // Find all matches in this block
            i64 search_pos = 0;
            while (search_pos < n) {
                i64 local_ms, local_me;
                enum Err err = bmh_forward(io->buf + search_pos, n - search_pos,
                    needle, nlen, &local_ms, &local_me);
                if (err != E_OK)
                    break;

                i64 global_ms = block_lo + search_pos + local_ms;
                i64 global_me = block_lo + search_pos + local_me;

                // Check if match is within our window
                if (global_ms >= win_lo && global_me <= win_hi) {
                    // Keep the rightmost match (largest ms)
                    if (global_ms > best_ms) {
                        best_ms = global_ms;
                        best_me = global_me;
                    }
                }

                search_pos += local_ms + 1; // Move past this match
            }
        }

        if (best_ms >= 0) {
            *ms = best_ms;
            *me = best_me;
            return E_OK;
        }

        return E_NO_MATCH;
    }
}

// UTF-8 character boundary and stepping functions

enum Err io_char_start(File* io, i64 pos, i64* out){
    if (pos <= 0) { *out = 0; return E_OK; }
    if (pos >= io->size){ *out = io->size; return E_OK; }

    // Read up to 3 bytes before pos to find a non-continuation
    i64 lo = pos - 3; if (lo < 0) lo = 0;
    i64 hi = pos;
    if (fseeko(io->f, lo, SEEK_SET) != 0) return E_IO;
    size_t n = fread(io->buf, 1, (size_t)(hi - lo), io->f);

    // Scan backward from pos-1 to lo for a non-cont byte
    i64 rel = (i64)n - 1;
    for (i64 k = 0; k < (i64)n; ++k){
        unsigned char b = io->buf[rel - k];
        if (!is_cont_byte(b)){
            // Validate forward length; if malformed, treat that byte as a single-char
            int len = utf8_len_from_lead(b);
            i64 start = hi - 1 - k;
            if (len == 0 || start + len > io->size){
                *out = start;
                return E_OK;
            }
            *out = start;
            return E_OK;
        }
    }
    // All were continuation bytes; treat lo as boundary (permissive)
    *out = lo;
    return E_OK;
}

enum Err io_step_chars_from(File* io, i64 start, int delta, i64* out){
    if (start < 0) start = 0;
    if (start > io->size) start = io->size;

    i64 cur = start;

    if (delta >= 0){
        // forward
        for (int i = 0; i < delta; ++i){
            if (cur >= io->size){ *out = io->size; return E_OK; }
            // Read a small window [cur, cur+4]
            i64 hi = cur + 4; if (hi > io->size) hi = io->size;
            if (fseeko(io->f, cur, SEEK_SET) != 0) return E_IO;
            size_t n = fread(io->buf, 1, (size_t)(hi - cur), io->f);
            if (n == 0){ *out = cur; return E_OK; }

            unsigned char b0 = io->buf[0];
            int len = utf8_len_from_lead(b0);
            if (len == 0){
                // malformed lead -> count as 1
                cur += 1;
            } else {
                // ensure we have len bytes and continuations are well-formed; else permissive 1
                if ((i64)len <= (i64)n){
                    bool ok = true;
                    for (int j=1;j<len;j++){ if (!is_cont_byte(io->buf[j])) { ok=false; break; } }
                    cur += ok ? len : 1;
                } else {
                    // truncated at EOF -> accept partial as 1
                    cur += 1;
                }
            }
        }
        *out = cur;
        return E_OK;
    } else {
        // backward
        int steps = -delta;
        for (int i = 0; i < steps; ++i){
            if (cur <= 0){ *out = 0; return E_OK; }
            i64 start_char;
            // snap to the start of the char immediately before cur
            enum Err e = io_char_start(io, cur - 1, &start_char);
            if (e != E_OK) return e;
            cur = start_char;
        }
        *out = cur;
        return E_OK;
    }
}

// Boyer-Moore-Horspool algorithm implementation
static enum Err bmh_forward(const unsigned char* text, size_t text_len,
    const unsigned char* needle, size_t nlen, i64* ms, i64* me)
{
    if (nlen == 0 || nlen > text_len) return E_NO_MATCH;

    size_t shift[256];
    for (size_t i = 0; i < 256; ++i) shift[i] = nlen;
    for (size_t i = 0; i + 1 < nlen; ++i) shift[needle[i]] = nlen - 1 - i;

    size_t pos = 0;
    while (pos <= text_len - nlen) {
        unsigned char last = text[pos + nlen - 1];
        if (last == needle[nlen - 1] && memcmp(text + pos, needle, nlen) == 0) {
            *ms = (i64)pos; *me = (i64)(pos + nlen); return E_OK;
        }
        pos += shift[last];
    }
    return E_NO_MATCH;
}


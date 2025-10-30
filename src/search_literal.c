#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "search_literal.h"
#include "error.h"
#include "util.h"
#include <string.h>
#include <sys/types.h>

#if defined(_WIN32) && !defined(__MINGW32__) && !defined(__MINGW64__)
#define fseeko _fseeki64
#define ftello _ftelli64
#endif

// BK_BLK constant is defined in fileio.h

/*******************************
 * BOYER-MOORE-HORSPOOL SEARCH *
 *******************************/

// Internal helper: BMH search in memory buffer
static enum Err bmh_search_forward(const unsigned char* text, size_t text_len,
    const unsigned char* needle, size_t nlen, i64* ms, i64* me)
{
    if (nlen == 0 || nlen > text_len) {
        return E_NO_MATCH;
    }

    size_t shift[256];
    for (size_t i = 0; i < 256; ++i) {
        shift[i] = nlen;
    }
    for (size_t i = 0; i + 1 < nlen; ++i) {
        shift[needle[i]] = nlen - 1 - i;
    }

    size_t pos = 0;
    while (pos <= text_len - nlen) {
        unsigned char last = text[pos + nlen - 1];
        if (last == needle[nlen - 1] && memcmp(text + pos, needle, nlen) == 0) {
            *ms = (i64)pos;
            *me = (i64)(pos + nlen);
            return E_OK;
        }
        pos += shift[last];
    }
    return E_NO_MATCH;
}

/*****************
 * STRING SEARCH *
 *****************/

enum Err literal_search_window(File* io, i64 win_lo, i64 win_hi,
    const unsigned char* needle, size_t nlen,
    enum Dir dir, i64* ms, i64* me)
{
    if (nlen == 0) {
        return E_BAD_NEEDLE;
    }

    // Clamp window to file bounds (be permissive; callers may pass slightly OOB)
    win_lo = clamp64(win_lo, 0, io->size);
    win_hi = clamp64(win_hi, 0, io->size);

    if (win_lo >= win_hi) {
        return E_NO_MATCH;
    }

    if (dir == DIR_FWD) {
        // Forward search: chunked scan with overlap
        size_t overlap = nlen > 0 ? nlen - 1 : 0;
        if (overlap >= io->buf_cap) {
            overlap = io->buf_cap - 1;
        }

        i64 pos = win_lo;
        while (pos < win_hi) {
            i64 block_lo = pos;
            i64 block_hi = block_lo + (i64)io->buf_cap;
            if (block_hi > win_hi) {
                block_hi = win_hi;
            }

            if (fseeko(io->f, block_lo, SEEK_SET) != 0) {
                return E_IO;
            }
            size_t n = fread(io->buf, 1, (size_t)(block_hi - block_lo), io->f);
            if (n == 0) {
                break;
            }

            i64 local_ms;
            i64 local_me;
            enum Err err = bmh_search_forward(io->buf, n, needle, nlen, &local_ms, &local_me);
            if (err == E_OK) {
                *ms = block_lo + local_ms;
                *me = block_lo + local_me;
                return E_OK;
            }

            if (block_hi == win_hi) {
                break;
            }
            pos = block_hi - (i64)overlap; // retain overlap for boundary matches
            if (pos <= block_lo) {
                pos = block_hi; // guard
            }
        }
        return E_NO_MATCH;
    }

    // Backward search: scan blocks backwards
    i64 best_ms = -1;
    i64 best_me = -1;

    // Calculate overlap (enough to catch boundary-spanning matches)
    size_t overlap = nlen > 0 ? nlen - 1 : 0;
    if (overlap >= io->buf_cap) {
        overlap = io->buf_cap - 1;
    }

    // Scan backwards in blocks
    for (i64 pos = win_hi; pos > win_lo; pos -= (i64)(BK_BLK - overlap)) {
        i64 block_hi = pos;
        i64 block_lo = block_hi - BK_BLK;
        if (block_lo < win_lo) {
            block_lo = win_lo;
        }

        i64 block_size = block_hi - block_lo;
        if (block_size <= 0) {
            break;
        }

        if (fseeko(io->f, block_lo, SEEK_SET) != 0) {
            return E_IO;
        }

        size_t n = fread(io->buf, 1, (size_t)block_size, io->f);
        if (n == 0) {
            break;
        }

        // Find all matches in this block
        i64 search_pos = 0;
        while (search_pos < (i64)n) {
            i64 local_ms;
            i64 local_me;
            enum Err err = bmh_search_forward(io->buf + search_pos, (size_t)((i64)n - search_pos),
                needle, nlen, &local_ms, &local_me);
            if (err != E_OK) {
                break;
            }

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

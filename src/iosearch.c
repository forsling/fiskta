// iosearch.c
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "iosearch.h"
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> // for off_t

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

// Forward declarations for Boyer-Moore-Horspool algorithm
static enum Err bmh_forward(const unsigned char* text, size_t text_len,
    const unsigned char* needle, size_t nlen, i64* ms, i64* me);

// Forward declaration for line indexing
static enum Err get_line_block(File* io, i64 pos, LineBlockIdx** out);

// UTF-8 helper functions
static inline i32 is_cont_byte(unsigned char b) { return (b & 0xC0) == 0x80; }
static inline i32 utf8_len_from_lead(unsigned char b)
{
    if ((b & 0x80) == 0x00)
        return 1;
    if ((b & 0xE0) == 0xC0)
        return 2;
    if ((b & 0xF0) == 0xE0)
        return 3;
    if ((b & 0xF8) == 0xF0)
        return 4;
    return 0; // invalid lead
}

// io_open removed - use io_open_arena2 with arena allocation instead

enum Err io_open(File* io, const char* path,
    unsigned char* search_buf, size_t search_buf_cap)
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
        // size will be determined with ftello after writing

        while (1) {
            size_t n = fread(buf, 1, sizeof(buf), stdin);
            if (n == 0) {
                if (ferror(stdin)) {
                    fclose(io->f);
                    return E_IO;
                }
                break;
            }

            size_t written = fwrite(buf, 1, n, io->f);
            if (written != n) {
                fclose(io->f);
                return E_IO;
            }
            /* no running size_t accumulator needed */
        }

        if (fflush(io->f) != 0) {
            fclose(io->f);
            return E_IO;
        }
        if (fseeko(io->f, 0, SEEK_END) != 0) {
            fclose(io->f);
            return E_IO;
        }
        off_t sz = ftello(io->f);
        if (sz < 0) {
            fclose(io->f);
            return E_IO;
        }
        io->size = (i64)sz;
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

    // Wire arena-backed buffers
    io->buf = search_buf;
    io->buf_cap = search_buf_cap;

    // Initialize LRU line index cache (counts are embedded)
    io->line_idx_gen = 0;
    for (i32 i = 0; i < IDX_MAX_BLOCKS; ++i) {
        io->line_idx[i].in_use = false;
        io->line_idx[i].gen = 0;
        io->line_idx[i].sub_count = 0;
    }

    // Regex scratch not set yet
    io->re.curr = NULL;
    io->re.next = NULL;
    io->re.cap = 0;
    io->re.seen_curr = io->re.seen_next = NULL;
    io->re.seen_bytes = 0;

    return E_OK;
}

void io_close(File* io)
{
    if (io->f) {
        fclose(io->f);
        io->f = NULL;
    }

    io->size = 0;
    io->buf_cap = 0;
}

// Full reset of File state for clause execution
void io_reset_full(File* io)
{
    if (!io)
        return;
    if (io->f) {
        // 64-bit safe seek to BOF
        if (fseeko(io->f, 0, SEEK_SET) != 0) {
            // Best effort: clear any error state
            clearerr(io->f);
        }
    }

    // Reset all line index cache but preserve arena-owned slabs
    io->line_idx_gen = 0;
    for (int i = 0; i < IDX_MAX_BLOCKS; i++) {
        io->line_idx[i].in_use = false;
        io->line_idx[i].gen = 0;
        io->line_idx[i].block_lo = 0;
        io->line_idx[i].block_hi = 0;
        io->line_idx[i].sub_count = 0;
        // Don't reset lf_counts pointer - it's managed by arena
    }
}

enum Err io_emit(File* io, i64 start, i64 end, FILE* out)
{
    if (start >= end)
        return E_OK;
    if (start < 0 || end > io->size)
        return E_IO; // outside file is an error

    if (fseeko(io->f, start, SEEK_SET) != 0) {
        return E_IO;
    }

    i64 remaining = end - start;
    while (remaining > 0) {
        size_t chunk_size = remaining > io->buf_cap ? io->buf_cap : (size_t)remaining;
        size_t n = fread(io->buf, 1, chunk_size, io->f);
        if (n == 0) {
            if (ferror(io->f))
                return E_IO;
            break; // shouldn't happen with bounded ranges, but be defensive
        }

        size_t written = fwrite(io->buf, 1, n, out);
        if (written != n)
            return E_IO;

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

    // Use line index for fast reverse block jumping
    i64 cur_pos = pos - 1; // We want the previous LF before or at pos-1
    while (cur_pos >= 0) {
        LineBlockIdx* block;
        enum Err err = get_line_block(io, cur_pos, &block);
        if (err != E_OK)
            return err;

        // If we're before this block, move to previous block
        if (cur_pos < block->block_lo) {
            cur_pos = block->block_lo - 1;
            continue;
        }

        // Find subchunk and offset within subchunk
        i64 sub_offset = cur_pos - block->block_lo;
        i32 sub_idx = (i32)(sub_offset / IDX_SUB);
        i64 sub_start = block->block_lo + (i64)sub_idx * IDX_SUB;
        i64 sub_end = sub_start + IDX_SUB;
        if (sub_end > block->block_hi)
            sub_end = block->block_hi;

        // Fast skip: if this subchunk has no LFs and we're at its end, skip it
        if (block->lf_counts[sub_idx] == 0 && cur_pos == sub_end - 1) {
            cur_pos = sub_start - 1;
            continue;
        }

        // Check if previous subchunks in this block have any LFs
        i32 previous_lfs = 0;
        for (i32 s = 0; s <= sub_idx; ++s) {
            previous_lfs += block->lf_counts[s];
        }
        if (previous_lfs == 0) {
            cur_pos = block->block_lo - 1;
            continue;
        }

        // Scan this subchunk for the last LF
        if (fseeko(io->f, sub_start, SEEK_SET) != 0)
            return E_IO;
        size_t n = fread(io->buf, 1, (size_t)(sub_end - sub_start), io->f);
        if (n != (size_t)(sub_end - sub_start) && ferror(io->f))
            return E_IO;

        i64 scan_end = cur_pos - sub_start;
        for (i64 i = scan_end; i >= 0; --i) {
            if (io->buf[i] == '\n') {
                *out = sub_start + i + 1;
                return E_OK;
            }
        }

        // No LF found in this subchunk, move to previous
        cur_pos = sub_start - 1;
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

    // Use line index for fast block jumping
    i64 cur_pos = pos;
    while (cur_pos < io->size) {
        LineBlockIdx* block;
        enum Err err = get_line_block(io, cur_pos, &block);
        if (err != E_OK)
            return err;

        // If we're at the end of this block, move to next block
        if (cur_pos >= block->block_hi) {
            cur_pos = block->block_hi;
            continue;
        }

        // Find subchunk and offset within subchunk
        i64 sub_offset = cur_pos - block->block_lo;
        i32 sub_idx = (i32)(sub_offset / IDX_SUB);
        i64 sub_start = block->block_lo + (i64)sub_idx * IDX_SUB;
        i64 sub_end = sub_start + IDX_SUB;
        if (sub_end > block->block_hi)
            sub_end = block->block_hi;

        // Fast skip: if this subchunk has no LFs and we're at its start, skip it
        if (block->lf_counts[sub_idx] == 0 && cur_pos == sub_start) {
            cur_pos = sub_end;
            continue;
        }

        // Check if remaining subchunks in this block have any LFs
        i32 remaining_lfs = 0;
        for (i32 s = sub_idx; s < block->sub_count; ++s) {
            remaining_lfs += block->lf_counts[s];
        }
        if (remaining_lfs == 0) {
            cur_pos = block->block_hi;
            continue;
        }

        // Scan this subchunk for the first LF
        if (fseeko(io->f, sub_start, SEEK_SET) != 0)
            return E_IO;
        size_t n = fread(io->buf, 1, (size_t)(sub_end - sub_start), io->f);
        if (n != (size_t)(sub_end - sub_start) && ferror(io->f))
            return E_IO;

        i64 scan_start = cur_pos - sub_start;
        for (size_t i = scan_start; i < n; ++i) {
            if (io->buf[i] == '\n') {
                *out = sub_start + (i64)i + 1;
                return E_OK;
            }
        }

        // No LF found in this subchunk, move to next
        cur_pos = sub_end;
    }

    *out = io->size;
    return E_OK;
}

enum Err io_step_lines_from(File* io, i64 start_line_start, i32 delta, i64* out_line_start)
{
    if (start_line_start < 0 || start_line_start > io->size) {
        return E_LOC_RESOLVE;
    }

    i64 current = start_line_start;

    if (delta > 0) {
        // Move forward by delta lines
        for (i32 i = 0; i < delta; i++) {
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
        for (i32 i = 0; i < -delta; i++) {
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
        if (overlap > OVERLAP_MAX)
            overlap = OVERLAP_MAX;

        i64 pos = win_lo;
        while (pos < win_hi) {
            i64 block_lo = pos;
            i64 block_hi = block_lo + io->buf_cap;
            if (block_hi > win_hi)
                block_hi = win_hi;

            if (fseeko(io->f, block_lo, SEEK_SET) != 0)
                return E_IO;
            size_t n = fread(io->buf, 1, (size_t)(block_hi - block_lo), io->f);
            if (n == 0)
                break;

            i64 local_ms, local_me;
            enum Err err = bmh_forward(io->buf, n, needle, nlen, &local_ms, &local_me);
            if (err == E_OK) {
                *ms = block_lo + local_ms;
                *me = block_lo + local_me;
                return E_OK;
            }

            if (block_hi == win_hi)
                break;
            pos = block_hi - (i64)overlap; // retain overlap for boundary matches
            if (pos <= block_lo)
                pos = block_hi; // guard
        }
        return E_NO_MATCH;

    } else {
        // Backward search: scan blocks backwards
        i64 best_ms = -1;
        i64 best_me = -1;

        // Calculate overlap (enough to catch boundary-spanning matches)
        size_t overlap = nlen > 0 ? nlen - 1 : 0;
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

enum Err io_char_start(File* io, i64 pos, i64* out)
{
    if (pos <= 0) {
        *out = 0;
        return E_OK;
    }
    if (pos >= io->size) {
        *out = io->size;
        return E_OK;
    }

    // Read up to 3 bytes before pos to find a non-continuation
    i64 lo = pos - 3;
    if (lo < 0)
        lo = 0;
    i64 hi = pos;
    if (fseeko(io->f, lo, SEEK_SET) != 0)
        return E_IO;
    size_t n = fread(io->buf, 1, (size_t)(hi - lo), io->f);

    // Scan backward from pos-1 to lo for a non-cont byte
    i64 rel = (i64)n - 1;
    for (i64 k = 0; k < (i64)n; ++k) {
        unsigned char b = io->buf[rel - k];
        if (!is_cont_byte(b)) {
            // Validate forward length; if malformed, treat that byte as a single-char
            i32 len = utf8_len_from_lead(b);
            i64 start = hi - 1 - k;
            if (len == 0 || start + len > io->size) {
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

enum Err io_step_chars_from(File* io, i64 start, i32 delta, i64* out)
{
    if (start < 0)
        start = 0;
    if (start > io->size)
        start = io->size;

    i64 cur = start;

    if (delta >= 0) {
        // forward
        for (i32 i = 0; i < delta; ++i) {
            if (cur >= io->size) {
                *out = io->size;
                return E_OK;
            }
            // Read a small window [cur, cur+4]
            i64 hi = cur + 4;
            if (hi > io->size)
                hi = io->size;
            if (fseeko(io->f, cur, SEEK_SET) != 0)
                return E_IO;
            size_t n = fread(io->buf, 1, (size_t)(hi - cur), io->f);
            if (n == 0) {
                *out = cur;
                return E_OK;
            }

            unsigned char b0 = io->buf[0];
            i32 len = utf8_len_from_lead(b0);
            if (len == 0) {
                // malformed lead -> count as 1
                cur += 1;
            } else {
                // ensure we have len bytes and continuations are well-formed; else permissive 1
                if ((i64)len <= (i64)n) {
                    bool ok = true;
                    for (i32 j = 1; j < len; j++) {
                        if (!is_cont_byte(io->buf[j])) {
                            ok = false;
                            break;
                        }
                    }
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
        i32 steps = -delta;
        for (i32 i = 0; i < steps; ++i) {
            if (cur <= 0) {
                *out = 0;
                return E_OK;
            }
            i64 start_char;
            // snap to the start of the char immediately before cur
            enum Err e = io_char_start(io, cur - 1, &start_char);
            if (e != E_OK)
                return e;
            cur = start_char;
        }
        *out = cur;
        return E_OK;
    }
}

// Line indexing implementation

static enum Err get_line_block(File* io, i64 pos, LineBlockIdx** out)
{
    if (pos < 0)
        pos = 0;
    if (pos > io->size)
        pos = io->size;

    i64 block_lo = (pos / IDX_BLOCK) * (i64)IDX_BLOCK;
    i64 block_hi = block_lo + IDX_BLOCK;
    if (block_hi > io->size)
        block_hi = io->size;

    // 1) hit?
    i32 free_slot = -1, lru_slot = 0;
    for (i32 i = 0; i < IDX_MAX_BLOCKS; ++i) {
        LineBlockIdx* e = &io->line_idx[i];
        if (!e->in_use) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }
        if (e->block_lo == block_lo && e->block_hi == block_hi) {
            e->gen = ++io->line_idx_gen;
            *out = e;
            return E_OK;
        }
        if (io->line_idx[i].gen < io->line_idx[lru_slot].gen)
            lru_slot = i;
    }

    i32 slot = (free_slot >= 0) ? free_slot : lru_slot;

    // 2) (Re)build index in slot
    LineBlockIdx* e = &io->line_idx[slot];

    i32 sub_count = (i32)((block_hi - block_lo + IDX_SUB - 1) / IDX_SUB);
    if (sub_count <= 0)
        sub_count = 1;
    if (sub_count > IDX_SUB_MAX)
        sub_count = IDX_SUB_MAX; // defensive
    e->sub_count = sub_count;

    // compute counts
    for (i32 s = 0; s < sub_count; ++s)
        e->lf_counts[s] = 0;

    i64 pos_cur = block_lo;
    for (i32 s = 0; s < sub_count; ++s) {
        i64 sub_lo = pos_cur;
        i64 sub_hi = sub_lo + IDX_SUB;
        if (sub_hi > block_hi)
            sub_hi = block_hi;

        if (fseeko(io->f, sub_lo, SEEK_SET) != 0)
            return E_IO;
        size_t n = fread(io->buf, 1, (size_t)(sub_hi - sub_lo), io->f);
        if (n != (size_t)(sub_hi - sub_lo) && ferror(io->f))
            return E_IO;

        unsigned short cnt = 0;
        for (size_t i = 0; i < n; ++i) {
            if (io->buf[i] == '\n')
                cnt++;
        }
        e->lf_counts[s] = cnt;
        pos_cur = sub_hi;
    }

    e->block_lo = block_lo;
    e->block_hi = block_hi;
    e->gen = ++io->line_idx_gen;
    e->in_use = true;

    *out = e;
    return E_OK;
}

// Boyer-Moore-Horspool algorithm implementation
static enum Err bmh_forward(const unsigned char* text, size_t text_len,
    const unsigned char* needle, size_t nlen, i64* ms, i64* me)
{
    if (nlen == 0 || nlen > text_len)
        return E_NO_MATCH;

    size_t shift[256];
    for (size_t i = 0; i < 256; ++i)
        shift[i] = nlen;
    for (size_t i = 0; i + 1 < nlen; ++i)
        shift[needle[i]] = nlen - 1 - i;

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

// --------------------------
// Regex streaming NFA engine
// --------------------------
typedef struct {
    ReThread* v;
    int n, cap;
} ReList;

static inline int cls_has(const ReClass* c, unsigned char ch)
{
    return (c->bits[ch >> 3] >> (ch & 7)) & 1;
}

static void rlist_init(ReList* L, ReThread* buf, int cap)
{
    L->v = buf;
    L->n = 0;
    L->cap = cap;
}
static inline void rlist_clear(ReList* L) { L->n = 0; }
static inline void seen_clear(unsigned char* seen, int n) { memset(seen, 0, (size_t)n); }

// Ordered epsilon-closure push. Sets *match_found if RI_MATCH reachable for current pos and min_start.
// Returns E_OOM if thread list capacity is exceeded.
static enum Err add_thread_ordered(const ReProg* P, ReList* L, int pc, i64 start,
    i64 pos, i64 win_lo, i64 win_hi, i64 file_size,
    unsigned char* seen, int* match_found, i64 min_start,
    unsigned char curr_char, unsigned char prev_char,
    int at_bol, int at_eol)
{
    while (1) {
        if (pc < 0 || pc >= P->nins)
            return E_OK;
        if (seen[pc])
            return E_OK;
        ReInst* I = &P->ins[pc];
        switch (I->op) {
        case RI_SPLIT:
            // IMPORTANT: mark as seen before processing split
            seen[pc] = 1;
            // IMPORTANT: X first (preferred), then Y
            enum Err err = add_thread_ordered(P, L, I->x, start, pos, win_lo, win_hi, file_size, seen, match_found, min_start, curr_char, prev_char, at_bol, at_eol);
            if (err != E_OK)
                return err;
            pc = I->y; // continue tail-call
            continue;
        case RI_JMP:
            seen[pc] = 1; // mark as seen before jumping
            pc = I->x;
            continue;
        case RI_BOL:
            if (at_bol) {
                pc++;
                continue;
            }
            return E_OK;
        case RI_EOL:
            if (at_eol) {
                pc++;
                continue;
            }
            return E_OK;
        case RI_MATCH:
            if (start == min_start)
                *match_found = 1;
            // Add the thread to the list so consumption step can detect it
            if (L->n >= L->cap) {
                return E_OOM; // Thread list is full
            }
            L->v[L->n].pc = pc;
            L->v[L->n].start = start;
            L->n++;
            seen[pc] = 1;
            return E_OK;
        case RI_CHAR:
        case RI_ANY:
        case RI_CLASS:
            // consuming; add once
            if (L->n >= L->cap) {
                return E_OOM; // Thread list is full
            }
            L->v[L->n].pc = pc;
            L->v[L->n].start = start;
            L->n++;
            seen[pc] = 1;
            return E_OK;
        default:
            return E_OK;
        }
    }
}

enum Err io_findr_window(File* io, i64 win_lo, i64 win_hi,
    const ReProg* re, enum Dir dir, i64* ms, i64* me)
{
    if (!re || re->nins <= 0)
        return E_PARSE;
    if (win_lo < 0)
        win_lo = 0;
    if (win_hi > io->size)
        win_hi = io->size;
    if (win_lo >= win_hi)
        return E_NO_MATCH;

    // Tail byte prefetching for cheap lookahead
    unsigned char tail1 = 0, tail2 = 0;
    int tails = 0;

    // Use arena-backed scratch provided at startup
    const int nins = re->nins;
    const int cap = io->re.cap;
    if (cap <= 0 || !io->re.curr || !io->re.next || !io->re.seen_curr || !io->re.seen_next)
        return E_OOM;
    if ((size_t)nins > io->re.seen_bytes)
        return E_OOM;

    ReThread* curr_buf = io->re.curr;
    ReThread* next_buf = io->re.next;
    unsigned char* seen_curr = io->re.seen_curr;
    unsigned char* seen_next = io->re.seen_next;
    ReList curr, next;
    rlist_init(&curr, curr_buf, cap);
    rlist_init(&next, next_buf, cap);
    seen_clear(seen_curr, nins);
    seen_clear(seen_next, nins);

    i64 best_ms = -1, best_me = -1;
    i64 min_start = 0;
    int have_min = 0;

    i64 pos = win_lo;
    i64 block_lo = win_lo, block_hi = win_lo;
    size_t n = 0;
    // carry previous byte across block boundaries
    unsigned char prev_c = 0;
    int have_prev = 0;

    // Initialize previous character correctly for anchor semantics
    if (win_lo > 0) {
        if (fseeko(io->f, win_lo - 1, SEEK_SET) != 0)
            return E_IO;
        unsigned char b;
        size_t n_read = fread(&b, 1, 1, io->f);
        if (n_read == 1) {
            prev_c = b;
            have_prev = 1;
        }
    }

    for (;;) {
        // Refill buffer if needed for consumption step
        if (pos == block_hi && pos < win_hi) {
            block_lo = pos;
            block_hi = block_lo + (i64)io->buf_cap;
            if (block_hi > win_hi)
                block_hi = win_hi;
            if (fseeko(io->f, block_lo, SEEK_SET) != 0)
                return E_IO;
            n = fread(io->buf, 1, (size_t)(block_hi - block_lo), io->f);
            if (n == 0 && ferror(io->f))
                return E_IO;
            block_hi = block_lo + (i64)n; // clamp to bytes actually in buf

            // Prefetch up to two "tail" bytes beyond the block for cheap lookahead
            tail1 = tail2 = 0;
            tails = 0;
            if (block_hi < win_hi) {
                if (fseeko(io->f, block_hi, SEEK_SET) != 0)
                    return E_IO;
                unsigned char t[2];
                size_t m = fread(t, 1, (size_t)((win_hi - block_hi) >= 2 ? 2 : 1), io->f);
                if (m >= 1) {
                    tail1 = t[0];
                    tails = 1;
                }
                if (m >= 2) {
                    tail2 = t[1];
                    tails = 2;
                }
            }
        }

        // current/previous chars at position pos
        unsigned char curr_c = (pos < win_hi) ? io->buf[pos - block_lo] : 0;

        // Compute next1/next2 using tail bytes for cheap lookahead
        unsigned char next1 = 0, next2 = 0;
        if (pos + 1 < block_hi)
            next1 = io->buf[pos + 1 - block_lo];
        else if (pos + 1 == block_hi && tails >= 1)
            next1 = tail1;

        if (pos + 2 < block_hi)
            next2 = io->buf[pos + 2 - block_lo];
        else if (pos + 2 == block_hi && tails >= 1)
            next2 = tail1;
        else if (pos + 2 == block_hi + 1 && tails >= 2)
            next2 = tail2;

        unsigned char prev_char = have_prev ? prev_c : 0;

        // Compute anchor booleans once per loop
        int at_bol = (pos == win_lo) || (have_prev && prev_c == '\n');
        int at_eol = (pos == win_hi) || (pos + 1 == win_hi) || // at last character of file
            (next1 == '\n') || (next1 == '\r' && next2 == '\n'); // CRLF-aware

        // If no active threads, start a new leftmost attempt at pos
        if (curr.n == 0) {
            have_min = 1;
            min_start = pos;
            seen_clear(seen_curr, nins);
            int match_found = 0;
            enum Err err = add_thread_ordered(re, &curr, 0, pos, pos, win_lo, win_hi, io->size,
                seen_curr, &match_found, min_start, curr_c, prev_char, at_bol, at_eol);
            if (err != E_OK)
                return err;
            if (match_found) {
                // epsilon-only match (no consumption): end == pos
                if (dir == DIR_FWD) {
                    *ms = min_start;
                    *me = pos;
                    return E_OK;
                } else {
                    best_ms = min_start;
                    best_me = pos; /* reset for later starts */
                    curr.n = 0;
                    have_min = 0;
                }
            }
        } else {
            seen_clear(seen_curr, nins);
            // Re-run epsilon to discover MATCH at this pos (no consumption)
            int match_found = 0;
            unsigned char curr_char = curr_c;
            // prev_char already computed above
            for (int k = 0; k < curr.n; k++) {
                // IMPORTANT: keep global min_start
                enum Err err = add_thread_ordered(re, &curr, curr.v[k].pc, curr.v[k].start, pos, win_lo, win_hi, io->size,
                    seen_curr, &match_found, min_start, curr_char, prev_char, at_bol, at_eol);
                if (err != E_OK)
                    return err;
            }
            if (match_found) {
                // epsilon-only match at current pos
                if (dir == DIR_FWD) {
                    *ms = min_start;
                    *me = pos;
                    return E_OK;
                } else {
                    best_ms = min_start;
                    best_me = pos;
                    curr.n = 0;
                    have_min = 0;
                }
            }
        }

        if (pos == win_hi)
            break; // nothing to consume

        unsigned char c = curr_c;
        // Build next from curr by consuming c
        rlist_clear(&next);
        seen_clear(seen_next, nins);

        for (int i = 0; i < curr.n; i++) {
            int pc = curr.v[i].pc;
            i64 st = curr.v[i].start;
            // Only proceed for threads at current leftmost start
            if (have_min && st > min_start)
                continue;
            ReInst* I = &re->ins[pc];
            switch (I->op) {
            case RI_CHAR:
                if (c == I->ch) {
                    enum Err err = add_thread_ordered(re, &next, pc + 1, st, pos + 1, win_lo, win_hi, io->size, seen_next, &(int) { 0 }, min_start, c, prev_char, at_bol, at_eol);
                    if (err != E_OK)
                        return err;
                }
                break;
            case RI_ANY:
                if (c != '\n') { // dot ≠ newline
                    enum Err err = add_thread_ordered(re, &next, pc + 1, st, pos + 1, win_lo, win_hi, io->size, seen_next, &(int) { 0 }, min_start, c, prev_char, at_bol, at_eol);
                    if (err != E_OK)
                        return err;
                }
                break;
            case RI_CLASS:
                if (I->cls_idx >= 0 && I->cls_idx < re->nclasses && cls_has(&re->classes[I->cls_idx], c)) {
                    enum Err err = add_thread_ordered(re, &next, pc + 1, st, pos + 1, win_lo, win_hi, io->size, seen_next, &(int) { 0 }, min_start, c, prev_char, at_bol, at_eol);
                    if (err != E_OK)
                        return err;
                }
                break;
            default:
                // Shouldn't be here; closure should have removed epsilons
                break;
            }
        }

        // Check for matches in the next threads
        int match_found = 0;
        for (int i = 0; i < next.n; i++) {
            int pc = next.v[i].pc;
            if (pc >= 0 && pc < re->nins && re->ins[pc].op == RI_MATCH) {
                match_found = 1;
                break;
            }
        }

        if (match_found) {
            if (dir == DIR_FWD) {
                *ms = min_start;
                *me = pos + 1;
                return E_OK;
            } else {
                best_ms = min_start;
                best_me = pos + 1;
                curr.n = 0;
                have_min = 0; // reset for later leftmost starts
            }
        }

        // Advance: swap lists (do NOT swap raw buffers; swap the structs)
        ReList tmpL = curr;
        curr = next;
        next = tmpL;
        unsigned char* tmpb = seen_curr;
        seen_curr = seen_next;
        seen_next = tmpb;
        // advance and carry previous char
        prev_c = curr_c;
        have_prev = (pos < win_hi);
        pos++;
    }

    if (dir == DIR_BWD && best_ms >= 0) {
        *ms = best_ms;
        *me = best_me;
        return E_OK;
    }
    return E_NO_MATCH;
}

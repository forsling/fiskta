#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "fileio.h"
#include "error.h"
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> // for off_t

#if defined(_WIN32) && !defined(__MINGW32__) && !defined(__MINGW64__)
#define fseeko _fseeki64
#define ftello _ftelli64
#endif

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#ifndef FISKTA_STDIN_SPOOL
#define FISKTA_STDIN_SPOOL (256 * 1024)
#endif

/**********************
 * FILE I/O OPERATIONS
 **********************/

enum Err io_open(File* io, const char* path,
    unsigned char* search_buf, size_t search_buf_cap)
{
    memset(io, 0, sizeof(*io));

    if (strcmp(path, "-") == 0) {
        // Spool stdin to temp file
        io->f = tmpfile();
        if (!io->f) {
            return E_IO;
        }

#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif

        // Copy stdin to temp file in chunks
        unsigned char buf[FISKTA_STDIN_SPOOL];
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
        if (fseek(io->f, 0, SEEK_SET) != 0) {
            fclose(io->f);
            return E_IO;
        }
    } else {
        io->f = fopen(path, "rb");
        if (!io->f) {
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
        if (fseek(io->f, 0, SEEK_SET) != 0) {
            fclose(io->f);
            return E_IO;
        }
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
    if (!io) {
        return;
    }
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
        // Don't reset lf_counts array - it's embedded in the struct
    }
}

enum Err io_emit(File* io, i64 start, i64 end, FILE* out)
{
    if (start >= end) {
        return E_OK;
    }
    if (start < 0 || end > io->size) {
        return E_IO; // outside file is an error
    }

    if (fseeko(io->f, start, SEEK_SET) != 0) {
        return E_IO;
    }

    i64 remaining = end - start;
    while (remaining > 0) {
        size_t chunk_size = (remaining > (i64)io->buf_cap) ? io->buf_cap : (size_t)remaining;
        size_t n = fread(io->buf, 1, chunk_size, io->f);
        if (n == 0) {
            if (ferror(io->f)) {
                return E_IO;
            }
            break; // shouldn't happen with bounded ranges, but be defensive
        }

        size_t written = fwrite(io->buf, 1, n, out);
        if (written != n) {
            return E_IO;
        }

        remaining -= (i64)n;
    }

    return E_OK;
}

/******************
 * LINE NAVIGATION
 ******************/

// UTF-8 helper functions
static inline i32 utf8_is_cont_byte(unsigned char b) { return (b & 0xC0) == 0x80; }
static inline i32 utf8_len_from_lead_byte(unsigned char b)
{
    if ((b & 0x80) == 0x00) {
        return 1;
    }
    if ((b & 0xE0) == 0xC0) {
        return 2;
    }
    if ((b & 0xF0) == 0xE0) {
        return 3;
    }
    if ((b & 0xF8) == 0xF0) {
        return 4;
    }
    return 0; // invalid lead
}

// Forward declaration for internal helper
static enum Err get_line_block(File* io, i64 pos, LineBlockIdx** out);

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
        if (err != E_OK) {
            return err;
        }

        //  If we're before this block, move to previous block
        if (cur_pos < block->block_lo) {
            cur_pos = block->block_lo - 1;
            continue;
        }

        // Find subchunk and offset within subchunk
        i64 sub_offset = cur_pos - block->block_lo;
        i32 sub_idx = (i32)(sub_offset / IDX_SUB);
        i64 sub_start = block->block_lo + (i64)sub_idx * IDX_SUB;
        i64 sub_end = sub_start + IDX_SUB;
        if (sub_end > block->block_hi) {
            sub_end = block->block_hi;
        }

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
        if (fseeko(io->f, sub_start, SEEK_SET) != 0) {
            return E_IO;
        }
        size_t n = fread(io->buf, 1, (size_t)(sub_end - sub_start), io->f);
        if (n != (size_t)(sub_end - sub_start) && ferror(io->f)) {
            return E_IO;
        }

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
        if (err != E_OK) {
            return err;
        }

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
        if (sub_end > block->block_hi) {
            sub_end = block->block_hi;
        }

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
        if (fseeko(io->f, sub_start, SEEK_SET) != 0) {
            return E_IO;
        }
        size_t n = fread(io->buf, 1, (size_t)(sub_end - sub_start), io->f);
        if (n != (size_t)(sub_end - sub_start) && ferror(io->f)) {
            return E_IO;
        }

        i64 scan_start = cur_pos - sub_start;
        for (size_t i = (size_t)scan_start; i < n; ++i) {
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

enum Err io_step_lines(File* io, i64 start_line_start, i32 delta, i64* out_line_start)
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
            if (err != E_OK) {
                return err;
            }

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
            if (err != E_OK) {
                return err;
            }
            current = line_start;
        }
    }

    *out_line_start = current;
    return E_OK;
}

/*****************************
 * UTF-8 CHARACTER NAVIGATION
 *****************************/

enum Err io_prev_char_start(File* io, i64 pos, i64* out)
{
    if (pos <= 0) {
        *out = 0;
        return E_OK;
    }
    if (pos >= io->size) {
        *out = io->size;
        return E_OK;
    }

    // Read up to 4 bytes before pos to find a non-continuation byte
    i64 lo = pos - 4;
    if (lo < 0) {
        lo = 0;
    }
    i64 hi = pos;
    if (fseeko(io->f, lo, SEEK_SET) != 0) {
        return E_IO;
    }
    size_t n = fread(io->buf, 1, (size_t)(hi - lo), io->f);
    if (n == 0 && ferror(io->f)) {
        return E_IO;
    }

    i64 rel_end = (i64)n; // number of bytes we have (hi - lo)
    if (rel_end <= 0) {
        *out = pos;
        return E_OK;
    }

    // Scan backward from pos-1 toward lo to find a non-continuation byte
    for (i64 k = 1; k <= rel_end; ++k) {
        unsigned char b = io->buf[rel_end - k];
        if (!utf8_is_cont_byte(b)) {
            // Validate forward length; if malformed, treat that byte as a single-char
            i32 len = utf8_len_from_lead_byte(b);
            i64 start = hi - k;
            if (len == 0 || start + len > io->size) {
                *out = start;
                return E_OK;
            }
            // If the character we found ends exactly at 'pos' and there's more data,
            // the cursor is already on a character boundary; keep it there.
            if (start + len == pos && pos < io->size) {
                *out = pos;
                return E_OK;
            }
            *out = start;
            return E_OK;
        }
    }
    // All were continuation bytes; treat lo as boundary (permissive)
    *out = lo ? lo : pos;
    return E_OK;
}

enum Err io_step_chars(File* io, i64 start, i32 delta, i64* out)
{
    if (start < 0) {
        start = 0;
    }
    if (start > io->size) {
        start = io->size;
    }

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
            if (hi > io->size) {
                hi = io->size;
            }
            if (fseeko(io->f, cur, SEEK_SET) != 0) {
                return E_IO;
            }
            size_t n = fread(io->buf, 1, (size_t)(hi - cur), io->f);
            if (n == 0) {
                *out = cur;
                return E_OK;
            }

            unsigned char b0 = io->buf[0];
            i32 len = utf8_len_from_lead_byte(b0);
            if (len == 0) {
                // malformed lead -> count as 1
                cur += 1;
            } else {
                // ensure we have len bytes and continuations are well-formed; else permissive 1
                if ((i64)len <= (i64)n) {
                    bool ok = true;
                    for (i32 j = 1; j < len; j++) {
                        if (!utf8_is_cont_byte(io->buf[j])) {
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
    }

    // backward
    i32 steps = -delta;
    for (i32 i = 0; i < steps; ++i) {
        if (cur <= 0) {
            *out = 0;
            return E_OK;
        }
        i64 start_char;
        // snap to the start of the char immediately before cur
        enum Err e = io_prev_char_start(io, cur - 1, &start_char);
        if (e != E_OK) {
            return e;
        }
        cur = start_char;
    }
    *out = cur;
    return E_OK;
}

/****************
 * LINE INDEXING
 ****************/

static enum Err get_line_block(File* io, i64 pos, LineBlockIdx** out)
{
    if (pos < 0) {
        pos = 0;
    }
    if (pos > io->size) {
        pos = io->size;
    }

    i64 block_lo = (pos / IDX_BLOCK) * (i64)IDX_BLOCK;
    i64 block_hi = block_lo + IDX_BLOCK;
    if (block_hi > io->size) {
        block_hi = io->size;
    }

    // 1) hit?
    i32 free_slot = -1;
    i32 lru_slot = 0;
    for (i32 i = 0; i < IDX_MAX_BLOCKS; ++i) {
        LineBlockIdx* e = &io->line_idx[i];
        if (!e->in_use) {
            if (free_slot < 0) {
                free_slot = i;
            }
            continue;
        }
        if (e->block_lo == block_lo && e->block_hi == block_hi) {
            e->gen = ++io->line_idx_gen;
            *out = e;
            return E_OK;
        }
        if (io->line_idx[i].gen < io->line_idx[lru_slot].gen) {
            lru_slot = i;
        }
    }

    i32 slot = (free_slot >= 0) ? free_slot : lru_slot;

    // 2) (Re)build index in slot
    LineBlockIdx* e = &io->line_idx[slot];

    i32 sub_count = (i32)((block_hi - block_lo + IDX_SUB - 1) / IDX_SUB);
    if (sub_count <= 0) {
        sub_count = 1;
    }
    if (sub_count > IDX_SUB_MAX) {
        sub_count = IDX_SUB_MAX; // defensive
    }
    e->sub_count = sub_count;

    // compute counts
    for (i32 s = 0; s < sub_count; ++s) {
        e->lf_counts[s] = 0;
    }

    i64 pos_cur = block_lo;
    for (i32 s = 0; s < sub_count; ++s) {
        i64 sub_lo = pos_cur;
        i64 sub_hi = sub_lo + IDX_SUB;
        if (sub_hi > block_hi) {
            sub_hi = block_hi;
        }

        if (fseeko(io->f, sub_lo, SEEK_SET) != 0) {
            return E_IO;
        }
        size_t n = fread(io->buf, 1, (size_t)(sub_hi - sub_lo), io->f);
        if (n != (size_t)(sub_hi - sub_lo) && ferror(io->f)) {
            return E_IO;
        }

        unsigned short cnt = 0;
        for (size_t i = 0; i < n; ++i) {
            if (io->buf[i] == '\n') {
                cnt++;
            }
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

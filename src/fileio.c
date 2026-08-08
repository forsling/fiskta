#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "fileio.h"
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

/***********************
 * FILE I/O OPERATIONS *
 ***********************/

enum FisktaErr io_open(File* io, const char* path,
    unsigned char* search_buf, size_t search_buf_cap)
{
    memset(io, 0, sizeof(*io));
    io->mode = FILE_MODE_DISK;

    if (strcmp(path, "-") == 0) {
        // Spool stdin to temp file
        io->disk.f = tmpfile();
        if (!io->disk.f) {
            return FISKTA_E_IO;
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
                    fclose(io->disk.f);
                    return FISKTA_E_IO;
                }
                break;
            }

            size_t written = fwrite(buf, 1, n, io->disk.f);
            if (written != n) {
                fclose(io->disk.f);
                return FISKTA_E_IO;
            }
        }

        if (fflush(io->disk.f) != 0) {
            fclose(io->disk.f);
            return FISKTA_E_IO;
        }
        if (fseeko(io->disk.f, 0, SEEK_END) != 0) {
            fclose(io->disk.f);
            return FISKTA_E_IO;
        }
        off_t sz = ftello(io->disk.f);
        if (sz < 0) {
            fclose(io->disk.f);
            return FISKTA_E_IO;
        }
        io->size = (i64)sz;
        if (fseek(io->disk.f, 0, SEEK_SET) != 0) {
            fclose(io->disk.f);
            return FISKTA_E_IO;
        }
    } else {
        io->disk.f = fopen(path, "rb");
        if (!io->disk.f) {
            return FISKTA_E_IO;
        }

        if (fseeko(io->disk.f, 0, SEEK_END) != 0) {
            fclose(io->disk.f);
            return FISKTA_E_IO;
        }
        off_t sz = ftello(io->disk.f);
        if (sz < 0) {
            fclose(io->disk.f);
            return FISKTA_E_IO;
        }
        io->size = (i64)sz;
        if (fseek(io->disk.f, 0, SEEK_SET) != 0) {
            fclose(io->disk.f);
            return FISKTA_E_IO;
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

    return FISKTA_E_OK;
}

enum FisktaErr io_open_buffer(File* io, const unsigned char* data, size_t len,
    unsigned char* search_buf, size_t search_buf_cap)
{
    memset(io, 0, sizeof(*io));
    io->mode = FILE_MODE_MEMORY;

    io->mem.data = data;
    io->mem.len = len;
    io->mem.pos = 0;
    io->size = (i64)len;

    io->buf = search_buf;
    io->buf_cap = search_buf_cap;

    io->line_idx_gen = 0;
    for (i32 i = 0; i < IDX_MAX_BLOCKS; ++i) {
        io->line_idx[i].in_use = false;
        io->line_idx[i].gen = 0;
        io->line_idx[i].sub_count = 0;
    }

    return FISKTA_E_OK;
}

void io_close(File* io)
{
    if (io->mode == FILE_MODE_DISK && io->disk.f) {
        fclose(io->disk.f);
        io->disk.f = NULL;
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
    if (io->mode == FILE_MODE_DISK && io->disk.f) {
        // 64-bit safe seek to BOF
        if (fseeko(io->disk.f, 0, SEEK_SET) != 0) {
            // Best effort: clear any error state
            clearerr(io->disk.f);
        }
    } else if (io->mode == FILE_MODE_MEMORY) {
        io->mem.pos = 0;
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

// Helper to read from a specific offset into a buffer
// Returns FISKTA_E_OK on success, FISKTA_E_IO on error
// Sets *actual_out to the number of bytes actually read
enum FisktaErr io_read_at(File* io, i64 offset, unsigned char* dest, size_t requested, size_t* actual_out)
{
    if (offset < 0 || offset > io->size) {
        *actual_out = 0;
        return FISKTA_E_IO;
    }

    if (io->mode == FILE_MODE_DISK) {
        if (fseeko(io->disk.f, offset, SEEK_SET) != 0) {
            *actual_out = 0;
            return FISKTA_E_IO;
        }
        size_t n = fread(dest, 1, requested, io->disk.f);
        if (n == 0 && ferror(io->disk.f)) {
            *actual_out = 0;
            return FISKTA_E_IO;
        }
        *actual_out = n;
        return FISKTA_E_OK;
    } else {
        // FILE_MODE_MEMORY
        i64 available = io->size - offset;
        if (available <= 0) {
            *actual_out = 0;
            return FISKTA_E_OK;
        }
        size_t to_copy = (size_t)available < requested ? (size_t)available : requested;
        memcpy(dest, io->mem.data + offset, to_copy);
        *actual_out = to_copy;
        return FISKTA_E_OK;
    }
}

enum FisktaErr io_emit(File* io, i64 start, i64 end, FILE* out)
{
    if (start >= end) {
        return FISKTA_E_OK;
    }
    if (start < 0 || end > io->size) {
        return FISKTA_E_IO;
    }

    i64 offset = start;
    i64 remaining = end - start;
    while (remaining > 0) {
        size_t chunk_size = (remaining > (i64)io->buf_cap) ? io->buf_cap : (size_t)remaining;
        size_t n;
        enum FisktaErr err = io_read_at(io, offset, io->buf, chunk_size, &n);
        if (err != FISKTA_E_OK) {
            return err;
        }
        if (n == 0) {
            break;
        }

        size_t written = fwrite(io->buf, 1, n, out);
        if (written != n) {
            return FISKTA_E_IO;
        }

        offset += (i64)n;
        remaining -= (i64)n;
    }

    return FISKTA_E_OK;
}

/*******************
 * LINE NAVIGATION *
 *******************/

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
static enum FisktaErr get_line_block(File* io, i64 pos, LineBlockIdx** out);

enum FisktaErr io_line_start(File* io, i64 pos, i64* out)
{
    if (pos <= 0) {
        *out = 0;
        return FISKTA_E_OK;
    }

    // Use line index for fast reverse block jumping
    i64 cur_pos = pos - 1; // We want the previous LF before or at pos-1
    while (cur_pos >= 0) {
        LineBlockIdx* block;
        enum FisktaErr err = get_line_block(io, cur_pos, &block);
        if (err != FISKTA_E_OK) {
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
        size_t n;
        enum FisktaErr read_err = io_read_at(io, sub_start, io->buf, (size_t)(sub_end - sub_start), &n);
        if (read_err != FISKTA_E_OK) {
            return read_err;
        }

        i64 scan_end = cur_pos - sub_start;
        for (i64 i = scan_end; i >= 0; --i) {
            if (io->buf[i] == '\n') {
                *out = sub_start + i + 1;
                return FISKTA_E_OK;
            }
        }

        // No LF found in this subchunk, move to previous
        cur_pos = sub_start - 1;
    }

    *out = 0;
    return FISKTA_E_OK;
}

enum FisktaErr io_line_end(File* io, i64 pos, i64* out)
{
    if (pos < 0) {
        *out = 0;
        return FISKTA_E_OK;
    }
    if (pos >= io->size) {
        *out = io->size;
        return FISKTA_E_OK;
    }

    // Use line index for fast block jumping
    i64 cur_pos = pos;
    while (cur_pos < io->size) {
        LineBlockIdx* block;
        enum FisktaErr err = get_line_block(io, cur_pos, &block);
        if (err != FISKTA_E_OK) {
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
        size_t n;
        enum FisktaErr read_err = io_read_at(io, sub_start, io->buf, (size_t)(sub_end - sub_start), &n);
        if (read_err != FISKTA_E_OK) {
            return read_err;
        }

        i64 scan_start = cur_pos - sub_start;
        for (size_t i = (size_t)scan_start; i < n; ++i) {
            if (io->buf[i] == '\n') {
                *out = sub_start + (i64)i + 1;
                return FISKTA_E_OK;
            }
        }

        // No LF found in this subchunk, move to next
        cur_pos = sub_end;
    }

    *out = io->size;
    return FISKTA_E_OK;
}

enum FisktaErr io_step_lines(File* io, i64 start_line_start, i64 delta, i64* out_line_start)
{
    if (start_line_start < 0 || start_line_start > io->size) {
        return FISKTA_E_LOC_RESOLVE;
    }

    i64 current = start_line_start;

    if (delta > 0) {
        // Move forward by delta lines
        for (i64 i = 0; i < delta; i++) {
            i64 line_end;
            enum FisktaErr err = io_line_end(io, current, &line_end);
            if (err != FISKTA_E_OK) {
                return err;
            }

            if (line_end >= io->size) {
                *out_line_start = io->size;
                return FISKTA_E_OK;
            }
            current = line_end;
        }
    } else if (delta < 0) {
        // Move backward — count up from delta to avoid negating INT64_MIN
        for (i64 i = delta; i < 0; i++) {
            if (current == 0) {
                *out_line_start = 0;
                return FISKTA_E_OK;
            }

            i64 line_start;
            enum FisktaErr err = io_line_start(io, current - 1, &line_start);
            if (err != FISKTA_E_OK) {
                return err;
            }
            current = line_start;
        }
    }

    *out_line_start = current;
    return FISKTA_E_OK;
}

/******************************
 * UTF-8 CHARACTER NAVIGATION *
 ******************************/

enum FisktaErr io_prev_char_start(File* io, i64 pos, i64* out)
{
    if (pos <= 0) {
        *out = 0;
        return FISKTA_E_OK;
    }
    if (pos >= io->size) {
        *out = io->size;
        return FISKTA_E_OK;
    }

    // Read up to 4 bytes before pos to find a non-continuation byte
    i64 lo = pos - 4;
    if (lo < 0) {
        lo = 0;
    }
    i64 hi = pos;
    size_t n;
    enum FisktaErr err = io_read_at(io, lo, io->buf, (size_t)(hi - lo), &n);
    if (err != FISKTA_E_OK) {
        return err;
    }

    i64 rel_end = (i64)n; // number of bytes we have (hi - lo)
    if (rel_end <= 0) {
        *out = pos;
        return FISKTA_E_OK;
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
                return FISKTA_E_OK;
            }
            // If the character we found ends exactly at 'pos' and there's more data,
            // the cursor is already on a character boundary; keep it there.
            if (start + len == pos && pos < io->size) {
                *out = pos;
                return FISKTA_E_OK;
            }
            *out = start;
            return FISKTA_E_OK;
        }
    }
    // All were continuation bytes; treat lo as boundary (permissive)
    *out = lo ? lo : pos;
    return FISKTA_E_OK;
}

enum FisktaErr io_step_chars(File* io, i64 start, i64 delta, i64* out)
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
        for (i64 i = 0; i < delta; ++i) {
            if (cur >= io->size) {
                *out = io->size;
                return FISKTA_E_OK;
            }
            // Read a small window [cur, cur+4]
            i64 hi = cur + 4;
            if (hi > io->size) {
                hi = io->size;
            }
            size_t n;
            enum FisktaErr err = io_read_at(io, cur, io->buf, (size_t)(hi - cur), &n);
            if (err != FISKTA_E_OK) {
                return err;
            }
            if (n == 0) {
                *out = cur;
                return FISKTA_E_OK;
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
        return FISKTA_E_OK;
    }

    // backward — count up from delta to avoid negating INT64_MIN
    for (i64 i = delta; i < 0; ++i) {
        if (cur <= 0) {
            *out = 0;
            return FISKTA_E_OK;
        }
        i64 start_char;
        // snap to the start of the char immediately before cur
        enum FisktaErr e = io_prev_char_start(io, cur - 1, &start_char);
        if (e != FISKTA_E_OK) {
            return e;
        }
        cur = start_char;
    }
    *out = cur;
    return FISKTA_E_OK;
}

enum FisktaErr io_char_start_window(File* io, i64 pos, i64 lo, i64 hi, i64* out)
{
    lo = lo < 0 ? 0 : lo;
    hi = hi > io->size ? io->size : hi;
    if (hi < lo) {
        hi = lo;
    }
    if (pos <= lo) {
        *out = lo;
        return FISKTA_E_OK;
    }
    if (pos >= hi) {
        *out = hi;
        return FISKTA_E_OK;
    }

    // A position is inside a character only when a complete, valid sequence
    // spans it wholly inside the window. Otherwise it is already a boundary;
    // this matches the one-byte fallback used for malformed UTF-8.
    i64 read_lo = pos - 3;
    if (read_lo < lo) {
        read_lo = lo;
    }
    i64 read_hi = pos + 3;
    if (read_hi > hi) {
        read_hi = hi;
    }
    size_t n;
    enum FisktaErr err = io_read_at(io, read_lo, io->buf, (size_t)(read_hi - read_lo), &n);
    if (err != FISKTA_E_OK) {
        return err;
    }

    i64 available_hi = read_lo + (i64)n;
    for (i64 candidate = read_lo; candidate < pos; candidate++) {
        i32 len = utf8_len_from_lead_byte(io->buf[candidate - read_lo]);
        i64 end = candidate + len;
        if (len <= 1 || end <= pos || end > hi || end > available_hi) {
            continue;
        }
        bool valid = true;
        for (i32 j = 1; j < len; j++) {
            if (!utf8_is_cont_byte(io->buf[candidate - read_lo + j])) {
                valid = false;
                break;
            }
        }
        if (valid) {
            *out = candidate;
            return FISKTA_E_OK;
        }
    }

    *out = pos;
    return FISKTA_E_OK;
}

enum FisktaErr io_step_chars_window(File* io, i64 start, i64 delta, i64 lo, i64 hi, i64* out)
{
    lo = lo < 0 ? 0 : lo;
    hi = hi > io->size ? io->size : hi;
    if (hi < lo) {
        hi = lo;
    }
    i64 cur = start;
    if (cur < lo) {
        cur = lo;
    }
    if (cur > hi) {
        cur = hi;
    }

    if (delta >= 0) {
        for (i64 i = 0; i < delta; i++) {
            if (cur >= hi) {
                *out = hi;
                return FISKTA_E_OK;
            }
            i64 read_hi = cur + 4;
            if (read_hi > hi) {
                read_hi = hi;
            }
            size_t n;
            enum FisktaErr err = io_read_at(io, cur, io->buf, (size_t)(read_hi - cur), &n);
            if (err != FISKTA_E_OK) {
                return err;
            }
            if (n == 0) {
                *out = cur;
                return FISKTA_E_OK;
            }

            i32 len = utf8_len_from_lead_byte(io->buf[0]);
            bool valid = len > 0 && (size_t)len <= n;
            for (i32 j = 1; valid && j < len; j++) {
                if (!utf8_is_cont_byte(io->buf[j])) {
                    valid = false;
                }
            }
            cur += valid ? len : 1;
        }
        *out = cur;
        return FISKTA_E_OK;
    }

    // Backward movement uses the same window-local decoding rule as forward
    // movement, so truncated and malformed bytes count individually.
    for (i64 i = delta; i < 0; i++) {
        if (cur <= lo) {
            *out = lo;
            return FISKTA_E_OK;
        }
        enum FisktaErr err = io_char_start_window(io, cur - 1, lo, hi, &cur);
        if (err != FISKTA_E_OK) {
            return err;
        }
    }
    *out = cur;
    return FISKTA_E_OK;
}

/*****************
 * LINE INDEXING *
 *****************/

static enum FisktaErr get_line_block(File* io, i64 pos, LineBlockIdx** out)
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
            return FISKTA_E_OK;
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

        size_t n;
        enum FisktaErr err = io_read_at(io, sub_lo, io->buf, (size_t)(sub_hi - sub_lo), &n);
        if (err != FISKTA_E_OK) {
            return err;
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
    return FISKTA_E_OK;
}

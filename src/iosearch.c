#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "iosearch.h"
#include "fileio.h"
#include "error.h"
#include "util.h"
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

// Forward declarations
static enum Err bmh_search_forward(const unsigned char* text, size_t text_len,
    const unsigned char* needle, size_t nlen, i64* ms, i64* me);
static enum Err get_line_block(File* io, i64 pos, LineBlockIdx** out);

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

/******************
 * LINE NAVIGATION
 ******************/

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

/****************
 * STRING SEARCH
 ****************/

enum Err io_find_window(File* io, i64 win_lo, i64 win_hi,
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

/******************************
 * BOYER-MOORE-HORSPOOL SEARCH
 ******************************/

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

/*******************
 * REGEX NFA ENGINE
 *******************/

typedef struct {
    ReThread* v;
    int n, cap;
} ReList;

static inline int cls_has(const ReClass* c, unsigned char ch)
{
    return (c->bits[ch >> 3] >> (ch & 7)) & 1;
}

static void rlist_init(ReList* l, ReThread* buf, int cap)
{
    l->v = buf;
    l->n = 0;
    l->cap = cap;
}
static inline void rlist_clear(ReList* l) { l->n = 0; }
static inline void seen_clear_bytes(unsigned char* seen, size_t bytes) { memset(seen, 0, bytes); }

// Number of 32-bit signature slots per pc in the seen table.
// 8 slots for robustness with nested quantifiers
#ifndef RE_SEEN_SLOTS
#define RE_SEEN_SLOTS 8
#endif

// Fast 32-bit signature over the counter array. Guarantees non-zero.
// Only hashes active counters (up to highest non-zero index) to reduce aliasing.
static inline u32 re_counters_sig(const int* cnt, int n)
{
    // Find highest non-zero counter
    int active_n = 0;
    for (int i = n - 1; i >= 0; i--) {
        if (cnt[i] != 0) {
            active_n = i + 1;
            break;
        }
    }

    // FNV-1a with a tiny avalanche; counters are small ints (0..big)
    u32 h = 2166136261u;
    for (int i = 0; i < active_n; i++) {
        h ^= (u32)cnt[i];
        h *= 16777619u;
        // mix a bit to decorrelate low variance
        h ^= h >> 13;
        h *= 0x9E3779B1u;
    }
    if (h == 0) h = 1; // reserve 0 for "empty"
    return h;
}

// Probe up to RE_SEEN_SLOTS 32-bit entries for this pc. If found, return 1.
// If an empty slot (0), store and return 0. If full, do a deterministic replace.
static inline int re_seen_hit_or_set(unsigned char* seen, int pc, u32 sig)
{
    // Base pointer to this pc's slot array
    u32* slots = (u32*)(seen + ((size_t)pc * RE_SEEN_SLOTS * sizeof(u32)));

    // Exact match or empty slot fast path
    for (int i = 0; i < RE_SEEN_SLOTS; i++) {
        u32 v = slots[i];
        if (v == sig) {
            return 1; // already visited
        }
        if (v == 0) {
            slots[i] = sig;
            return 0; // new
        }
    }

    // All slots full with different signatures: replace a slot deterministically
    int idx = (int)(sig % RE_SEEN_SLOTS);
    slots[idx] = sig;
    return 0;
}

// Maximum recursion depth to prevent stack overflow with pathological patterns
#define MAX_EPSILON_RECURSION_DEPTH 500

// Ordered epsilon-closure push. Sets *match_found if RI_MATCH reachable for current pos and min_start.
// Returns E_OOM if thread list capacity is exceeded.
// Returns E_CAPACITY if recursion depth exceeds limit.
static enum Err add_thread_ordered(const ReProg* p, ReList* l, int pc, i64 start,
    i64 pos, i64 win_lo, i64 win_hi, i64 file_size,
    unsigned char* seen, int* match_found, i64 min_start,
    unsigned char curr_char, unsigned char prev_char,
    int at_bol, int at_eol, const int* counters, u64 priority, int depth)
{
    // Guard against stack overflow from pathological patterns like ((x*){0}){999,}
    if (depth > MAX_EPSILON_RECURSION_DEPTH) {
        return E_CAPACITY;
    }

    // Local counter state for this thread
    int local_counters[MAX_RE_COUNTERS];
    int counter_count = p->counter_count;
    if (counter_count > 0) {
        memcpy(local_counters, counters, (size_t)counter_count * sizeof(int));
    }

    while (1) {
        if (pc < 0 || pc >= p->nins) {
            return E_OK;
        }
        // Dedup by (pc, counter_signature)
        u32 sig = (counter_count > 0) ? re_counters_sig(local_counters, counter_count) : 1;
        if (re_seen_hit_or_set(seen, pc, sig)) {
            return E_OK;  // Already visited this (pc, counter_state) combination
        }

        ReInst* i = &p->ins[pc];
        switch (i->op) {
        case RI_SPLIT: {
            // Snapshot counter state so both branches see the same starting values
            int saved_counters[MAX_RE_COUNTERS];
            if (counter_count > 0) {
                memcpy(saved_counters, local_counters, (size_t)counter_count * sizeof(int));
            }

            // Lazy-only penalty scheme:
            // - Extract flags: bit0=repeat, bit1=lazy
            // - Only apply +1 penalty to y-branch when BOTH repeat AND lazy are set
            // - Greedy quantifiers and alternation get no penalty (priorities tie, longer match wins)
            int repeat = (i->ch & 0x01) != 0;
            int lazy = (i->ch & 0x02) != 0;

            u64 prio_x, prio_y;
            if (repeat && lazy) {
                // Lazy quantifier SPLIT: x=exit (preferred), y=loop (penalized)
                prio_x = priority;       // X-branch: no penalty (lazy exit)
                prio_y = priority + 1;   // Y-branch: +1 penalty (lazy loop)
            } else {
                // Greedy quantifier or alternation: no penalty
                prio_x = priority;
                prio_y = priority;
            }

            // Explore X-branch first (always preferred by compiler's x/y assignment)
            enum Err err = add_thread_ordered(p, l, i->x, start, pos, win_lo, win_hi, file_size, seen, match_found, min_start, curr_char, prev_char, at_bol, at_eol, local_counters, prio_x, depth + 1);
            if (err != E_OK) {
                return err;
            }

            // Restore counters before exploring the alternate branch
            if (counter_count > 0) {
                memcpy(local_counters, saved_counters, (size_t)counter_count * sizeof(int));
            }

            // Continue with Y-branch
            priority = prio_y;
            pc = i->y;
            continue;
        }
        case RI_JMP:
            pc = i->x;
            continue;
        case RI_COUNTER_RESET:
            // Reset counter to 0 and continue
            if (i->x >= 0 && i->x < MAX_RE_COUNTERS) {
                local_counters[i->x] = 0;
            }
            pc++;
            continue;
        case RI_COUNTER_INC:
            // Increment counter and continue to next instruction
            if (i->x >= 0 && i->x < MAX_RE_COUNTERS) {
                local_counters[i->x]++;
            }
            pc++;
            continue;
        case RI_COUNTER_CHECK:
            // If counter[x] >= y, fail this thread; else continue
            if (i->x >= 0 && i->x < MAX_RE_COUNTERS) {
                if (local_counters[i->x] >= i->y) {
                    // Counter limit reached, fail this thread
                    return E_OK;
                }
            }
            pc++;
            continue;
        case RI_COUNTER_CHECK_MIN:
            // If counter[x] < y, fail this thread; else continue
            if (i->x >= 0 && i->x < MAX_RE_COUNTERS) {
                if (local_counters[i->x] < i->y) {
                    // Minimum not met, fail this thread
                    return E_OK;
                }
            }
            pc++;
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
            if (start == min_start) {
                *match_found = 1;
            }
            // DEBUG: Print match info
            #ifdef DEBUG_PRIORITY
            fprintf(stderr, "MATCH: start=%lld, pos=%lld, priority=%llu\n", (long long)start, (long long)pos, (unsigned long long)priority);
            #endif
            // Add the thread to the list so consumption step can detect it
            if (l->n >= l->cap) {
                return E_CAPACITY; // Thread list is full
            }
            l->v[l->n].pc = pc;
            l->v[l->n].start = start;
            if (counter_count > 0) {
                memcpy(l->v[l->n].counters, local_counters, (size_t)counter_count * sizeof(int));
            }
            l->v[l->n].priority = priority;
            l->n++;
            return E_OK;
        case RI_CHAR:
        case RI_ANY:
        case RI_CLASS:
            // consuming; add once
            if (l->n >= l->cap) {
                return E_CAPACITY; // Thread list is full
            }
            l->v[l->n].pc = pc;
            l->v[l->n].start = start;
            if (counter_count > 0) {
                memcpy(l->v[l->n].counters, local_counters, (size_t)counter_count * sizeof(int));
            }
            l->v[l->n].priority = priority;
            l->n++;
            return E_OK;
        default:
            return E_OK;
        }
    }
}

enum Err io_find_regex_window(File* io, i64 win_lo, i64 win_hi,
    const ReProg* re, enum Dir dir, i64* ms, i64* me)
{
    if (!re || re->nins <= 0) {
        return E_PARSE;
    }
    if (win_lo < 0) {
        win_lo = 0;
    }
    if (win_hi > io->size) {
        win_hi = io->size;
    }
    if (win_lo >= win_hi) {
        return E_NO_MATCH;
    }

    // Tail byte prefetching for cheap lookahead
    unsigned char tail1 = 0;
    unsigned char tail2 = 0;
    int tails = 0;

    // Use arena-backed scratch provided at startup
    const int nins = re->nins;
    const int cap = io->re.cap;
    if (cap <= 0 || !io->re.curr || !io->re.next || !io->re.seen_curr || !io->re.seen_next) {
        return E_OOM;
    }
    const size_t need_seen = (size_t)nins * RE_SEEN_SLOTS * sizeof(u32);
    if (need_seen > io->re.seen_bytes) {
        error_detail_set(E_CAPACITY, -1,
            "regex: seen buffer too small (need %zu bytes, have %zu); increase re_ins_estimate",
            need_seen, io->re.seen_bytes);
        return E_CAPACITY;  // Seen buffer not large enough for regex
    }

    ReThread* curr_buf = io->re.curr;
    ReThread* next_buf = io->re.next;
    unsigned char* seen_curr = io->re.seen_curr;
    unsigned char* seen_next = io->re.seen_next;
    ReList curr;
    ReList next;
    rlist_init(&curr, curr_buf, cap);
    rlist_init(&next, next_buf, cap);
    seen_clear_bytes(seen_curr, need_seen);
    seen_clear_bytes(seen_next, need_seen);

    i64 best_ms = -1;
    i64 best_me = -1;
    u64 best_priority = UINT64_MAX;  // Worst priority (higher = worse)
    i64 min_start = 0;
    int have_min = 0;

    i64 pos = win_lo;
    i64 block_lo = win_lo;
    i64 block_hi = win_lo;
    size_t n = 0;
    // carry previous byte across block boundaries
    unsigned char prev_c = 0;
    int have_prev = 0;

    // Initialize previous character correctly for anchor semantics
    if (win_lo > 0) {
        if (fseeko(io->f, win_lo - 1, SEEK_SET) != 0) {
            return E_IO;
        }
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
            if (block_hi > win_hi) {
                block_hi = win_hi;
            }
            if (fseeko(io->f, block_lo, SEEK_SET) != 0) {
                return E_IO;
            }
            n = fread(io->buf, 1, (size_t)(block_hi - block_lo), io->f);
            if (n == 0 && ferror(io->f)) {
                return E_IO;
            }
            block_hi = block_lo + (i64)n; // clamp to bytes actually in buf

            // Prefetch up to two "tail" bytes beyond the block for cheap lookahead
            tail1 = tail2 = 0;
            tails = 0;
            if (block_hi < win_hi) {
                if (fseeko(io->f, block_hi, SEEK_SET) != 0) {
                    return E_IO;
                }
                unsigned char t[2];
                size_t bytes_to_read = (win_hi - block_hi) >= 2 ? 2 : (size_t)(win_hi - block_hi);
                size_t m = fread(t, 1, bytes_to_read, io->f);
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
        unsigned char next1 = 0;
        unsigned char next2 = 0;
        if (pos + 1 < block_hi) {
            next1 = io->buf[pos + 1 - block_lo];
        } else if (pos + 1 == block_hi && tails >= 1) {
            next1 = tail1;
        }

        if (pos + 2 < block_hi) {
            next2 = io->buf[pos + 2 - block_lo];
        } else if (pos + 2 == block_hi && tails >= 1) {
            next2 = tail1;
        } else if (pos + 2 == block_hi + 1 && tails >= 2) {
            next2 = tail2;
        }

        unsigned char prev_char = have_prev ? prev_c : 0;

        // Compute anchor booleans once per loop
        int at_bol = (pos == win_lo) || (have_prev && prev_c == '\n');
        int at_eol = (pos == win_hi) || (pos + 1 == win_hi) || // at last character of file
            (next1 == '\n') || (next1 == '\r' && next2 == '\n'); // CRLF-aware

        // If no active threads, start a new leftmost attempt at pos
        if (curr.n == 0) {
            have_min = 1;
            min_start = pos;
            seen_clear_bytes(seen_curr, need_seen);
            int match_found = 0;
            int zero_counters[MAX_RE_COUNTERS] = {0};
            enum Err err = add_thread_ordered(re, &curr, 0, pos, pos, win_lo, win_hi, io->size,
                seen_curr, &match_found, min_start, curr_c, prev_char, at_bol, at_eol, zero_counters, 0ULL, 0);
            if (err != E_OK) {
                return err;
            }
            if (match_found) {
                // epsilon-only match (no consumption): end == pos
                // Find the priority of the best MATCH thread at min_start
                u64 match_priority = UINT64_MAX;
                for (int i = 0; i < curr.n; i++) {
                    if (curr.v[i].start == min_start && curr.v[i].pc >= 0 &&
                        curr.v[i].pc < re->nins && re->ins[curr.v[i].pc].op == RI_MATCH) {
                        if (curr.v[i].priority < match_priority) {
                            match_priority = curr.v[i].priority;
                        }
                    }
                }

                // Three-tier comparison per user spec:
                // (1) earlier start wins
                // (2) same start → compare (priority, end) lexicographically
                //     - Better priority wins regardless of end
                //     - Equal priority → longer end wins
                //     - Worse priority AND shorter/equal end → reject
                int accept_match = 0;
                if (best_ms < 0) {
                    accept_match = 1;  // First match
                } else if (min_start < best_ms) {
                    accept_match = 1;  // Tier 1: Earlier start wins
                } else if (min_start == best_ms) {
                    // Lexicographic comparison: (priority, -end)
                    // Lower priority is better; for equal priority, longer end is better
                    if (match_priority < best_priority) {
                        accept_match = 1;  // Better priority
                    } else if (match_priority == best_priority && pos > best_me) {
                        accept_match = 1;  // Equal priority, longer end
                    }
                    // Note: worse priority is rejected even if longer
                }

                #ifdef DEBUG_PRIORITY
                fprintf(stderr, "Epsilon-1: min_start=%lld, pos=%lld, match_prio=%llu, best_ms=%lld, best_me=%lld, best_prio=%llu, accept=%d\n",
                    (long long)min_start, (long long)pos, (unsigned long long)match_priority,
                    (long long)best_ms, (long long)best_me, (unsigned long long)best_priority, accept_match);
                #endif
                if (accept_match) {
                    best_ms = min_start;
                    best_me = pos;
                    best_priority = match_priority;
                }
                if (dir == DIR_FWD) {
                    // Remove MATCH threads but keep other threads to continue greedy matching
                    int write_idx = 0;
                    for (int i = 0; i < curr.n; i++) {
                        int pc = curr.v[i].pc;
                        i64 st = curr.v[i].start;
                        // Keep only non-MATCH threads from min_start
                        if (st == min_start && (pc < 0 || pc >= re->nins || re->ins[pc].op != RI_MATCH)) {
                            curr.v[write_idx++] = curr.v[i];
                        }
                    }
                    curr.n = write_idx;
                    // If no more threads from min_start, return the best match
                    if (curr.n == 0) {
                        *ms = best_ms;
                        *me = best_me;
                        return E_OK;
                    }
                } else {
                    // Backward search: reset and try next position
                    curr.n = 0;
                    have_min = 0;
                }
            }
        } else {
            seen_clear_bytes(seen_curr, need_seen);
            // Re-run epsilon to discover MATCH at this pos (no consumption)
            int match_found = 0;
            unsigned char curr_char = curr_c;
            // prev_char already computed above
            for (int k = 0; k < curr.n; k++) {
                // IMPORTANT: keep global min_start
                enum Err err = add_thread_ordered(re, &curr, curr.v[k].pc, curr.v[k].start, pos, win_lo, win_hi, io->size,
                    seen_curr, &match_found, min_start, curr_char, prev_char, at_bol, at_eol, curr.v[k].counters, curr.v[k].priority, 0);
                if (err != E_OK) {
                    return err;
                }
            }
            if (match_found) {
                // epsilon-only match at current pos
                // Find the priority of the best MATCH thread at min_start
                u64 match_priority = UINT64_MAX;
                for (int i = 0; i < curr.n; i++) {
                    if (curr.v[i].start == min_start && curr.v[i].pc >= 0 &&
                        curr.v[i].pc < re->nins && re->ins[curr.v[i].pc].op == RI_MATCH) {
                        if (curr.v[i].priority < match_priority) {
                            match_priority = curr.v[i].priority;
                        }
                    }
                }

                if (dir == DIR_FWD) {
                    // For forward search, return immediately with first match (using priority for tie-breaking)
                    *ms = min_start;
                    *me = pos;
                    return E_OK;
                }

                // Backward search: apply three-tier comparison
                int accept_match = 0;
                if (best_ms < 0) {
                    accept_match = 1;  // First match
                } else if (min_start < best_ms) {
                    accept_match = 1;  // Tier 1: Earlier start wins
                } else if (min_start == best_ms) {
                    if (match_priority < best_priority) {
                        accept_match = 1;  // Tier 2: Better priority → ALWAYS wins
                    } else if (match_priority == best_priority && pos > best_me) {
                        accept_match = 1;  // Tier 3: SAME priority → longer end wins
                    }
                }

                if (accept_match) {
                    best_ms = min_start;
                    best_me = pos;
                    best_priority = match_priority;
                }
                curr.n = 0;
                have_min = 0;
            }
        }

        if (pos == win_hi) {
            break; // nothing to consume
        }

        unsigned char c = curr_c;
        // Build next from curr by consuming c
        rlist_clear(&next);
        seen_clear_bytes(seen_next, need_seen);

        for (int i = 0; i < curr.n; i++) {
            int pc = curr.v[i].pc;
            i64 st = curr.v[i].start;
            // Only proceed for threads at current leftmost start
            if (have_min && st > min_start) {
                continue;
            }

            ReInst* inst = &re->ins[pc];
            switch (inst->op) {
            case RI_CHAR:
                if (c == inst->ch) {
                    enum Err err = add_thread_ordered(re, &next, pc + 1, st, pos + 1, win_lo, win_hi, io->size, seen_next, &(int) { 0 }, min_start, c, prev_char, at_bol, at_eol, curr.v[i].counters, curr.v[i].priority, 0);
                    if (err != E_OK) {
                        return err;
                    }
                }
                break;
            case RI_ANY:
                if (c != '\n') { // dot ≠ newline
                    enum Err err = add_thread_ordered(re, &next, pc + 1, st, pos + 1, win_lo, win_hi, io->size, seen_next, &(int) { 0 }, min_start, c, prev_char, at_bol, at_eol, curr.v[i].counters, curr.v[i].priority, 0);
                    if (err != E_OK) {
                        return err;
                    }
                }
                break;
            case RI_CLASS:
                if (inst->cls_idx >= 0 && inst->cls_idx < re->nclasses && cls_has(&re->classes[inst->cls_idx], c)) {
                    enum Err err = add_thread_ordered(re, &next, pc + 1, st, pos + 1, win_lo, win_hi, io->size, seen_next, &(int) { 0 }, min_start, c, prev_char, at_bol, at_eol, curr.v[i].counters, curr.v[i].priority, 0);
                    if (err != E_OK) {
                        return err;
                    }
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
            // For greedy matching, record the match but continue as long as there are active threads from min_start
            // Find the priority of the best MATCH thread at min_start
            u64 match_priority = UINT64_MAX;
            for (int i = 0; i < next.n; i++) {
                if (next.v[i].start == min_start && next.v[i].pc >= 0 &&
                    next.v[i].pc < re->nins && re->ins[next.v[i].pc].op == RI_MATCH) {
                    if (next.v[i].priority < match_priority) {
                        match_priority = next.v[i].priority;
                    }
                }
            }

            // Three-tier comparison
            int accept_match = 0;
            if (best_ms < 0) {
                accept_match = 1;  // First match
            } else if (min_start < best_ms) {
                accept_match = 1;  // Tier 1: Earlier start wins
            } else if (min_start == best_ms) {
                if (match_priority < best_priority) {
                    accept_match = 1;  // Tier 2: Better priority → ALWAYS wins
                } else if (match_priority == best_priority && (pos + 1) > best_me) {
                    accept_match = 1;  // Tier 3: SAME priority → longer end wins
                }
            }

            #ifdef DEBUG_PRIORITY
            fprintf(stderr, "Consume: min_start=%lld, pos+1=%lld, match_prio=%llu, best_ms=%lld, best_me=%lld, best_prio=%llu, accept=%d\n",
                (long long)min_start, (long long)(pos+1), (unsigned long long)match_priority,
                (long long)best_ms, (long long)best_me, (unsigned long long)best_priority, accept_match);
            #endif
            if (accept_match) {
                best_ms = min_start;
                best_me = pos + 1;
                best_priority = match_priority;
            }
            if (dir == DIR_FWD) {
                // Remove MATCH threads and threads not from min_start
                int write_idx = 0;
                for (int i = 0; i < next.n; i++) {
                    int pc = next.v[i].pc;
                    i64 st = next.v[i].start;
                    // Keep only non-MATCH threads from min_start
                    if (st == min_start && (pc < 0 || pc >= re->nins || re->ins[pc].op != RI_MATCH)) {
                        next.v[write_idx++] = next.v[i];
                    }
                }
                next.n = write_idx;
                // If no more threads from min_start, return the best match
                if (next.n == 0) {
                    *ms = best_ms;
                    *me = best_me;
                    return E_OK;
                }
            } else {
                curr.n = 0;
                have_min = 0; // reset for later leftmost starts
            }
        }

        // Advance: swap lists (do NOT swap raw buffers; swap the structs)
        ReList tmp_l = curr;
        curr = next;
        next = tmp_l;
        unsigned char* tmpb = seen_curr;
        seen_curr = seen_next;
        seen_next = tmpb;
        seen_clear_bytes(seen_curr, need_seen);
        // advance and carry previous char
        prev_c = curr_c;
        have_prev = (pos < win_hi);
        pos++;
    }

    if (best_ms >= 0) {
        *ms = best_ms;
        *me = best_me;
        return E_OK;
    }
    return E_NO_MATCH;
}

#pragma once
#include "fiskta.h"
#include "reprog.h"
#include <stdio.h>

// Regex thread state (exposed so startup code can size/allocate scratch)
typedef struct {
    int pc;
    i64 start;
} ReThread;

#ifndef FISKTA_IDX_BLOCK
#define FISKTA_IDX_BLOCK (512 * 1024) // 512 KiB index block
#endif
#ifndef FISKTA_IDX_SUB
#define FISKTA_IDX_SUB (2 * 1024) // 2 KiB subchunks
#endif
#ifndef FISKTA_IDX_MAX_BLOCKS
#define FISKTA_IDX_MAX_BLOCKS 16
#endif

// Line indexing constants
enum {
    IDX_BLOCK = FISKTA_IDX_BLOCK,
    IDX_SUB = FISKTA_IDX_SUB,
    IDX_MAX_BLOCKS = FISKTA_IDX_MAX_BLOCKS,
    IDX_SUB_MAX = IDX_BLOCK / IDX_SUB
};

// Line block index structure
typedef struct {
    i64 block_lo; // file offset of block start (aligned to IDX_BLOCK)
    i64 block_hi; // file offset of block end   (<= block_lo + IDX_BLOCK, clipped at EOF)
    i32 sub_count; // number of subchunks = ceil((block_hi - block_lo)/IDX_SUB)
    // For each subchunk, how many LF bytes are in that subchunk.
    // uint16 is enough: max 4096 LFs per 4 KiB. Fixed-size to simplify ownership.
    unsigned short lf_counts[IDX_SUB_MAX];
    u64 gen; // for LRU
    bool in_use;
} LineBlockIdx;

typedef struct {
    FILE* f;
    i64 size;
    // one reusable buffer for searching
    unsigned char* buf;
    size_t buf_cap; // allocate once, e.g., max(FW_WIN, BK_BLK + OVERLAP_MAX)

    LineBlockIdx line_idx[IDX_MAX_BLOCKS];
    u64 line_idx_gen;

    // Arena-backed regex scratch (set once at startup)
    struct {
        ReThread* curr;
        ReThread* next;
        int cap; // capacity in ReThread entries for curr/next each
        unsigned char* seen_curr;
        unsigned char* seen_next;
        size_t seen_bytes; // bytes available in seen_* (must be >= re->nins)
    } re;
} File;

enum Dir { DIR_FWD = +1,
    DIR_BWD = -1 };

// Open using caller-provided search buffer. No dynamic ownership here.
enum Err io_open(File* io, const char* path,
    unsigned char* search_buf, size_t search_buf_cap);
void io_close(File* io);
void io_reset_full(File* io);
enum Err io_emit(File* io, i64 start, i64 end, FILE* out);

// Provide preallocated regex scratch to File (no mallocs during search).
static inline void io_set_regex_scratch(File* io,
    ReThread* curr, ReThread* next, int cap,
    unsigned char* seen_curr, unsigned char* seen_next, size_t seen_bytes)
{
    io->re.curr = curr;
    io->re.next = next;
    io->re.cap = cap;
    io->re.seen_curr = seen_curr;
    io->re.seen_next = seen_next;
    io->re.seen_bytes = seen_bytes;
}

static inline i64 io_size(const File* io) { return io->size; }

// Line navigation functions
// IMPORTANT: Both functions return positions AFTER newlines:
// - io_line_start: returns position after the previous '\n' (or 0 if no previous '\n')
// - io_line_end: returns position after the next '\n' (or EOF if no next '\n')
// This means: line content with newline = [line_start, line_end)
enum Err io_line_start(File* io, i64 pos, i64* out);
enum Err io_line_end(File* io, i64 pos, i64* out);
enum Err io_step_lines(File* io, i64 start_line_start, i32 delta, i64* out_line_start);

enum Err io_prev_char_start(File* io, i64 pos, i64* out_char_start); // snap to start of the char containing/after pos
enum Err io_step_chars(File* io, i64 start_char_start, i32 delta, i64* out_char_start);

enum Err io_find_window(File* io, i64 win_lo, i64 win_hi,
    const unsigned char* needle, size_t nlen,
    enum Dir dir, i64* ms, i64* me);

// Regex (ordered Thompson NFA), streaming
enum Err io_find_regex_window(File* io, i64 win_lo, i64 win_hi,
    const ReProg* re, enum Dir dir, i64* ms, i64* me);

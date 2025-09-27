// iosearch.h
#pragma once
#include "fiskta.h"
#include <stdio.h>

// Line indexing constants (tunable via environment variables)
enum {
    IDX_BLOCK = 1 * 1024 * 1024,   // 1 MiB index block
    IDX_SUB   = 4 * 1024,          // 4 KiB subchunks
    IDX_MAX_BLOCKS = 64,            // LRU cache size (bounded memory)
    IDX_SUB_MAX = IDX_BLOCK / IDX_SUB  // 256
};

// Line block index structure
typedef struct {
    i64 block_lo;     // file offset of block start (aligned to IDX_BLOCK)
    i64 block_hi;     // file offset of block end   (<= block_lo + IDX_BLOCK, clipped at EOF)
    int sub_count;    // number of subchunks = ceil((block_hi - block_lo)/IDX_SUB)
    // For each subchunk, how many LF bytes are in that subchunk
    // uint16 is enough: max 4096 LFs per 4 KiB
    unsigned short *lf_counts; // length = sub_count
    u64 gen;           // for LRU
    bool in_use;
} LineBlockIdx;

typedef struct {
    FILE* f;
    i64 size;
    // one reusable buffer for searching
    unsigned char* buf;
    size_t buf_cap; // allocate once, e.g., max(FW_WIN, BK_BLK + OVERLAP_MAX)
    bool arena_backed; // if true, buf/lf_counts are arena-owned and must not be freed

    // Bounded LRU cache of line indices
    LineBlockIdx line_idx[IDX_MAX_BLOCKS];
    u64 line_idx_gen;
} File;

enum Dir { DIR_FWD = +1,
    DIR_BWD = -1 };

// io_open removed - use io_open_arena2 with arena allocation instead
enum Err io_open_arena2(File* io, const char* path,
                        unsigned char* search_buf, size_t search_buf_cap,
                        unsigned short* counts_slab /* IDX_MAX_BLOCKS*IDX_SUB_MAX */);
void io_close(File* io);
enum Err io_emit(File* io, i64 start, i64 end, FILE* out);

static inline i64 io_size(const File* io) { return io->size; }

// line boundaries
enum Err io_line_start(File* io, i64 pos, i64* out);
enum Err io_line_end(File* io, i64 pos, i64* out);
enum Err io_step_lines_from(File* io, i64 start_line_start, i32 delta, i64* out_line_start);

// UTF-8 character stepping (permissive)
enum Err io_char_start(File* io, i64 pos, i64* out_char_start);   // snap to start of the char containing/after pos
enum Err io_step_chars_from(File* io, i64 start_char_start, i32 delta, i64* out_char_start);

// search within [win_lo, win_hi); returns E_NO_MATCH if none
enum Err io_find_window(File* io, i64 win_lo, i64 win_hi,
    const unsigned char* needle, size_t nlen,
    enum Dir dir, i64* ms, i64* me);

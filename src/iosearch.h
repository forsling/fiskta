// iosearch.h
#pragma once
#include "fiskta.h"
#include <stdio.h>

typedef struct {
    FILE* f;
    i64 size;
    // one reusable buffer for searching
    unsigned char* buf;
    size_t buf_cap; // allocate once, e.g., max(FW_WIN, BK_BLK + OVERLAP_MAX)
} File;

enum Dir { DIR_FWD = +1,
    DIR_BWD = -1 };

enum Err io_open(File* io, const char* path); // path or "-"
void io_close(File* io);
enum Err io_emit(File* io, i64 start, i64 end, FILE* out);

static inline i64 io_size(const File* io) { return io->size; }

// line boundaries
enum Err io_line_start(File* io, i64 pos, i64* out);
enum Err io_line_end(File* io, i64 pos, i64* out);
enum Err io_step_lines_from(File* io, i64 start_line_start, int delta, i64* out_line_start);

// UTF-8 character stepping (permissive)
enum Err io_char_start(File* io, i64 pos, i64* out_char_start);   // snap to start of the char containing/after pos
enum Err io_step_chars_from(File* io, i64 start_char_start, int delta, i64* out_char_start);

// search within [win_lo, win_hi); returns E_NO_MATCH if none
enum Err io_find_window(File* io, i64 win_lo, i64 win_hi,
    const unsigned char* needle, size_t nlen,
    enum Dir dir, i64* ms, i64* me);

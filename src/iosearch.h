#pragma once
#include "fiskta.h"
#include "regex_prog.h"
#include "fileio.h"
#include <stdio.h>

// Search buffer size constants
#ifndef FISKTA_FW_WIN
#define FISKTA_FW_WIN (6 * 1024 * 1024)
#endif
#ifndef FISKTA_BK_BLK
#define FISKTA_BK_BLK (3 * 1024 * 1024)
#endif
#ifndef FISKTA_OVERLAP_MIN
#define FISKTA_OVERLAP_MIN (4 * 1024)
#endif
#ifndef FISKTA_OVERLAP_MAX
#define FISKTA_OVERLAP_MAX (64 * 1024)
#endif

enum {
    FW_WIN = FISKTA_FW_WIN,
    BK_BLK = FISKTA_BK_BLK,
    OVERLAP_MIN = FISKTA_OVERLAP_MIN,
    OVERLAP_MAX = FISKTA_OVERLAP_MAX
};

enum Dir { DIR_FWD = +1,
    DIR_BWD = -1 };

enum Err io_find_window(File* io, i64 win_lo, i64 win_hi,
    const unsigned char* needle, size_t nlen,
    enum Dir dir, i64* ms, i64* me);

// Regex (ordered Thompson NFA), streaming
enum Err io_find_regex_window(File* io, i64 win_lo, i64 win_hi,
    const ReProg* re, enum Dir dir, i64* ms, i64* me);

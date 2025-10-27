// search_literal.h
//
// Literal pattern search using Boyer-Moore-Horspool algorithm.
// Searches for exact byte sequences in file streams with windowed buffering.
// Supports both forward and backward search directions.

#pragma once
#include "fiskta.h"
#include "fileio.h"

// Search direction
enum Dir { DIR_FWD = +1,
    DIR_BWD = -1 };

// Search for literal needle in file window [win_lo, win_hi)
// Returns match position in [ms, me) on success, E_NO_MATCH if not found
enum Err literal_search_window(File* io, i64 win_lo, i64 win_hi,
    const unsigned char* needle, size_t nlen,
    enum Dir dir, i64* ms, i64* me);

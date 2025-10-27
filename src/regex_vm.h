// regex_vm.h
//
// Streaming ordered Thompson NFA executor for regex matching.
// Given a compiled ReProg and a File (byte-accessible range),
// finds the best match in [win_lo, win_hi) using leftmost-start semantics.
//
// Match tie-breaking rules:
//  1. Smallest start offset (leftmost)
//  2. Lower priority wins (influenced by lazy quantifiers)
//  3. Longer match wins
//
// Handles CRLF-aware anchors and uses preallocated scratch space
// (no runtime allocation).

#pragma once
#include "fiskta.h"
#include "fileio.h"
#include "regex_prog.h"
#include "search_literal.h"

// Regex search in file window [win_lo, win_hi)
// Returns match position in [ms, me) on success, E_NO_MATCH if not found
// Uses scratch buffers provided in io->re (must be preallocated)
enum Err io_find_regex_window(File* io, i64 win_lo, i64 win_hi,
    const ReProg* re, enum Dir dir, i64* ms, i64* me);

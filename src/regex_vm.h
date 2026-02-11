// regex_vm.h
//
// Streaming ordered Thompson NFA executor for regex matching.
// Given a compiled FisktaReProg and a File (byte-accessible range),
// finds the best match in [win_lo, win_hi) using leftmost-start semantics.
//
// Match tie-breaking rules:
//  1. Smallest start offset (leftmost)
//  2. Lower priority wins (influenced by lazy quantifiers)
//  3. Longer match wins
//
// Anchor semantics (CRLF-aware):
//  - '^' (RI_BOL): matches at win_lo OR immediately after '\n' (including '\n' in CRLF)
//  - '$' (RI_EOL): matches at win_hi OR immediately before '\n' (treating CRLF as single line ending)
//
// Uses preallocated scratch space (no runtime allocation).

#pragma once
#include "fileio.h"
#include "fiskta_types.h"
#include "regex_prog.h"

// Regex search in file window [win_lo, win_hi)
//
// Parameters:
//   io:      File handle with preallocated regex scratch (io->re.curr, io->re.next, io->re.seen_*)
//   win_lo:  Start of search window (inclusive)
//   win_hi:  End of search window (exclusive)
//   re:      Compiled regex program (from regex_prog.h)
//   dir:     DIR_FWD (find leftmost) or DIR_BWD (find rightmost)
//   ms, me:  Output match range [ms, me) on success
//
// Returns:
//   FISKTA_E_OK:        Match found, [ms, me) contains match position
//   FISKTA_E_NO_MATCH:  No match in window
//   FISKTA_E_CAPACITY:  Thread list capacity exceeded (pattern too complex)
//   FISKTA_E_IO:        File read error
//
// Match selection (when multiple matches exist):
//   1. Smallest start offset (leftmost match)
//   2. If same start: lower priority wins (lazy quantifiers increase priority)
//   3. If same start+priority: longer match wins
//
// Guarantees:
//   - No heap allocation during search
//   - File position unchanged on return
//   - Thread/seen buffers must be sized per FisktaReProg requirements (see regex_prog.h)
enum FisktaErr regex_search_window(File* io, i64 win_lo, i64 win_hi,
    const FisktaReProg* re, enum Dir dir, i64* ms, i64* me);

#pragma once
#include "fiskta.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RI_CHAR,
    RI_ANY,
    RI_CLASS,
    RI_BOL, // '^'  (true at win_lo or after \n)
    RI_EOL, // '$'  (true at win_hi or before \n)
    RI_SPLIT, // ordered epsilon: x then y (order encodes greediness)
    RI_JMP,
    RI_COUNTER_RESET, // Reset counter[x] = 0; continue to next instruction
    RI_COUNTER_INC,   // Increment counter[x]; continue to next instruction
    RI_COUNTER_CHECK, // If counter[x] >= y, fail thread; else continue
    RI_COUNTER_CHECK_MIN, // If counter[x] < y, fail thread; else continue
    RI_MATCH
} ReOp;

typedef struct {
    unsigned char bits[32]; // 256-bit ASCII bitmap
} ReClass;

typedef struct {
    ReOp op;
    int x, y; // for SPLIT/JMP
    unsigned char ch; // for CHAR
    int cls_idx; // for CLASS
} ReInst;

// Compiled regex program
//
// Resource requirements for execution (must be preallocated by caller):
//   - Thread buffers: Caller must provide capacity for NFA thread lists.
//                     Recommendation: start with 20× nins, cap at 10K threads.
//                     Complex nested quantifiers may require more.
//
//   - Seen table:     nins × RE_SEEN_SLOTS × sizeof(u32) bytes
//                     (RE_SEEN_SLOTS = 8, defined in regex_vm.c)
//                     Used for deduplicating (pc, counter_state) tuples.
//
//   - Counter array:  counter_count integers per thread (max MAX_RE_COUNTERS=16)
//
// Fields:
//   nins:          Number of compiled instructions
//   counter_count: Number of {n,m} quantifiers (affects thread state size)
//   has_lazy:      If 1, lazy quantifiers are present (affects priority tracking)
typedef struct ReProg {
    ReInst* ins;
    int nins;
    ReClass* classes;
    int nclasses;
    int counter_count;  // Number of counters used (0 if none)
    unsigned char has_lazy;  // 1 if pattern contains lazy quantifiers
} ReProg;

// Compile pattern into preallocated pools; appends instructions/classes to the pools.
// Starts at offsets *ins_used / *cls_used and ADVANCES them on success.
enum Err re_compile_into(String pattern,
    ReProg* out,
    ReInst* ins_base, int ins_cap, int* ins_used,
    ReClass* cls_base, int cls_cap, int* cls_used);

// =============================================================================
// Resource requirements API
// =============================================================================

// Runtime memory requirements for a compiled regex program
typedef struct {
    size_t nins;           // Number of compiled instructions (diagnostic/logging)
    size_t seen_bytes;     // Seen table size: nins × RE_SEEN_SLOTS × sizeof(u32), aligned
    size_t thread_cap;     // Recommended thread list capacity
    int counter_count;     // Number of {n,m} quantifiers
    bool has_lazy;         // True if lazy quantifiers present (affects priority tracking)
} ReProgRequirements;

// Compute execution requirements for a compiled regex program
//
// Returns the memory requirements needed to execute this program.
// All sizes are pre-aligned and ready for allocation.
//
// Usage:
//   ReProg prog = ...;
//   ReProgRequirements req;
//   regex_prog_requirements(&prog, &req);
//
//   // Allocate scratch buffers using req.seen_bytes, req.thread_cap, etc.
void regex_prog_requirements(const ReProg* prog, ReProgRequirements* out);

#pragma once
#define _FILE_OFFSET_BITS 64

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int64_t i64;
typedef uint64_t u64;
typedef int32_t i32;
typedef uint32_t u32;
typedef int16_t i16;
typedef uint16_t u16;

// Constants
enum {
    FISKTA_MAX_LABELS = 128,
    FISKTA_MAX_LABEL_LEN = 15,
    FISKTA_MAX_ALTS = 256, // Maximum alternations in regex (a|b|c|...)
    FISKTA_MAX_INLINE_LIT = 24 // Per-\c expansion buffer budget (bytes) reserved
                        // for inline cursor injection during print staging
};

typedef struct {
    const char* bytes;
    size_t len;
} FisktaString;

// Unit type: bytes, lines, chars
typedef uint8_t FisktaUnit;
enum {
    FISKTA_UNIT_BYTES,
    FISKTA_UNIT_LINES,
    FISKTA_UNIT_CHARS // UTF-8 code points
};

typedef uint8_t FisktaOpKind;
enum {
    FISKTA_OP_FIND,
    FISKTA_OP_FIND_RE,
    FISKTA_OP_FIND_BIN,
    FISKTA_OP_SKIP,
    FISKTA_OP_TAKE_LEN,
    FISKTA_OP_TAKE_TO,
    FISKTA_OP_TAKE_UNTIL,
    FISKTA_OP_TAKE_UNTIL_RE,
    FISKTA_OP_TAKE_UNTIL_BIN,
    FISKTA_OP_LABEL,
    FISKTA_OP_LABEL_CLEAR,
    FISKTA_OP_VIEW,
    FISKTA_OP_VIEW_CLEAR,
    FISKTA_OP_PRINT,
    FISKTA_OP_FAIL
};

typedef uint8_t FisktaLocBase;
enum {
    FISKTA_LOC_CURSOR,
    FISKTA_LOC_BOF,
    FISKTA_LOC_EOF,
    FISKTA_LOC_NAME,
    FISKTA_LOC_MATCH_START,
    FISKTA_LOC_MATCH_END,
    FISKTA_LOC_LINE_START,
    FISKTA_LOC_LINE_END
};

enum FisktaErr {
    FISKTA_E_OK = 0,
    FISKTA_E_PARSE,
    FISKTA_E_BAD_NEEDLE,
    FISKTA_E_BAD_HEX,
    FISKTA_E_LOC_RESOLVE,
    FISKTA_E_NO_MATCH,
    FISKTA_E_FAIL_OP,
    FISKTA_E_LABEL_FMT,
    FISKTA_E_LABEL_EXISTS, // Label already set; use 'clear label' to overwrite
    FISKTA_E_IO,
    FISKTA_E_CAPACITY,
    FISKTA_E_OOM
};

// Exit codes
enum FisktaExitCode {
    FISKTA_EXIT_OK = 0,
    FISKTA_EXIT_PROGRAM_FAIL = 1,
    FISKTA_EXIT_TIMEOUT = 2,
    // 3-6 reserved for future outcomes
    FISKTA_EXIT_USAGE = 7, // CLI misuse (unknown flags, missing values)
    FISKTA_EXIT_PARSE = 8, // Parse error (program grammar, regex syntax)
    FISKTA_EXIT_CAPACITY = 9, // Policy limit exceeded (input too complex)
    FISKTA_EXIT_IO = 10,
    FISKTA_EXIT_RESOURCE = 11 // System resource exhaustion (malloc failed, OOM)
};

typedef struct {
    i64 offset;
    i32 name_idx; // index into label table assigned at parse time (-1 otherwise)
    FisktaLocBase base;
    FisktaUnit unit;
} FisktaLocExpr;

typedef struct FisktaReProg FisktaReProg;

typedef struct {
    i64 lo, hi; // half-open [lo, hi)
    bool active;
} FisktaView;

typedef struct {
    FisktaOpKind kind;
    union {
        struct {
            FisktaLocExpr to;
            FisktaString needle;
        } find;
        struct {
            FisktaLocExpr to;
            FisktaString pattern;
            struct FisktaReProg* prog;
        } find_re;
        struct {
            FisktaLocExpr to;
            FisktaString needle; // parsed hex bytes
        } find_bin;
        struct {
            bool is_location; // true for "skip to <loc>", false for "skip <offset><unit>"
            union {
                struct {
                    i64 offset;
                    FisktaUnit unit;
                } by_offset;
                struct {
                    FisktaLocExpr to;
                } to_location;
            };
        } skip;
        struct {
            i64 offset;
            FisktaUnit unit;
        } take_len;
        struct {
            FisktaLocExpr to;
        } take_to;
        struct {
            FisktaString needle;
            bool has_at;
            FisktaLocExpr at;
        } take_until;
        struct {
            FisktaString pattern;
            bool has_at;
            FisktaLocExpr at;
            struct FisktaReProg* prog;
        } take_until_re;
        struct {
            FisktaString needle; // parsed hex bytes
            bool has_at;
            FisktaLocExpr at;
        } take_until_bin;
        struct {
            i32 name_idx;
        } label;
        struct {
            i32 name_idx;
        } label_clear;
        struct {
            FisktaLocExpr a, b;
        } view;
        struct {
            int _; // Required for -pedantic (empty structs non-standard)
        } view_clear;
        struct {
            FisktaString string;
            i16* cursor_offsets;
            i32 cursor_marks;
            i32 literal_segments;
        } print;
        struct {
            FisktaString message;
        } fail;
    } u;
} FisktaOp;

// How clauses are linked together
typedef enum {
    FISKTA_LINK_NONE, // No link (last clause)
    FISKTA_LINK_THEN, // Sequential
    FISKTA_LINK_OR // First success wins
} FisktaClauseLink;

typedef struct {
    FisktaOp* ops;
    i32 op_count;
    FisktaClauseLink link;
} FisktaClause;

typedef struct {
    FisktaClause* clauses;
    i32 clause_count;
    i32 name_count;
} FisktaProgram;

typedef struct {
    i64 start, end;
    bool valid;
} FisktaMatch;

// FisktaVM state is snapshotted per clause execution.
// - cursor, last_match, view, and label_pos[] are staged in StagedResult
//   and only committed on clause success.
// - On clause failure, FisktaVM must be restored exactly to its prior state.
typedef struct {
    i64 cursor;
    FisktaMatch last_match;
    FisktaView view;

    i64 label_pos[FISKTA_MAX_LABELS]; // name_idx -> position mapping (-1 = not set)
} FisktaVM;

// Build-time options for program compilation
//
// Controls resource limits for regex engine. These limits apply globally across
// all patterns in the program and are allocated once at build time.
//
// Default behavior (pass NULL or zero fields):
//   - regex_budget_bytes: 2 MiB total (split between seen tables + thread lists)
//   - regex_work_budget: 50M thread enqueues per search operation
//
// The regex_budget_bytes is split between:
//   - Seen tables: 2 tables (curr+next), sized per-pattern (nins × 32 bytes each)
//   - Thread lists: remaining budget → typically ~10K threads for normal patterns
//
// The regex_work_budget limits total thread enqueues during one search to prevent
// step-count explosion from pathological patterns like ((a?){50}){50}.
typedef struct {
    size_t regex_budget_bytes; // Total regex VM memory budget (0 = 2 MiB default)
    u64 regex_work_budget;     // Max thread enqueues per search (0 = 50M default)
} FisktaBuildOptions;

// Staged capture range or literal string
typedef enum { FISKTA_RANGE_FILE,
    FISKTA_RANGE_LIT } FisktaRangeKind;
typedef struct {
    FisktaRangeKind kind;
    union {
        struct {
            i64 start, end; // used when kind == FISKTA_RANGE_FILE
        } file;
        FisktaString lit; // used when kind == FISKTA_RANGE_LIT
    };
} FisktaRange;

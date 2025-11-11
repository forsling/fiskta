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
    MAX_LABELS = 128,
    MAX_LABEL_LEN = 15,
    MAX_ALTS = 256, // Maximum alternations in regex (a|b|c|...)
    INLINE_LIT_CAP = 24 // Per-\c expansion buffer budget (bytes) reserved
                        // for inline cursor injection during print staging
};

typedef struct {
    const char* bytes;
    size_t len;
} String;

// Unit type: bytes, lines, chars
typedef uint8_t Unit;
enum {
    UNIT_BYTES,
    UNIT_LINES,
    UNIT_CHARS // UTF-8 code points
};

typedef uint8_t OpKind;
enum {
    OP_FIND,
    OP_FIND_RE,
    OP_FIND_BIN,
    OP_SKIP,
    OP_TAKE_LEN,
    OP_TAKE_TO,
    OP_TAKE_UNTIL,
    OP_TAKE_UNTIL_RE,
    OP_TAKE_UNTIL_BIN,
    OP_LABEL,
    OP_VIEWSET,
    OP_VIEWCLEAR,
    OP_PRINT,
    OP_FAIL
};

typedef uint8_t LocBase;
enum {
    LOC_CURSOR,
    LOC_BOF,
    LOC_EOF,
    LOC_NAME,
    LOC_MATCH_START,
    LOC_MATCH_END,
    LOC_LINE_START,
    LOC_LINE_END
};

enum Err {
    E_OK = 0,
    E_PARSE,
    E_BAD_NEEDLE,
    E_BAD_HEX,
    E_LOC_RESOLVE,
    E_NO_MATCH,
    E_FAIL_OP,
    E_LABEL_FMT,
    E_IO,
    E_CAPACITY,
    E_OOM
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
    LocBase base;
    Unit unit;
} LocExpr;

typedef struct ReProg ReProg;

typedef struct {
    i64 lo, hi; // half-open [lo, hi)
    bool active;
} View;

typedef struct {
    OpKind kind;
    union {
        struct {
            LocExpr to;
            String needle;
        } find;
        struct {
            LocExpr to;
            String pattern;
            struct ReProg* prog;
        } findr;
        struct {
            LocExpr to;
            String needle; // parsed hex bytes
        } findbin;
        struct {
            bool is_location; // true for "skip to <loc>", false for "skip <offset><unit>"
            union {
                struct {
                    i64 offset;
                    Unit unit;
                } by_offset;
                struct {
                    LocExpr to;
                } to_location;
            };
        } skip;
        struct {
            i64 offset;
            Unit unit;
        } take_len;
        struct {
            LocExpr to;
        } take_to;
        struct {
            String needle;
            bool has_at;
            LocExpr at;
        } take_until;
        struct {
            String pattern;
            bool has_at;
            LocExpr at;
            struct ReProg* prog;
        } take_until_re;
        struct {
            String needle; // parsed hex bytes
            bool has_at;
            LocExpr at;
        } take_until_bin;
        struct {
            i32 name_idx;
        } label;
        struct {
            LocExpr a, b;
        } viewset;
        struct {
            int _; // Required for -pedantic (empty structs non-standard)
        } viewclear;
        struct {
            String string;
            i16* cursor_offsets;
            i32 cursor_marks;
            i32 literal_segments;
        } print;
        struct {
            String message;
        } fail;
    } u;
} Op;

// How clauses are linked together
typedef enum {
    LINK_NONE, // No link (last clause)
    LINK_THEN, // Sequential
    LINK_OR // First success wins
} ClauseLink;

typedef struct {
    Op* ops;
    i32 op_count;
    ClauseLink link;
} Clause;

typedef struct {
    Clause* clauses;
    i32 clause_count;
    i32 name_count;
} Program;

typedef struct {
    i64 start, end;
    bool valid;
} Match;

// VM state is snapshotted per clause execution.
// - cursor, last_match, view, and label_pos[] are staged in StagedResult
//   and only committed on clause success.
// - On clause failure, VM must be restored exactly to its prior state.
typedef struct {
    i64 cursor;
    Match last_match;
    View view;

    i64 label_pos[MAX_LABELS]; // name_idx -> position mapping (-1 = not set)
} VM;

// Build-time options for program compilation
typedef struct {
    size_t regex_budget_bytes; // 0 = use default (2 MiB)
    u64 regex_work_budget;     // 0 = use default (1M enqueues)
} BuildOptions;

// Staged capture range or literal string
typedef enum { RANGE_FILE,
    RANGE_LIT } RangeKind;
typedef struct {
    RangeKind kind;
    union {
        struct {
            i64 start, end; // used when kind == RANGE_FILE
        } file;
        String lit; // used when kind == RANGE_LIT
    };
} Range;

#pragma once
#define _FILE_OFFSET_BITS 64

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef int64_t i64;
typedef uint64_t u64;
typedef int32_t i32;
typedef uint32_t u32;

// Constants
enum { MAX_LABELS = 128,
    MAX_LABEL_LEN = 15 };

typedef struct {
    const char* bytes;
    i32 len;
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

typedef uint8_t LocRef;
enum {
    REF_CURSOR, // resolve relative to staged cursor
    REF_MATCH // resolve relative to last match (used by 'at' expressions)
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
    E_OOM
};

// Exit codes
enum FisktaExitCode {
    FISKTA_EXIT_OK = 0,
    FISKTA_EXIT_PROGRAM_FAIL = 1,
    FISKTA_EXIT_TIMEOUT = 2,
    // 3-9 reserved for future user-controlled outcomes
    FISKTA_EXIT_IO = 10,
    FISKTA_EXIT_RESOURCE = 11,
    FISKTA_EXIT_PARSE = 12,
    FISKTA_EXIT_REGEX = 13,
};

typedef struct {
    i64 offset;
    i32 name_idx; // index into program->names[], -1 otherwise
    LocBase base;
    Unit unit;
} LocExpr;

typedef struct ReProg ReProg;

typedef struct {
    i64 lo, hi; // half-open [lo, hi)
    bool active;
} View;

typedef enum {
    CLAMP_NONE,
    CLAMP_FILE,
    CLAMP_VIEW
} ClampPolicy;

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
            int _;
        } viewclear;
        struct {
            String string;
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
    char names[MAX_LABELS][MAX_LABEL_LEN + 1];
    i32 name_count;
} Program;

typedef struct {
    i64 start, end;
    bool valid;
} Match;

typedef struct {
    i64 cursor;
    Match last_match;
    View view;

    i64 label_pos[MAX_LABELS]; // name_idx -> position mapping (-1 = not set)
} VM;

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

enum { INLINE_LIT_CAP = 24 };

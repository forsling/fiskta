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
    OP_SKIP,
    OP_TAKE_LEN,
    OP_TAKE_TO,
    OP_TAKE_UNTIL,
    OP_TAKE_UNTIL_RE,
    OP_BOX,
    OP_LABEL,
    OP_GOTO,
    OP_VIEWSET,
    OP_VIEWCLEAR,
    OP_PRINT
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
    E_LOC_RESOLVE,
    E_NO_MATCH,
    E_LABEL_FMT,
    E_IO,
    E_OOM
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
            u64 n;
            Unit unit;
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
            i32 right_offset;
            i32 down_offset;
            Unit unit; // UNIT_BYTES or UNIT_CHARS for horizontal offset
        } box;
        struct {
            i32 name_idx;
        } label;
        struct {
            LocExpr to;
        } go;
        struct {
            LocExpr a, b;
        } viewset;
        struct {
            int _;
        } viewclear;
        struct {
            String string;
        } print;
    } u;
} Op;

// How clauses are linked together
typedef enum {
    LINK_NONE, // No link (last clause)
    LINK_THEN, // Sequential
    LINK_AND, // Both must succeed
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

enum { FW_WIN = 8 * 1024 * 1024,
    BK_BLK = 4 * 1024 * 1024,
    OVERLAP_MIN = 4 * 1024,
    OVERLAP_MAX = 64 * 1024 };

static inline i64 clamp64(i64 x, i64 lo, i64 hi) { return x < lo ? lo : (x > hi ? hi : x); }

// Forward declarations for engine functions
typedef struct {
    i64 pos;
    i32 name_idx;
} LabelWrite;

// Staged execution result
typedef struct {
    VM staged_vm; // Staged VM state (cursor, last_match, view)
    Range* ranges; // Staged output ranges
    i32 range_count; // Number of staged ranges
    LabelWrite* label_writes; // Staged label writes
    i32 label_count; // Number of staged labels
    enum Err err; // Execution result
} StagedResult;

void clause_caps(const Clause* c, i32* out_ranges_cap, i32* out_labels_cap);
enum Err stage_clause(const Clause* clause,
    void* io, VM* vm,
    Range* ranges, i32 ranges_cap,
    LabelWrite* label_writes, i32 label_cap,
    StagedResult* result);

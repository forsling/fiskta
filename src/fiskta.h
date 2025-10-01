// fiskta.h
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

// Length-carrying string (handles embedded NULs)
typedef struct { const char* p; i32 n; } String;

enum Unit {
    UNIT_BYTES,
    UNIT_LINES,
    UNIT_CHARS // UTF-8 code points
};

enum OpKind {
    OP_FIND,
    OP_FINDR,
    OP_SKIP,
    OP_TAKE_LEN,
    OP_TAKE_TO,
    OP_TAKE_UNTIL,
    OP_LABEL,
    OP_GOTO,
    OP_VIEWSET,
    OP_VIEWCLEAR,
    OP_PRINT
};

enum LocBase {
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
    E_LOC_RESOLVE,
    E_NO_MATCH,
    E_LABEL_FMT,
    E_IO,
    E_OOM
};

typedef struct {
    enum LocBase base; // LOC_NAME uses name_idx
    enum Unit unit;
    i64 offset;
    i32 name_idx; // index into program->names[], -1 otherwise
} LocExpr;

typedef struct ReProg ReProg;

// View state (staged per clause; committed on clause success)
typedef struct {
    bool active;
    i64 lo, hi; // half-open [lo, hi)
} View;

// Clamp policy
typedef enum { CLAMP_NONE,
    CLAMP_FILE,
    CLAMP_VIEW } ClampPolicy;

typedef struct {
    union {
        struct {
            LocExpr to;
            String needle;
        } find;
        struct {
            LocExpr to;
            String pattern; // raw pattern text (from pool)
            struct ReProg* prog; // compiled program (arena-backed), set during setup
        } findr;
        struct {
            u64 n;
            enum Unit unit;
        } skip;
        struct {
            i64 offset;
            enum Unit unit;
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
    enum OpKind kind;
} Op;

typedef struct {
    Op* ops;
    i32 op_count;
    i32 op_cap;
} Clause;

typedef struct {
    Clause* clauses;
    i32 clause_count;
    i32 clause_cap;
    /* Static name table (UPPER / '_' / '-', ≤16 chars + NUL) */
    char names[128][17];
    i32 name_count;
} Program;

typedef struct {
    i64 start, end;
    bool valid;
} Match;

typedef struct {
    // global state
    i64 cursor;
    Match last_match;
    View view;

    // Direct label mapping (128 slots, no eviction)
    i64 label_pos[128]; // name_idx -> position mapping
    unsigned char label_set[128]; // 0/1 flags for which labels are set
} VM;

// Staged capture range
typedef enum { RANGE_FILE, RANGE_LIT } RangeKind;
typedef struct {
    RangeKind kind;
    i64 start, end;  // used when kind == RANGE_FILE
    String lit;       // used when kind == RANGE_LIT
} Range;

// ---- constants (tuneable, but fixed here) ----
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

void clause_caps(const Clause* c, i32* out_ranges_cap, i32* out_labels_cap);
enum Err execute_clause_with_scratch(const Clause* clause,
    void* io, VM* vm, FILE* out,
    Range* ranges, i32 ranges_cap,
    LabelWrite* label_writes, i32 label_cap);

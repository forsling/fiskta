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

enum Unit {
    UNIT_BYTES,
    UNIT_LINES,
    UNIT_CHARS // UTF-8 code points
};

enum OpKind {
    OP_FIND,
    OP_SKIP,
    OP_TAKE_LEN,
    OP_TAKE_TO,
    OP_TAKE_UNTIL,
    OP_LABEL,
    OP_GOTO
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
    i32 name_idx; // index into program->names[], -1 otherwise
    // optional offset:
    bool has_off;
    i32 sign; // +1 or -1
    u64 n; // count
    enum Unit unit;
} LocExpr;

typedef struct { // at-expr used only by TAKE_UNTIL
    enum LocBase at; // match-start/end or line-start/end
    bool has_off;
    i32 sign;
    u64 n;
    enum Unit unit;
} AtExpr;

typedef struct {
    enum OpKind kind;
    union {
        struct {
            LocExpr to;
            char* needle;
        } find;
        struct {
            u64 n;
            enum Unit unit;
        } skip;
        struct {
            i32 sign;
            u64 n;
            enum Unit unit;
        } take_len;
        struct {
            LocExpr to;
        } take_to;
        struct {
            char* needle;
            bool has_at;
            AtExpr at;
        } take_until;
        struct {
            i32 name_idx;
        } label;
        struct {
            LocExpr to;
        } go;
    } u;
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
    char (*names)[17];
    i32 name_count;
    i32 name_cap; // dedupbed label names
} Program;

typedef struct {
    i64 start, end;
    bool valid;
} Match;

typedef struct {
    // global state
    i64 cursor;
    Match last_match;

    // labels (LRU of 32)
    struct {
        i32 name_idx;
        i64 pos;
        u64 gen;
        bool in_use;
    } labels[32];
    u64 gen_counter;
} VM;

// Staged capture range
typedef struct {
    i64 start, end;
} Range;

// ---- constants (tuneable, but fixed here) ----
enum { FW_WIN = 8 * 1024 * 1024,
    BK_BLK = 4 * 1024 * 1024,
    OVERLAP_MIN = 4 * 1024,
    OVERLAP_MAX = 64 * 1024 };

static inline i64 clamp64(i64 x, i64 lo, i64 hi) { return x < lo ? lo : (x > hi ? hi : x); }

// Forward declarations for engine functions
typedef struct {
    i32 name_idx;
    i64 pos;
} LabelWrite;

void clause_caps(const Clause* c, i32* out_ranges_cap, i32* out_labels_cap);
enum Err execute_clause_with_scratch(const Clause* clause, const Program* prg,
    void* io, VM* vm, FILE* out,
    Range* ranges, i32 ranges_cap,
    LabelWrite* label_writes, i32 label_cap);

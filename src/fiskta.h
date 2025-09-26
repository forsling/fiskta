// fiskta.h
#pragma once
#define _FILE_OFFSET_BITS 64

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int64_t i64;
typedef uint64_t u64;

enum Unit {
    UNIT_BYTES,
    UNIT_LINES,
    UNIT_CHARS   // NEW: UTF-8 code points
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
    int name_idx; // index into program->names[], -1 otherwise
    // optional offset:
    bool has_off;
    int sign; // +1 or -1
    u64 n; // count
    enum Unit unit;
} LocExpr;

typedef struct { // at-expr used only by TAKE_UNTIL
    enum LocBase at; // match-start/end or line-start/end
    bool has_off;
    int sign;
    u64 n;
    enum Unit unit;
} AtExpr;

typedef struct {
    enum OpKind kind;
    union {
        struct {
            LocExpr to;
            char* needle;
        } find; // OP_FIND
        struct {
            u64 n;
            enum Unit unit;
        } skip; // OP_SKIP
        struct {
            int sign;
            u64 n;
            enum Unit unit;
        } take_len; // OP_TAKE_LEN
        struct {
            LocExpr to;
        } take_to; // OP_TAKE_TO
        struct {
            char* needle;
            bool has_at;
            AtExpr at;
        } take_until; // OP_TAKE_UNTIL
        struct {
            int name_idx;
        } label; // OP_LABEL
        struct {
            LocExpr to;
        } go; // OP_GOTO
    } u;
} Op;

typedef struct {
    Op* ops;
    int op_count;
    int op_cap;
} Clause;

typedef struct {
    Clause* clauses;
    int clause_count;
    int clause_cap;
    char (*names)[17];
    int name_count;
    int name_cap; // dedupbed label names
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
        int name_idx;
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

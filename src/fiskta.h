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

#ifndef FISKTA_FW_WIN
#define FISKTA_FW_WIN (6 * 1024 * 1024)
#endif
#ifndef FISKTA_BK_BLK
#define FISKTA_BK_BLK (3 * 1024 * 1024)
#endif
#ifndef FISKTA_OVERLAP_MIN
#define FISKTA_OVERLAP_MIN (4 * 1024)
#endif
#ifndef FISKTA_OVERLAP_MAX
#define FISKTA_OVERLAP_MAX (64 * 1024)
#endif

enum { FW_WIN = FISKTA_FW_WIN,
    BK_BLK = FISKTA_BK_BLK,
    OVERLAP_MIN = FISKTA_OVERLAP_MIN,
    OVERLAP_MAX = FISKTA_OVERLAP_MAX };

static inline i64 clamp64(i64 x, i64 lo, i64 hi)
{
    if (x < lo) {
        return lo;
    }
    if (x > hi) {
        return hi;
    }
    return x;
}

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

typedef struct ParsePlan {
    i32 clause_count;
    i32 total_ops;
    i32 sum_take_ops;
    i32 sum_label_ops;
    i32 needle_count;
    size_t needle_bytes;
    i32 sum_findr_ops;
    i32 re_ins_estimate;
    i32 re_classes_estimate;
    i32 re_ins_estimate_max;
} ParsePlan;

enum { PARSE_ERROR_MESSAGE_MAX = 160 };

typedef struct {
    i32 token_index;
    char message[PARSE_ERROR_MESSAGE_MAX];
} ParseError;

enum Err engine_run(const Program* prg, const char* in_path, FILE* out);
enum Err parse_preflight(i32 token_count, const String* tokens, const char* in_path, ParsePlan* plan, const char** in_path_out);
enum Err parse_build(i32 token_count, const String* tokens, const char* in_path, Program* prg, const char** in_path_out,
    Clause* clauses_buf, Op* ops_buf,
    char* str_pool, size_t str_pool_cap);
const ParseError* parse_error_last(void);
void commit_labels(VM* vm, const LabelWrite* label_writes, i32 label_count);

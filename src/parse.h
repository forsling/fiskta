#pragma once
#include "fiskta.h"

// Parse planning: determine memory requirements before allocation
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
    i32 sum_inline_lits;
} ParsePlan;

// Two-phase parsing: preflight to measure, build to construct
enum Err parse_preflight(i32 token_count, const String* tokens, const char* in_path, ParsePlan* plan, const char** in_path_out);
enum Err parse_build(i32 token_count, const String* tokens, const char* in_path, Program* prg, const char** in_path_out,
    Clause* clauses_buf, Op* ops_buf, char* str_pool, size_t str_pool_cap);

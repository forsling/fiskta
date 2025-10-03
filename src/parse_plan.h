#pragma once

#include "fiskta.h"

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

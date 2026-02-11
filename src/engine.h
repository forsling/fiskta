#pragma once
#include "fiskta_types.h"

typedef struct File File;

// Clamping policy for location resolution
typedef enum {
    CLAMP_NONE,
    CLAMP_FILE,
    CLAMP_VIEW
} ClampPolicy;

// Label write staging
typedef struct LabelWrite {
    i64 pos;
    i32 name_idx;
} LabelWrite;

// Staged execution result
typedef struct {
    // staged_vm: FisktaVM state after clause execution
    //   - cursor, last_match, view after the clause
    //   - label_pos[] is NOT committed; staged label writes live in label_writes[]
    FisktaVM staged_vm;
    FisktaRange* ranges; // Staged output ranges
    i32 range_count; // Number of staged ranges
    LabelWrite* label_writes; // Staged label writes
    i32 label_count; // Number of staged labels
    enum FisktaErr err; // Execution result
} StagedResult;

// Determine clause resource requirements
void clause_caps(const FisktaClause* c, i32* out_ranges_cap, i32* out_labels_cap, i32* out_inline_cap);

// Execute clause with staging (atomic commit/rollback)
enum FisktaErr stage_clause(const FisktaClause* clause,
    File* io, FisktaVM* vm,
    FisktaRange* ranges, i32 ranges_cap,
    LabelWrite* label_writes, i32 label_cap,
    char* inline_buf, i32 inline_cap,
    StagedResult* result);

// Commit staged label writes to FisktaVM
void commit_labels(FisktaVM* vm, const LabelWrite* label_writes, i32 label_count);

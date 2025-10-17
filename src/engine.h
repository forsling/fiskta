#pragma once
#include "fiskta.h"

// Forward declaration for File type (defined in iosearch.h)
typedef struct File File;

// Label write staging
typedef struct {
    i64 pos;
    i32 name_idx;
} LabelWrite;

// Staged execution result
typedef struct {
    VM staged_vm;        // Staged VM state (cursor, last_match, view)
    Range* ranges;       // Staged output ranges
    i32 range_count;     // Number of staged ranges
    LabelWrite* label_writes; // Staged label writes
    i32 label_count;     // Number of staged labels
    enum Err err;        // Execution result
} StagedResult;

// Determine clause resource requirements
void clause_caps(const Clause* c, i32* out_ranges_cap, i32* out_labels_cap, i32* out_inline_cap);

// Execute clause with staging (atomic commit/rollback)
enum Err stage_clause(const Clause* clause,
    File* io, VM* vm,
    Range* ranges, i32 ranges_cap,
    LabelWrite* label_writes, i32 label_cap,
    char* inline_buf, i32 inline_cap,
    StagedResult* result);

// Commit staged label writes to VM
void commit_labels(VM* vm, const LabelWrite* label_writes, i32 label_count);

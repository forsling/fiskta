// runtime.h
//
// CLI/Runtime boundary: connects user clauses to execution
//
// This module is the bridge between:
//   - CLI argument parsing (parse.c)
//   - Program execution (engine.c)
//   - File I/O (fileio.c)
//   - Search operations (search_literal, regex_vm)
//
// Responsibilities:
//   - Memory allocation and arena management
//   - Regex compilation from parsed patterns
//   - Loop/streaming modes (--follow, --monitor, --continue)
//   - Timeout handling (--for, --until-idle)
//   - Output formatting and emission
//
// Core subsystems (fileio, search_literal, regex_vm, regex_prog) do NOT
// depend on this header - they are pure libraries usable without the runtime.

#pragma once

#include "fiskta.h"

// Loop execution modes
typedef enum {
    LOOP_MODE_FOLLOW, // --follow, -f: only new data (delta)
    LOOP_MODE_MONITOR, // --monitor, -m: restart from BOF (rescan)
    LOOP_MODE_CONTINUE // --continue, -c: resume from cursor (default)
} LoopMode;

// Runtime configuration from CLI
typedef struct {
    const char* input_path;
    i32 loop_ms;
    bool loop_enabled;
    bool ignore_loop_failures;
    i32 idle_timeout_ms;
    i32 exec_timeout_ms;
    LoopMode loop_mode;
} RuntimeConfig;

// =============================================================================
// Two-phase execution API (library-friendly)
// =============================================================================

// RuntimeScratch: All execution-time working memory for one Program.
//
// Lifetime:
//   - Created by build_program() via single malloc (arena_block)
//   - Used (and mutated) by runtime_execute()
//   - Freed by runtime_scratch_free() when done
//
// Reusability:
//   - The same RuntimeScratch may be reused across multiple calls to
//     runtime_execute() as long as those calls are NOT concurrent.
//   - Thread-safety: NOT thread-safe. One RuntimeScratch per thread.
//
// Ownership:
//   - All pointers point into arena_block
//   - Caller owns arena_block and must free via runtime_scratch_free()
typedef struct {
    // Search buffers (mutated during file reading)
    unsigned char* search_buf;
    size_t search_buf_cap;

    // Regex VM scratch (mutated during pattern matching)
    struct ReThread* re_curr;
    struct ReThread* re_next;
    int re_thread_cap;
    unsigned char* seen_curr;
    unsigned char* seen_next;
    size_t seen_bytes;

    // Staging buffers for clause execution (mutated per clause)
    struct Range* clause_ranges;
    struct LabelWrite* clause_labels;
    char* clause_inline;
    i32 sum_inline_lits;

    // Arena for cleanup (owns all memory above)
    void* arena_block;
    size_t arena_size;
} RuntimeScratch;

// Build program from tokens (compile-time phase)
//
// Parse tokens, perform resource sizing, compile regexes, and allocate
// all memory needed to execute later.
//
// This is the "compile-time" phase: no file I/O, no execution, no loops.
//
// On success:
//   - prog_out points into scratch_out->arena_block (read-only view)
//   - scratch_out is fully initialized (mutable execution state)
//   - Returns FISKTA_EXIT_OK
//
// On failure:
//   - scratch_out may be partially initialized
//   - Caller must call runtime_scratch_free(scratch_out) regardless
//   - Returns FISKTA_EXIT_PARSE, FISKTA_EXIT_REGEX, or FISKTA_EXIT_RESOURCE
//
// Memory ownership:
//   - Program is a read-only view into scratch_out->arena_block
//   - Freeing RuntimeScratch invalidates Program
//   - Do not free Program separately
int build_program(i32 token_count, const String* tokens,
                  Program* prog_out,
                  RuntimeScratch* scratch_out);

// Execute program against file (runtime phase)
//
// Execute a previously-built Program against the given file.
//
// This is the "runtime" phase: file I/O, VM execution, looping/streaming.
//
// Uses and mutates scratch:
//   - Regex thread lists
//   - Staging buffers (ranges, labels, inline literals)
//   - VM state (cursor, view, label positions)
//
// Blocking behavior (per config):
//   - FOLLOW mode: loops forever, processing new data
//   - MONITOR mode: loops forever, rescanning from BOF
//   - CONTINUE mode: single pass, resuming from saved cursor
//   - Honors --every interval and --for/--until-idle timeouts
//
// Returns FISKTA_EXIT_* code:
//   - FISKTA_EXIT_OK (0)       - Success
//   - FISKTA_EXIT_FAILURE (1)  - Program failed
//   - FISKTA_EXIT_TIMEOUT (2)  - Timeout reached
//   - FISKTA_EXIT_IO (10)      - File I/O error
//   - FISKTA_EXIT_RESOURCE(11) - Resource exhaustion during execution
int runtime_execute(const Program* prog,
                   const char* file_path,
                   RuntimeScratch* scratch,
                   const RuntimeConfig* config);

// Free RuntimeScratch resources
void runtime_scratch_free(RuntimeScratch* s);

// =============================================================================
// One-shot convenience API (CLI-friendly)
// =============================================================================

// Single-call wrapper: build + execute + cleanup
//
// Convenience one-shot for CLI: build + execute + cleanup.
//
// This is the legacy/simple path that most tests and CLI usage goes through.
// Equivalent to:
//   build_program(tokens) -> runtime_execute() -> cleanup
//
// For library embedding or testing, prefer calling build_program() and
// runtime_execute() separately.
//
// Main runtime entry point - executes parsed program with given configuration
//
// Parameters:
//   token_count: Number of operation tokens (from parse)
//   tokens:      Array of operation strings (e.g., "find", "ERROR", "take", "5c")
//   config:      Runtime settings (file path, loop mode, timeouts, etc.)
//
// Returns: Exit code
//   FISKTA_EXIT_OK (0)            - Program completed successfully
//   FISKTA_EXIT_PROGRAM_FAIL (1)  - Program failed (clause failed, no match, etc.)
//   FISKTA_EXIT_TIMEOUT (2)       - Timeout reached (--for or --until-idle)
//   FISKTA_EXIT_IO (10)           - File I/O error
//   FISKTA_EXIT_RESOURCE (11)     - Resource exhaustion (OOM)
//   FISKTA_EXIT_PARSE (12)        - Parse error in operations
//   FISKTA_EXIT_REGEX (13)        - Regex compilation error (includes capacity errors)
//
// Execution phases:
//   1. Parse preflight (estimate memory requirements)
//   2. Allocate arena (single malloc for all data structures)
//   3. Parse build (construct Program from tokens)
//   4. Compile regexes (all patterns compiled upfront)
//   5. Open file
//   6. Execute program (possibly in loop for streaming modes)
//
// Memory allocation:
//   - Single arena allocated based on preflight estimates
//   - No allocations during execution
//   - Memory usage independent of input file size
int run_program(i32 token_count, const String* tokens, const RuntimeConfig* config);

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

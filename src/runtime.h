#pragma once

#include "fiskta.h"

// Loop execution modes
typedef enum {
    LOOP_MODE_FOLLOW,   // --follow, -f: only new data (delta)
    LOOP_MODE_MONITOR,  // --monitor, -m: restart from BOF (rescan)
    LOOP_MODE_CONTINUE  // --continue, -c: resume from cursor (default)
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

// Main runtime entry point
// Takes parsed operations and configuration, executes the program
int run_program(i32 token_count, const String* tokens, const RuntimeConfig* config);

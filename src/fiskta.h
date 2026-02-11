// fiskta.h - Main public API
//
// This is the primary header for library users. It provides:
//   - Core types and constants (via fiskta_types.h)
//   - Program building and execution API
//   - Memory requirement queries
//   - Runtime configuration
//
// For library users, this is the single header to include.
//
// Internal organization:
//   - fiskta_types.h: Core types, enums, data structures
//   - engine.h: VM execution internals
//   - fileio.h: I/O buffering and streaming
//
// Core subsystems (fileio, search_literal, regex_vm, regex_prog) are
// pure libraries usable without this runtime layer.

#pragma once

#ifndef FISKTA_VERSION
#define FISKTA_VERSION "dev"
#endif

#ifndef FISKTA_ABI_MAJOR
#define FISKTA_ABI_MAJOR 0
#endif

#ifndef FISKTA_ABI_MINOR
#define FISKTA_ABI_MINOR 0
#endif

#ifndef FISKTA_API
#if defined(_WIN32)
#if defined(FISKTA_STATIC)
#define FISKTA_API
#elif defined(FISKTA_BUILD)
#define FISKTA_API __declspec(dllexport)
#else
#define FISKTA_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#if defined(FISKTA_BUILD)
#define FISKTA_API __attribute__((visibility("default")))
#else
#define FISKTA_API
#endif
#else
#define FISKTA_API
#endif
#endif // FISKTA_API

#include "fiskta_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Callback types
typedef void (*FisktaErrorCallback)(enum Err err, const char* context,
    i32 position, const char* message, void* userdata);
typedef void (*FisktaOutputCallback)(const void* data, size_t len, void* userdata);

/********************************
 * REGEX ENGINE RESOURCE LIMITS *
 ********************************/
//
// Total memory budget for regex VM execution. This budget is split between:
// - Seen tables: 2 tables (curr+next), sized per-pattern (nins × 32 bytes each)
// - Thread lists: gets remaining budget, typically ~9-10K threads for normal patterns
//
// With 2 MiB default budget:
// - Small pattern (60 ins): ~4 KiB total seen (2×2 KiB), ~10K threads
// - Large pattern (500 ins): ~32 KiB total seen (2×16 KiB), ~10K threads
// - Max pattern (16K ins): ~2 MiB total seen (2×1 MiB), minimal threads
//
// Memory usage is predictable and independent of runtime complexity. Patterns
// that exceed the budget fail gracefully with E_CAPACITY.
//
// These defaults can be overridden via BuildOptions (see fiskta_types.h).
//
#ifndef FISKTA_REGEX_BUDGET_DEFAULT
#define FISKTA_REGEX_BUDGET_DEFAULT (2 * 1024 * 1024) // 2 MiB total
#endif

// Minimum viable thread capacity (NFA needs at least this many concurrent states)
#ifndef MIN_THREAD_CAP
#define MIN_THREAD_CAP 32
#endif

// Thread sizing constants for budget calculations
// Each ReThread is ~84 bytes + padding ≈ 100 bytes; we keep 2 lists (curr+next)
#define RE_THREAD_BYTES 100
#define RE_LISTS 2

/*************************
 * RUNTIME CONFIGURATION *
 *************************/
typedef struct {
    i32 loop_ms;               // Delay between loop iterations in ms (0 = tight loop)
    bool loop_enabled;         // Enable continue/loop mode
    bool ignore_loop_failures; // Keep looping even when clauses fail
    i32 idle_timeout_ms;       // Stop after idle period: -1 = disabled, 0 = immediate, >0 = ms
    i32 exec_timeout_ms;       // Stop after total time: -1 = disabled, >=0 = ms

    FisktaErrorCallback error_callback;  // Optional error handler (NULL = use stderr)
    void* error_userdata;
    FisktaOutputCallback output_callback; // Optional output handler (NULL = use stdout)
    void* output_userdata;
} RuntimeConfig;

/***************************
 * TWO-PHASE EXECUTION API *
 ***************************/
// RuntimeBuffers: All execution-time working memory for one Program.
//
// Lifetime:
//   - Initialized by build_program() with pointers into caller's arena
//   - Used (and mutated) by runtime_execute()
//   - Valid as long as arena is not freed
//
// Reusability:
//   - The same RuntimeBuffers may be reused across multiple calls to
//     runtime_execute() as long as those calls are NOT concurrent.
//   - Thread-safety: NOT thread-safe. One RuntimeBuffers per thread.
//
// Ownership:
//   - All pointers point into arena_block
//   - Caller owns arena_block and must free() it when done
typedef struct {
    // Search buffers (mutated during file reading)
    unsigned char* search_buf;
    size_t search_buf_cap;

    // Regex VM scratch (mutated during pattern matching)
    struct ReThread* re_curr;
    struct ReThread* re_next;
    int re_thread_cap;
    u64 regex_work_budget; // Max thread enqueues per search (prevents step-count explosion)
    unsigned char* seen_curr;
    unsigned char* seen_next;
    size_t seen_bytes;

    // Staging buffers for clause execution (mutated per clause)
    Range* clause_ranges;
    struct LabelWrite* clause_labels;
    char* clause_inline;
    i32 sum_inline_lits;

    // Arena metadata (caller owns and must free arena_block)
    void* arena_block;
    size_t arena_size;
} RuntimeBuffers;

// Runtime memory requirements for executing a Program
//
// Returned by program_requirements() to expose all memory needs BEFORE allocation.
// Enables:
//   - Pre-flight inspection (validate resource needs before committing)
//   - DoS detection (large allocations surface early)
//   - Custom allocators (zero-malloc embedding)
//   - Resource monitoring (log/enforce limits)
typedef struct {
    // Per-run working memory (mutable scratch)
    size_t search_buf_cap; // File I/O buffer size

    // Regex VM scratch (fixed policy budget)
    // These are NOT computed per-pattern - they're fixed policy limits that
    // all regexes must work within. See FISKTA_REGEX_*_DEFAULT constants.
    size_t regex_seen_bytes_max; // Per-table seen budget (1 MiB default; 2 tables needed)
    size_t regex_thread_cap_max; // Fixed thread capacity (~10K default)

    // Per-clause temporary working memory at runtime
    // (ranges, label writes, inline expansion buffer)
    //
    // Staging semantics:
    //   - Each clause execution accumulates operations (output ranges, label writes)
    //     in temporary buffers WITHOUT committing them to VM or stdout.
    //   - On clause success: staged changes commit atomically (emit output, update labels)
    //   - On clause failure: staged changes discard, VM rolls back to pre-clause state
    //   - This enables atomic clause semantics (all-or-nothing execution)
    size_t staging_bytes;

    // Static program data (compiled clauses, regexes, string pool)
    // This is the total arena size needed
    size_t arena_bytes;

    // Arena breakdown (informational, for debugging/monitoring)
    size_t ops_bytes; // Op array
    size_t clauses_bytes; // Clause array
    size_t regex_prog_bytes; // ReProg structs
    size_t regex_ins_bytes; // ReInst instruction pool
    size_t regex_cls_bytes; // ReClass character class pool
    size_t str_pool_bytes; // String literal pool

    // Program-level regex characteristics (fast-path selection hints)
    bool any_lazy_quantifiers; // True if any regex has lazy quantifiers
    bool any_counters; // True if any regex has {n,m} quantifiers
} RuntimeRequirements;

// Analyze program and compute memory requirements WITHOUT allocating
//
// This is the preflight query: returns what memory you'd need to run tokens.
// Useful for:
//   - Validating input before committing to execution
//   - Enforcing resource limits (fail early on DoS inputs)
//   - Custom allocator sizing for zero-malloc embedding
//
// Side effects: NONE
//   - Does not malloc
//   - Does not touch disk
//   - Does not mutate global state
//   - May run regex compiler in "dry-run" mode to count instructions
//
// Error handling:
//   - Returns FISKTA_EXIT_OK on success, fills *out
//   - Returns FISKTA_EXIT_PARSE if tokens or pattern invalid
//   - Returns FISKTA_EXIT_CAPACITY if capacity exceeded
//   - Errors reported via fiskta_set_error_handler() or stderr
FISKTA_API int fiskta_program_requirements(i32 token_count, const String* tokens,
    const BuildOptions* options,
    RuntimeRequirements* out);

// Build program from tokens (compile-time phase)
//
// Parse tokens, compile regexes, and build executable program structure.
// Uses caller-provided arena for all memory allocation.
//
// This is the "compile-time" phase: no file I/O, no execution, no loops.
//
// Caller must:
//   1. Call program_requirements() to get req.arena_bytes
//   2. Allocate arena_block with at least that size (malloc, stack, pool, etc.)
//   3. Pass it here
//
// On success:
//   - prog_out points into caller's arena_block (read-only view)
//   - buffers_out is fully initialized (mutable execution state)
//   - Returns FISKTA_EXIT_OK
//
// On failure:
//   - Returns FISKTA_EXIT_PARSE, FISKTA_EXIT_CAPACITY, or FISKTA_EXIT_RESOURCE
//   - arena_block untouched, caller still owns it
//
// Memory ownership:
//   - Caller owns arena_block and must free it when done
//   - Program and buffers are invalidated when arena is freed
//
// Example usage:
//   BuildOptions opts = {0};  // Use defaults
//   RuntimeRequirements req;
//   fiskta_program_requirements(tokens, &opts, &req);
//   void* arena = malloc(req.arena_bytes);
//   fiskta_build_program(tokens, &opts, &prog, arena, req.arena_bytes, &buffers);
//   fiskta_runtime_execute(&prog, file, &buffers, &config);
//   free(arena);
FISKTA_API int fiskta_build_program(i32 token_count, const String* tokens,
    const BuildOptions* options,
    Program* prog_out,
    void* arena_block, size_t arena_size,
    RuntimeBuffers* buffers_out);

// Execute program against file (runtime phase)
//
// Execute a previously-built Program against the given file.
//
// This is the "runtime" phase: file I/O, VM execution, continue loop.
//
// Uses and mutates buffers:
//   - Regex thread lists
//   - Staging buffers (ranges, labels, inline literals)
//   - VM state (cursor, view, label positions)
//
// Blocking behavior (per config):
//   - Continue loop: resumes from saved cursor position each iteration
//   - Honors --continue interval and --for/--until-idle timeouts
//   - Use program clauses for follow/monitor emulation (see README recipes)
//
// Returns FISKTA_EXIT_* code:
//   - FISKTA_EXIT_OK (0)            - Success
//   - FISKTA_EXIT_PROGRAM_FAIL (1)  - Program failed
//   - FISKTA_EXIT_TIMEOUT (2)       - Timeout reached
//   - FISKTA_EXIT_IO (10)           - File I/O error
//   - FISKTA_EXIT_RESOURCE (11)     - Resource exhaustion (OOM)
//   - FISKTA_EXIT_CAPACITY (9)      - Capacity exceeded
FISKTA_API int fiskta_runtime_execute(const Program* prog,
    const char* file_path,
    RuntimeBuffers* buffers,
    const RuntimeConfig* config);

// Execute a compiled program on an in-memory buffer.
//
// Parameters:
//   - prog: Compiled program structure
//   - data: Pointer to input data buffer
//   - len: Length of input data in bytes
//   - buffers: Pre-allocated scratch space (from program_requirements)
//   - config: Runtime configuration (loop mode, timeouts, callbacks)
//
// Returns same exit codes as fiskta_runtime_execute().
FISKTA_API int fiskta_runtime_execute_buffer(const Program* prog,
    const unsigned char* data, size_t len,
    RuntimeBuffers* buffers,
    const RuntimeConfig* config);

// Set error handler for this thread (optional)
//
// If set, errors will invoke the callback with diagnostic information.
// If not set, errors print to stderr (default behavior).
//
// Thread-local: each thread can have its own error handler.
// Safe to call multiple times to change handler.
//
// Example:
//   void my_handler(enum Err err, const char* ctx, i32 pos, const char* msg, void* data) {
//       fprintf(stderr, "Error: %s\n", msg);
//   }
//   fiskta_set_error_handler(my_handler, NULL);
FISKTA_API void fiskta_set_error_handler(FisktaErrorCallback callback, void* userdata);

// Get last error code (thread-local)
FISKTA_API enum Err fiskta_error_code(void);

// Get last error message (thread-local, may be NULL)
FISKTA_API const char* fiskta_error_message(void);

// Get last error token position (thread-local, -1 if not set)
FISKTA_API i32 fiskta_error_position(void);

// Get human-readable string for error code
FISKTA_API const char* fiskta_err_str(enum Err e);

// Library version helpers
FISKTA_API const char* fiskta_version(void);
FISKTA_API void fiskta_abi_version(int* major, int* minor);

#ifdef __cplusplus
} // extern "C"
#endif

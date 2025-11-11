#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "fiskta.h"
#include "engine.h"
#include "fileio.h"
#include "fiskta_internal.h"
#include "fiskta_types.h"
#include "parse.h"
#include "regex_prog.h"
#include "util.h"
#include <stdalign.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#if !defined(__MINGW32__) && !defined(__MINGW64__)
#define fseeko _fseeki64
#define ftello _ftelli64
#endif
#endif

/******************
 * ERROR HANDLING *
 ******************/
#define ERROR_MESSAGE_MAX 160

_Thread_local static enum Err tl_err = E_OK;
_Thread_local static i32 tl_position = -1;
_Thread_local static char tl_message[ERROR_MESSAGE_MAX] = { 0 };
_Thread_local static FiskataErrorCallback tl_callback = NULL;
_Thread_local static void* tl_userdata = NULL;

void error_set(enum Err err, i32 position, const char* fmt, ...)
{
    tl_err = err;
    tl_position = position;

    if (fmt) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(tl_message, ERROR_MESSAGE_MAX, fmt, args);
        va_end(args);
    } else {
        tl_message[0] = '\0';
    }
}

enum Err fiskta_error_code(void)
{
    return tl_err;
}

i32 fiskta_error_position(void)
{
    return tl_position;
}

const char* fiskta_error_message(void)
{
    return tl_message[0] != '\0' ? tl_message : NULL;
}

void fiskta_set_error_handler(FiskataErrorCallback callback, void* userdata)
{
    tl_callback = callback;
    tl_userdata = userdata;
}

// Sentinel: means "no saved VM yet"
#define VM_CURSOR_UNSET ((i64) - 1)

/****************************
 * REGEX BUDGET CALCULATION *
 ****************************/
// Default work budget: max thread enqueues per regex search
//
// This limits "step count explosion" - patterns like ((X?){50}){50} that don't
// blow out thread_cap (width) but burn massive CPU exploring state variations.
//
// 50M enqueues = room for legitimate searches over large files while catching
// adversarial nested quantifiers that would spin for seconds. Pathological patterns
// hit this limit in ~0.5-1s on modern CPU, well below the 2s global timeout.
#define REGEX_WORK_BUDGET_DEFAULT (50 * 1000 * 1000)

// Compute thread capacity from total budget and maximum seen table requirement.
// Returns 0 if budget is insufficient (caller should fail with E_CAPACITY).
static int compute_thread_cap_from_budget(size_t total_budget, size_t max_seen_bytes)
{
    // We need 2 seen tables (curr + next)
    size_t seen_total = max_seen_bytes * 2;

    if (seen_total >= total_budget) {
        return 0; // Budget too small for even the seen tables
    }

    size_t thread_budget = total_budget - seen_total;

    // Each thread needs ~100 bytes, and we keep 2 lists (curr + next)
    size_t thread_bytes_total = RE_THREAD_BYTES * RE_LISTS;
    int thread_cap = (int)(thread_budget / thread_bytes_total);

    if (thread_cap < MIN_THREAD_CAP) {
        return 0; // Not enough budget for minimum viable threads
    }

    return thread_cap;
}

// Iteration result status
typedef enum {
    ITER_OK,
    ITER_PROGRAM_FAIL,
    ITER_IO_ERROR,
    ITER_RESOURCE_ERROR,
    ITER_CAPACITY_ERROR
} IterStatus;

typedef struct {
    IterStatus status;
    enum Err last_err;
    i32 emitted_ranges;
} IterResult;

// Loop state (internal to runtime)
typedef struct {
    bool enabled;
    i32 loop_ms, idle_timeout_ms, exec_timeout_ms;
    u64 t0_ms, last_activity_ms;
    i64 last_size; // last observed file size
    VM vm; // Continue loop state (vm.cursor == VM_CURSOR_UNSET => none)
    IterResult last_result;
    int exit_code;
    int exit_reason; // 0 normal, 2 exec timeout
} LoopState;

/****************************************
 * ERROR HANDLING AND EXIT CODE MAPPING *
 ****************************************/
// Map internal errors to CLI exit codes
static int err_to_exit_code(enum Err e)
{
    switch (e) {
    case E_OK:
        return FISKTA_EXIT_OK;
    case E_PARSE:
    case E_BAD_NEEDLE:
    case E_BAD_HEX:
    case E_LABEL_FMT:
        return FISKTA_EXIT_PARSE;
    case E_IO:
        return FISKTA_EXIT_IO;
    case E_CAPACITY:
        return FISKTA_EXIT_CAPACITY;
    case E_OOM:
        return FISKTA_EXIT_RESOURCE;
    case E_LOC_RESOLVE:
    case E_NO_MATCH:
    case E_FAIL_OP:
        return FISKTA_EXIT_PROGRAM_FAIL;
    default:
        return FISKTA_EXIT_PROGRAM_FAIL;
    }
}

const char* fiskta_err_str(enum Err e)
{
    switch (e) {
    case E_OK:
        return "ok";
    case E_PARSE:
        return "parse error";
    case E_BAD_NEEDLE:
        return "empty needle";
    case E_BAD_HEX:
        return "invalid hex string";
    case E_LOC_RESOLVE:
        return "location not resolvable";
    case E_NO_MATCH:
        return "no match in window";
    case E_FAIL_OP:
        return "fail operation";
    case E_LABEL_FMT:
        return "bad label (A-Z0-9_-; first A-Z; <16)";
    case E_IO:
        return "I/O error";
    case E_CAPACITY:
        return "buffer capacity exceeded";
    case E_OOM:
        return "out of memory";
    default:
        return "unknown error";
    }
}

static enum Err emit_output(const void* data, size_t len, const RuntimeConfig* cfg)
{
    if (cfg && cfg->output_callback) {
        cfg->output_callback(data, len, cfg->output_userdata);
        return E_OK;
    }

    if (fwrite(data, 1, len, stdout) != len) {
        return E_IO;
    }
    return E_OK;
}

// Align a size value, returning non-zero on failure without exiting.
static int align_or_fail(size_t x, size_t align, size_t* out)
{
    size_t aligned = safe_align(x, align);
    if (aligned == SIZE_MAX) {
        error_set(E_OOM, -1, "arena alignment overflow");
        return -1;
    }
    *out = aligned;
    return 0;
}

/********************
 * PLATFORM HELPERS *
 ********************/
static u64 now_millis(void)
{
#ifdef _WIN32
    return (u64)GetTickCount64();
#else
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (u64)ts.tv_sec * 1000ULL + (u64)ts.tv_nsec / 1000000ULL;
#endif
}

static void refresh_file_size(File* io)
{
    if (!io) {
        return;
    }

    if (io->mode == FILE_MODE_MEMORY) {
        // Memory buffers have fixed size
        return;
    }

    if (!io->disk.f) {
        return;
    }

    int fd = fileno(io->disk.f);
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        // Regular file: we can refresh size without disturbing FILE* position
        io->size = (i64)st.st_size;
    }
}

/******************************
 * LOOP ORCHESTRATION HELPERS *
 ******************************/
static void loop_init(LoopState* state, const RuntimeConfig* config)
{
    if (!state || !config) {
        return;
    }

    memset(state, 0, sizeof *state);
    state->enabled = config->loop_enabled;
    state->loop_ms = config->loop_ms;
    state->idle_timeout_ms = config->idle_timeout_ms;
    state->exec_timeout_ms = config->exec_timeout_ms;
    state->t0_ms = state->last_activity_ms = now_millis();
    state->exit_code = FISKTA_EXIT_OK;
    state->exit_reason = 0;
    state->last_result = (IterResult) {
        .status = ITER_OK,
        .last_err = E_OK,
        .emitted_ranges = 0
    };

    // Initialize last_size to -1 so first iteration sees file as "changed"
    state->last_size = -1;

    state->vm.cursor = VM_CURSOR_UNSET;
    for (i32 i = 0; i < MAX_LABELS; i++) {
        state->vm.label_pos[i] = -1;
    }
}

static void loop_compute_window(LoopState* state, File* io, i64* lo, i64* hi, bool* out_size_changed)
{
    (void)out_size_changed; // unused in continue-only mode
    refresh_file_size(io);
    i64 size = io_size(io);
    if (size != state->last_size) {
        state->last_size = size;
        state->last_activity_ms = now_millis(); // data arrived/truncated
    }
    *hi = size;

    // Continue mode: resume from cursor (or 0 if unset)
    if (state->vm.cursor != VM_CURSOR_UNSET) {
        *lo = clamp64(state->vm.cursor, 0, size);
    } else {
        *lo = 0;
    }
    if (*lo > *hi) {
        *lo = *hi; // truncation safety
    }
}

static bool loop_should_wait_or_stop(LoopState* state, bool no_new_data, int* out_exit_reason)
{
    if (out_exit_reason) {
        *out_exit_reason = 0; // default: normal stop
    }
    u64 now = now_millis();
    if (state->exec_timeout_ms >= 0 && now - state->t0_ms >= (u64)state->exec_timeout_ms) {
        if (out_exit_reason) {
            *out_exit_reason = FISKTA_EXIT_TIMEOUT;
        }
        return false; // stop
    }
    if (!state->enabled) {
        return false; // one-shot
    }

    if (no_new_data) {
        if (state->idle_timeout_ms >= 0 && now - state->last_activity_ms >= (u64)state->idle_timeout_ms) {
            if (out_exit_reason) {
                *out_exit_reason = 0; // normal (idle)
            }
            return false; // stop
        }
        sleep_msec(state->loop_ms);
        return true; // wait & continue
    }
    return false; // have work, go execute
}

static void loop_commit(LoopState* state, i64 data_hi, IterResult result, bool ignore_fail)
{
    (void)data_hi; // unused in continue-only mode
    state->last_result = result;

    switch (result.status) {
    case ITER_OK:
        // success: mark activity
        state->last_activity_ms = now_millis();
        state->exit_code = FISKTA_EXIT_OK;
        break;
    case ITER_PROGRAM_FAIL:
        if (ignore_fail && state->enabled) {
            // treat as success to keep looping
            state->last_result.status = ITER_OK;
            state->last_result.last_err = E_OK;
            state->last_result.emitted_ranges = 0;
            state->exit_code = FISKTA_EXIT_OK;
        } else {
            state->exit_code = FISKTA_EXIT_PROGRAM_FAIL; // program failed (no clause succeeded)
        }
        break;
    case ITER_IO_ERROR:
        state->exit_code = FISKTA_EXIT_IO;
        break;
    case ITER_RESOURCE_ERROR:
        state->exit_code = FISKTA_EXIT_RESOURCE;
        break;
    case ITER_CAPACITY_ERROR:
        state->exit_code = FISKTA_EXIT_CAPACITY;
        break;
    }
}

/*********************
 * PROGRAM ITERATION *
 *********************/
static IterResult execute_program_iteration(const Program* prg, File* io, VM* vm,
    Range* clause_ranges, LabelWrite* clause_labels,
    char* clause_inline, i32 inline_slots_total,
    i64 data_lo, i64 data_hi, const RuntimeConfig* cfg)
{
    io_reset_full(io);

    VM local_vm;
    VM* vm_exec = vm ? vm : &local_vm;
    if (!vm) {
        memset(vm_exec, 0, sizeof(*vm_exec));
        for (i32 i = 0; i < MAX_LABELS; i++) {
            vm_exec->label_pos[i] = -1;
        }
    }

    i64 lo = clamp64(data_lo, 0, io_size(io));
    i64 hi = clamp64(data_hi, 0, io_size(io));
    if (vm_exec->cursor < lo) {
        vm_exec->cursor = lo;
    }
    if (vm_exec->cursor > hi) {
        vm_exec->cursor = hi;
    }
    vm_exec->view.active = true;
    vm_exec->view.lo = lo;
    vm_exec->view.hi = hi;
    vm_exec->last_match.valid = false;

    bool any_success = false;
    enum Err last_err = E_OK;
    IterResult iter_result = {
        .status = ITER_OK,
        .last_err = E_OK,
        .emitted_ranges = 0
    };

    StagedResult result;
    char* inline_cursor = clause_inline;
    char* inline_end = NULL;
    if (clause_inline && inline_slots_total > 0) {
        inline_end = clause_inline + (size_t)inline_slots_total * INLINE_LIT_CAP;
    }

    for (i32 ci = 0; ci < prg->clause_count; ++ci) {
        i32 rc = 0;
        i32 lc = 0;
        i32 ic = 0;
        clause_caps(&prg->clauses[ci], &rc, &lc, &ic);
        Range* r_tmp = (rc > 0) ? clause_ranges : NULL;
        LabelWrite* lw_tmp = (lc > 0) ? clause_labels : NULL;
        char* inline_tmp = NULL;
        if (ic > 0) {
            if (!inline_cursor || !inline_end || inline_cursor + (size_t)ic * INLINE_LIT_CAP > inline_end) {
                iter_result.status = ITER_CAPACITY_ERROR;
                iter_result.last_err = E_CAPACITY;
                return iter_result;
            }
            inline_tmp = inline_cursor;
            inline_cursor += (size_t)ic * INLINE_LIT_CAP;
        }

        enum Err e = stage_clause(&prg->clauses[ci], io, vm_exec,
            r_tmp, rc, lw_tmp, lc,
            inline_tmp, ic,
            &result);
        if (e == E_OK) {
            // Commit staged ranges to stdout / file as appropriate
            for (i32 i = 0; i < result.range_count; i++) {
                const Range* range = &result.ranges[i];
                if (range->kind == RANGE_FILE) {
                    i64 start = range->file.start;
                    i64 end = range->file.end;
                    if (start < end) {
                        if (start < 0 || end > io->size) {
                            e = E_IO;
                            break;
                        }
                        i64 offset = start;
                        i64 remaining = end - start;
                        while (remaining > 0) {
                            size_t chunk_size = (remaining > (i64)io->buf_cap) ? io->buf_cap : (size_t)remaining;
                            size_t n;
                            e = io_read_at(io, offset, io->buf, chunk_size, &n);
                            if (e != E_OK) {
                                break;
                            }
                            if (n == 0) {
                                break;
                            }
                            e = emit_output(io->buf, n, cfg);
                            if (e != E_OK) {
                                break;
                            }
                            offset += (i64)n;
                            remaining -= (i64)n;
                        }
                    }
                } else {
                    e = emit_output(range->lit.bytes, (size_t)range->lit.len, cfg);
                }
                if (e != E_OK) {
                    break;
                }
                iter_result.emitted_ranges++;
            }
            if (e == E_OK) {
                // Commit staged VM state now that I/O succeeded
                commit_labels(vm_exec, result.label_writes, result.label_count);
                vm_exec->cursor = result.staged_vm.cursor;
                vm_exec->last_match = result.staged_vm.last_match;
                vm_exec->view = result.staged_vm.view;
                any_success = true;
            }
        }

        if (e == E_OK) {
            if (prg->clauses[ci].link == LINK_OR) {
                // Skip remaining alternatives in this OR-chain once one succeeds
                while (ci + 1 < prg->clause_count && prg->clauses[ci].link == LINK_OR) {
                    ci++;
                }
            }
        } else {
            last_err = e;
            if (e == E_IO) {
                iter_result.status = ITER_IO_ERROR;
                iter_result.last_err = E_IO;
                break;
            }
            if (e == E_OOM) {
                iter_result.status = ITER_RESOURCE_ERROR;
                iter_result.last_err = e;
                break;
            }
            if (e == E_CAPACITY) {
                iter_result.status = ITER_CAPACITY_ERROR;
                iter_result.last_err = e;
                break;
            }
        }
    }

    if (iter_result.status == ITER_IO_ERROR || iter_result.status == ITER_RESOURCE_ERROR || iter_result.status == ITER_CAPACITY_ERROR) {
        return iter_result;
    }
    if (any_success) {
        iter_result.status = ITER_OK;
        iter_result.last_err = E_OK;
    } else {
        iter_result.status = ITER_PROGRAM_FAIL;
        iter_result.last_err = (last_err != E_OK) ? last_err : E_FAIL_OP;
    }
    return iter_result;
}

/***************************************************
 * RESOURCE REQUIREMENTS QUERY (PRE-FLIGHT SIZING) *
 ***************************************************/
int fiskta_program_requirements(i32 token_count, const String* tokens,
    const BuildOptions* options,
    RuntimeRequirements* out)
{
    if (!tokens || !out) {
        return FISKTA_EXIT_PARSE;
    }

    memset(out, 0, sizeof(*out));

    // Apply defaults if options is NULL or fields are zero
    size_t regex_budget = FISKTA_REGEX_BUDGET_DEFAULT;

    if (options && options->regex_budget_bytes > 0) {
        regex_budget = options->regex_budget_bytes;
    }

    /*******************************************************
     * PHASE 1: PREFLIGHT PARSE                            *
     * Analyze operations to determine memory requirements *
     *******************************************************/
    ParsePlan plan = (ParsePlan) { 0 };
    const char* path = NULL;
    enum Err e = parse_preflight(token_count, tokens, NULL, &plan, &path);
    if (e != E_OK) {
        return err_to_exit_code(e);
    }

    /*********************************************************
     * PHASE 2: COMPUTE SIZES                                *
     * Calculate total memory needed for all data structures *
     *********************************************************/

    // File I/O buffer
    const size_t search_buf_cap = (FW_WIN > (BK_BLK + OVERLAP_MAX)) ? (size_t)FW_WIN : (size_t)(BK_BLK + OVERLAP_MAX);
    out->search_buf_cap = search_buf_cap;

    // Program structure sizes
    const size_t ops_bytes = (size_t)plan.total_ops * sizeof(Op);
    const size_t clauses_bytes = (size_t)plan.clause_count * sizeof(Clause);
    const size_t str_pool_bytes = plan.needle_bytes;

    // Regex compilation sizes
    const size_t re_prog_bytes = (size_t)plan.sum_findr_ops * sizeof(ReProg);
    const size_t re_ins_bytes = (size_t)plan.re_ins_estimate * sizeof(ReInst);
    const size_t re_cls_bytes = (size_t)plan.re_classes_estimate * sizeof(ReClass);

    // Regex VM buffers: unified budget (actual split computed after compilation)
    // Report worst-case allocations since we don't know actual pattern sizes yet.
    // Actual thread_cap and seen_bytes will be derived in build_program().
    out->regex_thread_cap_max = (size_t)(regex_budget / (RE_THREAD_BYTES * RE_LISTS));
    // Worst case: half of budget for one seen table (×2 for curr+next = full budget)
    out->regex_seen_bytes_max = regex_budget / 2;

    // Staging buffers
    size_t ranges_bytes = (plan.sum_take_ops > 0) ? (size_t)plan.sum_take_ops * sizeof(Range) : 0;
    size_t labels_bytes = (plan.sum_label_ops > 0) ? (size_t)plan.sum_label_ops * sizeof(LabelWrite) : 0;
    size_t inline_bytes = (plan.sum_inline_lits > 0) ? (size_t)plan.sum_inline_lits * INLINE_LIT_CAP : 0;
    out->staging_bytes = ranges_bytes + labels_bytes + inline_bytes;

    // Fill breakdown fields
    out->ops_bytes = ops_bytes;
    out->clauses_bytes = clauses_bytes;
    out->regex_prog_bytes = re_prog_bytes;
    out->regex_ins_bytes = re_ins_bytes;
    out->regex_cls_bytes = re_cls_bytes;
    out->str_pool_bytes = str_pool_bytes;

    // Regex characteristics (would need additional tracking in ParsePlan)
    // For now, conservatively assume both are present if there are any regexes
    out->any_lazy_quantifiers = (plan.sum_findr_ops > 0);
    out->any_counters = (plan.sum_findr_ops > 0);

    /****************************************************
     * PHASE 3: COMPUTE TOTAL ARENA SIZE WITH ALIGNMENT *
     ****************************************************/
    size_t search_buf_size = 0, clauses_size = 0, ops_size = 0, re_prog_size = 0, re_ins_size = 0, re_cls_size = 0, str_pool_size = 0;
    if (align_or_fail(search_buf_cap, alignof(unsigned char), &search_buf_size) != 0
        || align_or_fail(clauses_bytes, alignof(Clause), &clauses_size) != 0
        || align_or_fail(ops_bytes, alignof(Op), &ops_size) != 0
        || align_or_fail(re_prog_bytes, alignof(ReProg), &re_prog_size) != 0
        || align_or_fail(re_ins_bytes, alignof(ReInst), &re_ins_size) != 0
        || align_or_fail(re_cls_bytes, alignof(ReClass), &re_cls_size) != 0
        || align_or_fail(str_pool_bytes, alignof(char), &str_pool_size) != 0) {
        return FISKTA_EXIT_RESOURCE;
    }

    // Two thread buffers + two seen arrays (using fixed policy budgets)
    const size_t re_threads_bytes = out->regex_thread_cap_max * sizeof(ReThread);
    size_t re_seen_size;
    if (add_overflow(out->regex_seen_bytes_max, out->regex_seen_bytes_max, &re_seen_size)) {
        error_set(E_OOM, -1, "regex 'seen' size overflow");
        return FISKTA_EXIT_RESOURCE;
    }
    size_t re_thrbufs_aligned = 0;
    if (align_or_fail(re_threads_bytes, alignof(ReThread), &re_thrbufs_aligned) != 0) {
        return FISKTA_EXIT_RESOURCE;
    }
    size_t re_thrbufs_size = 0;
    if (mul_overflow(re_thrbufs_aligned, 2, &re_thrbufs_size)) {
        error_set(E_OOM, -1, "thread buffer size overflow");
        return FISKTA_EXIT_RESOURCE;
    }

    // Staging buffers (already computed above, but need alignment)
    size_t ranges_size = 0, labels_size = 0, inline_size = 0;
    if (plan.sum_take_ops > 0) {
        if (align_or_fail(ranges_bytes, alignof(Range), &ranges_size) != 0) {
            return FISKTA_EXIT_RESOURCE;
        }
    }
    if (plan.sum_label_ops > 0) {
        if (align_or_fail(labels_bytes, alignof(LabelWrite), &labels_size) != 0) {
            return FISKTA_EXIT_RESOURCE;
        }
    }
    if (plan.sum_inline_lits > 0) {
        if (align_or_fail(inline_bytes, alignof(char), &inline_size) != 0) {
            return FISKTA_EXIT_RESOURCE;
        }
    }

    // Sum everything with overflow checking
    size_t total = search_buf_size;
    if (add_overflow(total, clauses_size, &total) || add_overflow(total, ops_size, &total) || add_overflow(total, re_prog_size, &total) || add_overflow(total, re_ins_size, &total) || add_overflow(total, re_cls_size, &total) || add_overflow(total, str_pool_size, &total) || add_overflow(total, re_thrbufs_size, &total) || add_overflow(total, re_seen_size, &total) || add_overflow(total, ranges_size, &total) || add_overflow(total, labels_size, &total) || add_overflow(total, inline_size, &total) || add_overflow(total, 64, &total)) { // small cushion
        error_set(E_OOM, -1, "arena size overflow");
        return FISKTA_EXIT_RESOURCE;
    }

    out->arena_bytes = total;
    return FISKTA_EXIT_OK;
}

/**************************************
 * BUILD PROGRAM (COMPILE-TIME PHASE) *
 **************************************/
// Build program using caller-provided arena
int fiskta_build_program(i32 token_count, const String* tokens,
    const BuildOptions* options,
    Program* prog_out,
    void* arena_block, size_t arena_size,
    RuntimeBuffers* buffers_out)
{
    if (!tokens || !prog_out || !buffers_out || !arena_block) {
        return FISKTA_EXIT_PARSE;
    }

    memset(prog_out, 0, sizeof(*prog_out));
    memset(buffers_out, 0, sizeof(*buffers_out));

    // Apply defaults if options is NULL or fields are zero
    size_t regex_budget = FISKTA_REGEX_BUDGET_DEFAULT;
    u64 work_budget = REGEX_WORK_BUDGET_DEFAULT;

    if (options) {
        if (options->regex_budget_bytes > 0) {
            regex_budget = options->regex_budget_bytes;
        }
        if (options->regex_work_budget > 0) {
            work_budget = options->regex_work_budget;
        }
    }

    // Perform same build logic as build_program(), but use provided arena
    // This is essentially a copy of build_program() with malloc() replaced

    /****************************
     * PHASE 1: PREFLIGHT PARSE *
     ****************************/
    ParsePlan plan = (ParsePlan) { 0 };
    const char* path = NULL;
    enum Err e = parse_preflight(token_count, tokens, NULL, &plan, &path);
    if (e != E_OK) {
        return err_to_exit_code(e);
    }

    /************************************************************
     * PHASE 2: COMPUTE SIZES (TO VERIFY ARENA IS LARGE ENOUGH) *
     ************************************************************/
    const size_t search_buf_cap = (FW_WIN > (BK_BLK + OVERLAP_MAX)) ? (size_t)FW_WIN : (size_t)(BK_BLK + OVERLAP_MAX);
    const size_t ops_bytes = (size_t)plan.total_ops * sizeof(Op);
    const size_t clauses_bytes = (size_t)plan.clause_count * sizeof(Clause);
    const size_t str_pool_bytes = plan.needle_bytes;
    const size_t re_prog_bytes = (size_t)plan.sum_findr_ops * sizeof(ReProg);
    const size_t re_ins_bytes = (size_t)plan.re_ins_estimate * sizeof(ReInst);
    const size_t re_cls_bytes = (size_t)plan.re_classes_estimate * sizeof(ReClass);

    // Allocate conservatively for regex VM (actual usage computed after compilation)
    // Worst case: entire budget goes to threads (if patterns are tiny)
    const int re_threads_cap_alloc = (int)(regex_budget / (RE_THREAD_BYTES * RE_LISTS));
    const size_t re_threads_bytes = (size_t)re_threads_cap_alloc * sizeof(ReThread);
    // Worst case: entire budget goes to seen tables (if patterns are huge).
    // Half budget per table (×2 for curr+next = full budget)
    const size_t re_seen_bytes_each = regex_budget / 2;

    /*******************************
     * PHASE 3: USE PROVIDED ARENA *
     *******************************/
    Arena arena;
    arena_init(&arena, arena_block, arena_size);

    /*******************************************************
     * PHASE 4: CARVE ARENA SLICES (SAME AS BUILD_PROGRAM) *
     *******************************************************/
    unsigned char* search_buf = arena_alloc(&arena, search_buf_cap, alignof(unsigned char));
    Clause* clauses_buf = arena_alloc(&arena, clauses_bytes, alignof(Clause));
    Op* ops_buf = arena_alloc(&arena, ops_bytes, alignof(Op));
    ReThread* re_curr_thr = arena_alloc(&arena, re_threads_bytes, alignof(ReThread));
    ReThread* re_next_thr = arena_alloc(&arena, re_threads_bytes, alignof(ReThread));
    unsigned char* seen_curr = arena_alloc(&arena, re_seen_bytes_each, alignof(u32));
    unsigned char* seen_next = arena_alloc(&arena, re_seen_bytes_each, alignof(u32));
    ReProg* re_progs = arena_alloc(&arena, re_prog_bytes, alignof(ReProg));
    ReInst* re_ins = arena_alloc(&arena, re_ins_bytes, alignof(ReInst));
    ReClass* re_cls = arena_alloc(&arena, re_cls_bytes, alignof(ReClass));
    char* str_pool = arena_alloc(&arena, str_pool_bytes, alignof(char));
    i16* offset_pool = (plan.sum_inline_lits > 0) ? arena_alloc(&arena, (size_t)plan.sum_inline_lits * sizeof(i16), alignof(i16)) : NULL;
    Range* clause_ranges = (plan.sum_take_ops > 0) ? arena_alloc(&arena, (size_t)plan.sum_take_ops * sizeof(Range), alignof(Range)) : NULL;
    LabelWrite* clause_labels = (plan.sum_label_ops > 0) ? arena_alloc(&arena, (size_t)plan.sum_label_ops * sizeof(LabelWrite), alignof(LabelWrite)) : NULL;
    char* clause_inline = (plan.sum_inline_lits > 0) ? arena_alloc(&arena, (size_t)plan.sum_inline_lits * INLINE_LIT_CAP, alignof(char)) : NULL;

    if (!search_buf || !clauses_buf || !ops_buf
        || !re_curr_thr || !re_next_thr || !seen_curr || !seen_next
        || !re_progs || !re_ins || !re_cls || !str_pool
        || (plan.sum_take_ops > 0 && !clause_ranges)
        || (plan.sum_label_ops > 0 && !clause_labels)
        || (plan.sum_inline_lits > 0 && !clause_inline)
        || (plan.sum_inline_lits > 0 && !offset_pool)) {
        error_set(E_OOM, -1, "arena carve (provided arena too small?)");
        return FISKTA_EXIT_RESOURCE;
    }

    /**************************************************
     * PHASE 5: BUILD PROGRAM (SAME AS BUILD_PROGRAM) *
     **************************************************/
    e = parse_build(token_count, tokens, NULL, prog_out, &path,
        clauses_buf, ops_buf, str_pool, str_pool_bytes, offset_pool);
    if (e != E_OK) {
        return err_to_exit_code(e);
    }
    if (prog_out->clause_count == 0) {
        error_set(E_PARSE, -1, "no operations parsed");
        return FISKTA_EXIT_PARSE;
    }

    // Compile all regex patterns upfront
    i32 re_prog_idx = 0;
    i32 re_ins_idx = 0;
    i32 re_cls_idx = 0;
    for (i32 ci = 0; ci < prog_out->clause_count; ++ci) {
        Clause* clause = &prog_out->clauses[ci];
        for (i32 i = 0; i < clause->op_count; ++i) {
            Op* op = &clause->ops[i];
            if (op->kind == OP_FIND_RE) {
                ReProg* prog = &re_progs[re_prog_idx++];
                enum Err err = re_compile_into(op->u.findr.pattern, prog,
                    re_ins, (i32)(re_ins_bytes / sizeof(ReInst)), &re_ins_idx,
                    re_cls, (i32)(re_cls_bytes / sizeof(ReClass)), &re_cls_idx);
                if (err != E_OK) {
                    return err_to_exit_code(err);
                }
                op->u.findr.prog = prog;
            } else if (op->kind == OP_TAKE_UNTIL_RE) {
                ReProg* prog = &re_progs[re_prog_idx++];
                enum Err err = re_compile_into(op->u.take_until_re.pattern, prog,
                    re_ins, (i32)(re_ins_bytes / sizeof(ReInst)), &re_ins_idx,
                    re_cls, (i32)(re_cls_bytes / sizeof(ReClass)), &re_cls_idx);
                if (err != E_OK) {
                    return err_to_exit_code(err);
                }
                op->u.take_until_re.prog = prog;
            }
        }
    }

    // Compute actual regex budget split based on compiled patterns
    size_t max_seen_bytes = 0;
    for (i32 pi = 0; pi < re_prog_idx; ++pi) {
        ReProgRequirements req;
        regex_prog_requirements(&re_progs[pi], &req);
        if (req.seen_bytes > max_seen_bytes) {
            max_seen_bytes = req.seen_bytes;
        }
    }

    int actual_thread_cap = compute_thread_cap_from_budget(regex_budget, max_seen_bytes);
    if (actual_thread_cap == 0) {
        error_set(E_CAPACITY, -1,
            "regex patterns require %zu bytes for seen tables, exceeding budget of %zu bytes",
            max_seen_bytes * 2, regex_budget);
        return FISKTA_EXIT_CAPACITY;
    }

    if (actual_thread_cap > re_threads_cap_alloc) {
        actual_thread_cap = re_threads_cap_alloc;
    }
    const size_t actual_seen_bytes = max_seen_bytes;

    // Fill RuntimeBuffers with buffers from PROVIDED arena
    buffers_out->search_buf = search_buf;
    buffers_out->search_buf_cap = search_buf_cap;
    buffers_out->re_curr = re_curr_thr;
    buffers_out->re_next = re_next_thr;
    buffers_out->re_thread_cap = actual_thread_cap;
    buffers_out->regex_work_budget = work_budget;
    buffers_out->seen_curr = seen_curr;
    buffers_out->seen_next = seen_next;
    buffers_out->seen_bytes = actual_seen_bytes;
    buffers_out->clause_ranges = clause_ranges;
    buffers_out->clause_labels = clause_labels;
    buffers_out->clause_inline = clause_inline;
    buffers_out->sum_inline_lits = plan.sum_inline_lits;
    buffers_out->arena_block = arena_block;
    buffers_out->arena_size = arena_size;

    return FISKTA_EXIT_OK;
}

/***********************************
 * EXECUTE PROGRAM (RUNTIME PHASE) *
 ***********************************/
int fiskta_runtime_execute(const Program* prog,
    const char* file_path,
    RuntimeBuffers* buffers,
    const RuntimeConfig* config)
{
    if (!prog || !file_path || !buffers || !config) {
        return FISKTA_EXIT_PARSE;
    }

    /*********************************************
     * PHASE 6: OPEN FILE I/O                    *
     * Initialize file handle and search buffers *
     *********************************************/
    File io = { 0 };
    enum Err e = io_open(&io, file_path, buffers->search_buf, buffers->search_buf_cap);
    if (e != E_OK) {
        return err_to_exit_code(e);
    }

    io_set_regex_scratch(&io, buffers->re_curr, buffers->re_next, buffers->re_thread_cap,
        buffers->regex_work_budget, buffers->seen_curr, buffers->seen_next, buffers->seen_bytes);

    /**********************************************
     * PHASE 7: EXECUTE PROGRAM                   *
     * Run operations with optional continue loop *
     **********************************************/
    LoopState loop_state;
    loop_init(&loop_state, config);

    for (;;) {
        // Handle --for timeout even if no iteration has executed yet
        int reason = 0;
        (void)loop_should_wait_or_stop(&loop_state, /*no_new_data=*/false, &reason);
        if (reason == FISKTA_EXIT_TIMEOUT) {
            loop_state.exit_reason = reason;
            break;
        }

        i64 lo;
        i64 hi;
        loop_compute_window(&loop_state, &io, &lo, &hi, NULL);

        // Detect idle condition: empty window [lo, hi)
        bool no_new_data = (lo >= hi);

        // Handle idle timeout if enabled and no new data
        if (loop_state.enabled && no_new_data) {
            if (loop_state.idle_timeout_ms == 0) {
                // -u 0: exit immediately on idle
                loop_state.exit_reason = 0;
                break;
            }
            if (loop_state.idle_timeout_ms > 0) {
                // -u <positive>: wait or stop based on timeout
                reason = 0;
                if (loop_should_wait_or_stop(&loop_state, /*no_new_data=*/true, &reason)) {
                    continue; // just slept; try again
                }
                loop_state.exit_reason = reason; // 0 => idle stop, FISKTA_EXIT_TIMEOUT => exec timeout
                break;
            }
        }

        // Continue mode: pass saved VM to preserve cursor and labels
        IterResult iteration = execute_program_iteration(prog, &io, &loop_state.vm,
            buffers->clause_ranges, buffers->clause_labels,
            buffers->clause_inline, buffers->sum_inline_lits,
            lo, hi, config);

        loop_commit(&loop_state, hi, iteration, config->ignore_loop_failures);
        fflush(stdout);

        if (!loop_state.enabled || loop_state.exit_code) {
            break;
        }

        // Throttle between iterations
        if (loop_state.loop_ms > 0) {
            sleep_msec(loop_state.loop_ms);
        }
        // Honor --for (exec timeout)
        reason = 0;
        (void)loop_should_wait_or_stop(&loop_state, /*no_new_data=*/false, &reason);
        if ((loop_state.exit_reason = reason) != 0) {
            break;
        }
    }

    io_close(&io);

    if (loop_state.exit_code) {
        return loop_state.exit_code;
    }
    if (loop_state.exit_reason == FISKTA_EXIT_TIMEOUT) {
        return FISKTA_EXIT_TIMEOUT;
    }

    // Otherwise evaluate last iteration outcome
    switch (loop_state.last_result.status) {
    case ITER_OK:
        return FISKTA_EXIT_OK;
    case ITER_IO_ERROR:
        return FISKTA_EXIT_IO;
    case ITER_RESOURCE_ERROR:
        return FISKTA_EXIT_RESOURCE;
    case ITER_CAPACITY_ERROR:
        return FISKTA_EXIT_CAPACITY;
    case ITER_PROGRAM_FAIL:
        return FISKTA_EXIT_PROGRAM_FAIL;
    default:
        return FISKTA_EXIT_IO;
    }
}

int fiskta_runtime_execute_buffer(const Program* prog,
    const unsigned char* data, size_t len,
    RuntimeBuffers* buffers,
    const RuntimeConfig* config)
{
    if (!prog || !data || !buffers || !config) {
        return FISKTA_EXIT_PARSE;
    }

    File io = { 0 };
    enum Err e = io_open_buffer(&io, data, len, buffers->search_buf, buffers->search_buf_cap);
    if (e != E_OK) {
        return err_to_exit_code(e);
    }

    io_set_regex_scratch(&io, buffers->re_curr, buffers->re_next, buffers->re_thread_cap,
        buffers->regex_work_budget, buffers->seen_curr, buffers->seen_next, buffers->seen_bytes);

    LoopState loop_state;
    loop_init(&loop_state, config);

    for (;;) {
        int reason = 0;
        (void)loop_should_wait_or_stop(&loop_state, /*no_new_data=*/false, &reason);
        if (reason == FISKTA_EXIT_TIMEOUT) {
            loop_state.exit_reason = reason;
            break;
        }

        i64 lo;
        i64 hi;
        loop_compute_window(&loop_state, &io, &lo, &hi, NULL);

        bool no_new_data = (lo >= hi);

        if (loop_state.enabled && no_new_data) {
            if (loop_state.idle_timeout_ms == 0) {
                loop_state.exit_reason = 0;
                break;
            }
            if (loop_state.idle_timeout_ms > 0) {
                reason = 0;
                if (loop_should_wait_or_stop(&loop_state, /*no_new_data=*/true, &reason)) {
                    continue;
                }
                loop_state.exit_reason = reason;
                break;
            }
        }

        IterResult iteration = execute_program_iteration(prog, &io, &loop_state.vm,
            buffers->clause_ranges, buffers->clause_labels,
            buffers->clause_inline, buffers->sum_inline_lits,
            lo, hi, config);

        loop_commit(&loop_state, hi, iteration, config->ignore_loop_failures);
        fflush(stdout);

        if (!loop_state.enabled || loop_state.exit_code) {
            break;
        }

        if (loop_state.loop_ms > 0) {
            sleep_msec(loop_state.loop_ms);
        }
        reason = 0;
        (void)loop_should_wait_or_stop(&loop_state, /*no_new_data=*/false, &reason);
        if ((loop_state.exit_reason = reason) != 0) {
            break;
        }
    }

    io_close(&io);

    if (loop_state.exit_code) {
        return loop_state.exit_code;
    }
    if (loop_state.exit_reason == FISKTA_EXIT_TIMEOUT) {
        return FISKTA_EXIT_TIMEOUT;
    }

    switch (loop_state.last_result.status) {
    case ITER_OK:
        return FISKTA_EXIT_OK;
    case ITER_IO_ERROR:
        return FISKTA_EXIT_IO;
    case ITER_RESOURCE_ERROR:
        return FISKTA_EXIT_RESOURCE;
    case ITER_CAPACITY_ERROR:
        return FISKTA_EXIT_CAPACITY;
    case ITER_PROGRAM_FAIL:
        return FISKTA_EXIT_PROGRAM_FAIL;
    default:
        return FISKTA_EXIT_IO;
    }
}

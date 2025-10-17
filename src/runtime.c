#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "runtime.h"
#include "engine.h"
#include "error.h"
#include "fiskta.h"
#include "iosearch.h"
#include "parse.h"
#include "reprog.h"
#include "util.h"
#include <stdalign.h>
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

// Sentinel: means "no saved VM yet"
#define VM_CURSOR_UNSET ((i64)-1)

// Iteration result status
typedef enum {
    ITER_OK,
    ITER_PROGRAM_FAIL,
    ITER_IO_ERROR
} IterStatus;

typedef struct {
    IterStatus status;
    enum Err last_err;
    i32 emitted_ranges;
} IterResult;

// Loop state (internal to runtime)
typedef struct {
    bool enabled;
    LoopMode mode;
    i32 loop_ms, idle_timeout_ms, exec_timeout_ms;
    u64 t0_ms, last_activity_ms;
    i64 baseline; // FOLLOW: last processed end; CONTINUE: unused; MONITOR: 0
    i64 last_size; // last observed file size
    VM vm; // CONTINUE state (vm.cursor == VM_CURSOR_UNSET => none)
    IterResult last_result;
    int exit_code;
    int exit_reason; // 0 normal, 2 exec timeout
} LoopState;

// =============================================================================
// Error handling
// =============================================================================

static const char* err_str(enum Err e)
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
    case E_OOM:
        return "out of memory";
    default:
        return "unknown error";
    }
}

static void print_err(enum Err e, const char* msg)
{
    fprintf(stderr, "fiskta: ");
    if (msg) {
        fprintf(stderr, "%s (%s)", msg, err_str(e));
    } else {
        fprintf(stderr, "%s", err_str(e));
    }

    const ErrorDetail* detail = error_detail_last();
    if (detail && detail->message[0] != '\0' && detail->err == e) {
        if (detail->position >= 0) {
            fprintf(stderr, ": %s (token %d)", detail->message, detail->position + 1);
        } else {
            fprintf(stderr, ": %s", detail->message);
        }
    }

    fputc('\n', stderr);
}

static size_t align_or_die(size_t x, size_t align)
{
    size_t aligned = safe_align(x, align);
    if (aligned == SIZE_MAX) {
        print_err(E_OOM, "arena alignment overflow");
        exit(FISKTA_EXIT_RESOURCE);
    }
    return aligned;
}

// =============================================================================
// Platform helpers
// =============================================================================

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
    if (!io || !io->f) {
        return;
    }

    int fd = fileno(io->f);
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        // Regular file: we can refresh size without disturbing FILE* position
        io->size = (i64)st.st_size;
    }
}

// =============================================================================
// Loop orchestration helpers
// =============================================================================

static void loop_init(LoopState* state, const RuntimeConfig* config)
{
    if (!state || !config) {
        return;
    }

    memset(state, 0, sizeof *state);
    state->enabled = config->loop_enabled;
    state->mode = config->loop_mode;
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
    // This ensures MONITOR mode runs at least once before checking idle timeout
    state->last_size = -1;

    state->vm.cursor = VM_CURSOR_UNSET;
    for (i32 i = 0; i < MAX_LABELS; i++) {
        state->vm.label_pos[i] = -1;
    }

    // All modes start at beginning; FOLLOW advances baseline to EOF after first successful iteration
    state->baseline = 0;
}

static void loop_compute_window(LoopState* state, File* io, i64* lo, i64* hi, bool* out_size_changed)
{
    bool size_changed = false;
    refresh_file_size(io);
    i64 size = io_size(io);
    if (size != state->last_size) {
        state->last_size = size;
        state->last_activity_ms = now_millis(); // data arrived/truncated
        size_changed = true;
    }
    if (state->mode == LOOP_MODE_FOLLOW && size < state->baseline) {
        state->baseline = size; // file shrank: restart tail at new EOF
    }
    *hi = size;

    switch (state->mode) {
    case LOOP_MODE_MONITOR:
        *lo = 0;
        break;
    case LOOP_MODE_FOLLOW:
        *lo = state->baseline;
        break;
    case LOOP_MODE_CONTINUE:
        if (state->vm.cursor != VM_CURSOR_UNSET) {
            *lo = clamp64(state->vm.cursor, 0, size);
        } else {
            *lo = 0;
        }
        break;
    default:
        *lo = 0;
        break;
    }
    if (*lo > *hi) {
        *lo = *hi; // truncation safety
    }
    if (out_size_changed) {
        *out_size_changed = size_changed;
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
    state->last_result = result;

    switch (result.status) {
    case ITER_OK:
        // success: mark activity and bump baselines
        state->last_activity_ms = now_millis();
        if (state->mode == LOOP_MODE_CONTINUE) {
            if (state->vm.cursor != VM_CURSOR_UNSET) {
                // baseline becomes new cursor for next pass
                state->baseline = clamp64(state->vm.cursor, 0, data_hi);
            }
        } else if (state->mode == LOOP_MODE_FOLLOW) {
            state->baseline = data_hi; // tail at EOF
        } else { /* MONITOR: stays 0 */
        }
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
    }
}

// =============================================================================
// Program iteration
// =============================================================================

static IterResult execute_program_iteration(const Program* prg, File* io, VM* vm,
    Range* clause_ranges, LabelWrite* clause_labels,
    char* clause_inline, i32 inline_slots_total,
    i64 data_lo, i64 data_hi)
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
                iter_result.status = ITER_IO_ERROR;
                iter_result.last_err = E_OOM;
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
                    e = io_emit(io, range->file.start, range->file.end, stdout);
                } else {
                    if ((size_t)fwrite(range->lit.bytes, 1, (size_t)range->lit.len, stdout) != (size_t)range->lit.len) {
                        e = E_IO;
                    }
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
        }
    }

    if (iter_result.status == ITER_IO_ERROR) {
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

// =============================================================================
// Main runtime orchestrator
// =============================================================================

int run_program(i32 token_count, const String* tokens, const RuntimeConfig* config)
{
    if (!tokens || !config) {
        return FISKTA_EXIT_PARSE;
    }

    /************************************************************
     * PHASE 1: PREFLIGHT PARSE
     * Analyze operations to determine memory requirements
     *************************************************************/
    ParsePlan plan = (ParsePlan) { 0 };
    const char* path = NULL;
    enum Err e = parse_preflight(token_count, tokens, config->input_path, &plan, &path);
    if (e != E_OK) {
        print_err(e, "parse preflight");
        return FISKTA_EXIT_PARSE;
    }

    /************************************************************
     * PHASE 2: COMPUTE ARENA SIZES
     * Calculate total memory needed for all data structures
     ************************************************************/
    const size_t search_buf_cap = (FW_WIN > (BK_BLK + OVERLAP_MAX)) ? (size_t)FW_WIN : (size_t)(BK_BLK + OVERLAP_MAX);
    const size_t ops_bytes = (size_t)plan.total_ops * sizeof(Op);
    const size_t clauses_bytes = (size_t)plan.clause_count * sizeof(Clause);
    const size_t str_pool_bytes = plan.needle_bytes;

    const size_t re_prog_bytes = (size_t)plan.sum_findr_ops * sizeof(ReProg);
    const size_t re_ins_bytes = (size_t)plan.re_ins_estimate * sizeof(ReInst);
    const size_t re_cls_bytes = (size_t)plan.re_classes_estimate * sizeof(ReClass);

    // Choose per-run thread capacity as ~2x max nins, min 32.
    int re_threads_cap = plan.re_ins_estimate_max > 0 ? 2 * plan.re_ins_estimate_max : 32;
    if (re_threads_cap < 32) {
        re_threads_cap = 32;
    }
    const size_t re_threads_bytes = (size_t)re_threads_cap * sizeof(ReThread);

    /************************************************************
     * PHASE 3: ARENA ALLOCATION
     * Allocate single memory block and compute aligned offsets
     ************************************************************/
    size_t search_buf_size = align_or_die(search_buf_cap, alignof(unsigned char));
    size_t clauses_size = align_or_die(clauses_bytes, alignof(Clause));
    size_t ops_size = align_or_die(ops_bytes, alignof(Op));
    size_t re_prog_size = align_or_die(re_prog_bytes, alignof(ReProg));
    size_t re_ins_size = align_or_die(re_ins_bytes, alignof(ReInst));
    size_t re_cls_size = align_or_die(re_cls_bytes, alignof(ReClass));
    size_t str_pool_size = align_or_die(str_pool_bytes, alignof(char));
    // Two thread buffers + two seen arrays sized to max estimated nins
    size_t re_seen_bytes_each = (size_t)(plan.re_ins_estimate_max > 0 ? plan.re_ins_estimate_max : 32);
    size_t re_seen_size;
    if (add_overflow(re_seen_bytes_each, re_seen_bytes_each, &re_seen_size)) {
        print_err(E_OOM, "regex 'seen' size overflow");
        return FISKTA_EXIT_RESOURCE;
    }
    size_t re_thrbufs_size = align_or_die(re_threads_bytes, alignof(ReThread)) * 2;

    size_t ranges_bytes = (plan.sum_take_ops > 0) ? align_or_die((size_t)plan.sum_take_ops * sizeof(Range), alignof(Range)) : 0;
    size_t labels_bytes = (plan.sum_label_ops > 0) ? align_or_die((size_t)plan.sum_label_ops * sizeof(LabelWrite), alignof(LabelWrite)) : 0;
    size_t inline_bytes = (plan.sum_inline_lits > 0) ? align_or_die((size_t)plan.sum_inline_lits * INLINE_LIT_CAP, alignof(char)) : 0;

    size_t total = search_buf_size;
    if (add_overflow(total, clauses_size, &total) || add_overflow(total, ops_size, &total) || add_overflow(total, re_prog_size, &total) || add_overflow(total, re_ins_size, &total) || add_overflow(total, re_cls_size, &total) || add_overflow(total, str_pool_size, &total) || add_overflow(total, re_thrbufs_size, &total) || add_overflow(total, re_seen_size, &total) || add_overflow(total, ranges_bytes, &total) || add_overflow(total, labels_bytes, &total) || add_overflow(total, inline_bytes, &total) || add_overflow(total, 64, &total)) { // small cushion
        print_err(E_OOM, "arena size overflow");
        return FISKTA_EXIT_RESOURCE;
    }

    void* block = malloc(total);
    if (!block) {
        print_err(E_OOM, "arena alloc");
        return FISKTA_EXIT_RESOURCE;
    }
    Arena arena;
    arena_init(&arena, block, total);

    /************************************************************
     * PHASE 4: CARVE ARENA SLICES
     * Partition the memory block into specific buffers
     ************************************************************/
    unsigned char* search_buf = arena_alloc(&arena, search_buf_cap, alignof(unsigned char));
    Clause* clauses_buf = arena_alloc(&arena, clauses_bytes, alignof(Clause));
    Op* ops_buf = arena_alloc(&arena, ops_bytes, alignof(Op));
    ReThread* re_curr_thr = arena_alloc(&arena, re_threads_bytes, alignof(ReThread));
    ReThread* re_next_thr = arena_alloc(&arena, re_threads_bytes, alignof(ReThread));
    unsigned char* seen_curr = arena_alloc(&arena, re_seen_bytes_each, 1);
    unsigned char* seen_next = arena_alloc(&arena, re_seen_bytes_each, 1);
    ReProg* re_progs = arena_alloc(&arena, re_prog_bytes, alignof(ReProg));
    ReInst* re_ins = arena_alloc(&arena, re_ins_bytes, alignof(ReInst));
    ReClass* re_cls = arena_alloc(&arena, re_cls_bytes, alignof(ReClass));
    char* str_pool = arena_alloc(&arena, str_pool_bytes, alignof(char));
    Range* clause_ranges = (plan.sum_take_ops > 0) ? arena_alloc(&arena, (size_t)plan.sum_take_ops * sizeof(Range), alignof(Range)) : NULL;
    LabelWrite* clause_labels = (plan.sum_label_ops > 0) ? arena_alloc(&arena, (size_t)plan.sum_label_ops * sizeof(LabelWrite), alignof(LabelWrite)) : NULL;
    char* clause_inline = (plan.sum_inline_lits > 0) ? arena_alloc(&arena, (size_t)plan.sum_inline_lits * INLINE_LIT_CAP, alignof(char)) : NULL;

    if (!search_buf || !clauses_buf || !ops_buf
        || !re_curr_thr || !re_next_thr || !seen_curr || !seen_next
        || !re_progs || !re_ins || !re_cls || !str_pool
        || (plan.sum_take_ops > 0 && !clause_ranges)
        || (plan.sum_label_ops > 0 && !clause_labels)
        || (plan.sum_inline_lits > 0 && !clause_inline)) {
        print_err(E_OOM, "arena carve");
        free(block);
        return FISKTA_EXIT_RESOURCE;
    }

    /************************************************************
     * PHASE 5: BUILD PROGRAM
     * Parse operations into executable program structure
     ************************************************************/
    Program prg = (Program) { 0 };
    e = parse_build(token_count, tokens, config->input_path, &prg, &path,
        clauses_buf, ops_buf, str_pool, str_pool_bytes);
    if (e != E_OK) {
        print_err(e, "parse build");
        free(block);
        return FISKTA_EXIT_PARSE;
    }
    if (prg.clause_count == 0) {
        print_err(E_PARSE, "no operations parsed");
        free(block);
        return FISKTA_EXIT_PARSE;
    }

    // Compile all regex patterns upfront
    i32 re_prog_idx = 0;
    i32 re_ins_idx = 0;
    i32 re_cls_idx = 0;
    for (i32 ci = 0; ci < prg.clause_count; ++ci) {
        Clause* clause = &prg.clauses[ci];
        for (i32 i = 0; i < clause->op_count; ++i) {
            Op* op = &clause->ops[i];
            if (op->kind == OP_FIND_RE) {
                ReProg* prog = &re_progs[re_prog_idx++];
                enum Err err = re_compile_into(op->u.findr.pattern, prog,
                    re_ins + re_ins_idx, (i32)(re_ins_bytes / sizeof(ReInst)) - re_ins_idx, &re_ins_idx,
                    re_cls + re_cls_idx, (i32)(re_cls_bytes / sizeof(ReClass)) - re_cls_idx, &re_cls_idx);
                if (err != E_OK) {
                    print_err(err, "regex compile");
                    free(block);
                    return (err == E_PARSE || err == E_BAD_NEEDLE) ? FISKTA_EXIT_REGEX : FISKTA_EXIT_RESOURCE;
                }
                op->u.findr.prog = prog;
            } else if (op->kind == OP_TAKE_UNTIL_RE) {
                ReProg* prog = &re_progs[re_prog_idx++];
                enum Err err = re_compile_into(op->u.take_until_re.pattern, prog,
                    re_ins + re_ins_idx, (i32)(re_ins_bytes / sizeof(ReInst)) - re_ins_idx, &re_ins_idx,
                    re_cls + re_cls_idx, (i32)(re_cls_bytes / sizeof(ReClass)) - re_cls_idx, &re_cls_idx);
                if (err != E_OK) {
                    print_err(err, "regex compile");
                    free(block);
                    return (err == E_PARSE || err == E_BAD_NEEDLE) ? FISKTA_EXIT_REGEX : FISKTA_EXIT_RESOURCE;
                }
                op->u.take_until_re.prog = prog;
            }
        }
    }

    /********************************************
     * PHASE 6: OPEN FILE I/O
     * Initialize file handle and search buffers
     ********************************************/
    File io = { 0 };
    e = io_open(&io, path, search_buf, search_buf_cap);
    if (e != E_OK) {
        free(block);
        print_err(e, "I/O open");
        return FISKTA_EXIT_IO;
    }

    io_set_regex_scratch(&io, re_curr_thr, re_next_thr, re_threads_cap,
        seen_curr, seen_next, re_seen_bytes_each);

    /*****************************************************
     * PHASE 7: EXECUTE PROGRAM
     * Run operations with optional looping for streaming
     *****************************************************/
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
        bool size_changed = false;
        loop_compute_window(&loop_state, &io, &lo, &hi, &size_changed);

        // Detect idle condition (mode-dependent)
        bool no_new_data = false;
        if (loop_state.mode == LOOP_MODE_MONITOR) {
            // MONITOR: re-scans entire file, so idle = file unchanged
            no_new_data = !size_changed;
        } else {
            // FOLLOW/CONTINUE: scan window [lo, hi), so idle = empty window
            no_new_data = (lo >= hi);
        }

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

        // CONTINUE passes saved VM; other modes run with ephemeral VM
        VM* vm_ptr = (loop_state.mode == LOOP_MODE_CONTINUE) ? &loop_state.vm : NULL;
        IterResult iteration = execute_program_iteration(&prg, &io, vm_ptr,
            clause_ranges, clause_labels,
            clause_inline, plan.sum_inline_lits,
            lo, hi);

        loop_commit(&loop_state, hi, iteration, config->ignore_loop_failures);
        fflush(stdout);

        if (!loop_state.enabled || loop_state.exit_code) {
            break;
        }

        // Throttle non-FOLLOW modes between passes; FOLLOW sleeps only when there's no new data.
        if (loop_state.mode != LOOP_MODE_FOLLOW) {
            if (loop_state.loop_ms > 0) {
                sleep_msec(loop_state.loop_ms);
            }
            // still honor --for (exec timeout)
            reason = 0;
            (void)loop_should_wait_or_stop(&loop_state, /*no_new_data=*/false, &reason); // only checks exec timeout here
            if ((loop_state.exit_reason = reason) != 0) {
                break;
            }
        }
    }

    io_close(&io);
    free(block);

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
    case ITER_PROGRAM_FAIL:
        return FISKTA_EXIT_PROGRAM_FAIL;
    default:
        return FISKTA_EXIT_IO;
    }
}

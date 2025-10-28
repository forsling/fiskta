#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "runtime.h"
#include "engine.h"
#include "error.h"
#include "fileio.h"
#include "fiskta.h"
#include "parse.h"
#include "regex_prog.h"
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
#define VM_CURSOR_UNSET ((i64) - 1)

// =============================================================================
// Regex budget calculation
// =============================================================================

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
    case E_CAPACITY:
        return "buffer capacity exceeded";
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
// RuntimeScratch management
// =============================================================================

void runtime_scratch_free(RuntimeScratch* s)
{
    if (!s) {
        return;
    }
    // Only free if we allocated it (not if caller provided via build_program_with_scratch)
    if (s->arena_owned && s->arena_block) {
        free(s->arena_block);
    }
    memset(s, 0, sizeof(*s));
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

// =============================================================================
// Resource requirements query (pre-flight sizing)
// =============================================================================

int program_requirements(i32 token_count, const String* tokens,
    RuntimeRequirements* out)
{
    if (!tokens || !out) {
        return FISKTA_EXIT_PARSE;
    }

    // Initialize output
    memset(out, 0, sizeof(*out));

    /************************************************************
     * PHASE 1: PREFLIGHT PARSE
     * Analyze operations to determine memory requirements
     *************************************************************/
    ParsePlan plan = (ParsePlan) { 0 };
    const char* path = NULL;
    enum Err e = parse_preflight(token_count, tokens, NULL, &plan, &path);
    if (e != E_OK) {
        print_err(e, "parse preflight");
        if (e == E_CAPACITY) {
            return FISKTA_EXIT_CAPACITY;
        }
        return FISKTA_EXIT_PARSE;
    }

    /************************************************************
     * PHASE 2: COMPUTE SIZES
     * Calculate total memory needed for all data structures
     *************************************************************/

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

    // Regex VM scratch: unified budget (actual split computed after compilation)
    // Report worst-case allocations since we don't know actual pattern sizes yet.
    // Actual thread_cap and seen_bytes will be derived in build_program().
    out->regex_thread_cap_max = (size_t)(FISKTA_REGEX_BUDGET_DEFAULT / (RE_THREAD_BYTES * RE_LISTS));
    out->regex_seen_bytes_max = FISKTA_REGEX_BUDGET_DEFAULT / 2;

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

    /************************************************************
     * PHASE 3: COMPUTE TOTAL ARENA SIZE WITH ALIGNMENT
     *************************************************************/
    size_t search_buf_size = align_or_die(search_buf_cap, alignof(unsigned char));
    size_t clauses_size = align_or_die(clauses_bytes, alignof(Clause));
    size_t ops_size = align_or_die(ops_bytes, alignof(Op));
    size_t re_prog_size = align_or_die(re_prog_bytes, alignof(ReProg));
    size_t re_ins_size = align_or_die(re_ins_bytes, alignof(ReInst));
    size_t re_cls_size = align_or_die(re_cls_bytes, alignof(ReClass));
    size_t str_pool_size = align_or_die(str_pool_bytes, alignof(char));

    // Two thread buffers + two seen arrays (using fixed policy budgets)
    const size_t re_threads_bytes = out->regex_thread_cap_max * sizeof(ReThread);
    size_t re_seen_size;
    if (add_overflow(out->regex_seen_bytes_max, out->regex_seen_bytes_max, &re_seen_size)) {
        print_err(E_OOM, "regex 'seen' size overflow");
        return FISKTA_EXIT_RESOURCE;
    }
    size_t re_thrbufs_size = align_or_die(re_threads_bytes, alignof(ReThread)) * 2;

    // Staging buffers (already computed above, but need alignment)
    size_t ranges_size = (plan.sum_take_ops > 0) ? align_or_die(ranges_bytes, alignof(Range)) : 0;
    size_t labels_size = (plan.sum_label_ops > 0) ? align_or_die(labels_bytes, alignof(LabelWrite)) : 0;
    size_t inline_size = (plan.sum_inline_lits > 0) ? align_or_die(inline_bytes, alignof(char)) : 0;

    // Sum everything with overflow checking
    size_t total = search_buf_size;
    if (add_overflow(total, clauses_size, &total) || add_overflow(total, ops_size, &total) || add_overflow(total, re_prog_size, &total) || add_overflow(total, re_ins_size, &total) || add_overflow(total, re_cls_size, &total) || add_overflow(total, str_pool_size, &total) || add_overflow(total, re_thrbufs_size, &total) || add_overflow(total, re_seen_size, &total) || add_overflow(total, ranges_size, &total) || add_overflow(total, labels_size, &total) || add_overflow(total, inline_size, &total) || add_overflow(total, 64, &total)) { // small cushion
        print_err(E_OOM, "arena size overflow");
        return FISKTA_EXIT_RESOURCE;
    }

    out->arena_bytes = total;
    return FISKTA_EXIT_OK;
}

// =============================================================================
// Build program (compile-time phase)
// =============================================================================

int build_program(i32 token_count, const String* tokens,
    Program* prog_out,
    RuntimeScratch* scratch_out)
{
    if (!tokens || !prog_out || !scratch_out) {
        return FISKTA_EXIT_PARSE;
    }

    // Initialize outputs
    memset(prog_out, 0, sizeof(*prog_out));
    memset(scratch_out, 0, sizeof(*scratch_out));

    /************************************************************
     * PHASE 1: PREFLIGHT PARSE
     * Analyze operations to determine memory requirements
     *************************************************************/
    ParsePlan plan = (ParsePlan) { 0 };
    const char* path = NULL;
    enum Err e = parse_preflight(token_count, tokens, NULL, &plan, &path);
    if (e != E_OK) {
        print_err(e, "parse preflight");
        if (e == E_CAPACITY) {
            return FISKTA_EXIT_CAPACITY;
        }
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

    // Allocate conservatively for regex VM (actual usage computed after compilation)
    // Worst case: entire budget goes to threads (if patterns are tiny)
    const int re_threads_cap_alloc = (int)(FISKTA_REGEX_BUDGET_DEFAULT / (RE_THREAD_BYTES * RE_LISTS));
    const size_t re_threads_bytes = (size_t)re_threads_cap_alloc * sizeof(ReThread);
    // Worst case: entire budget goes to seen tables (if patterns are huge)
    const size_t re_seen_bytes_each = FISKTA_REGEX_BUDGET_DEFAULT / 2;

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
    // Two thread buffers + two seen arrays (fixed budgets)
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
    unsigned char* seen_curr = arena_alloc(&arena, re_seen_bytes_each, alignof(u32));
    unsigned char* seen_next = arena_alloc(&arena, re_seen_bytes_each, alignof(u32));
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
    e = parse_build(token_count, tokens, NULL, prog_out, &path,
        clauses_buf, ops_buf, str_pool, str_pool_bytes);
    if (e != E_OK) {
        print_err(e, "parse build");
        free(block);
        if (e == E_CAPACITY) {
            return FISKTA_EXIT_CAPACITY;
        }
        return FISKTA_EXIT_PARSE;
    }
    if (prog_out->clause_count == 0) {
        print_err(E_PARSE, "no operations parsed");
        free(block);
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
                    print_err(err, "regex compile");
                    free(block);
                    if (err == E_PARSE || err == E_BAD_NEEDLE) {
                        return FISKTA_EXIT_PARSE;
                    } else if (err == E_CAPACITY) {
                        return FISKTA_EXIT_CAPACITY;
                    } else {
                        return FISKTA_EXIT_RESOURCE;
                    }
                }
                op->u.findr.prog = prog;
            } else if (op->kind == OP_TAKE_UNTIL_RE) {
                ReProg* prog = &re_progs[re_prog_idx++];
                enum Err err = re_compile_into(op->u.take_until_re.pattern, prog,
                    re_ins, (i32)(re_ins_bytes / sizeof(ReInst)), &re_ins_idx,
                    re_cls, (i32)(re_cls_bytes / sizeof(ReClass)), &re_cls_idx);
                if (err != E_OK) {
                    print_err(err, "regex compile");
                    free(block);
                    if (err == E_PARSE || err == E_BAD_NEEDLE) {
                        return FISKTA_EXIT_PARSE;
                    } else if (err == E_CAPACITY) {
                        return FISKTA_EXIT_CAPACITY;
                    } else {
                        return FISKTA_EXIT_RESOURCE;
                    }
                }
                op->u.take_until_re.prog = prog;
            }
        }
    }

    // Compute actual regex budget split based on compiled patterns
    // Find the largest seen table requirement across all compiled regexes
    size_t max_seen_bytes = 0;
    for (i32 pi = 0; pi < re_prog_idx; ++pi) {
        ReProgRequirements req;
        regex_prog_requirements(&re_progs[pi], &req);
        if (req.seen_bytes > max_seen_bytes) {
            max_seen_bytes = req.seen_bytes;
        }
    }

    // Derive thread_cap from budget minus seen table requirement
    int actual_thread_cap = compute_thread_cap_from_budget(FISKTA_REGEX_BUDGET_DEFAULT, max_seen_bytes);
    if (actual_thread_cap == 0) {
        print_err(E_CAPACITY, NULL);
        error_detail_set(E_CAPACITY, -1,
            "regex patterns require %zu bytes for seen tables, exceeding budget of %zu bytes",
            max_seen_bytes * 2, (size_t)FISKTA_REGEX_BUDGET_DEFAULT);
        free(block);
        return FISKTA_EXIT_CAPACITY;
    }

    // Verify we allocated enough (should always be true with conservative estimates)
    if (actual_thread_cap > re_threads_cap_alloc) {
        actual_thread_cap = re_threads_cap_alloc; // Cap at allocated size
    }
    const size_t actual_seen_bytes = max_seen_bytes;

    // Fill RuntimeScratch with allocated buffers
    scratch_out->search_buf = search_buf;
    scratch_out->search_buf_cap = search_buf_cap;
    scratch_out->re_curr = re_curr_thr;
    scratch_out->re_next = re_next_thr;
    scratch_out->re_thread_cap = actual_thread_cap;  // Use derived cap, not allocated cap
    scratch_out->regex_work_budget = REGEX_WORK_BUDGET_DEFAULT;
    scratch_out->seen_curr = seen_curr;
    scratch_out->seen_next = seen_next;
    scratch_out->seen_bytes = actual_seen_bytes;  // Use actual requirement
    scratch_out->clause_ranges = clause_ranges;
    scratch_out->clause_labels = clause_labels;
    scratch_out->clause_inline = clause_inline;
    scratch_out->sum_inline_lits = plan.sum_inline_lits;
    scratch_out->arena_block = block;
    scratch_out->arena_size = total;
    scratch_out->arena_owned = true; // We malloc'd it, we own it

    return FISKTA_EXIT_OK;
}

// Build program with caller-provided arena (zero-malloc variant)
int build_program_with_scratch(i32 token_count, const String* tokens,
    Program* prog_out,
    void* arena_block, size_t arena_size,
    RuntimeScratch* scratch_out)
{
    if (!tokens || !prog_out || !scratch_out || !arena_block) {
        return FISKTA_EXIT_PARSE;
    }

    // Initialize outputs
    memset(prog_out, 0, sizeof(*prog_out));
    memset(scratch_out, 0, sizeof(*scratch_out));

    // Perform same build logic as build_program(), but use provided arena
    // This is essentially a copy of build_program() with malloc() replaced

    /************************************************************
     * PHASE 1: PREFLIGHT PARSE
     *************************************************************/
    ParsePlan plan = (ParsePlan) { 0 };
    const char* path = NULL;
    enum Err e = parse_preflight(token_count, tokens, NULL, &plan, &path);
    if (e != E_OK) {
        print_err(e, "parse preflight");
        if (e == E_CAPACITY) {
            return FISKTA_EXIT_CAPACITY;
        }
        return FISKTA_EXIT_PARSE;
    }

    /************************************************************
     * PHASE 2: COMPUTE SIZES (to verify arena is large enough)
     *************************************************************/
    const size_t search_buf_cap = (FW_WIN > (BK_BLK + OVERLAP_MAX)) ? (size_t)FW_WIN : (size_t)(BK_BLK + OVERLAP_MAX);
    const size_t ops_bytes = (size_t)plan.total_ops * sizeof(Op);
    const size_t clauses_bytes = (size_t)plan.clause_count * sizeof(Clause);
    const size_t str_pool_bytes = plan.needle_bytes;
    const size_t re_prog_bytes = (size_t)plan.sum_findr_ops * sizeof(ReProg);
    const size_t re_ins_bytes = (size_t)plan.re_ins_estimate * sizeof(ReInst);
    const size_t re_cls_bytes = (size_t)plan.re_classes_estimate * sizeof(ReClass);

    // Allocate conservatively for regex VM (actual usage computed after compilation)
    // Worst case: entire budget goes to threads (if patterns are tiny)
    const int re_threads_cap_alloc = (int)(FISKTA_REGEX_BUDGET_DEFAULT / (RE_THREAD_BYTES * RE_LISTS));
    const size_t re_threads_bytes = (size_t)re_threads_cap_alloc * sizeof(ReThread);
    // Worst case: entire budget goes to seen tables (if patterns are huge)
    const size_t re_seen_bytes_each = FISKTA_REGEX_BUDGET_DEFAULT / 2;

    /************************************************************
     * PHASE 3: USE PROVIDED ARENA
     *************************************************************/
    Arena arena;
    arena_init(&arena, arena_block, arena_size);

    /************************************************************
     * PHASE 4: CARVE ARENA SLICES (same as build_program)
     *************************************************************/
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
    Range* clause_ranges = (plan.sum_take_ops > 0) ? arena_alloc(&arena, (size_t)plan.sum_take_ops * sizeof(Range), alignof(Range)) : NULL;
    LabelWrite* clause_labels = (plan.sum_label_ops > 0) ? arena_alloc(&arena, (size_t)plan.sum_label_ops * sizeof(LabelWrite), alignof(LabelWrite)) : NULL;
    char* clause_inline = (plan.sum_inline_lits > 0) ? arena_alloc(&arena, (size_t)plan.sum_inline_lits * INLINE_LIT_CAP, alignof(char)) : NULL;

    if (!search_buf || !clauses_buf || !ops_buf
        || !re_curr_thr || !re_next_thr || !seen_curr || !seen_next
        || !re_progs || !re_ins || !re_cls || !str_pool
        || (plan.sum_take_ops > 0 && !clause_ranges)
        || (plan.sum_label_ops > 0 && !clause_labels)
        || (plan.sum_inline_lits > 0 && !clause_inline)) {
        print_err(E_OOM, "arena carve (provided arena too small?)");
        return FISKTA_EXIT_RESOURCE;
    }

    /************************************************************
     * PHASE 5: BUILD PROGRAM (same as build_program)
     *************************************************************/
    e = parse_build(token_count, tokens, NULL, prog_out, &path,
        clauses_buf, ops_buf, str_pool, str_pool_bytes);
    if (e != E_OK) {
        print_err(e, "parse build");
        if (e == E_CAPACITY) {
            return FISKTA_EXIT_CAPACITY;
        }
        return FISKTA_EXIT_PARSE;
    }
    if (prog_out->clause_count == 0) {
        print_err(E_PARSE, "no operations parsed");
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
                    print_err(err, "regex compile");
                    if (err == E_PARSE || err == E_BAD_NEEDLE) {
                        return FISKTA_EXIT_PARSE;
                    } else if (err == E_CAPACITY) {
                        return FISKTA_EXIT_CAPACITY;
                    } else {
                        return FISKTA_EXIT_RESOURCE;
                    }
                }
                op->u.findr.prog = prog;
            } else if (op->kind == OP_TAKE_UNTIL_RE) {
                ReProg* prog = &re_progs[re_prog_idx++];
                enum Err err = re_compile_into(op->u.take_until_re.pattern, prog,
                    re_ins, (i32)(re_ins_bytes / sizeof(ReInst)), &re_ins_idx,
                    re_cls, (i32)(re_cls_bytes / sizeof(ReClass)), &re_cls_idx);
                if (err != E_OK) {
                    print_err(err, "regex compile");
                    if (err == E_PARSE || err == E_BAD_NEEDLE) {
                        return FISKTA_EXIT_PARSE;
                    } else if (err == E_CAPACITY) {
                        return FISKTA_EXIT_CAPACITY;
                    } else {
                        return FISKTA_EXIT_RESOURCE;
                    }
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

    int actual_thread_cap = compute_thread_cap_from_budget(FISKTA_REGEX_BUDGET_DEFAULT, max_seen_bytes);
    if (actual_thread_cap == 0) {
        print_err(E_CAPACITY, NULL);
        error_detail_set(E_CAPACITY, -1,
            "regex patterns require %zu bytes for seen tables, exceeding budget of %zu bytes",
            max_seen_bytes * 2, (size_t)FISKTA_REGEX_BUDGET_DEFAULT);
        return FISKTA_EXIT_CAPACITY;
    }

    if (actual_thread_cap > re_threads_cap_alloc) {
        actual_thread_cap = re_threads_cap_alloc;
    }
    const size_t actual_seen_bytes = max_seen_bytes;

    // Fill RuntimeScratch with buffers from PROVIDED arena
    scratch_out->search_buf = search_buf;
    scratch_out->search_buf_cap = search_buf_cap;
    scratch_out->re_curr = re_curr_thr;
    scratch_out->re_next = re_next_thr;
    scratch_out->re_thread_cap = actual_thread_cap;
    scratch_out->regex_work_budget = REGEX_WORK_BUDGET_DEFAULT;
    scratch_out->seen_curr = seen_curr;
    scratch_out->seen_next = seen_next;
    scratch_out->seen_bytes = actual_seen_bytes;
    scratch_out->clause_ranges = clause_ranges;
    scratch_out->clause_labels = clause_labels;
    scratch_out->clause_inline = clause_inline;
    scratch_out->sum_inline_lits = plan.sum_inline_lits;
    scratch_out->arena_block = arena_block;
    scratch_out->arena_size = arena_size;
    scratch_out->arena_owned = false; // Caller provided it, caller owns it

    return FISKTA_EXIT_OK;
}

// =============================================================================
// Execute program (runtime phase)
// =============================================================================

int runtime_execute(const Program* prog,
    const char* file_path,
    RuntimeScratch* scratch,
    const RuntimeConfig* config)
{
    if (!prog || !file_path || !scratch || !config) {
        return FISKTA_EXIT_PARSE;
    }

    /********************************************
     * PHASE 6: OPEN FILE I/O
     * Initialize file handle and search buffers
     ********************************************/
    File io = { 0 };
    enum Err e = io_open(&io, file_path, scratch->search_buf, scratch->search_buf_cap);
    if (e != E_OK) {
        print_err(e, "I/O open");
        return FISKTA_EXIT_IO;
    }

    io_set_regex_scratch(&io, scratch->re_curr, scratch->re_next, scratch->re_thread_cap,
        scratch->regex_work_budget, scratch->seen_curr, scratch->seen_next, scratch->seen_bytes);

    /*****************************************************
     * PHASE 7: EXECUTE PROGRAM
     * Run operations with optional continue loop
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
            scratch->clause_ranges, scratch->clause_labels,
            scratch->clause_inline, scratch->sum_inline_lits,
            lo, hi);

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
        // Print error details before returning for non-OK exit codes
        if (loop_state.exit_code == FISKTA_EXIT_PROGRAM_FAIL && error_detail_has()) {
            print_err(loop_state.last_result.last_err, NULL);
        }
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
        // Print error details if available for helpful diagnostics
        if (error_detail_has()) {
            print_err(loop_state.last_result.last_err, NULL);
        }
        return FISKTA_EXIT_PROGRAM_FAIL;
    default:
        return FISKTA_EXIT_IO;
    }
}

// =============================================================================
// Main runtime orchestrator
// =============================================================================

int run_program(i32 token_count, const String* tokens, const RuntimeConfig* config)
{
    if (!tokens || !config) {
        return FISKTA_EXIT_PARSE;
    }

    Program prog = { 0 };
    RuntimeScratch scratch = { 0 };

    // Phase 1: Build (parsing, allocation, compilation)
    int ret = build_program(token_count, tokens, &prog, &scratch);
    if (ret != FISKTA_EXIT_OK) {
        runtime_scratch_free(&scratch);
        return ret;
    }

    // Phase 2: Execute (file I/O, VM execution, loops)
    ret = runtime_execute(&prog, config->input_path, &scratch, config);

    // Cleanup
    runtime_scratch_free(&scratch);
    return ret;
}

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "util.h"
#include "fiskta.h"
#include "iosearch.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#if !defined(__MINGW32__) && !defined(__MINGW64__)
#define fseeko _fseeki64
#define ftello _ftelli64
#endif
#endif

enum Err io_open(File* io, const char* path,
    unsigned char* search_buf, size_t search_buf_cap);

// View helpers
static inline i64 vbof(const View* v) { return (v && v->active) ? v->lo : 0; }
static inline i64 veof(const View* v, const File* io) { return (v && v->active) ? v->hi : io_size(io); }
static inline i64 vclamp(const View* v, const File* io, i64 x) { return clamp64(x, vbof(v), veof(v, io)); }

// Saturate byte add into range before addition (prevents overflow past clamp edges)
static inline void apply_byte_saturation(i64* base, i64 delta, const View* v, const File* io, ClampPolicy cp)
{
    i64 lo = 0, hi = io_size((File*)io);
    if (cp == CLAMP_VIEW) {
        lo = vbof(v);
        hi = veof(v, io);
    }
    if (delta >= 0) {
        if (*base > hi - delta)
            *base = hi - delta;
    } else {
        if (*base < lo - delta)
            *base = lo - delta;
    }
    *base += delta;
}

// Helper for overflow-safe size arithmetic
static int add_ovf(size_t a, size_t b, size_t* out)
{
    if (SIZE_MAX - a < b)
        return 1;
    *out = a + b;
    return 0;
}

static void sleep_msec(i32 msec)
{
    if (msec <= 0)
        return;

#ifdef _WIN32
    Sleep((DWORD)msec);
#else
    struct timespec req;
    req.tv_sec = msec / 1000;
    req.tv_nsec = (long)(msec % 1000) * 1000000L;
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
    }
#endif
}

void clause_caps(const Clause* c, i32* out_ranges_cap, i32* out_labels_cap)
{
    i32 rc = 0, lc = 0;
    for (i32 i = 0; i < c->op_count; i++) {
        switch (c->ops[i].kind) {
        case OP_TAKE_LEN:
        case OP_TAKE_TO:
        case OP_TAKE_UNTIL:
        case OP_TAKE_UNTIL_RE:
        case OP_PRINT:
            rc++;
            break;
        case OP_BOX: {
            // Box operation produces multiple ranges (2 per line: content + newline)
            i32 down_offset = c->ops[i].u.box.down_offset;
            i32 lines = (down_offset < 0 ? -down_offset : down_offset) + 1;
            rc += lines * 2; // content + newline for each line
            break;
        }
        case OP_LABEL:
            lc++;
            break;
        default:
            break;
        }
    }
    *out_ranges_cap = rc > 0 ? rc : 1; // avoid zero-length arrays
    *out_labels_cap = lc > 0 ? lc : 1;
}

static enum Err execute_op(const Op* op, File* io, VM* vm,
    i64* c_cursor, Match* c_last_match,
    Range** ranges, i32* range_count, i32* range_cap,
    LabelWrite** label_writes, i32* label_count, i32* label_cap,
    const View* c_view);
static enum Err resolve_loc_expr_cp(
    const LocExpr* loc, File* io, const VM* vm,
    const Match* staged_match, i64 staged_cursor,
    const LabelWrite* staged_labels, i32 staged_label_count,
    const View* c_view, ClampPolicy clamp, i64* out);
void commit_labels(VM* vm, const LabelWrite* label_writes, i32 label_count);

enum Err engine_run(const Program* prg, const char* in_path, FILE* out)
{
    const i32 cc = prg->clause_count;
    enum Err err = E_OK;

    void* block = NULL;
    File io = { 0 };
    bool io_opened = false;

    i32 max_r_cap = 0;
    i32 max_l_cap = 0;
    for (i32 i = 0; i < cc; ++i) {
        i32 rc = 0, lc = 0;
        clause_caps(&prg->clauses[i], &rc, &lc);
        if (rc > max_r_cap)
            max_r_cap = rc;
        if (lc > max_l_cap)
            max_l_cap = lc;
    }

    const size_t search_buf_cap = (FW_WIN > (BK_BLK + OVERLAP_MAX)) ? (size_t)FW_WIN : (size_t)(BK_BLK + OVERLAP_MAX);

    int max_nins = 0;
    for (i32 ci = 0; ci < prg->clause_count; ++ci) {
        const Clause* C = &prg->clauses[ci];
        for (i32 oi = 0; oi < C->op_count; ++oi) {
            const Op* op = &C->ops[oi];
            if (op->kind == OP_FIND_RE && op->u.findr.prog) {
                if (op->u.findr.prog->nins > max_nins)
                    max_nins = op->u.findr.prog->nins;
            }
        }
    }
    int re_threads_cap = max_nins > 0 ? 2 * max_nins : 32;
    if (re_threads_cap < 32)
        re_threads_cap = 32;
    const size_t re_thr_bytes = (size_t)re_threads_cap * sizeof(ReThread);
    const size_t re_seen_bytes_each = (size_t)(max_nins > 0 ? max_nins : 32);

    size_t total = safe_align(search_buf_cap, 1);
    if (total == SIZE_MAX) {
        err = E_OOM;
        goto cleanup;
    }

    size_t re_thr_size = safe_align(re_thr_bytes, alignof(ReThread)) * 2;
    if (re_thr_size == SIZE_MAX) {
        err = E_OOM;
        goto cleanup;
    }
    if (add_ovf(total, re_thr_size, &total)) {
        err = E_OOM;
        goto cleanup;
    }

    size_t re_seen_size = safe_align(re_seen_bytes_each, 1) * 2;
    if (re_seen_size == SIZE_MAX) {
        err = E_OOM;
        goto cleanup;
    }
    if (add_ovf(total, re_seen_size, &total)) {
        err = E_OOM;
        goto cleanup;
    }

    size_t ranges_bytes = (max_r_cap > 0) ? safe_align((size_t)max_r_cap * sizeof(Range), alignof(Range)) : 0;
    if (ranges_bytes == SIZE_MAX || add_ovf(total, ranges_bytes, &total)) {
        err = E_OOM;
        goto cleanup;
    }

    size_t labels_bytes = (max_l_cap > 0) ? safe_align((size_t)max_l_cap * sizeof(LabelWrite), alignof(LabelWrite)) : 0;
    if (labels_bytes == SIZE_MAX || add_ovf(total, labels_bytes, &total)) {
        err = E_OOM;
        goto cleanup;
    }

    if (add_ovf(total, 64, &total)) { // small cushion like main.c
        err = E_OOM;
        goto cleanup;
    }

    block = malloc(total);
    if (!block) {
        err = E_OOM;
        goto cleanup;
    }

    Arena arena;
    arena_init(&arena, block, total);

    unsigned char* search_buf = (unsigned char*)arena_alloc(&arena, search_buf_cap, 1);
    ReThread* re_curr_thr = (ReThread*)arena_alloc(&arena, re_thr_bytes, alignof(ReThread));
    ReThread* re_next_thr = (ReThread*)arena_alloc(&arena, re_thr_bytes, alignof(ReThread));
    unsigned char* seen_curr = (unsigned char*)arena_alloc(&arena, re_seen_bytes_each, 1);
    unsigned char* seen_next = (unsigned char*)arena_alloc(&arena, re_seen_bytes_each, 1);
    Range* ranges_buf = (max_r_cap > 0) ? (Range*)arena_alloc(&arena, (size_t)max_r_cap * sizeof(Range), alignof(Range)) : NULL;
    LabelWrite* labels_buf = (max_l_cap > 0) ? (LabelWrite*)arena_alloc(&arena, (size_t)max_l_cap * sizeof(LabelWrite), alignof(LabelWrite)) : NULL;

    if (!search_buf || !re_curr_thr || !re_next_thr || !seen_curr || !seen_next || (max_r_cap > 0 && !ranges_buf) || (max_l_cap > 0 && !labels_buf)) {
        err = E_OOM;
        goto cleanup;
    }

    err = io_open(&io, in_path, search_buf, search_buf_cap);
    if (err != E_OK)
        goto cleanup;
    io_opened = true;
    io_set_regex_scratch(&io, re_curr_thr, re_next_thr, re_threads_cap,
        seen_curr, seen_next, (size_t)re_seen_bytes_each);

    VM vm = { 0 };
    vm.cursor = 0;
    vm.last_match.valid = false;
    for (i32 i = 0; i < 128; i++)
        vm.label_pos[i] = -1;

    i32 successful_clauses = 0;
    enum Err last_err = E_OK;

    for (i32 i = 0; i < cc; ++i) {
        i32 rc = 0, lc = 0;
        clause_caps(&prg->clauses[i], &rc, &lc);
        Range* r_tmp = (rc > 0) ? ranges_buf : NULL;
        LabelWrite* lw_tmp = (lc > 0) ? labels_buf : NULL;

        StagedResult result;
        err = execute_clause_stage_only(&prg->clauses[i], &io, &vm,
            r_tmp, rc, lw_tmp, lc, &result);
        if (err == E_OK) {
            // Commit staged ranges to output
            for (i32 j = 0; j < result.range_count; j++) {
                if (result.ranges[j].kind == RANGE_FILE) {
                    err = io_emit(&io, result.ranges[j].file.start, result.ranges[j].file.end, out);
                } else {
                    // RANGE_LIT: write literal bytes
                    if ((size_t)fwrite(result.ranges[j].lit.bytes, 1, (size_t)result.ranges[j].lit.len, out) != (size_t)result.ranges[j].lit.len)
                        err = E_IO;
                }
                if (err != E_OK)
                    break;
            }
            if (err == E_OK) {
                // Commit staged VM state
                commit_labels(&vm, result.label_writes, result.label_count);
                vm.cursor = result.staged_vm.cursor;
                vm.last_match = result.staged_vm.last_match;
                vm.view = result.staged_vm.view;
            }
        }
        if (err == E_OK) {
            successful_clauses++;
        } else {
            last_err = err;
        }
    }

    err = (successful_clauses == 0) ? last_err : E_OK;

cleanup:
    if (io_opened)
        io_close(&io);
    free(block);
    return err;
}

enum Err execute_clause_stage_only(const Clause* clause,
    void* io_ptr, VM* vm,
    Range* ranges, i32 ranges_cap,
    LabelWrite* label_writes, i32 label_cap,
    StagedResult* result)
{
    File* io = (File*)io_ptr;
    i64 c_cursor = vm->cursor;
    Match c_last_match = vm->last_match;
    View c_view = vm->view;

    i32 range_count = 0;
    i32 label_count = 0;

    enum Err err = E_OK;
    for (i32 i = 0; i < clause->op_count; i++) {
        err = execute_op(&clause->ops[i], io, vm,
            &c_cursor, &c_last_match,
            &ranges, &range_count, &ranges_cap,
            &label_writes, &label_count, &label_cap,
            &c_view);
        if (err != E_OK)
            break;
    }

    result->staged_vm.cursor = c_cursor;
    result->staged_vm.last_match = c_last_match;
    result->staged_vm.view = c_view;
    result->ranges = ranges;
    result->range_count = range_count;
    result->label_writes = label_writes;
    result->label_count = label_count;
    result->err = err;

    return err;
}

enum Err execute_clause_with_scratch(const Clause* clause,
    void* io_ptr, VM* vm, FILE* out,
    Range* ranges, i32 ranges_cap,
    LabelWrite* label_writes, i32 label_cap)
{
    StagedResult result;
    enum Err err = execute_clause_stage_only(clause, io_ptr, vm,
        ranges, ranges_cap, label_writes, label_cap, &result);

    if (err == E_OK) {
        // Commit staged ranges to output
        for (i32 i = 0; i < result.range_count; i++) {
            if (result.ranges[i].kind == RANGE_FILE) {
                err = io_emit((File*)io_ptr, result.ranges[i].file.start, result.ranges[i].file.end, out);
            } else {
                // RANGE_LIT: write literal bytes
                if ((size_t)fwrite(result.ranges[i].lit.bytes, 1, (size_t)result.ranges[i].lit.len, out) != (size_t)result.ranges[i].lit.len)
                    err = E_IO;
            }
            if (err != E_OK)
                break;
        }
        if (err == E_OK) {
            // Commit staged VM state
            commit_labels(vm, result.label_writes, result.label_count);
            vm->cursor = result.staged_vm.cursor;
            vm->last_match = result.staged_vm.last_match;
            vm->view = result.staged_vm.view; // commit view only on clause success
        }
    }

    return err;
}

static enum Err execute_op(const Op* op, File* io, VM* vm,
    i64* c_cursor, Match* c_last_match,
    Range** ranges, i32* range_count, i32* range_cap,
    LabelWrite** label_writes, i32* label_count, i32* label_cap,
    const View* c_view)
{
    switch (op->kind) {
    case OP_FIND: {
        i64 win_lo, win_hi;

        enum Err err = resolve_loc_expr_cp(&op->u.find.to, io, vm, c_last_match, *c_cursor, *label_writes, *label_count, c_view, CLAMP_VIEW, &win_hi);
        if (err != E_OK)
            return err;

        win_lo = vclamp(c_view, io, *c_cursor);

        // Determine direction and adjust window
        enum Dir dir = DIR_FWD;
        if (win_hi < win_lo) {
            dir = DIR_BWD;
            i64 temp = win_lo;
            win_lo = win_hi;
            win_hi = temp;
        }

        i64 ms, me;
        err = io_find_window(io, win_lo, win_hi,
            (const unsigned char*)op->u.find.needle.bytes,
            op->u.find.needle.len, dir, &ms, &me);
        if (err != E_OK)
            return err;

        c_last_match->start = ms;
        c_last_match->end = me;
        c_last_match->valid = true;
        *c_cursor = ms;
        break;
    }

    case OP_FIND_RE: {
        i64 win_lo, win_hi;

        enum Err err = resolve_loc_expr_cp(&op->u.findr.to, io, vm, c_last_match, *c_cursor, *label_writes, *label_count, c_view, CLAMP_VIEW, &win_hi);
        if (err != E_OK)
            return err;

        win_lo = vclamp(c_view, io, *c_cursor);

        // Determine direction and adjust window
        enum Dir dir = DIR_FWD;
        if (win_hi < win_lo) {
            dir = DIR_BWD;
            i64 temp = win_lo;
            win_lo = win_hi;
            win_hi = temp;
        }

        i64 ms, me;
        err = io_findr_window(io, win_lo, win_hi, op->u.findr.prog, dir, &ms, &me);
        if (err != E_OK)
            return err;

        c_last_match->start = ms;
        c_last_match->end = me;
        c_last_match->valid = true;
        *c_cursor = ms;
        break;
    }

    case OP_SKIP: {
        if (op->u.skip.unit == UNIT_BYTES) {
            i64 cur = vclamp(c_view, io, *c_cursor);
            if (op->u.skip.n > (u64)INT64_MAX) {
                cur = veof(c_view, io);
            } else {
                apply_byte_saturation(&cur, (i64)op->u.skip.n, c_view, io, CLAMP_VIEW);
            }
            *c_cursor = cur;
        } else if (op->u.skip.unit == UNIT_LINES) {
            // Skip by lines
            i64 current_line_start;
            enum Err err = io_line_start(io, *c_cursor, &current_line_start);
            if (err != E_OK)
                return err;

            // Pin to view bounds
            if (current_line_start < vbof(c_view))
                current_line_start = vbof(c_view);

            if (op->u.skip.n > (u64)INT_MAX)
                return E_PARSE;
            err = io_step_lines_from(io, current_line_start,
                (i32)op->u.skip.n, c_cursor);
            if (err != E_OK)
                return err;
            *c_cursor = vclamp(c_view, io, *c_cursor);
        } else { // UNIT_CHARS
            if (op->u.skip.n > INT_MAX)
                return E_PARSE;
            i64 char_start;
            enum Err err = io_char_start(io, *c_cursor, &char_start);
            if (err != E_OK)
                return err;
            err = io_step_chars_from(io, char_start, (i32)op->u.skip.n, c_cursor);
            if (err != E_OK)
                return err;
            *c_cursor = vclamp(c_view, io, *c_cursor);
        }
        break;
    }

    case OP_TAKE_LEN: {
        i64 start, end;

        if (op->u.take_len.unit == UNIT_BYTES) {
            if (op->u.take_len.offset > 0) {
                start = vclamp(c_view, io, *c_cursor);
                end = start;
                apply_byte_saturation(&end, op->u.take_len.offset, c_view, io, CLAMP_VIEW);
            } else {
                end = vclamp(c_view, io, *c_cursor);
                start = end;
                apply_byte_saturation(&start, op->u.take_len.offset, c_view, io, CLAMP_VIEW);
                start = clamp64(start, vbof(c_view), end);
            }
        } else if (op->u.take_len.unit == UNIT_LINES) {
            // Take by lines
            i64 line_start;
            enum Err err = io_line_start(io, *c_cursor, &line_start);
            if (err != E_OK)
                return err;

            // Pin to view bounds
            if (line_start < vbof(c_view))
                line_start = vbof(c_view);

            if (op->u.take_len.offset > 0) {
                start = line_start;
                if (op->u.take_len.offset > INT_MAX)
                    return E_PARSE;
                err = io_step_lines_from(io, line_start,
                    (i32)op->u.take_len.offset, &end);
                if (err != E_OK)
                    return err;
                end = vclamp(c_view, io, end);
            } else {
                end = line_start;
                if (op->u.take_len.offset < -INT_MAX)
                    return E_PARSE;
                err = io_step_lines_from(io, line_start,
                    (i32)op->u.take_len.offset, &start);
                if (err != E_OK)
                    return err;
                start = clamp64(start, vbof(c_view), end);
            }
        } else { // UNIT_CHARS
            if (op->u.take_len.offset > INT_MAX || op->u.take_len.offset < -INT_MAX)
                return E_PARSE;
            i64 cstart;
            enum Err err = io_char_start(io, *c_cursor, &cstart);
            if (err != E_OK)
                return err;
            if (op->u.take_len.offset > 0) {
                start = cstart;
                err = io_step_chars_from(io, cstart, (i32)op->u.take_len.offset, &end);
                if (err != E_OK)
                    return err;
                end = vclamp(c_view, io, end);
            } else {
                end = cstart;
                i64 s;
                err = io_step_chars_from(io, cstart, (i32)op->u.take_len.offset, &s);
                if (err != E_OK)
                    return err;
                start = clamp64(s, vbof(c_view), end);
            }
        }

        // Stage the range
        if (*range_count >= *range_cap)
            return E_OOM;
        (*ranges)[*range_count].kind = RANGE_FILE;
        (*ranges)[*range_count].file.start = start;
        (*ranges)[*range_count].file.end = end;
        (*range_count)++;
        if (start != end) {
            *c_cursor = start > end ? start : end;
        }
        break;
    }

    case OP_TAKE_TO: {
        i64 target;
        enum Err err = resolve_loc_expr_cp(&op->u.take_to.to, io, vm, c_last_match, *c_cursor, *label_writes, *label_count, c_view, CLAMP_VIEW, &target);
        if (err != E_OK)
            return err;

        i64 start = vclamp(c_view, io, *c_cursor);
        i64 end = vclamp(c_view, io, target);

        // Order-normalized emit
        if (start > end) {
            i64 temp = start;
            start = end;
            end = temp;
        }

        // Stage the range
        if (*range_count >= *range_cap)
            return E_OOM;
        (*ranges)[*range_count].kind = RANGE_FILE;
        (*ranges)[*range_count].file.start = start;
        (*ranges)[*range_count].file.end = end;
        (*range_count)++;

        if (start != end) {
            *c_cursor = vclamp(c_view, io, end);
        }
        break;
    }

    case OP_TAKE_UNTIL: {
        // Search forward from cursor to view end
        i64 ms, me;
        enum Err err = io_find_window(io, vclamp(c_view, io, *c_cursor), veof(c_view, io),
            (const unsigned char*)op->u.take_until.needle.bytes,
            op->u.take_until.needle.len, DIR_FWD, &ms, &me);
        if (err != E_OK)
            return err;

        // Update staged match
        c_last_match->start = ms;
        c_last_match->end = me;
        c_last_match->valid = true;

        i64 target;
        if (op->u.take_until.has_at) {
            err = resolve_loc_expr_cp(&op->u.take_until.at, io, vm, c_last_match, ms,
                *label_writes, *label_count, c_view, CLAMP_VIEW, &target);
            if (err != E_OK)
                return err;
        } else {
            target = ms;
        }

        i64 dst = vclamp(c_view, io, target);

        // Stage [cursor, dst) ONLY (no order-normalization)
        if (*range_count >= *range_cap)
            return E_OOM;
        (*ranges)[*range_count].kind = RANGE_FILE;
        (*ranges)[*range_count].file.start = vclamp(c_view, io, *c_cursor);
        (*ranges)[*range_count].file.end = dst;
        (*range_count)++;

        // Move cursor only if non-empty
        if (dst > *c_cursor) {
            *c_cursor = dst;
        }
        break;
    }

    case OP_TAKE_UNTIL_RE: {
        // Search forward from cursor to view end using regex
        i64 ms, me;
        enum Err err = io_findr_window(io, vclamp(c_view, io, *c_cursor), veof(c_view, io),
            op->u.take_until_re.prog, DIR_FWD, &ms, &me);
        if (err != E_OK)
            return err;

        // Update staged match
        c_last_match->start = ms;
        c_last_match->end = me;
        c_last_match->valid = true;

        i64 target;
        if (op->u.take_until_re.has_at) {
            err = resolve_loc_expr_cp(&op->u.take_until_re.at, io, vm, c_last_match, ms,
                *label_writes, *label_count, c_view, CLAMP_VIEW, &target);
            if (err != E_OK)
                return err;
        } else {
            target = ms;
        }

        i64 dst = vclamp(c_view, io, target);

        // Stage [cursor, dst) ONLY (no order-normalization)
        if (*range_count >= *range_cap)
            return E_OOM;
        (*ranges)[*range_count].kind = RANGE_FILE;
        (*ranges)[*range_count].file.start = vclamp(c_view, io, *c_cursor);
        (*ranges)[*range_count].file.end = dst;
        (*range_count)++;

        // Move cursor only if non-empty
        if (dst > *c_cursor) {
            *c_cursor = dst;
        }
        break;
    }

    case OP_LABEL: {
        // Stage label write
        if (*label_count >= *label_cap)
            return E_OOM;
        (*label_writes)[*label_count].name_idx = op->u.label.name_idx;
        (*label_writes)[*label_count].pos = *c_cursor;
        (*label_count)++;
        break;
    }

    case OP_GOTO: {
        enum Err err = resolve_loc_expr_cp(&op->u.go.to, io, vm, c_last_match, *c_cursor, *label_writes, *label_count, c_view, CLAMP_NONE, c_cursor);
        if (err != E_OK)
            return err;

        // Check if target is outside view bounds
        if (c_view->active && (*c_cursor < c_view->lo || *c_cursor > c_view->hi)) {
            return E_LOC_RESOLVE;
        }

        *c_cursor = clamp64(*c_cursor, 0, io_size(io));
        break;
    }

    case OP_VIEWSET: {
        i64 a, b;
        enum Err err = resolve_loc_expr_cp(&op->u.viewset.a, io, vm, c_last_match, *c_cursor, *label_writes, *label_count, c_view, CLAMP_VIEW, &a);
        if (err != E_OK)
            return err;
        err = resolve_loc_expr_cp(&op->u.viewset.b, io, vm, c_last_match, *c_cursor, *label_writes, *label_count, c_view, CLAMP_VIEW, &b);
        if (err != E_OK)
            return err;

        i64 lo = a < b ? a : b;
        i64 hi = a < b ? b : a;

        // Stage view activation
        ((View*)c_view)->active = true;
        ((View*)c_view)->lo = lo;
        ((View*)c_view)->hi = hi;

        // Clamp cursor into new view
        *c_cursor = vclamp(c_view, io, *c_cursor);

        // Invalidate staged match if it straddles the view
        if (c_last_match->valid && (c_last_match->start < lo || c_last_match->end > hi)) {
            c_last_match->valid = false;
        }
        break;
    }

    case OP_VIEWCLEAR: {
        // Stage view deactivation
        ((View*)c_view)->active = false;
        ((View*)c_view)->lo = 0;
        ((View*)c_view)->hi = io_size(io);
        break;
    }

    case OP_PRINT: {
        // Stage literal range
        if (*range_count >= *range_cap)
            return E_OOM;
        Range* r = &(*ranges)[(*range_count)++];
        r->kind = RANGE_LIT;
        r->lit = op->u.print.string;
        break;
    }

    case OP_SLEEP: {
        sleep_msec(op->u.sleep.msec);
        break;
    }

    case OP_BOX: {
        i32 right_offset = op->u.box.right_offset;
        i32 down_offset = op->u.box.down_offset;

        // Start from cursor position
        i64 start_pos = *c_cursor;

        // Find current line start
        i64 current_line_start;
        enum Err err = io_line_start(io, start_pos, &current_line_start);
        if (err != E_OK) return err;

        // Calculate the offset of start_pos within its line
        i64 col_offset = start_pos - current_line_start;

        // Find the starting line for the box
        i64 box_start_line = current_line_start;
        if (down_offset < 0) {
            // Go up by |down_offset| lines
            err = io_step_lines_from(io, current_line_start, (i32)down_offset, &box_start_line);
            if (err != E_OK) return err;
        }

        // Output each line in the box
        i64 current_line = box_start_line;
        i64 last_output_pos = start_pos;
        i64 lines_to_process = (down_offset < 0 ? -down_offset : down_offset) + 1;

        for (i64 line_idx = 0; line_idx < lines_to_process; line_idx++) {
            // Check if we've reached the end of the file
            if (current_line >= io_size(io)) break;

            // Find line end
            i64 line_end;
            err = io_line_end(io, current_line, &line_end);
            if (err != E_OK) return err;

            // Calculate line bounds for this row based on the column offset
            i64 line_left = current_line + col_offset;
            i64 line_right = current_line + col_offset + right_offset + 1;

            // Ensure we have at least 1 byte
            if (line_right <= line_left) {
                line_right = line_left + 1;
            }

            // Clamp to actual line bounds
            if (line_left < current_line) line_left = current_line;
            if (line_left > line_end) line_left = line_end;
            if (line_right > line_end) line_right = line_end;
            if (line_right < line_left) line_right = line_left;

            // Stage this line segment
            if (*range_count >= *range_cap)
                return E_OOM;
            Range* r = &(*ranges)[(*range_count)++];
            r->kind = RANGE_FILE;
            r->file.start = line_left;
            r->file.end = line_right;

            // Track the last position we output
            if (line_right > last_output_pos) {
                last_output_pos = line_right;
            }

            // Add newline after each line segment
            if (*range_count >= *range_cap)
                return E_OOM;
            Range* nl = &(*ranges)[(*range_count)++];
            nl->kind = RANGE_LIT;
            nl->lit.bytes = "\n";
            nl->lit.len = 1;

            // Move to next line
            if (line_idx < lines_to_process - 1) {
                err = io_step_lines_from(io, current_line, 1, &current_line);
                if (err != E_OK) return err;
            }
        }

        // Move cursor to the last output position
        *c_cursor = last_output_pos;
        break;
    }

    default:
        return E_PARSE;
    }

    return E_OK;
}

static enum Err resolve_loc_expr_cp(
    const LocExpr* loc, File* io, const VM* vm,
    const Match* staged_match, i64 staged_cursor,
    const LabelWrite* staged_labels, i32 staged_label_count,
    const View* c_view, ClampPolicy clamp, i64* out)
{
    i64 base = 0;

    switch (loc->base) {
    case LOC_CURSOR:
        base = staged_cursor;
        break;
    case LOC_BOF:
        base = vbof(c_view);
        break;
    case LOC_EOF:
        base = veof(c_view, io);
        break;
    case LOC_NAME: {
        // Check staged labels first (overrides committed)
        bool found = false;
        for (i32 i = 0; i < staged_label_count; i++) {
            if (staged_labels[i].name_idx == loc->name_idx) {
                base = staged_labels[i].pos;
                found = true;
                break;
            }
        }
        // If not found in staged, check committed labels
        if (!found) {
            i64 committed_pos = vm->label_pos[loc->name_idx];
            if (committed_pos >= 0) {
                base = committed_pos;
                found = true;
            }
        }
        if (!found)
            return E_LOC_RESOLVE;
        break;
    }
    case LOC_MATCH_START:
        if (!staged_match->valid)
            return E_LOC_RESOLVE;
        base = staged_match->start;
        break;
    case LOC_MATCH_END:
        if (!staged_match->valid)
            return E_LOC_RESOLVE;
        base = staged_match->end;
        break;
    case LOC_LINE_START: {
        i64 anchor = staged_cursor;
        enum Err e = io_line_start(io, anchor, &base);
        if (e != E_OK)
            return e;
        if (base < vbof(c_view))
            base = vbof(c_view);
        break;
    }
    case LOC_LINE_END: {
        i64 anchor = staged_cursor;
        enum Err e = io_line_end(io, anchor, &base);
        if (e != E_OK)
            return e;
        if (base > veof(c_view, io))
            base = veof(c_view, io);
        break;
    }
    default:
        return E_PARSE;
    }

    if (loc->offset != 0) {
        if (loc->unit == UNIT_BYTES) {
            apply_byte_saturation(&base, loc->offset, c_view, io, clamp == CLAMP_FILE ? CLAMP_FILE : clamp);
        } else if (loc->unit == UNIT_LINES) {
            if (loc->offset > INT_MAX || loc->offset < -INT_MAX)
                return E_PARSE;
            i32 d = (i32)loc->offset;
            enum Err e = io_step_lines_from(io, base, d, &base);
            if (e != E_OK)
                return e;
        } else { // UNIT_CHARS
            if (loc->offset > INT_MAX || loc->offset < -INT_MAX)
                return E_PARSE;
            i64 cs;
            enum Err e = io_char_start(io, base, &cs);
            if (e != E_OK)
                return e;
            i32 d = (i32)loc->offset;
            e = io_step_chars_from(io, cs, d, &cs);
            if (e != E_OK)
                return e;
            base = cs;
        }
    }

    if (clamp == CLAMP_VIEW)
        *out = vclamp(c_view, io, base);
    else if (clamp == CLAMP_FILE)
        *out = clamp64(base, 0, io_size(io));
    else
        *out = base;
    return E_OK;
}

void commit_labels(VM* vm, const LabelWrite* label_writes, i32 label_count)
{
    for (i32 i = 0; i < label_count; i++) {
        vm->label_pos[label_writes[i].name_idx] = label_writes[i].pos;
    }
}

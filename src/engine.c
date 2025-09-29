// engine.c
#include "arena.h"
#include "fiskta.h"
#include "iosearch.h"
#include <alloca.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

// Forward declarations
// LabelWrite typedef moved to fiskta.h
enum Err io_open(File* io, const char* path,
    unsigned char* search_buf, size_t search_buf_cap);

// Arena alignment helper with overflow protection
static size_t safe_align(size_t x, size_t align)
{
    size_t aligned = a_align(x, align);
    if (aligned < x) { // overflow check
        return SIZE_MAX; // signal overflow
    }
    return aligned;
}

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

// Count capacity needs for a clause
void clause_caps(const Clause* c, i32* out_ranges_cap, i32* out_labels_cap)
{
    i32 rc = 0, lc = 0;
    for (i32 i = 0; i < c->op_count; i++) {
        switch (c->ops[i].kind) {
        case OP_TAKE_LEN:
        case OP_TAKE_TO:
        case OP_TAKE_UNTIL:
            rc++;
            break;
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

static enum Err execute_op(const Op* op, const Program* prg, File* io, VM* vm,
    i64* c_cursor, Match* c_last_match,
    Range** ranges, i32* range_count, i32* range_cap,
    LabelWrite** label_writes, i32* label_count, i32* label_cap,
    const View* c_view);
static enum Err resolve_loc_expr(const LocExpr* loc, const Program* prg, File* io,
    const VM* vm, const Match* staged_match, i64 staged_cursor,
    const LabelWrite* staged_labels, i32 staged_label_count, i64* out);
static enum Err resolve_at_expr(const AtExpr* at, File* io, const Match* match, i64* out);
static enum Err resolve_loc_expr_cp(
    const LocExpr* loc, const Program* prg, File* io, const VM* vm,
    const Match* staged_match, i64 staged_cursor,
    const LabelWrite* staged_labels, i32 staged_label_count,
    const View* c_view, ClampPolicy clamp, i64* out);
static enum Err resolve_at_expr_cp(
    const AtExpr* at, File* io, const Match* match,
    const View* c_view, ClampPolicy clamp, i64* out);
static void commit_labels(VM* vm, const Program* prg, const LabelWrite* label_writes, i32 label_count);

enum Err engine_run(const Program* prg, const char* in_path, FILE* out)
{
    // Calculate memory needs for arena allocation
    i32 cc = prg->clause_count;

    // Calculate scratch buffer sizes for each clause
    i32* r_caps = alloca(sizeof(i32) * cc);
    i32* l_caps = alloca(sizeof(i32) * cc);
    size_t total_ranges = 0;
    size_t total_labels = 0;

    for (i32 i = 0; i < cc; i++) {
        clause_caps(&prg->clauses[i], &r_caps[i], &l_caps[i]);
        total_ranges += (size_t)r_caps[i];
        total_labels += (size_t)l_caps[i];
    }

    // We'll allocate per-clause capture/label arrays on the stack during the loop.

    // Calculate total memory needed
    const size_t search_buf_cap = (FW_WIN > (BK_BLK + OVERLAP_MAX)) ? (size_t)FW_WIN : (size_t)(BK_BLK + OVERLAP_MAX);
    int max_nins = 0;
    for (i32 ci = 0; ci < prg->clause_count; ++ci) {
        const Clause* C = &prg->clauses[ci];
        for (i32 oi = 0; oi < C->op_count; ++oi) {
            const Op* op = &C->ops[oi];
            if (op->kind == OP_FINDR && op->u.findr.prog) {
                if (op->u.findr.prog->nins > max_nins)
                    max_nins = op->u.findr.prog->nins;
            }
        }
    }
    int re_threads_cap = max_nins > 0 ? 4 * max_nins : 32;
    if (re_threads_cap < 32)
        re_threads_cap = 32;
    const size_t re_thr_bytes = (size_t)re_threads_cap * sizeof(ReThread);
    const size_t re_seen_bytes_each = (size_t)(max_nins > 0 ? max_nins : 32);

    // Account for alignment padding between slices
    size_t total = safe_align(search_buf_cap, 1);
    if (total == SIZE_MAX)
        return E_OOM;

    size_t re_thr_size = safe_align(re_thr_bytes, alignof(ReThread)) * 2;
    if (re_thr_size == SIZE_MAX)
        return E_OOM;
    total += re_thr_size;

    size_t re_seen_size = safe_align(re_seen_bytes_each, 1) * 2;
    if (re_seen_size == SIZE_MAX)
        return E_OOM;
    total += re_seen_size;

    total += 64; // small cushion like main.c

    // Allocate single block
    void* block = malloc(total);
    if (!block)
        return E_OOM;

    // Initialize arena
    Arena arena;
    arena_init(&arena, block, total);

    // Carve out slices
    unsigned char* search_buf = (unsigned char*)arena_alloc(&arena, search_buf_cap, 1);
    ReThread* re_curr_thr = (ReThread*)arena_alloc(&arena, re_thr_bytes, alignof(ReThread));
    ReThread* re_next_thr = (ReThread*)arena_alloc(&arena, re_thr_bytes, alignof(ReThread));
    unsigned char* seen_curr = (unsigned char*)arena_alloc(&arena, re_seen_bytes_each, 1);
    unsigned char* seen_next = (unsigned char*)arena_alloc(&arena, re_seen_bytes_each, 1);

    if (!search_buf || !re_curr_thr || !re_next_thr || !seen_curr || !seen_next) {
        free(block);
        return E_OOM;
    }

    // Open I/O with arena-backed buffers
    File io = { 0 };
    enum Err err = io_open(&io, in_path, search_buf, search_buf_cap);
    if (err != E_OK) {
        free(block);
        return err;
    }
    io_set_regex_scratch(&io, re_curr_thr, re_next_thr, re_threads_cap,
        seen_curr, seen_next, (size_t)re_seen_bytes_each);

    VM vm = { 0 };
    vm.cursor = 0;
    vm.last_match.valid = false;
    // Initialize label arrays
    memset(vm.label_set, 0, sizeof(vm.label_set));

    // Execute each clause independently
    i32 successful_clauses = 0;
    enum Err last_err = E_OK;

    for (i32 i = 0; i < cc; i++) {
        i32 rc = 0, lc = 0;
        clause_caps(&prg->clauses[i], &rc, &lc);
        Range* r_tmp = rc ? alloca((size_t)rc * sizeof *r_tmp) : NULL;
        LabelWrite* lw_tmp = lc ? alloca((size_t)lc * sizeof *lw_tmp) : NULL;
        err = execute_clause_with_scratch(&prg->clauses[i], prg, &io, &vm, out,
            r_tmp, rc, lw_tmp, lc);
        if (err == E_OK) {
            successful_clauses++;
        } else {
            last_err = err;
        }
    }

    // Return error only if all clauses failed
    if (successful_clauses == 0) {
        err = last_err;
    }

    io_close(&io);
    free(block);
    return err;
}

// NEW signature: no allocations inside
enum Err execute_clause_with_scratch(const Clause* clause, const Program* prg,
    void* io_ptr, VM* vm, FILE* out,
    Range* ranges, i32 ranges_cap,
    LabelWrite* label_writes, i32 label_cap)
{
    File* io = (File*)io_ptr;
    i64 c_cursor = vm->cursor;
    Match c_last_match = vm->last_match;
    View c_view = vm->view; // staged view for this clause

    i32 range_count = 0;
    i32 label_count = 0;

    enum Err err = E_OK;
    for (i32 i = 0; i < clause->op_count; i++) {
        err = execute_op(&clause->ops[i], prg, io, vm,
            &c_cursor, &c_last_match,
            &ranges, &range_count, &ranges_cap,
            &label_writes, &label_count, &label_cap,
            &c_view);
        if (err != E_OK)
            break;
    }

    if (err == E_OK) {
        for (i32 i = 0; i < range_count; i++) {
            err = io_emit(io, ranges[i].start, ranges[i].end, out);
            if (err != E_OK)
                break;
        }
        if (err == E_OK) {
            commit_labels(vm, prg, label_writes, label_count);
            vm->cursor = c_cursor;
            vm->last_match = c_last_match;
            vm->view = c_view; // commit view only on clause success
        }
    }

    return err;
}

static enum Err execute_op(const Op* op, const Program* prg, File* io, VM* vm,
    i64* c_cursor, Match* c_last_match,
    Range** ranges, i32* range_count, i32* range_cap,
    LabelWrite** label_writes, i32* label_count, i32* label_cap,
    const View* c_view)
{
    switch (op->kind) {
    case OP_FIND: {
        i64 win_lo, win_hi;

        enum Err err = resolve_loc_expr_cp(&op->u.find.to, prg, io, vm, c_last_match, *c_cursor, *label_writes, *label_count, c_view, CLAMP_VIEW, &win_hi);
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
            (const unsigned char*)op->u.find.needle,
            strlen(op->u.find.needle), dir, &ms, &me);
        if (err != E_OK)
            return err;

        c_last_match->start = ms;
        c_last_match->end = me;
        c_last_match->valid = true;
        *c_cursor = ms;
        break;
    }

    case OP_FINDR: {
        i64 win_lo, win_hi;

        enum Err err = resolve_loc_expr_cp(&op->u.findr.to, prg, io, vm, c_last_match, *c_cursor, *label_writes, *label_count, c_view, CLAMP_VIEW, &win_hi);
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
            if (op->u.take_len.sign > 0) {
                start = vclamp(c_view, io, *c_cursor);
                end = start;
                if (op->u.take_len.n > (u64)INT64_MAX)
                    end = veof(c_view, io);
                else
                    apply_byte_saturation(&end, (i64)op->u.take_len.n, c_view, io, CLAMP_VIEW);
            } else {
                end = vclamp(c_view, io, *c_cursor);
                start = end;
                if (op->u.take_len.n > (u64)INT64_MAX)
                    start = vbof(c_view);
                else
                    apply_byte_saturation(&start, -(i64)op->u.take_len.n, c_view, io, CLAMP_VIEW);
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

            if (op->u.take_len.sign > 0) {
                start = line_start;
                if (op->u.take_len.n > (u64)INT_MAX)
                    return E_PARSE;
                err = io_step_lines_from(io, line_start,
                    (i32)op->u.take_len.n, &end);
                if (err != E_OK)
                    return err;
                end = vclamp(c_view, io, end);
            } else {
                end = line_start;
                if (op->u.take_len.n > (u64)INT_MAX)
                    return E_PARSE;
                err = io_step_lines_from(io, line_start,
                    -(i32)op->u.take_len.n, &start);
                if (err != E_OK)
                    return err;
                start = clamp64(start, vbof(c_view), end);
            }
        } else { // UNIT_CHARS
            if (op->u.take_len.n > INT_MAX)
                return E_PARSE;
            i64 cstart;
            enum Err err = io_char_start(io, *c_cursor, &cstart);
            if (err != E_OK)
                return err;
            if (op->u.take_len.sign > 0) {
                start = cstart;
                err = io_step_chars_from(io, cstart, (i32)op->u.take_len.n, &end);
                if (err != E_OK)
                    return err;
                end = vclamp(c_view, io, end);
            } else {
                end = cstart;
                i64 s;
                err = io_step_chars_from(io, cstart, -(i32)op->u.take_len.n, &s);
                if (err != E_OK)
                    return err;
                start = clamp64(s, vbof(c_view), end);
            }
        }

        // Stage the range
        if (*range_count >= *range_cap)
            return E_OOM;
        (*ranges)[*range_count].start = start;
        (*ranges)[*range_count].end = end;
        (*range_count)++;

        // Update cursor (Cursor Law)
        if (start != end) {
            *c_cursor = start > end ? start : end;
        }
        break;
    }

    case OP_TAKE_TO: {
        i64 target;
        enum Err err = resolve_loc_expr_cp(&op->u.take_to.to, prg, io, vm, c_last_match, *c_cursor, *label_writes, *label_count, c_view, CLAMP_VIEW, &target);
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
        (*ranges)[*range_count].start = start;
        (*ranges)[*range_count].end = end;
        (*range_count)++;

        *c_cursor = vclamp(c_view, io, target);
        break;
    }

    case OP_TAKE_UNTIL: {
        // Search forward from cursor to view end
        i64 ms, me;
        enum Err err = io_find_window(io, vclamp(c_view, io, *c_cursor), veof(c_view, io),
            (const unsigned char*)op->u.take_until.needle,
            strlen(op->u.take_until.needle), DIR_FWD, &ms, &me);
        if (err != E_OK)
            return err;

        // Update staged match
        c_last_match->start = ms;
        c_last_match->end = me;
        c_last_match->valid = true;

        // Resolve at-expr
        i64 target;
        if (op->u.take_until.has_at) {
            err = resolve_at_expr_cp(&op->u.take_until.at, io, c_last_match, c_view, CLAMP_VIEW, &target);
            if (err != E_OK)
                return err;
        } else {
            target = ms; // Default to match-start
        }

        i64 dst = vclamp(c_view, io, target);

        // Stage [cursor, dst) ONLY (no order-normalization)
        if (*range_count >= *range_cap)
            return E_OOM;
        (*ranges)[*range_count].start = vclamp(c_view, io, *c_cursor);
        (*ranges)[*range_count].end = dst;
        (*range_count)++;

        // Cursor law: move only if non-empty
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
        enum Err err = resolve_loc_expr_cp(&op->u.go.to, prg, io, vm, c_last_match, *c_cursor, *label_writes, *label_count, c_view, CLAMP_NONE, c_cursor);
        if (err != E_OK)
            return err;

        // Check if target is outside view bounds
        if (c_view->active && (*c_cursor < c_view->lo || *c_cursor >= c_view->hi)) {
            return E_LOC_RESOLVE;
        }

        *c_cursor = clamp64(*c_cursor, 0, io_size(io));
        break;
    }

    case OP_VIEWSET: {
        i64 a, b;
        enum Err err = resolve_loc_expr_cp(&op->u.viewset.a, prg, io, vm, c_last_match, *c_cursor, *label_writes, *label_count, c_view, CLAMP_VIEW, &a);
        if (err != E_OK)
            return err;
        err = resolve_loc_expr_cp(&op->u.viewset.b, prg, io, vm, c_last_match, *c_cursor, *label_writes, *label_count, c_view, CLAMP_VIEW, &b);
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
        // Cursor unchanged
        break;
    }

    default:
        return E_PARSE;
    }

    return E_OK;
}

static enum Err resolve_loc_expr(const LocExpr* loc, const Program* prg, File* io,
    const VM* vm, const Match* staged_match, i64 staged_cursor,
    const LabelWrite* staged_labels, i32 staged_label_count, i64* out)
{
    i64 base;

    switch (loc->base) {
    case LOC_CURSOR:
        base = staged_cursor;
        break;
    case LOC_BOF:
        base = 0;
        break;
    case LOC_EOF:
        base = io_size(io);
        break;
    case LOC_NAME: {
        // Look up label in staged labels first (staged labels override committed ones)
        bool found = false;
        for (i32 i = 0; i < staged_label_count; i++) {
            if (staged_labels[i].name_idx == loc->name_idx) {
                base = staged_labels[i].pos;
                found = true;
                break;
            }
        }

        // If not found in staged labels, check committed labels
        if (!found) {
            if (vm->label_set[loc->name_idx]) {
                base = vm->label_pos[loc->name_idx];
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
        // relative to cursor
        enum Err err = io_line_start(io, staged_cursor, &base);
        if (err != E_OK)
            return err;
        break;
    }
    case LOC_LINE_END: {
        // io_line_end expects a byte inside the line
        i64 ref = staged_cursor;
        enum Err err = io_line_end(io, ref, &base);
        if (err != E_OK)
            return err;
        break;
    }
    default:
        return E_PARSE;
    }

    // Apply offset if present
    if (loc->n != 0) {
        if (loc->unit == UNIT_BYTES) {
            // clamp u64 -> i64 delta safely
            if (loc->n > (u64)INT64_MAX) {
                // saturate at extremes
                base = (loc->sign > 0) ? io_size(io) : 0;
            } else {
                i64 delta = loc->sign > 0 ? (i64)loc->n : -(i64)loc->n;
                base += delta;
            }
        } else if (loc->unit == UNIT_LINES) {
            if (loc->n > (u64)INT_MAX)
                return E_PARSE;
            i32 delta = loc->sign > 0 ? (i32)loc->n : -(i32)loc->n;
            enum Err err = io_step_lines_from(io, base, delta, &base);
            if (err != E_OK)
                return err;
        } else { // UNIT_CHARS
            if (loc->n > (u64)INT_MAX)
                return E_PARSE;
            i64 char_base;
            enum Err err = io_char_start(io, base, &char_base);
            if (err != E_OK)
                return err;
            i32 delta = loc->sign > 0 ? (i32)loc->n : -(i32)loc->n;
            err = io_step_chars_from(io, char_base, delta, &char_base);
            if (err != E_OK)
                return err;
            base = char_base;
        }
    }

    *out = clamp64(base, 0, io_size(io));
    return E_OK;
}

static enum Err resolve_at_expr(const AtExpr* at, File* io, const Match* match, i64* out)
{
    i64 base;

    switch (at->at) {
    case LOC_MATCH_START:
        base = match->start;
        break;
    case LOC_MATCH_END:
        base = match->end;
        break;
    case LOC_LINE_START: {
        enum Err err = io_line_start(io, match->start, &base);
        if (err != E_OK)
            return err;
        break;
    }
    case LOC_LINE_END: {
        i64 ref = match->end;
        enum Err err = io_line_end(io, ref, &base);
        if (err != E_OK)
            return err;
        break;
    }
    default:
        return E_PARSE;
    }

    // Apply offset if present
    if (at->n != 0) {
        if (at->unit == UNIT_BYTES) {
            if (at->n > (u64)INT64_MAX) {
                base = (at->sign > 0) ? io_size(io) : 0;
            } else {
                i64 delta = at->sign > 0 ? (i64)at->n : -(i64)at->n;
                base += delta;
            }
        } else if (at->unit == UNIT_LINES) {
            if (at->n > (u64)INT_MAX)
                return E_PARSE;
            i32 delta = at->sign > 0 ? (i32)at->n : -(i32)at->n;
            enum Err err = io_step_lines_from(io, base, delta, &base);
            if (err != E_OK)
                return err;
        } else { // UNIT_CHARS
            if (at->n > (u64)INT_MAX)
                return E_PARSE;
            i64 char_base;
            enum Err err = io_char_start(io, base, &char_base);
            if (err != E_OK)
                return err;
            i32 delta = at->sign > 0 ? (i32)at->n : -(i32)at->n;
            err = io_step_chars_from(io, char_base, delta, &char_base);
            if (err != E_OK)
                return err;
            base = char_base;
        }
    }

    *out = clamp64(base, 0, io_size(io));
    return E_OK;
}

static enum Err resolve_loc_expr_cp(
    const LocExpr* loc, const Program* prg, File* io, const VM* vm,
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
            if (vm->label_set[loc->name_idx]) {
                base = vm->label_pos[loc->name_idx];
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
        enum Err e = io_line_start(io, staged_cursor, &base);
        if (e != E_OK)
            return e;
        if (base < vbof(c_view))
            base = vbof(c_view);
        break;
    }
    case LOC_LINE_END: {
        enum Err e = io_line_end(io, staged_cursor, &base);
        if (e != E_OK)
            return e;
        if (base > veof(c_view, io))
            base = veof(c_view, io);
        break;
    }
    default:
        return E_PARSE;
    }

    if (loc->n != 0) {
        if (loc->unit == UNIT_BYTES) {
            if (loc->n > (u64)INT64_MAX) {
                base = (clamp == CLAMP_VIEW)
                    ? (loc->sign > 0 ? veof(c_view, io) : vbof(c_view))
                    : (loc->sign > 0 ? io_size(io) : 0);
            } else {
                i64 delta = loc->sign > 0 ? (i64)loc->n : -(i64)loc->n;
                apply_byte_saturation(&base, delta, c_view, io, clamp == CLAMP_FILE ? CLAMP_FILE : clamp);
            }
        } else if (loc->unit == UNIT_LINES) {
            if (loc->n > (u64)INT_MAX)
                return E_PARSE;
            i32 d = loc->sign > 0 ? (i32)loc->n : -(i32)loc->n;
            enum Err e = io_step_lines_from(io, base, d, &base);
            if (e != E_OK)
                return e;
        } else { // UNIT_CHARS
            if (loc->n > (u64)INT_MAX)
                return E_PARSE;
            i64 cs;
            enum Err e = io_char_start(io, base, &cs);
            if (e != E_OK)
                return e;
            i32 d = loc->sign > 0 ? (i32)loc->n : -(i32)loc->n;
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

static enum Err resolve_at_expr_cp(
    const AtExpr* at, File* io, const Match* match,
    const View* c_view, ClampPolicy clamp, i64* out)
{
    i64 base = 0;
    switch (at->at) {
    case LOC_MATCH_START:
        base = match->start;
        break;
    case LOC_MATCH_END:
        base = match->end;
        break;
    case LOC_LINE_START: {
        enum Err e = io_line_start(io, match->start, &base);
        if (e != E_OK)
            return e;
        if (base < vbof(c_view))
            base = vbof(c_view);
        break;
    }
    case LOC_LINE_END: {
        enum Err e = io_line_end(io, match->end, &base);
        if (e != E_OK)
            return e;
        if (base > veof(c_view, io))
            base = veof(c_view, io);
        break;
    }
    default:
        return E_PARSE;
    }

    if (at->n != 0) {
        if (at->unit == UNIT_BYTES) {
            if (at->n > (u64)INT64_MAX) {
                base = (clamp == CLAMP_VIEW)
                    ? (at->sign > 0 ? veof(c_view, io) : vbof(c_view))
                    : (at->sign > 0 ? io_size(io) : 0);
            } else {
                i64 delta = at->sign > 0 ? (i64)at->n : -(i64)at->n;
                apply_byte_saturation(&base, delta, c_view, io, clamp == CLAMP_FILE ? CLAMP_FILE : clamp);
            }
        } else if (at->unit == UNIT_LINES) {
            if (at->n > (u64)INT_MAX)
                return E_PARSE;
            i32 d = at->sign > 0 ? (i32)at->n : -(i32)at->n;
            enum Err e = io_step_lines_from(io, base, d, &base);
            if (e != E_OK)
                return e;
        } else { // UNIT_CHARS
            if (at->n > (u64)INT_MAX)
                return E_PARSE;
            i64 cs;
            enum Err e = io_char_start(io, base, &cs);
            if (e != E_OK)
                return e;
            i32 d = at->sign > 0 ? (i32)at->n : -(i32)at->n;
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

static void commit_labels(VM* vm, const Program* prg, const LabelWrite* label_writes, i32 label_count)
{
    for (i32 i = 0; i < label_count; i++) {
        i32 name_idx = label_writes[i].name_idx;
        i64 pos = label_writes[i].pos;

        // Direct assignment - no eviction needed
        vm->label_pos[name_idx] = pos;
        vm->label_set[name_idx] = 1;
    }
}

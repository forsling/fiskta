#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "engine.h"
#include "error.h"
#include "fiskta.h"
#include "iosearch.h"
#include "util.h"
#include <errno.h>
#include <limits.h>
#include <stdalign.h>
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

// View helpers
static inline i64 view_bof(const View* v) { return (v && v->active) ? v->lo : 0; }
static inline i64 view_eof(const View* v, const File* io) { return (v && v->active) ? v->hi : io_size(io); }
static inline i64 view_clamp(const View* v, const File* io, i64 x) { return clamp64(x, view_bof(v), view_eof(v, io)); }

// Apply delta with clamping to prevent overflow past clamp edges
static inline void apply_delta_with_clamp(i64* base, i64 delta, const View* v, const File* io, ClampPolicy cp)
{
    i64 lo = (cp == CLAMP_VIEW) ? view_bof(v) : 0;
    i64 hi = (cp == CLAMP_VIEW) ? view_eof(v, io) : io_size(io);
    i64 tgt;
    if ((delta > 0) && (*base > INT64_MAX - delta)) {
        tgt = INT64_MAX;
    } else if ((delta < 0) && (*base < INT64_MIN - delta)) {
        tgt = INT64_MIN;
    } else {
        tgt = *base + delta;
    }
    if (tgt < lo)
        tgt = lo;
    if (tgt > hi)
        tgt = hi;
    *base = tgt;
}

void clause_caps(const Clause* c, i32* out_ranges_cap, i32* out_labels_cap, i32* out_inline_cap)
{
    i32 rc = 0;
    i32 lc = 0;
    i32 ic = 0;
    for (i32 i = 0; i < c->op_count; i++) {
        switch (c->ops[i].kind) {
        case OP_TAKE_LEN:
        case OP_TAKE_TO:
        case OP_TAKE_UNTIL:
        case OP_TAKE_UNTIL_RE:
        case OP_TAKE_UNTIL_BIN:
            rc++;
            break;
        case OP_PRINT: {
            const Op* op = &c->ops[i];
            rc += op->u.print.literal_segments + op->u.print.cursor_marks;
            ic += op->u.print.cursor_marks;
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
    if (out_inline_cap) {
        *out_inline_cap = ic;
    }
}

static enum Err execute_op(const Op* op, File* io, VM* vm,
    i64* c_cursor, Match* c_last_match,
    Range** ranges, i32* range_count, const i32* range_cap,
    LabelWrite** label_writes, i32* label_count, const i32* label_cap,
    View* c_view,
    char** inline_ptr, char* inline_end);
static enum Err resolve_location(
    const LocExpr* loc, File* io, const VM* vm,
    const Match* staged_match, i64 staged_cursor,
    const LabelWrite* staged_labels, i32 staged_label_count,
    const View* c_view, ClampPolicy clamp, i64* out);

static enum Err stage_file_range(Range* ranges, i32* range_count, i32 range_cap, i64 start, i64 end)
{
    if (*range_count >= range_cap) {
        return E_CAPACITY;
    }
    ranges[*range_count].kind = RANGE_FILE;
    ranges[*range_count].file.start = start;
    ranges[*range_count].file.end = end;
    (*range_count)++;
    return E_OK;
}

static enum Err stage_lit_range(Range* ranges, i32* range_count, i32 range_cap, String lit)
{
    if (*range_count >= range_cap) {
        return E_CAPACITY;
    }
    ranges[*range_count].kind = RANGE_LIT;
    ranges[*range_count].lit = lit;
    (*range_count)++;
    return E_OK;
}

static enum Err print_literal_op(
    const Op* op,
    const File* io,
    const View* c_view,
    i64 cursor,
    Range* ranges,
    i32* range_count,
    i32 range_cap,
    char** inline_ptr,
    char* inline_end)
{
    const char* bytes = op->u.print.string.bytes;
    i32 len = op->u.print.string.len;
    i64 clamped = view_clamp(c_view, io, cursor);
    i32 start = 0;

    for (i32 i = 0; i <= len; ++i) {
        bool is_sentinel = (i < len && bytes[i] == PRINT_CURSOR_SENTINEL);
        if (is_sentinel || i == len) {
            if (i > start) {
                String seg = { bytes + start, i - start };
                enum Err err = stage_lit_range(ranges, range_count, range_cap, seg);
                if (err != E_OK) {
                    return err;
                }
            }
            if (is_sentinel) {
                if (!inline_ptr || !*inline_ptr || !inline_end || *inline_ptr + INLINE_LIT_CAP > inline_end) {
                    return E_CAPACITY;
                }
                char* slot = *inline_ptr;
                int written = snprintf(slot, INLINE_LIT_CAP, "%lld", (long long)clamped);
                if (written < 0) {
                    return E_IO;
                }
                if (written >= INLINE_LIT_CAP) {
                    written = INLINE_LIT_CAP - 1;
                    slot[written] = '\0';
                }
                String dyn = { slot, written };
                enum Err err = stage_lit_range(ranges, range_count, range_cap, dyn);
                if (err != E_OK) {
                    return err;
                }
                *inline_ptr += INLINE_LIT_CAP;
            }
            start = i + 1;
        }
    }

    return E_OK;
}

static enum Err fail_with_message_op(const Op* op)
{
    // Write message to stderr immediately (not staged)
    if (op->u.fail.message.len > 0) {
        fwrite(op->u.fail.message.bytes, 1, (size_t)op->u.fail.message.len, stderr);
    }
    // Always fail the clause
    return E_FAIL_OP;
}

static enum Err label_op(
    const Op* op,
    const i64* c_cursor,
    LabelWrite* label_writes,
    i32* label_count,
    i32 label_cap)
{
    // Stage label write
    if (*label_count >= label_cap) {
        return E_CAPACITY;
    }
    label_writes[*label_count].name_idx = op->u.label.name_idx;
    label_writes[*label_count].pos = *c_cursor;
    (*label_count)++;
    return E_OK;
}

static enum Err view_clear_op(
    File* io,
    View* c_view)
{
    if (!c_view) {
        return E_OK;
    }

    c_view->active = false;
    c_view->lo = 0;
    c_view->hi = io_size(io);
    return E_OK;
}

static enum Err find_bytes_op(
    File* io,
    const Op* op,
    VM* vm,
    i64* c_cursor,
    Match* c_last_match,
    LabelWrite* label_writes,
    i32 label_count,
    const View* c_view)
{

    i64 win_lo;
    i64 win_hi;

    // Handle both OP_FIND and OP_FIND_BIN (same structure, different union field names)
    const LocExpr* to_loc = (op->kind == OP_FIND_BIN) ? &op->u.findbin.to : &op->u.find.to;
    const String* needle = (op->kind == OP_FIND_BIN) ? &op->u.findbin.needle : &op->u.find.needle;

    enum Err err = resolve_location(to_loc, io, vm, c_last_match, *c_cursor, label_writes, label_count, c_view, CLAMP_VIEW, &win_hi);
    if (err != E_OK) {
        return err;
    }

    win_lo = view_clamp(c_view, io, *c_cursor);

    // Determine direction and adjust window
    enum Dir dir = DIR_FWD;
    if (win_hi < win_lo) {
        dir = DIR_BWD;
        i64 temp = win_lo;
        win_lo = win_hi;
        win_hi = temp;
    }

    i64 ms;
    i64 me;
    err = io_find_window(io, win_lo, win_hi,
        (const unsigned char*)needle->bytes,
        (size_t)needle->len, dir, &ms, &me);
    if (err != E_OK) {
        return err;
    }

    c_last_match->start = ms;
    c_last_match->end = me;
    c_last_match->valid = true;
    *c_cursor = ms;
    return E_OK;
}

static enum Err find_regex_op(
    File* io,
    const Op* op,
    VM* vm,
    i64* c_cursor,
    Match* c_last_match,
    LabelWrite* label_writes,
    i32 label_count,
    const View* c_view)
{
    i64 win_lo;
    i64 win_hi;

    enum Err err = resolve_location(&op->u.findr.to, io, vm, c_last_match, *c_cursor, label_writes, label_count, c_view, CLAMP_VIEW, &win_hi);
    if (err != E_OK) {
        return err;
    }

    win_lo = view_clamp(c_view, io, *c_cursor);

    // Determine direction and adjust window
    enum Dir dir = DIR_FWD;
    if (win_hi < win_lo) {
        dir = DIR_BWD;
        i64 temp = win_lo;
        win_lo = win_hi;
        win_hi = temp;
    }

    i64 ms;
    i64 me;
    err = io_find_regex_window(io, win_lo, win_hi, op->u.findr.prog, dir, &ms, &me);
    if (err != E_OK) {
        return err;
    }

    c_last_match->start = ms;
    c_last_match->end = me;
    c_last_match->valid = true;
    *c_cursor = ms;
    return E_OK;
}

static enum Err skip_op(
    File* io,
    const Op* op,
    VM* vm,
    i64* c_cursor,
    Match* c_last_match,
    LabelWrite* label_writes,
    i32 label_count,
    const View* c_view)
{
    if (op->u.skip.is_location) {
        // skip to <location>
        enum Err err = resolve_location(&op->u.skip.to_location.to, io, vm, c_last_match, *c_cursor, label_writes, label_count, c_view, CLAMP_NONE, c_cursor);
        if (err != E_OK) {
            return err;
        }

        // Check if target is outside view bounds
        // Note: cursor positions range [lo, hi] while view data is [lo, hi)
        // Cursor at hi is valid (points after last byte, like EOF)
        if (c_view->active && (*c_cursor < c_view->lo || *c_cursor > c_view->hi)) {
            error_detail_set(E_LOC_RESOLVE, -1, "skip to: target location (%lld) outside view bounds [%lld, %lld]",
                (long long)*c_cursor, (long long)c_view->lo, (long long)c_view->hi);
            return E_LOC_RESOLVE;
        }

        *c_cursor = clamp64(*c_cursor, 0, io_size(io));
        return E_OK;
    }

    // skip <offset><unit>
    if (op->u.skip.by_offset.unit == UNIT_BYTES) {
        i64 cur = view_clamp(c_view, io, *c_cursor);
        apply_delta_with_clamp(&cur, op->u.skip.by_offset.offset, c_view, io, CLAMP_VIEW);
        *c_cursor = cur;
    } else if (op->u.skip.by_offset.unit == UNIT_LINES) {
        // Skip by lines
        i64 current_line_start;
        enum Err err = io_line_start(io, *c_cursor, &current_line_start);
        if (err != E_OK) {
            return err;
        }

        // Pin to view bounds
        if (current_line_start < view_bof(c_view)) {
            current_line_start = view_bof(c_view);
        }

        err = io_step_lines(io, current_line_start,
            (i32)op->u.skip.by_offset.offset, c_cursor);
        if (err != E_OK) {
            return err;
        }
        *c_cursor = view_clamp(c_view, io, *c_cursor);
    } else { // UNIT_CHARS
        i64 char_start;
        enum Err err = io_prev_char_start(io, *c_cursor, &char_start);
        if (err != E_OK) {
            return err;
        }
        err = io_step_chars(io, char_start, (i32)op->u.skip.by_offset.offset, c_cursor);
        if (err != E_OK) {
            return err;
        }
        *c_cursor = view_clamp(c_view, io, *c_cursor);
    }
    return E_OK;
}

static enum Err viewset_op(
    File* io,
    const Op* op,
    VM* vm,
    i64* c_cursor,
    Match* c_last_match,
    LabelWrite* label_writes,
    i32 label_count,
    View* c_view)
{
    i64 a;
    i64 b;
    enum Err err = resolve_location(&op->u.viewset.a, io, vm, c_last_match, *c_cursor, label_writes, label_count, c_view, CLAMP_VIEW, &a);
    if (err != E_OK) {
        return err;
    }
    err = resolve_location(&op->u.viewset.b, io, vm, c_last_match, *c_cursor, label_writes, label_count, c_view, CLAMP_VIEW, &b);
    if (err != E_OK) {
        return err;
    }

    i64 lo = a < b ? a : b;
    i64 hi = a < b ? b : a;

    // Stage view activation
    c_view->active = true;
    c_view->lo = lo;
    c_view->hi = hi;

    // Clamp cursor into new view
    *c_cursor = view_clamp(c_view, io, *c_cursor);

    // Invalidate staged match if it straddles the view
    if (c_last_match->valid && (c_last_match->start < lo || c_last_match->end > hi)) {
        c_last_match->valid = false;
    }
    return E_OK;
}

static enum Err take_len_op(
    File* io,
    const Op* op,
    i64* c_cursor,
    Range* ranges,
    i32* range_count,
    i32 range_cap,
    const View* c_view)
{

    i64 start;
    i64 end;

    if (op->u.take_len.unit == UNIT_BYTES) {
        if (op->u.take_len.offset > 0) {
            start = view_clamp(c_view, io, *c_cursor);
            end = start;
            apply_delta_with_clamp(&end, op->u.take_len.offset, c_view, io, CLAMP_VIEW);
        } else {
            end = view_clamp(c_view, io, *c_cursor);
            start = end;
            apply_delta_with_clamp(&start, op->u.take_len.offset, c_view, io, CLAMP_VIEW);
            start = clamp64(start, view_bof(c_view), end);
        }
    } else if (op->u.take_len.unit == UNIT_LINES) {
        // Take by lines
        i64 line_start;
        enum Err err = io_line_start(io, *c_cursor, &line_start);
        if (err != E_OK) {
            return err;
        }

        // Pin to view bounds
        if (line_start < view_bof(c_view)) {
            line_start = view_bof(c_view);
        }

        if (op->u.take_len.offset > 0) {
            start = line_start;
            if (op->u.take_len.offset > INT_MAX) {
                return E_PARSE;
            }
            err = io_step_lines(io, line_start,
                (i32)op->u.take_len.offset, &end);
            if (err != E_OK) {
                return err;
            }
            end = view_clamp(c_view, io, end);
        } else {
            end = line_start;
            if (op->u.take_len.offset < -INT_MAX) {
                return E_PARSE;
            }
            err = io_step_lines(io, line_start,
                (i32)op->u.take_len.offset, &start);
            if (err != E_OK) {
                return err;
            }
            start = clamp64(start, view_bof(c_view), end);
        }
    } else { // UNIT_CHARS
        i64 cstart;
        enum Err err = io_prev_char_start(io, *c_cursor, &cstart);
        if (err != E_OK) {
            return err;
        }
        if (op->u.take_len.offset > 0) {
            start = cstart;
            err = io_step_chars(io, cstart, (i32)op->u.take_len.offset, &end);
            if (err != E_OK) {
                return err;
            }
            end = view_clamp(c_view, io, end);
        } else {
            end = cstart;
            i64 s;
            err = io_step_chars(io, cstart, (i32)op->u.take_len.offset, &s);
            if (err != E_OK) {
                return err;
            }
            start = clamp64(s, view_bof(c_view), end);
        }
    }

    // Stage the range
    enum Err err_stage = stage_file_range(ranges, range_count, range_cap, start, end);
    if (err_stage != E_OK) {
        return err_stage;
    }
    if (start != end) {
        *c_cursor = start > end ? start : end;
    }
    return E_OK;
}

static enum Err take_to_op(
    File* io,
    const Op* op,
    VM* vm,
    i64* c_cursor,
    Match* c_last_match,
    Range* ranges,
    i32* range_count,
    i32 range_cap,
    LabelWrite* label_writes,
    i32 label_count,
    const View* c_view)
{
    i64 target;
    enum Err err = resolve_location(&op->u.take_to.to, io, vm, c_last_match, *c_cursor, label_writes, label_count, c_view, CLAMP_VIEW, &target);
    if (err != E_OK) {
        return err;
    }

    i64 start = view_clamp(c_view, io, *c_cursor);
    i64 end = view_clamp(c_view, io, target);

    // Order-normalized emit
    if (start > end) {
        i64 temp = start;
        start = end;
        end = temp;
    }

    // Stage the range
    err = stage_file_range(ranges, range_count, range_cap, start, end);
    if (err != E_OK) {
        return err;
    }

    if (start != end) {
        *c_cursor = view_clamp(c_view, io, end);
    }
    return E_OK;
}

static enum Err take_until_common(
    File* io,
    const Op* op,
    VM* vm,
    i64* c_cursor,
    Match* c_last_match,
    Range* ranges,
    i32* range_count,
    i32 range_cap,
    LabelWrite* label_writes,
    i32 label_count,
    const View* c_view,
    OpKind kind)
{
    // Search forward from cursor to view end
    i64 ms;
    i64 me;
    enum Err err;
    if (kind == OP_TAKE_UNTIL_RE) {
        err = io_find_regex_window(io, view_clamp(c_view, io, *c_cursor), view_eof(c_view, io),
            op->u.take_until_re.prog, DIR_FWD, &ms, &me);
    } else if (kind == OP_TAKE_UNTIL_BIN) {
        err = io_find_window(io, view_clamp(c_view, io, *c_cursor), view_eof(c_view, io),
            (const unsigned char*)op->u.take_until_bin.needle.bytes,
            (size_t)op->u.take_until_bin.needle.len, DIR_FWD, &ms, &me);
    } else { // OP_TAKE_UNTIL
        err = io_find_window(io, view_clamp(c_view, io, *c_cursor), view_eof(c_view, io),
            (const unsigned char*)op->u.take_until.needle.bytes,
            (size_t)op->u.take_until.needle.len, DIR_FWD, &ms, &me);
    }
    if (err != E_OK) {
        return err;
    }

    // Update staged match
    c_last_match->start = ms;
    c_last_match->end = me;
    c_last_match->valid = true;

    i64 target;
    bool has_at;
    const LocExpr* at;
    if (kind == OP_TAKE_UNTIL_RE) {
        has_at = op->u.take_until_re.has_at;
        at = &op->u.take_until_re.at;
    } else if (kind == OP_TAKE_UNTIL_BIN) {
        has_at = op->u.take_until_bin.has_at;
        at = &op->u.take_until_bin.at;
    } else { // OP_TAKE_UNTIL
        has_at = op->u.take_until.has_at;
        at = &op->u.take_until.at;
    }

    if (has_at) {
        err = resolve_location(at, io, vm, c_last_match, ms,
            label_writes, label_count, c_view, CLAMP_VIEW, &target);
        if (err != E_OK) {
            return err;
        }
    } else {
        target = ms;
    }

    i64 dst = view_clamp(c_view, io, target);

    // Stage [cursor, dst) ONLY (no order-normalization)
    i64 range_start = view_clamp(c_view, io, *c_cursor);
    err = stage_file_range(ranges, range_count, range_cap, range_start, dst);
    if (err != E_OK) {
        return err;
    }

    // Move cursor only if non-empty
    if (dst > *c_cursor) {
        *c_cursor = dst;
    }
    return E_OK;
}

static enum Err take_until_op(
    File* io,
    const Op* op,
    VM* vm,
    i64* c_cursor,
    Match* c_last_match,
    Range* ranges,
    i32* range_count,
    i32 range_cap,
    LabelWrite* label_writes,
    i32 label_count,
    const View* c_view)
{
    return take_until_common(io, op, vm, c_cursor, c_last_match,
        ranges, range_count, range_cap, label_writes, label_count, c_view, OP_TAKE_UNTIL);
}

static enum Err take_until_re_op(
    File* io,
    const Op* op,
    VM* vm,
    i64* c_cursor,
    Match* c_last_match,
    Range* ranges,
    i32* range_count,
    i32 range_cap,
    LabelWrite* label_writes,
    i32 label_count,
    const View* c_view)
{
    return take_until_common(io, op, vm, c_cursor, c_last_match,
        ranges, range_count, range_cap, label_writes, label_count, c_view, OP_TAKE_UNTIL_RE);
}

static enum Err take_until_bin_op(
    File* io,
    const Op* op,
    VM* vm,
    i64* c_cursor,
    Match* c_last_match,
    Range* ranges,
    i32* range_count,
    i32 range_cap,
    LabelWrite* label_writes,
    i32 label_count,
    const View* c_view)
{
    return take_until_common(io, op, vm, c_cursor, c_last_match,
        ranges, range_count, range_cap, label_writes, label_count, c_view, OP_TAKE_UNTIL_BIN);
}

enum Err stage_clause(const Clause* clause,
    File* io, VM* vm,
    Range* ranges, i32 ranges_cap,
    LabelWrite* label_writes, i32 label_cap,
    char* inline_buf, i32 inline_cap,
    StagedResult* result)
{
    i64 c_cursor = vm->cursor;
    Match c_last_match = vm->last_match;
    View c_view = vm->view;

    i32 range_count = 0;
    i32 label_count = 0;
    char* inline_ptr = inline_buf;
    char* inline_end = NULL;
    if (inline_buf && inline_cap > 0) {
        inline_end = inline_buf + (size_t)inline_cap * INLINE_LIT_CAP;
    }

    enum Err err = E_OK;
    for (i32 i = 0; i < clause->op_count; i++) {
        err = execute_op(&clause->ops[i], io, vm,
            &c_cursor, &c_last_match,
            &ranges, &range_count, &ranges_cap,
            &label_writes, &label_count, &label_cap,
            &c_view,
            inline_buf ? &inline_ptr : NULL, inline_end);
        if (err != E_OK) {
            break;
        }
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

static enum Err execute_op(const Op* op, File* io, VM* vm,
    i64* c_cursor, Match* c_last_match,
    Range** ranges, i32* range_count, const i32* range_cap,
    LabelWrite** label_writes, i32* label_count, const i32* label_cap,
    View* c_view,
    char** inline_ptr, char* inline_end)
{
    switch (op->kind) {
    case OP_FIND:
        return find_bytes_op(io, op, vm, c_cursor, c_last_match, *label_writes, *label_count, c_view);
    case OP_FIND_RE:
        return find_regex_op(io, op, vm, c_cursor, c_last_match, *label_writes, *label_count, c_view);
    case OP_FIND_BIN:
        return find_bytes_op(io, op, vm, c_cursor, c_last_match, *label_writes, *label_count, c_view);
    case OP_SKIP:
        return skip_op(io, op, vm, c_cursor, c_last_match, *label_writes, *label_count, c_view);
    case OP_TAKE_LEN:
        return take_len_op(io, op, c_cursor, *ranges, range_count, *range_cap, c_view);
    case OP_TAKE_TO:
        return take_to_op(io, op, vm, c_cursor, c_last_match, *ranges, range_count, *range_cap, *label_writes, *label_count, c_view);
    case OP_TAKE_UNTIL:
        return take_until_op(io, op, vm, c_cursor, c_last_match, *ranges, range_count, *range_cap, *label_writes, *label_count, c_view);
    case OP_TAKE_UNTIL_RE:
        return take_until_re_op(io, op, vm, c_cursor, c_last_match, *ranges, range_count, *range_cap, *label_writes, *label_count, c_view);
    case OP_TAKE_UNTIL_BIN:
        return take_until_bin_op(io, op, vm, c_cursor, c_last_match, *ranges, range_count, *range_cap, *label_writes, *label_count, c_view);
    case OP_LABEL:
        return label_op(op, c_cursor, *label_writes, label_count, *label_cap);
    case OP_VIEWSET:
        return viewset_op(io, op, vm, c_cursor, c_last_match, *label_writes, *label_count, c_view);
    case OP_VIEWCLEAR:
        return view_clear_op(io, c_view);
    case OP_PRINT:
        return print_literal_op(op, io, c_view, *c_cursor, *ranges, range_count, *range_cap, inline_ptr, inline_end);
    case OP_FAIL:
        return fail_with_message_op(op);
    default:
        return E_PARSE;
    }
}

static enum Err resolve_location(
    const LocExpr* loc, File* io, const VM* vm,
    const Match* staged_match, i64 staged_cursor,
    const LabelWrite* staged_labels, i32 staged_label_count,
    const View* c_view, ClampPolicy clamp, i64* out)
{
    i64 base = 0;

    /************************
     * RESOLVE BASE LOCATION
     ************************/
    switch (loc->base) {
    case LOC_CURSOR:
        base = staged_cursor;
        break;
    case LOC_BOF:
        base = view_bof(c_view);
        break;
    case LOC_EOF:
        base = view_eof(c_view, io);
        break;
    case LOC_NAME: {
        // Validate label index bounds
        if (loc->name_idx < 0 || loc->name_idx >= MAX_LABELS) {
            return E_PARSE;
        }
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
        if (!found) {
            return E_LOC_RESOLVE;
        }
        break;
    }
    case LOC_MATCH_START:
        if (!staged_match->valid) {
            return E_LOC_RESOLVE;
        }
        base = staged_match->start;
        break;
    case LOC_MATCH_END:
        if (!staged_match->valid) {
            return E_LOC_RESOLVE;
        }
        base = staged_match->end;
        break;
    case LOC_LINE_START: {
        i64 anchor = staged_cursor;
        enum Err e = io_line_start(io, anchor, &base);
        if (e != E_OK) {
            return e;
        }
        if (base < view_bof(c_view)) {
            base = view_bof(c_view);
        }
        break;
    }
    case LOC_LINE_END: {
        i64 anchor = staged_cursor;
        enum Err e = io_line_end(io, anchor, &base);
        if (e != E_OK) {
            return e;
        }
        if (base > view_eof(c_view, io)) {
            base = view_eof(c_view, io);
        }
        break;
    }
    default:
        return E_PARSE;
    }

    /************************
     * APPLY OFFSET (if any)
     ************************/
    if (loc->offset != 0) {
        if (loc->unit == UNIT_BYTES) {
            apply_delta_with_clamp(&base, loc->offset, c_view, io, clamp == CLAMP_FILE ? CLAMP_FILE : clamp);
        } else if (loc->unit == UNIT_LINES) {
            if (loc->offset > INT_MAX || loc->offset < -INT_MAX) {
                return E_PARSE;
            }
            i32 d = (i32)loc->offset;
            enum Err e = io_step_lines(io, base, d, &base);
            if (e != E_OK) {
                return e;
            }
        } else { // UNIT_CHARS
            if (loc->offset > INT_MAX || loc->offset < -INT_MAX) {
                return E_PARSE;
            }
            i64 cs;
            enum Err e = io_prev_char_start(io, base, &cs);
            if (e != E_OK) {
                return e;
            }
            i32 d = (i32)loc->offset;
            e = io_step_chars(io, cs, d, &cs);
            if (e != E_OK) {
                return e;
            }
            base = cs;
        }
    }

    /******************
     * CLAMP TO BOUNDS
     ******************/
    if (clamp == CLAMP_VIEW) {
        *out = view_clamp(c_view, io, base);
    } else if (clamp == CLAMP_FILE) {
        *out = clamp64(base, 0, io_size(io));
    } else {
        *out = base;
    }
    return E_OK;
}

void commit_labels(VM* vm, const LabelWrite* label_writes, i32 label_count)
{
    for (i32 i = 0; i < label_count; i++) {
        vm->label_pos[label_writes[i].name_idx] = label_writes[i].pos;
    }
}

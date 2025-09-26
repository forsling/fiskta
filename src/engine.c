// engine.c
#include "fiskta.h"
#include "iosearch.h"
#include <stdlib.h>
#include <string.h>

// Forward declarations
typedef struct {
    int name_idx;
    i64 pos;
} LabelWrite;

static enum Err execute_clause(const Clause* clause, const Program* prg, File* io, VM* vm, FILE* out);
static enum Err execute_op(const Op* op, const Program* prg, File* io, VM* vm,
    i64* c_cursor, Match* c_last_match,
    Range** ranges, int* range_count, int* range_cap,
    LabelWrite** label_writes, int* label_count, int* label_cap);
static enum Err resolve_loc_expr(const LocExpr* loc, const Program* prg, File* io,
    const VM* vm, const Match* staged_match, i64 staged_cursor, 
    const LabelWrite* staged_labels, int staged_label_count, i64* out);
static enum Err resolve_at_expr(const AtExpr* at, File* io, const Match* match, i64* out);
static void commit_labels(VM* vm, const Program* prg, const LabelWrite* label_writes, int label_count);

enum Err engine_run(const Program* prg, const char* in_path, FILE* out)
{
    File io = { 0 };
    enum Err err = io_open(&io, in_path);
    if (err != E_OK)
        return err;

    VM vm = { 0 };
    vm.cursor = 0;
    vm.last_match.valid = false;
    vm.gen_counter = 0;

    // Execute each clause independently
    int successful_clauses = 0;
    enum Err last_err = E_OK;

    for (int i = 0; i < prg->clause_count; i++) {
        err = execute_clause(&prg->clauses[i], prg, &io, &vm, out);
        if (err == E_OK) {
            successful_clauses++;
        } else {
            last_err = err; // Remember the last error
        }
    }

    // Return error only if all clauses failed
    if (successful_clauses == 0) {
        io_close(&io);
        return last_err;
    }

    io_close(&io);
    return E_OK;
}

static enum Err execute_clause(const Clause* clause, const Program* prg, File* io, VM* vm, FILE* out)
{
    // Initialize staged state
    i64 c_cursor = vm->cursor;
    Match c_last_match = vm->last_match;

    // Staged captures
    Range* ranges = malloc(16 * sizeof(Range));
    if (!ranges)
        return E_OOM;
    int range_count = 0;
    int range_cap = 16;

    // Staged label writes
    LabelWrite* label_writes = malloc(16 * sizeof(LabelWrite));
    if (!label_writes) {
        free(ranges);
        return E_OOM;
    }
    int label_count = 0;
    int label_cap = 16;

    // Execute each operation
    enum Err err = E_OK;
    for (int i = 0; i < clause->op_count; i++) {
        err = execute_op(&clause->ops[i], prg, io, vm,
            &c_cursor, &c_last_match,
            &ranges, &range_count, &range_cap,
            &label_writes, &label_count, &label_cap);
        if (err != E_OK) {
            break;
        }
    }

    if (err == E_OK) {
        // Commit: emit ranges, commit labels, update global state
        for (int i = 0; i < range_count; i++) {
            err = io_emit(io, ranges[i].start, ranges[i].end, out);
            if (err != E_OK)
                break;
        }

        if (err == E_OK) {
            commit_labels(vm, prg, label_writes, label_count);
            vm->cursor = c_cursor;
            vm->last_match = c_last_match;
        }
    }

    free(ranges);
    free(label_writes);
    return err;
}

static enum Err execute_op(const Op* op, const Program* prg, File* io, VM* vm,
    i64* c_cursor, Match* c_last_match,
    Range** ranges, int* range_count, int* range_cap,
    LabelWrite** label_writes, int* label_count, int* label_cap)
{
    switch (op->kind) {
    case OP_FIND: {
        i64 win_lo, win_hi;

        if (op->u.find.to.base == LOC_EOF && !op->u.find.to.has_off) {
            win_hi = io_size(io);
        } else {
            enum Err err = resolve_loc_expr(&op->u.find.to, prg, io, vm, c_last_match, *c_cursor, *label_writes, *label_count, &win_hi);
            if (err != E_OK)
                return err;
        }

        win_lo = *c_cursor;

        // Determine direction and adjust window
        enum Dir dir = DIR_FWD;
        if (win_hi < win_lo) {
            dir = DIR_BWD;
            i64 temp = win_lo;
            win_lo = win_hi;
            win_hi = temp;
        }

        i64 ms, me;
        enum Err err = io_find_window(io, win_lo, win_hi,
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

    case OP_SKIP: {
        if (op->u.skip.unit == UNIT_BYTES) {
            *c_cursor = clamp64(*c_cursor + op->u.skip.n, 0, io_size(io));
        } else {
            // Skip by lines
            i64 current_line_start;
            enum Err err = io_line_start(io, *c_cursor, &current_line_start);
            if (err != E_OK)
                return err;

            err = io_step_lines_from(io, current_line_start, op->u.skip.n, c_cursor);
            if (err != E_OK)
                return err;
        }
        break;
    }

    case OP_TAKE_LEN: {
        i64 start, end;

        if (op->u.take_len.unit == UNIT_BYTES) {
            if (op->u.take_len.sign > 0) {
                start = *c_cursor;
                end = clamp64(start + op->u.take_len.n, 0, io_size(io));
            } else {
                end = *c_cursor;
                start = clamp64(end - op->u.take_len.n, 0, end);
            }
        } else {
            // Take by lines
            i64 line_start;
            enum Err err = io_line_start(io, *c_cursor, &line_start);
            if (err != E_OK)
                return err;

            if (op->u.take_len.sign > 0) {
                start = line_start;
                err = io_step_lines_from(io, line_start, op->u.take_len.n, &end);
                if (err != E_OK)
                    return err;
            } else {
                end = line_start;
                err = io_step_lines_from(io, line_start, -op->u.take_len.n, &start);
                if (err != E_OK)
                    return err;
            }
        }

        // Stage the range
        if (*range_count >= *range_cap) {
            *range_cap *= 2;
            Range* new_ranges = realloc(*ranges, *range_cap * sizeof(Range));
            if (!new_ranges)
                return E_OOM;
            *ranges = new_ranges;
        }

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
        enum Err err = resolve_loc_expr(&op->u.take_to.to, prg, io, vm, c_last_match, *c_cursor, *label_writes, *label_count, &target);
        if (err != E_OK)
            return err;

        i64 start = *c_cursor;
        i64 end = target;

        // Order-normalized emit
        if (start > end) {
            i64 temp = start;
            start = end;
            end = temp;
        }

        // Stage the range
        if (*range_count >= *range_cap) {
            *range_cap *= 2;
            Range* new_ranges = realloc(*ranges, *range_cap * sizeof(Range));
            if (!new_ranges)
                return E_OOM;
            *ranges = new_ranges;
        }

        (*ranges)[*range_count].start = start;
        (*ranges)[*range_count].end = end;
        (*range_count)++;

        *c_cursor = end;
        break;
    }

    case OP_TAKE_UNTIL: {
        // Search forward from cursor
        i64 ms, me;
        enum Err err = io_find_window(io, *c_cursor, io_size(io),
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
            err = resolve_at_expr(&op->u.take_until.at, io, c_last_match, &target);
            if (err != E_OK)
                return err;
        } else {
            target = ms; // Default to match-start
        }

        // Stage the range
        if (*range_count >= *range_cap) {
            *range_cap *= 2;
            Range* new_ranges = realloc(*ranges, *range_cap * sizeof(Range));
            if (!new_ranges)
                return E_OOM;
            *ranges = new_ranges;
        }

        (*ranges)[*range_count].start = *c_cursor;
        (*ranges)[*range_count].end = clamp64(target, 0, io_size(io));
        (*range_count)++;

        *c_cursor = target;
        break;
    }

    case OP_LABEL: {
        // Stage label write
        if (*label_count >= *label_cap) {
            *label_cap *= 2;
            LabelWrite* new_writes = realloc(*label_writes, *label_cap * sizeof(LabelWrite));
            if (!new_writes)
                return E_OOM;
            *label_writes = new_writes;
        }

        (*label_writes)[*label_count].name_idx = op->u.label.name_idx;
        (*label_writes)[*label_count].pos = *c_cursor;
        (*label_count)++;
        break;
    }

    case OP_GOTO: {
        enum Err err = resolve_loc_expr(&op->u.go.to, prg, io, vm, c_last_match, *c_cursor, *label_writes, *label_count, c_cursor);
        if (err != E_OK)
            return err;
        break;
    }

    default:
        return E_PARSE;
    }

    return E_OK;
}

static enum Err resolve_loc_expr(const LocExpr* loc, const Program* prg, File* io,
    const VM* vm, const Match* staged_match, i64 staged_cursor, 
    const LabelWrite* staged_labels, int staged_label_count, i64* out)
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
        for (int i = 0; i < staged_label_count; i++) {
            if (staged_labels[i].name_idx == loc->name_idx) {
                base = staged_labels[i].pos;
                found = true;
                break;
            }
        }
        
        // If not found in staged labels, check committed labels
        if (!found) {
            for (int i = 0; i < 32; i++) {
                if (vm->labels[i].in_use && strcmp(vm->labels[i].name, prg->names[loc->name_idx]) == 0) {
                    base = vm->labels[i].pos;
                    found = true;
                    break;
                }
            }
        }
        
        if (!found)
            return E_LOC_RESOLVE;
        break;
    }
    case LOC_MATCH_START:
    case LOC_MATCH_END:
    case LOC_LINE_START:
    case LOC_LINE_END:
        if (!staged_match->valid)
            return E_LOC_RESOLVE;

        if (loc->base == LOC_MATCH_START) {
            base = staged_match->start;
        } else if (loc->base == LOC_MATCH_END) {
            base = staged_match->end;
        } else if (loc->base == LOC_LINE_START) {
            enum Err err = io_line_start(io, staged_match->start, &base);
            if (err != E_OK)
                return err;
        } else { // LOC_LINE_END
            enum Err err = io_line_end(io, staged_match->end - 1, &base);
            if (err != E_OK)
                return err;
        }
        break;
    default:
        return E_PARSE;
    }

    // Apply offset if present
    if (loc->has_off) {
        if (loc->unit == UNIT_BYTES) {
            base += loc->sign * loc->n;
        } else {
            // Line offset
            enum Err err = io_step_lines_from(io, base, loc->sign * loc->n, &base);
            if (err != E_OK)
                return err;
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
        enum Err err = io_line_end(io, match->end - 1, &base);
        if (err != E_OK)
            return err;
        break;
    }
    default:
        return E_PARSE;
    }

    // Apply offset if present
    if (at->has_off) {
        if (at->unit == UNIT_BYTES) {
            base += at->sign * at->n;
        } else {
            // Line offset
            enum Err err = io_step_lines_from(io, base, at->sign * at->n, &base);
            if (err != E_OK)
                return err;
        }
    }

    *out = clamp64(base, 0, io_size(io));
    return E_OK;
}

static void commit_labels(VM* vm, const Program* prg, const LabelWrite* label_writes, int label_count)
{
    for (int i = 0; i < label_count; i++) {
        int name_idx = label_writes[i].name_idx;
        i64 pos = label_writes[i].pos;

        // Find existing label or free slot
        int slot = -1;
        for (int j = 0; j < 32; j++) {
            if (vm->labels[j].in_use && strcmp(vm->labels[j].name, prg->names[name_idx]) == 0) {
                slot = j;
                break;
            }
        }

        if (slot == -1) {
            // Find free slot
            for (int j = 0; j < 32; j++) {
                if (!vm->labels[j].in_use) {
                    slot = j;
                    break;
                }
            }

            if (slot == -1) {
                // Evict LRU (lowest gen)
                slot = 0;
                for (int j = 1; j < 32; j++) {
                    if (vm->labels[j].gen < vm->labels[slot].gen) {
                        slot = j;
                    }
                }
            }
        }

        // Update slot
        strcpy(vm->labels[slot].name, prg->names[name_idx]);
        vm->labels[slot].pos = pos;
        vm->labels[slot].gen = ++vm->gen_counter;
        vm->labels[slot].in_use = true;
    }
}

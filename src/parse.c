#include "fiskta.h"
#include "parse_plan.h"
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

// Helper function to find inline offset start
static const char* find_inline_offset_start(const char* s)
{
    // Return pointer to first '+' or '-' that begins an offset suffix, else NULL.
    // Skip the first char to avoid treating a leading sign as part of the base token.
    // Only treat +/- as offset if followed by a digit (not part of base token name).
    for (const char* p = s + 1; *p; ++p) {
        if ((*p == '+' || *p == '-') && isdigit(p[1])) {
            return p;
        }
    }
    return NULL;
}

static enum Err parse_op_build(char** tokens, i32* idx, i32 token_count, Op* op, Program* prg,
    char* str_pool, size_t* str_pool_off, size_t str_pool_cap);
static enum Err parse_loc_expr_build(char** tokens, i32* idx, i32 token_count, LocExpr* loc, Program* prg);
static enum Err parse_at_expr_build(char** tokens, i32* idx, i32 token_count, LocExpr* at);
static enum Err parse_string_to_bytes(const char* str, String* out_string, char* str_pool, size_t* str_pool_off, size_t str_pool_cap);
static enum Err parse_unsigned_number(const char* token, i32* sign, u64* n, enum Unit* unit);
static enum Err parse_signed_number(const char* token, i64* offset, enum Unit* unit);
static enum Err parse_milliseconds(const char* token, i32* out_ms);
static i32 find_or_add_name_build(Program* prg, const char* name);
static bool is_valid_label_name(const char* name);
enum Err parse_preflight(i32 token_count, char** tokens, const char* in_path, ParsePlan* plan, const char** in_path_out);
enum Err parse_build(i32 token_count, char** tokens, const char* in_path, Program* prg, const char** in_path_out,
    Clause* clauses_buf, Op* ops_buf,
    char* str_pool, size_t str_pool_cap);


enum Err parse_preflight(i32 token_count, char** tokens, const char* in_path, ParsePlan* plan, const char** in_path_out)
{
    memset(plan, 0, sizeof(*plan));

    if (token_count < 1) {
        return E_PARSE;
    }

    // Input path is passed separately
    *in_path_out = in_path;

    // Count clauses (number of "THEN" + 1)
    plan->clause_count = 1;
    for (i32 i = 0; i < token_count; i++) {
        if (strcmp(tokens[i], "THEN") == 0) {
            plan->clause_count++;
        }
    }

    // Count operations and analyze needs
    i32 idx = 0;
    while (idx < token_count) {
        // Count ops in this clause
        i32 clause_start = idx;
        while (idx < token_count && strcmp(tokens[idx], "THEN") != 0) {
            const char* cmd = tokens[idx];
            plan->total_ops++;

            if (strcmp(cmd, "find") == 0) {
                idx++;
                if (idx < token_count && strcmp(tokens[idx], "to") == 0) {
                    idx++;
                    // Skip location expression
                    if (idx < token_count) {
                        idx++;
                        // Skip offset if present
                        if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-')) {
                            idx++;
                        }
                    }
                }
                // Skip needle
                if (idx < token_count) {
                    plan->needle_count++;
                    plan->needle_bytes += strlen(tokens[idx]);
                    idx++;
                }
            } else if (strcmp(cmd, "findr") == 0) {
                idx++;
                if (idx < token_count && strcmp(tokens[idx], "to") == 0) {
                    idx++;
                    if (idx < token_count) {
                        idx++;
                        if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-'))
                            idx++;
                    }
                }
                if (idx < token_count) {
                    const char* pat = tokens[idx];
                    size_t L = strlen(pat);
                    plan->sum_findr_ops++;
                    i32 est = (i32)(4 * L + 8);
                    plan->re_ins_estimate += est;
                    if (est > plan->re_ins_estimate_max) {
                        plan->re_ins_estimate_max = est;
                    }
                    // crude class count estimate: count '[' and escape sequences that create classes
                    for (const char* p = pat; *p; ++p) {
                        if (*p == '[')
                            plan->re_classes_estimate++;
                        if (*p == '\\' && p[1] && strchr("dDwWsS", p[1]))
                            plan->re_classes_estimate++;
                    }
                    plan->needle_count++; // account string storage (shared pool)
                    plan->needle_bytes += L;
                    idx++;
                }
            } else if (strcmp(cmd, "skip") == 0) {
                idx++;
                if (idx < token_count)
                    idx++;
            } else if (strcmp(cmd, "take") == 0) {
                idx++;
                if (idx < token_count) {
                    const char* next = tokens[idx];
                    if (strcmp(next, "to") == 0) {
                        plan->sum_take_ops++;
                        idx++;
                        // Skip location expression
                        if (idx < token_count) {
                            idx++;
                            // Skip offset if present
                            if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-')) {
                                idx++;
                            }
                        }
                    } else if (strcmp(next, "until") == 0) {
                        plan->sum_take_ops++;
                        idx++;
                        // Skip needle
                        if (idx < token_count) {
                            plan->needle_count++;
                            plan->needle_bytes += strlen(tokens[idx]);
                            idx++;
                        }
                        // Skip "at" expression if present
                        if (idx < token_count && strcmp(tokens[idx], "at") == 0) {
                            idx++;
                            if (idx < token_count) {
                                idx++;
                                // Skip offset if present
                                if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-')) {
                                    idx++;
                                }
                            }
                        }
                    } else {
                        plan->sum_take_ops++;
                        idx++; // skip number+unit
                    }
                }
            } else if (strcmp(cmd, "label") == 0) {
                plan->sum_label_ops++;
                idx++;
                if (idx < token_count) {
                    idx++;
                }
            } else if (strcmp(cmd, "goto") == 0) {
                idx++;
                if (idx < token_count) {
                    idx++;
                    // Skip offset if present
                    if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-')) {
                        idx++;
                    }
                }
            } else if (strcmp(cmd, "viewset") == 0) {
                idx++;
                // Skip two location expressions
                if (idx < token_count) {
                    idx++;
                    // Skip offset if present
                    if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-')) {
                        idx++;
                    }
                }
                if (idx < token_count) {
                    idx++;
                    // Skip offset if present
                    if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-')) {
                        idx++;
                    }
                }
            } else if (strcmp(cmd, "viewclear") == 0) {
                idx++; // no additional tokens
            } else if (strcmp(cmd, "print") == 0 || strcmp(cmd, "echo") == 0) {
                idx++; // skip command token
                if (idx < token_count) {
                    plan->needle_count++;
                    plan->needle_bytes += strlen(tokens[idx]);
                    idx++; // skip string token
                }
            } else if (strcmp(cmd, "sleep") == 0) {
                idx++; // skip command token
                if (idx < token_count)
                    idx++; // skip duration token
            } else {
                idx++; // skip unknown token
            }
        }

        if (idx < token_count && strcmp(tokens[idx], "THEN") == 0) {
            idx++;
        }
    }

    return E_OK;
}

enum Err parse_build(i32 token_count, char** tokens, const char* in_path, Program* prg, const char** in_path_out,
    Clause* clauses_buf, Op* ops_buf,
    char* str_pool, size_t str_pool_cap)
{
    memset(prg, 0, sizeof(*prg));

    if (token_count < 1) {
        return E_PARSE;
    }

    // Input path is passed separately
    *in_path_out = in_path;

    // Initialize program with preallocated buffers
    prg->clauses = clauses_buf;
    prg->clause_count = 0;
    prg->name_count = 0;

    // Track string pool usage
    size_t str_pool_off = 0;

    // Parse clauses separated by "THEN"
    i32 idx = 0;
    i32 op_cursor = 0;

    while (idx < token_count) {
        Clause* clause = &prg->clauses[prg->clause_count];
        clause->ops = ops_buf + op_cursor;
        clause->op_count = 0;

        // Count ops in this clause first
        i32 clause_start = idx;
        i32 clause_op_count = 0;
        while (idx < token_count && strcmp(tokens[idx], "THEN") != 0) {
            const char* cmd = tokens[idx];
            clause_op_count++;
            idx++;

            // Skip command-specific tokens
            if (strcmp(cmd, "find") == 0) {
                if (idx < token_count && strcmp(tokens[idx], "to") == 0) {
                    idx++;
                    if (idx < token_count)
                        idx++; // skip location
                    if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-')) {
                        idx++; // skip offset
                    }
                }
                if (idx < token_count)
                    idx++; // skip needle
            } else if (strcmp(cmd, "skip") == 0) {
                if (idx < token_count)
                    idx++; // skip number+unit
            } else if (strcmp(cmd, "take") == 0) {
                if (idx < token_count) {
                    const char* next = tokens[idx];
                    if (strcmp(next, "to") == 0) {
                        idx++;
                        if (idx < token_count)
                            idx++; // skip location
                        if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-')) {
                            idx++; // skip offset
                        }
                    } else if (strcmp(next, "until") == 0) {
                        idx++;
                        if (idx < token_count)
                            idx++; // skip needle
                        if (idx < token_count && strcmp(tokens[idx], "at") == 0) {
                            idx++;
                            if (idx < token_count)
                                idx++; // skip location
                            if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-')) {
                                idx++; // skip offset
                            }
                        }
                    } else {
                        idx++; // skip number+unit
                    }
                }
            } else if (strcmp(cmd, "label") == 0) {
                if (idx < token_count)
                    idx++; // skip name
            } else if (strcmp(cmd, "goto") == 0) {
                if (idx < token_count)
                    idx++; // skip location
                if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-')) {
                    idx++; // skip offset
                }
            } else if (strcmp(cmd, "viewset") == 0) {
                if (idx < token_count)
                    idx++; // skip first location
                if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-')) {
                    idx++; // skip offset
                }
                if (idx < token_count)
                    idx++; // skip second location
                if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-')) {
                    idx++; // skip offset
                }
            } else if (strcmp(cmd, "viewclear") == 0) {
                // 2 no additional tokens
            } else if (strcmp(cmd, "print") == 0 || strcmp(cmd, "echo") == 0) {
                idx++; // skip command token
                if (idx < token_count)
                    idx++; // skip string
            } else if (strcmp(cmd, "sleep") == 0) {
                if (idx < token_count)
                    idx++; // skip duration token
            }
        }

        // 3 Reset idx to clause start and parse for real
        idx = clause_start;
        while (idx < token_count && strcmp(tokens[idx], "THEN") != 0) {
            Op* op = &clause->ops[clause->op_count];
            enum Err err = parse_op_build(tokens, &idx, token_count, op, prg, str_pool, &str_pool_off, str_pool_cap);
            if (err != E_OK) {
                return err;
            }
            clause->op_count++;
        }

        prg->clause_count++;
        op_cursor += clause_op_count;

        if (idx < token_count && strcmp(tokens[idx], "THEN") == 0) {
            idx++;
        }
    }

    return E_OK;
}

static i32 find_or_add_name_build(Program* prg, const char* name)
{
    // Linear search for existing name
    for (i32 i = 0; i < prg->name_count; i++) {
        if (strcmp(prg->names[i], name) == 0) {
            return i;
        }
    }

    // Add new name if space available
    if (prg->name_count < 128) {
        strcpy(prg->names[prg->name_count], name);
        return prg->name_count++;
    }

    return -1; // No space
}

static enum Err parse_op_build(char** tokens, i32* idx, i32 token_count, Op* op, Program* prg,
    char* str_pool, size_t* str_pool_off, size_t str_pool_cap)
{
    if (*idx >= token_count) {
        return E_PARSE;
    }

    const char* cmd = tokens[*idx];
    (*idx)++;

    if (strcmp(cmd, "find") == 0) {
        op->kind = OP_FIND;

        if (*idx < token_count && strcmp(tokens[*idx], "to") == 0) {
            (*idx)++;
            enum Err err = parse_loc_expr_build(tokens, idx, token_count, &op->u.find.to, prg);
            if (err != E_OK)
                return err;
        } else {
            // Default to EOF
            op->u.find.to.base = LOC_EOF;
            op->u.find.to.name_idx = -1;
            op->u.find.to.offset = 0;
            op->u.find.to.unit = UNIT_BYTES;
        }

        // Parse needle
        if (*idx >= token_count)
            return E_PARSE;
        const char* needle = tokens[*idx];
        (*idx)++;

        if (strlen(needle) == 0)
            return E_BAD_NEEDLE;

        // Copy needle to string pool using String
        enum Err err = parse_string_to_bytes(needle, &op->u.find.needle, str_pool, str_pool_off, str_pool_cap);
        if (err != E_OK)
            return err;

    } else if (strcmp(cmd, "findr") == 0) {
        op->kind = OP_FINDR;

        if (*idx < token_count && strcmp(tokens[*idx], "to") == 0) {
            (*idx)++;
            enum Err err = parse_loc_expr_build(tokens, idx, token_count, &op->u.findr.to, prg);
            if (err != E_OK)
                return err;
        } else {
            op->u.findr.to.base = LOC_EOF;
            op->u.findr.to.name_idx = -1;
            op->u.findr.to.offset = 0;
            op->u.findr.to.unit = UNIT_BYTES;
        }
        if (*idx >= token_count)
            return E_PARSE;
        const char* pat = tokens[*idx];
        (*idx)++;
        // Copy pattern to string pool using String
        enum Err err = parse_string_to_bytes(pat, &op->u.findr.pattern, str_pool, str_pool_off, str_pool_cap);
        if (err != E_OK)
            return err;
        op->u.findr.prog = NULL;

    } else if (strcmp(cmd, "skip") == 0) {
        op->kind = OP_SKIP;

        if (*idx >= token_count)
            return E_PARSE;
        enum Err err = parse_unsigned_number(tokens[*idx], NULL, &op->u.skip.n, &op->u.skip.unit);
        if (err != E_OK)
            return err;
        (*idx)++;

    } else if (strcmp(cmd, "take") == 0) {
        if (*idx >= token_count)
            return E_PARSE;

        const char* next = tokens[*idx];
        if (strcmp(next, "to") == 0) {
            op->kind = OP_TAKE_TO;
            (*idx)++;
            enum Err err = parse_loc_expr_build(tokens, idx, token_count, &op->u.take_to.to, prg);
            if (err != E_OK)
                return err;
        } else if (strcmp(next, "until") == 0) {
            op->kind = OP_TAKE_UNTIL;
            (*idx)++;

            // Parse needle
            if (*idx >= token_count)
                return E_PARSE;
            const char* needle = tokens[*idx];
            (*idx)++;

            if (strlen(needle) == 0)
                return E_BAD_NEEDLE;

            // Copy needle to string pool using String
            enum Err err = parse_string_to_bytes(needle, &op->u.take_until.needle, str_pool, str_pool_off, str_pool_cap);
            if (err != E_OK)
                return err;

            // Parse "at" expression if present
            if (*idx < token_count && strcmp(tokens[*idx], "at") == 0) {
                (*idx)++;
                op->u.take_until.has_at = true;
                enum Err err2 = parse_at_expr_build(tokens, idx, token_count, &op->u.take_until.at);
                if (err2 != E_OK)
                    return err2;
            } else {
                op->u.take_until.has_at = false;
            }
        } else {
            op->kind = OP_TAKE_LEN;
            enum Err err = parse_signed_number(tokens[*idx], &op->u.take_len.offset, &op->u.take_len.unit);
            if (err != E_OK)
                return err;
            (*idx)++;
        }

    } else if (strcmp(cmd, "label") == 0) {
        op->kind = OP_LABEL;

        if (*idx >= token_count)
            return E_PARSE;
        const char* name = tokens[*idx];
        (*idx)++;

        if (!is_valid_label_name(name))
            return E_LABEL_FMT;

        i32 name_idx = find_or_add_name_build(prg, name);
        if (name_idx < 0)
            return E_OOM;
        op->u.label.name_idx = name_idx;

    } else if (strcmp(cmd, "goto") == 0) {
        op->kind = OP_GOTO;

        if (*idx >= token_count)
            return E_PARSE;
        enum Err err = parse_loc_expr_build(tokens, idx, token_count, &op->u.go.to, prg);
        if (err != E_OK)
            return err;

    } else if (strcmp(cmd, "viewset") == 0) {
        op->kind = OP_VIEWSET;

        if (*idx >= token_count)
            return E_PARSE;
        enum Err err = parse_loc_expr_build(tokens, idx, token_count, &op->u.viewset.a, prg);
        if (err != E_OK)
            return err;

        if (*idx >= token_count)
            return E_PARSE;
        err = parse_loc_expr_build(tokens, idx, token_count, &op->u.viewset.b, prg);
        if (err != E_OK)
            return err;

    } else if (strcmp(cmd, "viewclear") == 0) {
        op->kind = OP_VIEWCLEAR;
        // No additional parsing needed

    } else if (strcmp(cmd, "print") == 0 || strcmp(cmd, "echo") == 0) {
        op->kind = OP_PRINT;

        // Parse string
        if (*idx >= token_count)
            return E_PARSE;
        const char* str = tokens[*idx];
        (*idx)++;

        if (strlen(str) == 0)
            return E_BAD_NEEDLE;

        // Parse string to String
        enum Err err = parse_string_to_bytes(str, &op->u.print.string, str_pool, str_pool_off, str_pool_cap);
        if (err != E_OK)
            return err;

    } else if (strcmp(cmd, "sleep") == 0) {
        op->kind = OP_SLEEP;

        if (*idx >= token_count)
            return E_PARSE;
        i32 msec = 0;
        enum Err err = parse_milliseconds(tokens[*idx], &msec);
        if (err != E_OK)
            return err;
        (*idx)++;
        op->u.sleep.msec = msec;

    } else {
        return E_PARSE;
    }

    return E_OK;
}

static enum Err parse_loc_expr_build(char** tokens, i32* idx, i32 token_count, LocExpr* loc, Program* prg)
{
    if (*idx >= token_count)
        return E_PARSE;

    const char* token = tokens[*idx];
    (*idx)++;

    const char* offset_start = find_inline_offset_start(token);
    if (offset_start) {
        // Parse base part
        char base_token[256];
        size_t base_len = offset_start - token;
        if (base_len >= sizeof(base_token))
            return E_PARSE;
        strncpy(base_token, token, base_len);
        base_token[base_len] = '\0';

        // Parse offset part
        enum Err err = parse_signed_number(offset_start, &loc->offset, &loc->unit);
        if (err != E_OK)
            return err;

        token = base_token;
    } else {
        // No offset - initialize to defaults
        loc->offset = 0;
        loc->unit = UNIT_BYTES; // default unit
    }

    // Parse base location
    if (strcmp(token, "cursor") == 0) {
        loc->base = LOC_CURSOR;
    } else if (strcmp(token, "BOF") == 0) {
        loc->base = LOC_BOF;
    } else if (strcmp(token, "EOF") == 0) {
        loc->base = LOC_EOF;
    } else if (strcmp(token, "match-start") == 0) {
        loc->base = LOC_MATCH_START;
    } else if (strcmp(token, "match-end") == 0) {
        loc->base = LOC_MATCH_END;
    } else if (strcmp(token, "line-start") == 0) {
        loc->base = LOC_LINE_START;
    } else if (strcmp(token, "line-end") == 0) {
        loc->base = LOC_LINE_END;
    } else if (is_valid_label_name(token)) {
        loc->base = LOC_NAME;
        i32 name_idx = find_or_add_name_build(prg, token);
        if (name_idx < 0)
            return E_OOM;
        loc->name_idx = name_idx;
    } else {
        return E_PARSE;
    }

    // Support detached offset as next token (e.g., "BOF +100b")
    if (*idx < token_count) {
        i64 offset_tmp;
        enum Unit unit_tmp;
        enum Err off_err = parse_signed_number(tokens[*idx], &offset_tmp, &unit_tmp);
        if (off_err == E_OK) {
            loc->offset = offset_tmp;
            loc->unit = unit_tmp;
            (*idx)++; // consume detached offset token
        }
    }

    return E_OK;
}

static enum Err parse_at_expr_build(char** tokens, i32* idx, i32 token_count, LocExpr* at)
{
    if (*idx >= token_count)
        return E_PARSE;

    const char* token = tokens[*idx];
    (*idx)++;

    const char* offset_start = find_inline_offset_start(token);
    if (offset_start) {
        // Parse base part
        char base_token[256];
        size_t base_len = offset_start - token;
        if (base_len >= sizeof(base_token))
            return E_PARSE;
        strncpy(base_token, token, base_len);
        base_token[base_len] = '\0';

        // Parse offset part
        enum Err err = parse_signed_number(offset_start, &at->offset, &at->unit);
        if (err != E_OK)
            return err;

        token = base_token;
    } else {
        // No offset - initialize to defaults
        at->offset = 0;
        at->unit = UNIT_BYTES; // default unit
    }

    // Parse base location
    if (strcmp(token, "match-start") == 0) {
        at->base = LOC_MATCH_START;
    } else if (strcmp(token, "match-end") == 0) {
        at->base = LOC_MATCH_END;
    } else if (strcmp(token, "line-start") == 0) {
        at->base = LOC_LINE_START;
    } else if (strcmp(token, "line-end") == 0) {
        at->base = LOC_LINE_END;
    } else {
        return E_PARSE;
    }
    
    // AtExpr never uses named labels, so set name_idx to -1
    at->name_idx = -1;

    // Support detached offset as next token (e.g., "line-start -2l")
    if (*idx < token_count) {
        i64 offset_tmp;
        enum Unit unit_tmp;
        enum Err off_err = parse_signed_number(tokens[*idx], &offset_tmp, &unit_tmp);
        if (off_err == E_OK) {
            at->offset = offset_tmp;
            at->unit = unit_tmp;
            (*idx)++; // consume
        }
    }

    return E_OK;
}

static enum Err parse_unsigned_number(const char* token, i32* sign, u64* n, enum Unit* unit)
{
    if (!token || strlen(token) == 0)
        return E_PARSE;

    const char* p = token;

    // Parse sign
    if (sign) {
        if (*p == '+') {
            *sign = 1;
            p++;
        } else if (*p == '-') {
            *sign = -1;
            p++;
        } else {
            *sign = 1; // Default positive
        }
    } else {
        // For skip, no sign allowed
        if (*p == '+' || *p == '-') {
            return E_PARSE;
        }
    }

    // Parse number
    if (!isdigit(*p))
        return E_PARSE;

    u64 num = 0;
    while (isdigit(*p)) {
        u64 new_num = num * 10 + (*p - '0');
        if (new_num < num)
            return E_PARSE; // Overflow
        num = new_num;
        p++;
    }

    // Parse unit
    if (*p == 'b') {
        *unit = UNIT_BYTES;
        p++;
    } else if (*p == 'l') {
        *unit = UNIT_LINES;
        p++;
    } else if (*p == 'c') {
        *unit = UNIT_CHARS;
        p++;
    } else {
        return E_PARSE;
    }

    if (*p != '\0')
        return E_PARSE; // Extra characters

    *n = num;
    return E_OK;
}

static enum Err parse_signed_number(const char* token, i64* offset, enum Unit* unit)
{
    if (!token || strlen(token) == 0)
        return E_PARSE;

    const char* p = token;

    // Parse sign
    i32 sign = 1;
    if (*p == '+') {
        sign = 1;
        p++;
    } else if (*p == '-') {
        sign = -1;
        p++;
    }

    // Parse number
    if (!isdigit(*p))
        return E_PARSE;

    u64 num = 0;
    while (isdigit(*p)) {
        u64 new_num = num * 10 + (*p - '0');
        if (new_num < num)
            return E_PARSE; // Overflow
        num = new_num;
        p++;
    }

    // Parse unit
    if (*p == 'b') {
        *unit = UNIT_BYTES;
        p++;
    } else if (*p == 'l') {
        *unit = UNIT_LINES;
        p++;
    } else if (*p == 'c') {
        *unit = UNIT_CHARS;
        p++;
    } else {
        return E_PARSE;
    }

    if (*p != '\0')
        return E_PARSE; // Extra characters

    // Convert to signed and apply sign
    if (num > (u64)INT64_MAX)
        return E_PARSE; // Overflow
    *offset = sign * (i64)num;
    return E_OK;
}

static enum Err parse_milliseconds(const char* token, i32* out_ms)
{
    if (!token)
        return E_PARSE;

    size_t len = strlen(token);
    if (len < 3)
        return E_PARSE;

    if (token[len - 2] != 'm' || token[len - 1] != 's')
        return E_PARSE;

    size_t num_len = len - 2;
    if (num_len == 0)
        return E_PARSE;

    u64 value = 0;
    for (size_t i = 0; i < num_len; ++i) {
        char c = token[i];
        if (!isdigit((unsigned char)c))
            return E_PARSE;
        value = value * 10 + (u64)(c - '0');
        if (value > INT32_MAX)
            return E_PARSE;
    }

    *out_ms = (i32)value;
    return E_OK;
}

static bool is_valid_label_name(const char* name)
{
    if (!name || strlen(name) == 0 || strlen(name) > 16) {
        return false;
    }

    // First character must be uppercase
    if (!isupper(name[0])) {
        return false;
    }

    // Remaining characters can be uppercase, underscore, hyphen, or digit
    for (const char* p = name + 1; *p; p++) {
        if (!isupper(*p) && *p != '_' && *p != '-' && !isdigit(*p)) {
            return false;
        }
    }

    return true;
}

static enum Err parse_string_to_bytes(const char* str, String* out_string, char* str_pool, size_t* str_pool_off, size_t str_pool_cap)
{
    // Process escape sequences and calculate final length
    size_t src_len = strlen(str);
    size_t dst_len = 0;
    
    // First pass: calculate the final length after processing escapes
    for (size_t i = 0; i < src_len; i++) {
        if (str[i] == '\\' && i + 1 < src_len) {
            // Escape sequence
            char esc = str[i + 1];
            switch (esc) {
            case 'n': case 't': case 'r': case '0': case '\\':
                dst_len++;
                i++; // skip the escape character
                break;
            default:
                // Unknown escape - treat as literal backslash
                dst_len++;
                break;
            }
        } else {
            dst_len++;
        }
    }
    
    // Check if we have space in the string pool
    if (*str_pool_off + dst_len > str_pool_cap)
        return E_OOM;
    
    // Second pass: process escapes and copy to pool
    char* dst = str_pool + *str_pool_off;
    size_t dst_pos = 0;
    
    for (size_t i = 0; i < src_len; i++) {
        if (str[i] == '\\' && i + 1 < src_len) {
            // Escape sequence
            char esc = str[i + 1];
            switch (esc) {
            case 'n':
                dst[dst_pos++] = '\n';
                i++;
                break;
            case 't':
                dst[dst_pos++] = '\t';
                i++;
                break;
            case 'r':
                dst[dst_pos++] = '\r';
                i++;
                break;
            case '0':
                dst[dst_pos++] = '\0';
                i++;
                break;
            case '\\':
                dst[dst_pos++] = '\\';
                i++;
                break;
            default:
                // Unknown escape - treat as literal backslash
                dst[dst_pos++] = '\\';
                break;
            }
        } else {
            dst[dst_pos++] = str[i];
        }
    }
    
    *str_pool_off += dst_len;
    
    // Return String struct
    out_string->bytes = dst;
    out_string->len = (i32)dst_len;
    
    return E_OK;
}

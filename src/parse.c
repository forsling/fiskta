#include "fiskta.h"
#include "parse_plan.h"
#include "util.h"
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define TOK_BYTES(tokens, idx) ((tokens)[idx].bytes)
#define TOK_FIRST(tokens, idx) string_first((tokens)[idx])

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

static enum Err parse_op_build(const String* tokens, i32* idx, i32 token_count, Op* op, Program* prg,
    char* str_pool, size_t* str_pool_off, size_t str_pool_cap);
static enum Err parse_loc_expr_build(const String* tokens, i32* idx, i32 token_count, LocExpr* loc, Program* prg);
static enum Err parse_at_expr_build(const String* tokens, i32* idx, i32 token_count, LocExpr* at);
static enum Err parse_unsigned_number(String token, i32* sign, u64* n, Unit* unit);
static enum Err parse_signed_number(String token, i64* offset, Unit* unit);
static i32 find_or_add_name_build(Program* prg, String name);
static bool is_valid_label_name(String name);
enum Err parse_preflight(i32 token_count, const String* tokens, const char* in_path, ParsePlan* plan, const char** in_path_out)
{
    memset(plan, 0, sizeof(*plan));

    if (token_count < 1) {
        return E_PARSE;
    }

    *in_path_out = in_path;

    plan->clause_count = 1;
    for (i32 i = 0; i < token_count; i++) {
        if (string_eq_keyword(tokens[i], &KW_THEN) || string_eq_keyword(tokens[i], &KW_OR)) {
            plan->clause_count++;
        }
    }

    i32 idx = 0;
    while (idx < token_count) {
        while (idx < token_count && !string_eq_keyword(tokens[idx], &KW_THEN) && !string_eq_keyword(tokens[idx], &KW_OR)) {
            const String cmd_tok = tokens[idx];
            plan->total_ops++;

            if (string_eq_keyword(cmd_tok, &KW_FIND)) {
                idx++;
                if (idx < token_count && string_eq_keyword(tokens[idx], &KW_TO)) {
                    idx++;
                    if (idx < token_count)
                        idx++;
                    if (idx < token_count) {
                        char first = string_first(tokens[idx]);
                        if (first == '+' || first == '-')
                            idx++;
                    }
                }
                if (idx < token_count) {
                    plan->needle_count++;
                    plan->needle_bytes += (size_t)tokens[idx].len;
                    idx++;
                }
            } else if (string_eq_cstr(cmd_tok, "find:re")) {
                idx++;
                if (idx < token_count && string_eq_keyword(tokens[idx], &KW_TO)) {
                    idx++;
                    if (idx < token_count)
                        idx++;
                    if (idx < token_count) {
                        char first = string_first(tokens[idx]);
                        if (first == '+' || first == '-')
                            idx++;
                    }
                }
                if (idx < token_count) {
                    const String pat_tok = tokens[idx];
                    const char* pat = pat_tok.bytes;
                    size_t L = (size_t)pat_tok.len;
                    plan->sum_findr_ops++;
                    i32 est = (i32)(4 * L + 8);
                    plan->re_ins_estimate += est;
                    if (est > plan->re_ins_estimate_max)
                        plan->re_ins_estimate_max = est;
                    for (i32 pi = 0; pi < pat_tok.len; ++pi) {
                        char c = pat[pi];
                        if (c == '[')
                            plan->re_classes_estimate++;
                        if (c == '\\' && pi + 1 < pat_tok.len) {
                            char next = pat[pi + 1];
                            if (string_char_in_set(next, "dDwWsS"))
                                plan->re_classes_estimate++;
                        }
                    }
                    plan->needle_count++;
                    plan->needle_bytes += L;
                    idx++;
                }
            } else if (string_eq_cstr(cmd_tok, "find:bin")) {
                idx++;
                if (idx < token_count && string_eq_keyword(tokens[idx], &KW_TO)) {
                    idx++;
                    if (idx < token_count)
                        idx++;
                    if (idx < token_count) {
                        char first = string_first(tokens[idx]);
                        if (first == '+' || first == '-')
                            idx++;
                    }
                }
                if (idx < token_count) {
                    const String hex_tok = tokens[idx];
                    size_t hex_digits = 0;
                    for (i32 pi = 0; pi < hex_tok.len; ++pi) {
                        unsigned char c = (unsigned char)hex_tok.bytes[pi];
                        if (!isspace(c))
                            hex_digits++;
                    }
                    plan->needle_count++;
                    plan->needle_bytes += hex_digits / 2;
                    idx++;
                }
            } else if (string_eq_cstr(cmd_tok, "skip")) {
                idx++;
                if (idx < token_count)
                    idx++;
            } else if (string_eq_cstr(cmd_tok, "take")) {
                idx++;
                if (idx < token_count) {
                    const String next_tok = tokens[idx];
                    if (string_eq_cstr(next_tok, "to")) {
                        plan->sum_take_ops++;
                        idx++;
                        if (idx < token_count)
                            idx++;
                        if (idx < token_count) {
                            char first = string_first(tokens[idx]);
                            if (first == '+' || first == '-')
                                idx++;
                        }
                    } else if (string_eq_cstr(next_tok, "until:re")) {
                        plan->sum_take_ops++;
                        idx++;
                        if (idx < token_count) {
                            const String pat_tok = tokens[idx];
                            const char* pat = pat_tok.bytes;
                            size_t L = (size_t)pat_tok.len;
                            plan->sum_findr_ops++;
                            i32 est = (i32)(4 * L + 8);
                            plan->re_ins_estimate += est;
                            if (est > plan->re_ins_estimate_max)
                                plan->re_ins_estimate_max = est;
                            for (i32 pi = 0; pi < pat_tok.len; ++pi) {
                                char c = pat[pi];
                                if (c == '[')
                                    plan->re_classes_estimate++;
                                if (c == '\\' && pi + 1 < pat_tok.len) {
                                    char next_c = pat[pi + 1];
                                    if (string_char_in_set(next_c, "dDwWsS"))
                                        plan->re_classes_estimate++;
                                }
                            }
                            plan->needle_count++;
                            plan->needle_bytes += L;
                            idx++;
                        }
                        if (idx < token_count && string_eq_cstr(tokens[idx], "at")) {
                            idx++;
                            if (idx < token_count)
                                idx++;
                            if (idx < token_count) {
                                char first = string_first(tokens[idx]);
                                if (first == '+' || first == '-')
                                    idx++;
                            }
                        }
                    } else if (string_eq_cstr(next_tok, "until:bin")) {
                        plan->sum_take_ops++;
                        idx++;
                        if (idx < token_count) {
                            const String hex_tok = tokens[idx];
                            size_t hex_digits = 0;
                            for (i32 pi = 0; pi < hex_tok.len; ++pi) {
                                unsigned char c = (unsigned char)hex_tok.bytes[pi];
                                if (!isspace(c))
                                    hex_digits++;
                            }
                            plan->needle_count++;
                            plan->needle_bytes += hex_digits / 2;
                            idx++;
                        }
                        if (idx < token_count && string_eq_cstr(tokens[idx], "at")) {
                            idx++;
                            if (idx < token_count)
                                idx++;
                            if (idx < token_count) {
                                char first = string_first(tokens[idx]);
                                if (first == '+' || first == '-')
                                    idx++;
                            }
                        }
                    } else if (string_eq_cstr(next_tok, "until")) {
                        plan->sum_take_ops++;
                        idx++;
                        if (idx < token_count) {
                            plan->needle_count++;
                            plan->needle_bytes += (size_t)tokens[idx].len;
                            idx++;
                        }
                        if (idx < token_count && string_eq_cstr(tokens[idx], "at")) {
                            idx++;
                            if (idx < token_count)
                                idx++;
                            if (idx < token_count) {
                                char first = string_first(tokens[idx]);
                                if (first == '+' || first == '-')
                                    idx++;
                            }
                        }
                    } else {
                        plan->sum_take_ops++;
                        if (string_eq_cstr(next_tok, "len")) {
                            idx++;
                            if (idx < token_count)
                                idx++;
                        } else {
                            idx++;
                        }
                    }
                }
            } else if (string_eq_cstr(cmd_tok, "label")) {
                plan->sum_label_ops++;
                idx++;
                if (idx < token_count)
                    idx++;
            } else if (string_eq_cstr(cmd_tok, "goto")) {
                idx++;
                if (idx < token_count)
                    idx++;
                if (idx < token_count) {
                    char first = string_first(tokens[idx]);
                    if (first == '+' || first == '-')
                        idx++;
                }
            } else if (string_eq_cstr(cmd_tok, "view")) {
                idx++;
                if (idx < token_count)
                    idx++;
                if (idx < token_count) {
                    char first = string_first(tokens[idx]);
                    if (first == '+' || first == '-')
                        idx++;
                }
                if (idx < token_count)
                    idx++;
                if (idx < token_count) {
                    char first = string_first(tokens[idx]);
                    if (first == '+' || first == '-')
                        idx++;
                }
            } else if (string_eq_cstr(cmd_tok, "clear")) {
                idx++;
                if (idx < token_count)
                    idx++;
            } else if (string_eq_cstr(cmd_tok, "print") || string_eq_cstr(cmd_tok, "echo")) {
                idx++;
                if (idx < token_count) {
                    plan->needle_count++;
                    plan->needle_bytes += (size_t)tokens[idx].len;
                    plan->sum_take_ops++;
                    idx++;
                }
            } else if (string_eq_cstr(cmd_tok, "fail")) {
                idx++;
                if (idx < token_count) {
                    plan->needle_count++;
                    plan->needle_bytes += (size_t)tokens[idx].len;
                    idx++;
                }
            } else {
                return E_PARSE;
            }
        }

        if (idx < token_count && (string_eq_cstr(tokens[idx], "THEN") || string_eq_cstr(tokens[idx], "OR"))) {
            idx++;
        }
    }

    return E_OK;
}


enum Err parse_build(i32 token_count, const String* tokens, const char* in_path, Program* prg, const char** in_path_out,
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
        clause->link = LINK_NONE; // Default to no link

        // Count ops in this clause first
        i32 clause_start = idx;
        i32 clause_op_count = 0;
        while (idx < token_count && !string_eq_keyword(tokens[idx], &KW_THEN) && !string_eq_keyword(tokens[idx], &KW_OR)) {
            String cmd_tok = tokens[idx];
            clause_op_count++;
            idx++;

            // Skip command-specific tokens
            if (string_eq_cstr(cmd_tok, "find") || string_eq_cstr(cmd_tok, "find:re") || string_eq_cstr(cmd_tok, "find:bin")) {
                if (idx < token_count && string_eq_keyword(tokens[idx], &KW_TO)) {
                    idx++;
                    if (idx < token_count)
                        idx++; // skip location
                    if (idx < token_count && (TOK_FIRST(tokens, idx) == '+' || TOK_FIRST(tokens, idx) == '-')) {
                        idx++; // skip offset
                    }
                }
                if (idx < token_count)
                    idx++; // skip needle/pattern/hex
            } else if (string_eq_cstr(cmd_tok, "skip")) {
                if (idx < token_count)
                    idx++; // skip number+unit
            } else if (string_eq_cstr(cmd_tok, "take")) {
                if (idx < token_count) {
                    const String next_tok = tokens[idx];
                    if (string_eq_cstr(next_tok, "to")) {
                        idx++;
                        if (idx < token_count)
                            idx++; // skip location
                        if (idx < token_count && (TOK_FIRST(tokens, idx) == '+' || TOK_FIRST(tokens, idx) == '-')) {
                            idx++; // skip offset
                        }
                    } else if (string_eq_cstr(next_tok, "until") || string_eq_cstr(next_tok, "until:re") || string_eq_cstr(next_tok, "until:bin")) {
                        idx++;
                        if (idx < token_count)
                            idx++; // skip needle/pattern/hex
                        if (idx < token_count && string_eq_cstr(tokens[idx], "at")) {
                            idx++;
                            if (idx < token_count)
                                idx++; // skip location
                            if (idx < token_count && (TOK_FIRST(tokens, idx) == '+' || TOK_FIRST(tokens, idx) == '-')) {
                                idx++; // skip offset
                            }
                        }
                    } else {
                        if (string_eq_cstr(next_tok, "len")) {
                            idx++;
                            if (idx < token_count)
                                idx++;
                        } else {
                            idx++;
                        }
                    }
                }
            } else if (string_eq_cstr(cmd_tok, "label")) {
                if (idx < token_count)
                    idx++; // skip name
            } else if (string_eq_cstr(cmd_tok, "goto")) {
                if (idx < token_count)
                    idx++; // skip location
                if (idx < token_count && (TOK_FIRST(tokens, idx) == '+' || TOK_FIRST(tokens, idx) == '-')) {
                    idx++; // skip offset
                }
            } else if (string_eq_cstr(cmd_tok, "view")) {
                if (idx < token_count)
                    idx++; // skip first location
                if (idx < token_count && (TOK_FIRST(tokens, idx) == '+' || TOK_FIRST(tokens, idx) == '-')) {
                    idx++; // skip offset
                }
                if (idx < token_count)
                    idx++; // skip second location
                if (idx < token_count && (TOK_FIRST(tokens, idx) == '+' || TOK_FIRST(tokens, idx) == '-')) {
                    idx++; // skip offset
                }
            } else if (string_eq_cstr(cmd_tok, "clear")) {
                if (idx < token_count)
                    idx++; // skip "view" or label name
            } else if (string_eq_cstr(cmd_tok, "print") || string_eq_cstr(cmd_tok, "echo")) {
                idx++; // skip command token
                if (idx < token_count)
                    idx++; // skip string
            } else if (string_eq_cstr(cmd_tok, "fail")) {
                idx++; // skip command token
                if (idx < token_count)
                    idx++; // skip message
            }
        }

        // 3 Reset idx to clause start and parse for real
        idx = clause_start;
        while (idx < token_count && !string_eq_keyword(tokens[idx], &KW_THEN) && !string_eq_keyword(tokens[idx], &KW_OR)) {
            Op* op = &clause->ops[clause->op_count];
            enum Err err = parse_op_build(tokens, &idx, token_count, op, prg, str_pool, &str_pool_off, str_pool_cap);
            if (err != E_OK) {
                return err;
            }
            clause->op_count++;
        }

        prg->clause_count++;
        op_cursor += clause_op_count;

        // Check for link keywords
        if (idx < token_count) {
            if (string_eq_keyword(tokens[idx], &KW_OR)) {
                clause->link = LINK_OR;
                idx++;
            } else if (string_eq_keyword(tokens[idx], &KW_THEN)) {
                clause->link = LINK_THEN;
                idx++;
            }
        }
    }

    return E_OK;
}

static i32 find_or_add_name_build(Program* prg, String name)
{
    // Linear search for existing name
    for (i32 i = 0; i < prg->name_count; i++) {
        if (string_eq_cstr(name, prg->names[i])) {
            return i;
        }
    }

    // Add new name if space available
    if (prg->name_count < 128) {
        if (!string_copy_to_buffer(name, prg->names[prg->name_count], sizeof(prg->names[0])))
            return -1; // Copy failed
        return prg->name_count++;
    }

    return -1; // No space
}

static enum Err parse_op_build(const String* tokens, i32* idx, i32 token_count, Op* op, Program* prg,
    char* str_pool, size_t* str_pool_off, size_t str_pool_cap)
{
    if (*idx >= token_count) {
        return E_PARSE;
    }

    const String cmd_tok = tokens[*idx];
    (*idx)++;

    /************************************************************
     * SEARCH OPERATIONS
     ************************************************************/
    if (string_eq_cstr(cmd_tok, "find")) {
        op->kind = OP_FIND;

        if (*idx < token_count && string_eq_cstr(tokens[*idx], "to")) {
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
        const String needle_tok = tokens[*idx];
        (*idx)++;

        if (needle_tok.len == 0)
            return E_BAD_NEEDLE;

        enum Err err = E_OK;
        op->u.find.needle = parse_string_to_bytes(needle_tok, str_pool, str_pool_off, str_pool_cap, &err);
        if (err != E_OK)
            return err;

    } else if (string_eq_cstr(cmd_tok, "find:re")) {
        op->kind = OP_FIND_RE;

        if (*idx < token_count && string_eq_cstr(tokens[*idx], "to")) {
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
        const String pat_tok = tokens[*idx];
        (*idx)++;
        enum Err err = E_OK;
        op->u.findr.pattern = parse_string_to_bytes(pat_tok, str_pool, str_pool_off, str_pool_cap, &err);
        if (err != E_OK)
            return err;
        op->u.findr.prog = NULL;

    } else if (string_eq_cstr(cmd_tok, "find:bin")) {
        op->kind = OP_FIND_BIN;

        if (*idx < token_count && string_eq_cstr(tokens[*idx], "to")) {
            (*idx)++;
            enum Err err = parse_loc_expr_build(tokens, idx, token_count, &op->u.findbin.to, prg);
            if (err != E_OK)
                return err;
        } else {
            // Default to EOF
            op->u.findbin.to.base = LOC_EOF;
            op->u.findbin.to.name_idx = -1;
            op->u.findbin.to.offset = 0;
            op->u.findbin.to.unit = UNIT_BYTES;
        }

        // Parse hex string
        if (*idx >= token_count)
            return E_PARSE;
        const String hex_tok = tokens[*idx];
        (*idx)++;

        if (hex_tok.len == 0)
            return E_BAD_NEEDLE;

        enum Err err = E_OK;
        op->u.findbin.needle = parse_hex_to_bytes(hex_tok, str_pool, str_pool_off, str_pool_cap, &err);
        if (err != E_OK)
            return err;

        /************************************************************
         * MOVEMENT OPERATIONS
         ************************************************************/
    } else if (string_eq_cstr(cmd_tok, "skip")) {
        op->kind = OP_SKIP;

        if (*idx >= token_count)
            return E_PARSE;
        enum Err err = parse_unsigned_number(tokens[*idx], NULL, &op->u.skip.n, &op->u.skip.unit);
        if (err != E_OK)
            return err;
        (*idx)++;

        /************************************************************
         * EXTRACTION OPERATIONS
         ************************************************************/
    } else if (string_eq_cstr(cmd_tok, "take")) {
        if (*idx >= token_count)
            return E_PARSE;

        const String next_tok = tokens[*idx];
        if (string_eq_cstr(next_tok, "to")) {
            op->kind = OP_TAKE_TO;
            (*idx)++;
            enum Err err = parse_loc_expr_build(tokens, idx, token_count, &op->u.take_to.to, prg);
            if (err != E_OK)
                return err;
        } else if (string_eq_cstr(next_tok, "until:re")) {
            op->kind = OP_TAKE_UNTIL_RE;
            (*idx)++;

            // Parse pattern
            if (*idx >= token_count)
                return E_PARSE;
            const String pattern_tok = tokens[*idx];
            (*idx)++;

            if (pattern_tok.len == 0)
                return E_BAD_NEEDLE;

            enum Err err = E_OK;
            op->u.take_until_re.pattern = parse_string_to_bytes(pattern_tok, str_pool, str_pool_off, str_pool_cap, &err);
            if (err != E_OK)
                return err;

            // Parse "at" expression if present
            if (*idx < token_count && string_eq_cstr(tokens[*idx], "at")) {
                (*idx)++;
                op->u.take_until_re.has_at = true;
                enum Err err2 = parse_at_expr_build(tokens, idx, token_count, &op->u.take_until_re.at);
                if (err2 != E_OK)
                    return err2;
            } else {
                op->u.take_until_re.has_at = false;
            }
            op->u.take_until_re.prog = NULL;
        } else if (string_eq_cstr(next_tok, "until:bin")) {
            op->kind = OP_TAKE_UNTIL_BIN;
            (*idx)++;

            // Parse hex string
            if (*idx >= token_count)
                return E_PARSE;
            const String hex_tok = tokens[*idx];
            (*idx)++;

            if (hex_tok.len == 0)
                return E_BAD_NEEDLE;

            enum Err err = E_OK;
            op->u.take_until_bin.needle = parse_hex_to_bytes(hex_tok, str_pool, str_pool_off, str_pool_cap, &err);
            if (err != E_OK)
                return err;

            // Parse "at" expression if present
            if (*idx < token_count && string_eq_cstr(tokens[*idx], "at")) {
                (*idx)++;
                op->u.take_until_bin.has_at = true;
                enum Err err2 = parse_at_expr_build(tokens, idx, token_count, &op->u.take_until_bin.at);
                if (err2 != E_OK)
                    return err2;
            } else {
                op->u.take_until_bin.has_at = false;
            }
        } else if (string_eq_cstr(next_tok, "until")) {
            op->kind = OP_TAKE_UNTIL;
            (*idx)++;

            // Parse needle
            if (*idx >= token_count)
                return E_PARSE;
            const String needle_tok = tokens[*idx];
            (*idx)++;

            if (needle_tok.len == 0)
                return E_BAD_NEEDLE;

            enum Err err = E_OK;
            op->u.take_until.needle = parse_string_to_bytes(needle_tok, str_pool, str_pool_off, str_pool_cap, &err);
            if (err != E_OK)
                return err;

            // Parse "at" expression if present
            if (*idx < token_count && string_eq_cstr(tokens[*idx], "at")) {
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
            if (string_eq_cstr(next_tok, "len")) {
                (*idx)++;
                if (*idx >= token_count)
                    return E_PARSE;
            }
            enum Err err = parse_signed_number(tokens[*idx], &op->u.take_len.offset, &op->u.take_len.unit);
            if (err != E_OK)
                return err;
            (*idx)++;
        }

        /************************************************************
         * CONTROL OPERATIONS
         ************************************************************/
    } else if (string_eq_cstr(cmd_tok, "label")) {
        op->kind = OP_LABEL;

        if (*idx >= token_count)
            return E_PARSE;
        String name_tok = tokens[*idx];
        (*idx)++;

        if (!is_valid_label_name(name_tok))
            return E_LABEL_FMT;

        i32 name_idx = find_or_add_name_build(prg, name_tok);
        if (name_idx < 0)
            return E_OOM;
        op->u.label.name_idx = name_idx;

    } else if (string_eq_cstr(cmd_tok, "goto")) {
        op->kind = OP_GOTO;

        if (*idx >= token_count)
            return E_PARSE;
        enum Err err = parse_loc_expr_build(tokens, idx, token_count, &op->u.go.to, prg);
        if (err != E_OK)
            return err;

        /************************************************************
         * VIEW OPERATIONS
         ************************************************************/
    } else if (string_eq_cstr(cmd_tok, "view")) {
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

    } else if (string_eq_cstr(cmd_tok, "clear")) {
        // Parse second token to determine what to clear
        if (*idx >= token_count)
            return E_PARSE;

        String target_tok = tokens[*idx];
        (*idx)++;

        if (string_eq_cstr(target_tok, "view")) {
            op->kind = OP_VIEWCLEAR;
            // No additional parsing needed
        } else {
            // Future: clear <LABEL> - for now, error
            return E_PARSE;
        }

        /************************************************************
         * OUTPUT/UTILITY OPERATIONS
         ************************************************************/
    } else if (string_eq_cstr(cmd_tok, "print") || string_eq_cstr(cmd_tok, "echo")) {
        op->kind = OP_PRINT;

        // Parse string
        if (*idx >= token_count)
            return E_PARSE;
        const String str_tok = tokens[*idx];
        (*idx)++;

        if (str_tok.len == 0)
            return E_BAD_NEEDLE;

        enum Err err = E_OK;
        op->u.print.string = parse_string_to_bytes(str_tok, str_pool, str_pool_off, str_pool_cap, &err);
        if (err != E_OK)
            return err;

    } else if (string_eq_cstr(cmd_tok, "fail")) {
        op->kind = OP_FAIL;

        // Parse message
        if (*idx >= token_count)
            return E_PARSE;
        const String message_tok = tokens[*idx];
        (*idx)++;

        enum Err err = E_OK;
        op->u.fail.message = parse_string_to_bytes(message_tok, str_pool, str_pool_off, str_pool_cap, &err);
        if (err != E_OK)
            return err;

    } else {
        return E_PARSE;
    }

    return E_OK;
}

static enum Err parse_loc_expr_build(const String* tokens, i32* idx, i32 token_count, LocExpr* loc, Program* prg)
{
    if (*idx >= token_count)
        return E_PARSE;

    // Initialize defaults
    loc->name_idx = -1;

    String token_tok = tokens[*idx];
    const char* token = token_tok.bytes;
    (*idx)++;

    const char* offset_start = find_inline_offset_start(token);
    String base_tok;
    if (offset_start) {
        // Parse base part
        char base_token[256];
        size_t base_len = (size_t)(offset_start - token);
        if (base_len >= sizeof(base_token))
            return E_PARSE;
        // Create String for base part and copy to buffer
        String base_str = { token, (i32)base_len };
        if (!string_copy_to_buffer(base_str, base_token, sizeof(base_token)))
            return E_PARSE;

        // Parse offset part
        // Create String for offset part
        String offset_str = { offset_start, (i32)strlen(offset_start) };
        enum Err err = parse_signed_number(offset_str, &loc->offset, &loc->unit);
        if (err != E_OK)
            return err;

        // Create String token for base part
        base_tok.bytes = base_token;
        base_tok.len = (i32)base_len;
    } else {
        // No offset - use original token
        base_tok = token_tok;
        loc->offset = 0;
        loc->unit = UNIT_BYTES; // default unit
    }

    // Parse base location
    if (string_eq_cstr(base_tok, "cursor")) {
        loc->base = LOC_CURSOR;
    } else if (string_eq_cstr(base_tok, "BOF")) {
        loc->base = LOC_BOF;
    } else if (string_eq_cstr(base_tok, "EOF")) {
        loc->base = LOC_EOF;
    } else if (string_eq_cstr(base_tok, "match-start")) {
        loc->base = LOC_MATCH_START;
    } else if (string_eq_cstr(base_tok, "match-end")) {
        loc->base = LOC_MATCH_END;
    } else if (string_eq_cstr(base_tok, "line-start")) {
        loc->base = LOC_LINE_START;
    } else if (string_eq_cstr(base_tok, "line-end")) {
        loc->base = LOC_LINE_END;
    } else if (is_valid_label_name(base_tok)) {
        loc->base = LOC_NAME;
        i32 name_idx = find_or_add_name_build(prg, base_tok);
        if (name_idx < 0)
            return E_OOM;
        loc->name_idx = name_idx;
    } else {
        return E_PARSE;
    }

    // Support detached offset as next token (e.g., "BOF +100b")
    if (*idx < token_count) {
        i64 offset_tmp;
        Unit unit_tmp;
        enum Err off_err = parse_signed_number(tokens[*idx], &offset_tmp, &unit_tmp);
        if (off_err == E_OK) {
            loc->offset = offset_tmp;
            loc->unit = unit_tmp;
            (*idx)++; // consume detached offset token
        }
    }

    return E_OK;
}

static enum Err parse_at_expr_build(const String* tokens, i32* idx, i32 token_count, LocExpr* at)
{
    if (*idx >= token_count)
        return E_PARSE;

    String token_tok = tokens[*idx];
    const char* token = token_tok.bytes;
    (*idx)++;

    const char* offset_start = find_inline_offset_start(token);
    String base_tok;
    if (offset_start) {
        // Parse base part
        char base_token[256];
        size_t base_len = (size_t)(offset_start - token);
        if (base_len >= sizeof(base_token))
            return E_PARSE;
        // Create String for base part and copy to buffer
        String base_str = { token, (i32)base_len };
        if (!string_copy_to_buffer(base_str, base_token, sizeof(base_token)))
            return E_PARSE;

        // Parse offset part
        // Create String for offset part
        String offset_str = { offset_start, (i32)strlen(offset_start) };
        enum Err err = parse_signed_number(offset_str, &at->offset, &at->unit);
        if (err != E_OK)
            return err;

        // Create String token for base part
        base_tok.bytes = base_token;
        base_tok.len = (i32)base_len;
    } else {
        // No offset - use original token
        base_tok = token_tok;
        at->offset = 0;
        at->unit = UNIT_BYTES; // default unit
    }

    // Parse base location
    if (string_eq_cstr(base_tok, "match-start")) {
        at->base = LOC_MATCH_START;
    } else if (string_eq_cstr(base_tok, "match-end")) {
        at->base = LOC_MATCH_END;
    } else if (string_eq_cstr(base_tok, "line-start")) {
        at->base = LOC_LINE_START;
    } else if (string_eq_cstr(base_tok, "line-end")) {
        at->base = LOC_LINE_END;
    } else {
        return E_PARSE;
    }

    // AtExpr never uses named labels, so set name_idx to -1
    at->name_idx = -1;

    // Support detached offset as next token (e.g., "line-start -2l")
    if (*idx < token_count) {
        i64 offset_tmp;
        Unit unit_tmp;
        enum Err off_err = parse_signed_number(tokens[*idx], &offset_tmp, &unit_tmp);
        if (off_err == E_OK) {
            at->offset = offset_tmp;
            at->unit = unit_tmp;
            (*idx)++; // consume
        }
    }

    return E_OK;
}

static enum Err parse_unsigned_number(String token, i32* sign, u64* n, Unit* unit)
{
    if (token.len <= 0)
        return E_PARSE;

    const char* p = token.bytes;

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
        u64 new_num = num * 10 + (u64)(*p - '0');
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

    // Check if we consumed the entire token
    if (p != token.bytes + token.len)
        return E_PARSE; // Extra characters

    *n = num;
    return E_OK;
}

static enum Err parse_signed_number(String token, i64* offset, Unit* unit)
{
    if (token.len <= 0)
        return E_PARSE;

    const char* p = token.bytes;

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
        u64 new_num = num * 10 + (u64)(*p - '0');
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

    // Check if we consumed the entire token
    if (p != token.bytes + token.len)
        return E_PARSE; // Extra characters

    // Convert to signed and apply sign
    if (num > (u64)INT64_MAX)
        return E_PARSE; // Overflow
    *offset = sign * (i64)num;
    return E_OK;
}

static bool is_valid_label_name(String name)
{
    return string_is_valid_label(name);
}

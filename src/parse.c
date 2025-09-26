// parse.c
#include "fiskta.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int clause_count;
    int total_ops;
    int sum_take_ops;
    int sum_label_ops;
    int needle_count;
    size_t needle_bytes;
    int max_name_count;
} ParsePlan;

// Helper function to find inline offset start
static const char* find_inline_offset_start(const char* s) {
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

// Forward declarations
static enum Err parse_clause(char** tokens, int* idx, int token_count, Clause* clause, Program* prg);
static enum Err parse_op(char** tokens, int* idx, int token_count, Op* op, Program* prg);
static enum Err parse_op_build(char** tokens, int* idx, int token_count, Op* op, Program* prg,
                               char* str_pool, size_t* str_pool_off, size_t str_pool_cap, int max_name_count);
static enum Err parse_loc_expr(char** tokens, int* idx, int token_count, LocExpr* loc, Program* prg);
static enum Err parse_loc_expr_build(char** tokens, int* idx, int token_count, LocExpr* loc, Program* prg, int max_name_count);
static enum Err parse_at_expr(char** tokens, int* idx, int token_count, AtExpr* at);
static enum Err parse_at_expr_build(char** tokens, int* idx, int token_count, AtExpr* at, Program* prg);
static enum Err parse_signed_number(const char* token, int* sign, u64* n, enum Unit* unit);
static int find_or_add_name(Program* prg, const char* name);
static int find_or_add_name_build(Program* prg, const char* name, int max_name_count);
static bool is_valid_label_name(const char* name);

// Function declarations
enum Err parse_program(int argc, char** argv, Program* prg, const char** in_path_out);
void parse_free(Program* prg);
enum Err parse_preflight(int token_count, char** tokens, const char* in_path, ParsePlan* plan, const char** in_path_out);
enum Err parse_build(int token_count, char** tokens, const char* in_path, Program* prg, const char** in_path_out,
                     Clause* clauses_buf, Op* ops_buf,
                     char (*names_buf)[17], int max_name_count,
                     char* str_pool, size_t str_pool_cap);

enum Err parse_program(int argc, char** argv, Program* prg, const char** in_path_out)
{
    memset(prg, 0, sizeof(*prg));

    if (argc < 3) {
        return E_PARSE;
    }

    // Last argument is input path
    *in_path_out = argv[argc - 1];
    int token_count = argc - 2; // Exclude program name and input path
    char** tokens = argv + 1;

    // Initialize program
    prg->clauses = malloc(16 * sizeof(Clause));
    if (!prg->clauses)
        return E_OOM;
    prg->clause_cap = 16;
    prg->clause_count = 0;

    prg->names = malloc(32 * sizeof(char[17]));
    if (!prg->names) {
        free(prg->clauses);
        return E_OOM;
    }
    prg->name_cap = 32;
    prg->name_count = 0;

    // Parse clauses separated by "::"
    int idx = 0;
    while (idx < token_count) {
        Clause clause = { 0 };
        clause.ops = malloc(16 * sizeof(Op));
        if (!clause.ops) {
            parse_free(prg);
            return E_OOM;
        }
        clause.op_cap = 16;
        clause.op_count = 0;

        enum Err err = parse_clause(tokens, &idx, token_count, &clause, prg);
        if (err != E_OK) {
            free(clause.ops);
            parse_free(prg);
            return err;
        }

        // Add clause to program
        if (prg->clause_count >= prg->clause_cap) {
            prg->clause_cap *= 2;
            Clause* new_clauses = realloc(prg->clauses, prg->clause_cap * sizeof(Clause));
            if (!new_clauses) {
                free(clause.ops);
                parse_free(prg);
                return E_OOM;
            }
            prg->clauses = new_clauses;
        }

        prg->clauses[prg->clause_count++] = clause;

        // Skip semicolon if present
        if (idx < token_count && strcmp(tokens[idx], "::") == 0) {
            idx++;
        }
    }

    return E_OK;
}

void parse_free(Program* prg)
{
    // Arena-backed allocation - no cleanup needed
    memset(prg, 0, sizeof(*prg));
}

enum Err parse_preflight(int token_count, char** tokens, const char* in_path, ParsePlan* plan, const char** in_path_out)
{
    memset(plan, 0, sizeof(*plan));

    if (token_count < 1) {
        return E_PARSE;
    }

    // Input path is passed separately
    *in_path_out = in_path;

    // Count clauses (number of "::" + 1)
    plan->clause_count = 1;
    for (int i = 0; i < token_count; i++) {
        if (strcmp(tokens[i], "::") == 0) {
            plan->clause_count++;
        }
    }

    // Count operations and analyze needs
    int idx = 0;
    while (idx < token_count) {
        // Count ops in this clause
        int clause_start = idx;
        while (idx < token_count && strcmp(tokens[idx], "::") != 0) {
            const char* cmd = tokens[idx];
            plan->total_ops++;

            if (strcmp(cmd, "find") == 0) {
                idx++;
                if (idx < token_count && strcmp(tokens[idx], "to") == 0) {
                    idx++;
                    // Skip location expression
                    if (idx < token_count) {
                        const char* loc_token = tokens[idx];
                        if (is_valid_label_name(loc_token)) {
                            plan->max_name_count++;
                        }
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
            } else if (strcmp(cmd, "skip") == 0) {
                idx++;
                if (idx < token_count) idx++;
            } else if (strcmp(cmd, "take") == 0) {
                idx++;
                if (idx < token_count) {
                    const char* next = tokens[idx];
                    if (strcmp(next, "to") == 0) {
                        plan->sum_take_ops++;
                        idx++;
                        // Skip location expression
                        if (idx < token_count) {
                            const char* loc_token = tokens[idx];
                            if (is_valid_label_name(loc_token)) {
                                plan->max_name_count++;
                            }
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
                                const char* at_token = tokens[idx];
                                if (is_valid_label_name(at_token)) {
                                    plan->max_name_count++;
                                }
                                idx++;
                                // Skip offset if present
                                if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-')) {
                                    idx++;
                                }
                            }
                        }
                    } else {
                        // Length form
                        plan->sum_take_ops++;
                        idx++; // skip number+unit
                    }
                }
            } else if (strcmp(cmd, "label") == 0) {
                plan->sum_label_ops++;
                idx++;
                if (idx < token_count) {
                    const char* name = tokens[idx];
                    if (is_valid_label_name(name)) {
                        plan->max_name_count++;
                    }
                    idx++;
                }
            } else if (strcmp(cmd, "goto") == 0) {
                idx++;
                if (idx < token_count) {
                    const char* loc_token = tokens[idx];
                    if (is_valid_label_name(loc_token)) {
                        plan->max_name_count++;
                    }
                    idx++;
                    // Skip offset if present
                    if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-')) {
                        idx++;
                    }
                }
            } else {
                idx++; // skip unknown token
            }
        }

        if (idx < token_count && strcmp(tokens[idx], "::") == 0) {
            idx++;
        }
    }

    return E_OK;
}

enum Err parse_build(int token_count, char** tokens, const char* in_path, Program* prg, const char** in_path_out,
                     Clause* clauses_buf, Op* ops_buf,
                     char (*names_buf)[17], int max_name_count,
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
    prg->names = names_buf;
    prg->name_count = 0;

    // Track string pool usage
    size_t str_pool_off = 0;

    // Parse clauses separated by "::"
    int idx = 0;
    int op_cursor = 0;

    while (idx < token_count) {
        Clause* clause = &prg->clauses[prg->clause_count];
        clause->ops = ops_buf + op_cursor;
        clause->op_count = 0;

        // Count ops in this clause first
        int clause_start = idx;
        int clause_op_count = 0;
        while (idx < token_count && strcmp(tokens[idx], "::") != 0) {
            const char* cmd = tokens[idx];
            clause_op_count++;
            idx++;

            // Skip command-specific tokens
            if (strcmp(cmd, "find") == 0) {
                if (idx < token_count && strcmp(tokens[idx], "to") == 0) {
                    idx++;
                    if (idx < token_count) idx++; // skip location
                    if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-')) {
                        idx++; // skip offset
                    }
                }
                if (idx < token_count) idx++; // skip needle
            } else if (strcmp(cmd, "skip") == 0) {
                if (idx < token_count) idx++; // skip number+unit
            } else if (strcmp(cmd, "take") == 0) {
                if (idx < token_count) {
                    const char* next = tokens[idx];
                    if (strcmp(next, "to") == 0) {
                        idx++;
                        if (idx < token_count) idx++; // skip location
                        if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-')) {
                            idx++; // skip offset
                        }
                    } else if (strcmp(next, "until") == 0) {
                        idx++;
                        if (idx < token_count) idx++; // skip needle
                        if (idx < token_count && strcmp(tokens[idx], "at") == 0) {
                            idx++;
                            if (idx < token_count) idx++; // skip location
                            if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-')) {
                                idx++; // skip offset
                            }
                        }
                    } else {
                        idx++; // skip number+unit
                    }
                }
            } else if (strcmp(cmd, "label") == 0) {
                if (idx < token_count) idx++; // skip name
            } else if (strcmp(cmd, "goto") == 0) {
                if (idx < token_count) idx++; // skip location
                if (idx < token_count && (tokens[idx][0] == '+' || tokens[idx][0] == '-')) {
                    idx++; // skip offset
                }
            }
        }

        // Reset idx to clause start and parse for real
        idx = clause_start;
        while (idx < token_count && strcmp(tokens[idx], "::") != 0) {
            Op* op = &clause->ops[clause->op_count];
            enum Err err = parse_op_build(tokens, &idx, token_count, op, prg, str_pool, &str_pool_off, str_pool_cap, max_name_count);
            if (err != E_OK) {
                return err;
            }
            clause->op_count++;
        }

        prg->clause_count++;
        op_cursor += clause_op_count;

        if (idx < token_count && strcmp(tokens[idx], "::") == 0) {
            idx++;
        }
    }

    return E_OK;
}

static int find_or_add_name_build(Program* prg, const char* name, int max_name_count)
{
    // Linear search for existing name
    for (int i = 0; i < prg->name_count; i++) {
        if (strcmp(prg->names[i], name) == 0) {
            return i;
        }
    }

    // Add new name if space available
    if (prg->name_count < max_name_count) {
        strcpy(prg->names[prg->name_count], name);
        return prg->name_count++;
    }

    return -1; // No space
}

static enum Err parse_op_build(char** tokens, int* idx, int token_count, Op* op, Program* prg,
                               char* str_pool, size_t* str_pool_off, size_t str_pool_cap, int max_name_count)
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
            enum Err err = parse_loc_expr_build(tokens, idx, token_count, &op->u.find.to, prg, max_name_count);
            if (err != E_OK)
                return err;
        } else {
            // Default to EOF
            op->u.find.to.base = LOC_EOF;
            op->u.find.to.name_idx = -1;
            op->u.find.to.has_off = false;
        }

        // Parse needle
        if (*idx >= token_count)
            return E_PARSE;
        const char* needle = tokens[*idx];
        (*idx)++;

        if (strlen(needle) == 0)
            return E_BAD_NEEDLE;

        // Copy needle to string pool
        size_t needle_len = strlen(needle) + 1;
        if (*str_pool_off + needle_len > str_pool_cap)
            return E_OOM;

        op->u.find.needle = str_pool + *str_pool_off;
        strcpy(op->u.find.needle, needle);
        *str_pool_off += needle_len;

    } else if (strcmp(cmd, "skip") == 0) {
        op->kind = OP_SKIP;

        if (*idx >= token_count)
            return E_PARSE;
        enum Err err = parse_signed_number(tokens[*idx], NULL, &op->u.skip.n, &op->u.skip.unit);
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
            enum Err err = parse_loc_expr_build(tokens, idx, token_count, &op->u.take_to.to, prg, max_name_count);
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

            // Copy needle to string pool
            size_t needle_len = strlen(needle) + 1;
            if (*str_pool_off + needle_len > str_pool_cap)
                return E_OOM;

            op->u.take_until.needle = str_pool + *str_pool_off;
            strcpy(op->u.take_until.needle, needle);
            *str_pool_off += needle_len;

            // Parse "at" expression if present
            if (*idx < token_count && strcmp(tokens[*idx], "at") == 0) {
                (*idx)++;
                op->u.take_until.has_at = true;
                enum Err err = parse_at_expr_build(tokens, idx, token_count, &op->u.take_until.at, prg);
                if (err != E_OK)
                    return err;
            } else {
                op->u.take_until.has_at = false;
            }
        } else {
            op->kind = OP_TAKE_LEN;
            enum Err err = parse_signed_number(tokens[*idx], &op->u.take_len.sign, &op->u.take_len.n, &op->u.take_len.unit);
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

        int name_idx = find_or_add_name_build(prg, name, max_name_count);
        if (name_idx < 0)
            return E_OOM;
        op->u.label.name_idx = name_idx;

    } else if (strcmp(cmd, "goto") == 0) {
        op->kind = OP_GOTO;

        if (*idx >= token_count)
            return E_PARSE;
        enum Err err = parse_loc_expr_build(tokens, idx, token_count, &op->u.go.to, prg, max_name_count);
        if (err != E_OK)
            return err;

    } else {
        return E_PARSE;
    }

    return E_OK;
}

static enum Err parse_loc_expr_build(char** tokens, int* idx, int token_count, LocExpr* loc, Program* prg, int max_name_count)
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
        enum Err err = parse_signed_number(offset_start, &loc->sign, &loc->n, &loc->unit);
        if (err != E_OK)
            return err;
        loc->has_off = true;

        token = base_token;
    } else {
        loc->has_off = false;
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
        int name_idx = find_or_add_name_build(prg, token, max_name_count);
        if (name_idx < 0)
            return E_OOM;
        loc->name_idx = name_idx;
    } else {
        return E_PARSE;
    }

    // Support detached offset as next token (e.g., "BOF +100b")
    if (*idx < token_count) {
        int sign_tmp; u64 n_tmp; enum Unit unit_tmp;
        enum Err off_err = parse_signed_number(tokens[*idx], &sign_tmp, &n_tmp, &unit_tmp);
        if (off_err == E_OK) {
            loc->has_off = true;
            loc->sign   = sign_tmp;
            loc->n      = n_tmp;
            loc->unit   = unit_tmp;
            (*idx)++; // consume detached offset token
        }
    }

    return E_OK;
}

static enum Err parse_at_expr_build(char** tokens, int* idx, int token_count, AtExpr* at, Program* prg)
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
        enum Err err = parse_signed_number(offset_start, &at->sign, &at->n, &at->unit);
        if (err != E_OK)
            return err;
        at->has_off = true;

        token = base_token;
    } else {
        at->has_off = false;
    }

    // Parse base location
    if (strcmp(token, "match-start") == 0) {
        at->at = LOC_MATCH_START;
    } else if (strcmp(token, "match-end") == 0) {
        at->at = LOC_MATCH_END;
    } else if (strcmp(token, "line-start") == 0) {
        at->at = LOC_LINE_START;
    } else if (strcmp(token, "line-end") == 0) {
        at->at = LOC_LINE_END;
    } else {
        return E_PARSE;
    }

    // Support detached offset as next token (e.g., "line-start -2l")
    if (*idx < token_count) {
        int sign_tmp; u64 n_tmp; enum Unit unit_tmp;
        enum Err off_err = parse_signed_number(tokens[*idx], &sign_tmp, &n_tmp, &unit_tmp);
        if (off_err == E_OK) {
            at->has_off = true;
            at->sign = sign_tmp;
            at->n    = n_tmp;
            at->unit = unit_tmp;
            (*idx)++; // consume
        }
    }

    return E_OK;
}

static enum Err parse_clause(char** tokens, int* idx, int token_count, Clause* clause, Program* prg)
{
    while (*idx < token_count && strcmp(tokens[*idx], "::") != 0) {
        Op op = { 0 };
        enum Err err = parse_op(tokens, idx, token_count, &op, prg);
        if (err != E_OK) {
            return err;
        }

        // Add op to clause
        if (clause->op_count >= clause->op_cap) {
            clause->op_cap *= 2;
            Op* new_ops = realloc(clause->ops, clause->op_cap * sizeof(Op));
            if (!new_ops)
                return E_OOM;
            clause->ops = new_ops;
        }

        clause->ops[clause->op_count++] = op;
    }

    return E_OK;
}

static enum Err parse_op(char** tokens, int* idx, int token_count, Op* op, Program* prg)
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
            enum Err err = parse_loc_expr(tokens, idx, token_count, &op->u.find.to, prg);
            if (err != E_OK)
                return err;
        } else {
            // Default to EOF
            op->u.find.to.base = LOC_EOF;
            op->u.find.to.name_idx = -1;
            op->u.find.to.has_off = false;
        }

        // Parse needle
        if (*idx >= token_count)
            return E_PARSE;
        const char* needle = tokens[*idx];
        (*idx)++;

        if (strlen(needle) == 0)
            return E_BAD_NEEDLE;

        op->u.find.needle = malloc(strlen(needle) + 1);
        if (!op->u.find.needle)
            return E_OOM;
        strcpy(op->u.find.needle, needle);

    } else if (strcmp(cmd, "skip") == 0) {
        op->kind = OP_SKIP;

        if (*idx >= token_count)
            return E_PARSE;
        enum Err err = parse_signed_number(tokens[*idx], NULL, &op->u.skip.n, &op->u.skip.unit);
        if (err != E_OK)
            return err;
        (*idx)++;

    } else if (strcmp(cmd, "take") == 0) {
        if (*idx >= token_count)
            return E_PARSE;

        const char* arg = tokens[*idx];
        (*idx)++;

        if (strcmp(arg, "to") == 0) {
            op->kind = OP_TAKE_TO;
            enum Err err = parse_loc_expr(tokens, idx, token_count, &op->u.take_to.to, prg);
            if (err != E_OK)
                return err;

        } else if (strcmp(arg, "until") == 0) {
            op->kind = OP_TAKE_UNTIL;

            // Parse needle
            if (*idx >= token_count)
                return E_PARSE;
            const char* needle = tokens[*idx];
            (*idx)++;

            if (strlen(needle) == 0)
                return E_BAD_NEEDLE;

            op->u.take_until.needle = malloc(strlen(needle) + 1);
            if (!op->u.take_until.needle)
                return E_OOM;
            strcpy(op->u.take_until.needle, needle);

            // Check for "at" keyword
            if (*idx < token_count && strcmp(tokens[*idx], "at") == 0) {
                (*idx)++;
                op->u.take_until.has_at = true;
                enum Err err = parse_at_expr(tokens, idx, token_count, &op->u.take_until.at);
                if (err != E_OK)
                    return err;
            } else {
                op->u.take_until.has_at = false;
            }

        } else {
            // Signed number with unit
            op->kind = OP_TAKE_LEN;
            enum Err err = parse_signed_number(arg, &op->u.take_len.sign,
                &op->u.take_len.n, &op->u.take_len.unit);
            if (err != E_OK)
                return err;
        }

    } else if (strcmp(cmd, "label") == 0) {
        op->kind = OP_LABEL;

        if (*idx >= token_count)
            return E_PARSE;
        const char* name = tokens[*idx];
        (*idx)++;

        if (!is_valid_label_name(name))
            return E_LABEL_FMT;

        int idx_name = find_or_add_name(prg, name);
        if (idx_name < 0) return E_OOM;
        op->u.label.name_idx = idx_name;

    } else if (strcmp(cmd, "goto") == 0) {
        op->kind = OP_GOTO;
        enum Err err = parse_loc_expr(tokens, idx, token_count, &op->u.go.to, prg);
        if (err != E_OK)
            return err;

    } else {
        return E_PARSE;
    }

    return E_OK;
}

static enum Err parse_loc_expr(char** tokens, int* idx, int token_count, LocExpr* loc, Program* prg)
{
    if (*idx >= token_count)
        return E_PARSE;

    const char* token = tokens[*idx]; // may be "BOF" or "BOF+100b" etc.
    (*idx)++;

    const char* off_inline = find_inline_offset_start(token);
    size_t base_len = off_inline ? (size_t)(off_inline - token) : strlen(token);

    // copy base into a small buffer to compare
    char base_buf[32];
    if (base_len >= sizeof(base_buf)) return E_PARSE;
    memcpy(base_buf, token, base_len);
    base_buf[base_len] = '\0';

    // Parse base
    if (strcmp(base_buf, "cursor") == 0)      loc->base = LOC_CURSOR;
    else if (strcmp(base_buf, "BOF") == 0)    loc->base = LOC_BOF;
    else if (strcmp(base_buf, "EOF") == 0)    loc->base = LOC_EOF;
    else if (strcmp(base_buf, "match-start") == 0) loc->base = LOC_MATCH_START;
    else if (strcmp(base_buf, "match-end") == 0)   loc->base = LOC_MATCH_END;
    else if (strcmp(base_buf, "line-start") == 0)  loc->base = LOC_LINE_START;
    else if (strcmp(base_buf, "line-end") == 0)    loc->base = LOC_LINE_END;
    else if (is_valid_label_name(base_buf)) {
        loc->base = LOC_NAME;
        int idx_name = find_or_add_name(prg, base_buf);
        if (idx_name < 0) return E_OOM;
        loc->name_idx = idx_name;
    } else {
        return E_PARSE;
    }

    // Offsets: inline or next token
    loc->has_off = false;
    if (off_inline) {
        enum Err err = parse_signed_number(off_inline, &loc->sign, &loc->n, &loc->unit);
        if (err != E_OK) return err;
        loc->has_off = true;
    } else if (*idx < token_count) {
        enum Err err = parse_signed_number(tokens[*idx], &loc->sign, &loc->n, &loc->unit);
        if (err == E_OK) { loc->has_off = true; (*idx)++; }
    }

    return E_OK;
}

static enum Err parse_at_expr(char** tokens, int* idx, int token_count, AtExpr* at)
{
    if (*idx >= token_count)
        return E_PARSE;

    const char* token = tokens[*idx];
    (*idx)++;

    const char* off_inline = find_inline_offset_start(token);
    size_t base_len = off_inline ? (size_t)(off_inline - token) : strlen(token);

    char base_buf[32];
    if (base_len >= sizeof(base_buf)) return E_PARSE;
    memcpy(base_buf, token, base_len);
    base_buf[base_len] = '\0';

    if      (strcmp(base_buf, "match-start") == 0) at->at = LOC_MATCH_START;
    else if (strcmp(base_buf, "match-end")   == 0) at->at = LOC_MATCH_END;
    else if (strcmp(base_buf, "line-start")  == 0) at->at = LOC_LINE_START;
    else if (strcmp(base_buf, "line-end")    == 0) at->at = LOC_LINE_END;
    else return E_PARSE;

    at->has_off = false;
    if (off_inline) {
        enum Err err = parse_signed_number(off_inline, &at->sign, &at->n, &at->unit);
        if (err != E_OK) return err;
        at->has_off = true;
    } else if (*idx < token_count) {
        enum Err err = parse_signed_number(tokens[*idx], &at->sign, &at->n, &at->unit);
        if (err == E_OK) { at->has_off = true; (*idx)++; }
    }

    return E_OK;
}

static enum Err parse_signed_number(const char* token, int* sign, u64* n, enum Unit* unit)
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

static int find_or_add_name(Program* prg, const char* name)
{
    // Check if name already exists
    for (int i = 0; i < prg->name_count; i++) {
        if (strcmp(prg->names[i], name) == 0) {
            return i;
        }
    }

    // Add new name
    if (prg->name_count >= prg->name_cap) {
        prg->name_cap *= 2;
        char(*new_names)[17] = realloc(prg->names, prg->name_cap * sizeof(char[17]));
        if (!new_names)
            return -1;
        prg->names = new_names;
    }

    strcpy(prg->names[prg->name_count], name);
    return prg->name_count++;
}

static bool is_valid_label_name(const char* name)
{
    if (!name || strlen(name) == 0 || strlen(name) > 16) {
        return false;
    }

    for (const char* p = name; *p; p++) {
        if (!isupper(*p) && *p != '_' && *p != '-') {
            return false;
        }
    }

    return true;
}

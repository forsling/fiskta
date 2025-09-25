// parse.c
#include "fiskta.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// Forward declarations
static enum Err parse_clause(char** tokens, int* idx, int token_count, Clause* clause, Program* prg);
static enum Err parse_op(char** tokens, int* idx, int token_count, Op* op, Program* prg);
static enum Err parse_loc_expr(char** tokens, int* idx, int token_count, LocExpr* loc, Program* prg);
static enum Err parse_at_expr(char** tokens, int* idx, int token_count, AtExpr* at);
static enum Err parse_signed_number(const char* token, int* sign, u64* n, enum Unit* unit);
static int find_or_add_name(Program* prg, const char* name);
static bool is_valid_label_name(const char* name);

// Function declarations
enum Err parse_program(int argc, char** argv, Program* prg, const char** in_path_out);
void parse_free(Program* prg);

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
    if (!prg)
        return;

    // Free all clause ops
    for (int i = 0; i < prg->clause_count; i++) {
        Clause* clause = &prg->clauses[i];
        for (int j = 0; j < clause->op_count; j++) {
            Op* op = &clause->ops[j];
            // Free string allocations
            if (op->kind == OP_FIND && op->u.find.needle) {
                free(op->u.find.needle);
            } else if (op->kind == OP_TAKE_UNTIL && op->u.take_until.needle) {
                free(op->u.take_until.needle);
            }
        }
        free(clause->ops);
    }

    free(prg->clauses);
    free(prg->names);
    memset(prg, 0, sizeof(*prg));
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

        // Check for "to" keyword
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

        op->u.label.name_idx = find_or_add_name(prg, name);

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

    const char* base = tokens[*idx];
    (*idx)++;

    // Parse base location
    if (strcmp(base, "cursor") == 0) {
        loc->base = LOC_CURSOR;
    } else if (strcmp(base, "BOF") == 0) {
        loc->base = LOC_BOF;
    } else if (strcmp(base, "EOF") == 0) {
        loc->base = LOC_EOF;
    } else if (strcmp(base, "match-start") == 0) {
        loc->base = LOC_MATCH_START;
    } else if (strcmp(base, "match-end") == 0) {
        loc->base = LOC_MATCH_END;
    } else if (strcmp(base, "line-start") == 0) {
        loc->base = LOC_LINE_START;
    } else if (strcmp(base, "line-end") == 0) {
        loc->base = LOC_LINE_END;
    } else if (is_valid_label_name(base)) {
        loc->base = LOC_NAME;
        loc->name_idx = find_or_add_name(prg, base);
    } else {
        return E_PARSE;
    }

    // Check for offset
    if (*idx < token_count) {
        const char* offset_token = tokens[*idx];
        int sign;
        u64 n;
        enum Unit unit;

        enum Err err = parse_signed_number(offset_token, &sign, &n, &unit);
        if (err == E_OK) {
            loc->has_off = true;
            loc->sign = sign;
            loc->n = n;
            loc->unit = unit;
            (*idx)++;
        } else {
            loc->has_off = false;
        }
    } else {
        loc->has_off = false;
    }

    return E_OK;
}

static enum Err parse_at_expr(char** tokens, int* idx, int token_count, AtExpr* at)
{
    if (*idx >= token_count)
        return E_PARSE;

    const char* base = tokens[*idx];
    (*idx)++;

    if (strcmp(base, "match-start") == 0) {
        at->at = LOC_MATCH_START;
    } else if (strcmp(base, "match-end") == 0) {
        at->at = LOC_MATCH_END;
    } else if (strcmp(base, "line-start") == 0) {
        at->at = LOC_LINE_START;
    } else if (strcmp(base, "line-end") == 0) {
        at->at = LOC_LINE_END;
    } else {
        return E_PARSE;
    }

    // Check for offset
    if (*idx < token_count) {
        const char* offset_token = tokens[*idx];
        int sign;
        u64 n;
        enum Unit unit;

        enum Err err = parse_signed_number(offset_token, &sign, &n, &unit);
        if (err == E_OK) {
            at->has_off = true;
            at->sign = sign;
            at->n = n;
            at->unit = unit;
            (*idx)++;
        } else {
            at->has_off = false;
        }
    } else {
        at->has_off = false;
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

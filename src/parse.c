#include "parse.h"
#include "error.h"
#include "fiskta.h"
#include "fileio.h"
#include "util.h"
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define TOK_BYTES(tokens, idx) ((tokens)[idx].bytes)
#define TOK_FIRST(tokens, idx) string_first((tokens)[idx])

// Maximum pattern/needle length (16KB - generous for any legitimate pattern)
#define MAX_PATTERN_LENGTH 16384

// Helper function to skip optional token
static void skip_optional_token(i32* idx, i32 token_count)
{
    if (*idx < token_count) {
        (*idx)++;
    }
}

// Helper function to skip one token if available
static void skip_one_token(i32* idx, i32 token_count)
{
    if (*idx < token_count) {
        (*idx)++;
    }
}

static int parse_hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static bool compute_print_stats(String token, size_t* out_len, i32* out_segments, i32* out_marks)
{
    size_t len = 0;
    i32 segments = 0;
    i32 marks = 0;
    bool in_literal = false;

    for (size_t i = 0; i < (size_t)token.len; ++i) {
        char c = token.bytes[i];
        if (c == '\\' && i + 1 < (size_t)token.len) {
            char esc = token.bytes[i + 1];
            if (esc == 'n' || esc == 't' || esc == 'r' || esc == '0' || esc == '\\') {
                len++;
                if (!in_literal) {
                    in_literal = true;
                    segments++;
                }
                i++;
                continue;
            }
            if (esc == 'x') {
                if (i + 3 >= (size_t)token.len) {
                    return false;
                }
                int hi = parse_hex_digit(token.bytes[i + 2]);
                int lo = parse_hex_digit(token.bytes[i + 3]);
                if (hi < 0 || lo < 0) {
                    return false;
                }
                len++;
                if (!in_literal) {
                    in_literal = true;
                    segments++;
                }
                i += 3;
                continue;
            }
            if (esc == 'c' || esc == 'C') {
                len++;
                marks++;
                in_literal = false;
                i++;
                continue;
            }
            len++;
            if (!in_literal) {
                in_literal = true;
                segments++;
            }
            continue;
        }

        len++;
        if (!in_literal) {
            in_literal = true;
            segments++;
        }
    }

    if (out_len) {
        *out_len = len;
    }
    if (out_segments) {
        *out_segments = segments;
    }
    if (out_marks) {
        *out_marks = marks;
    }
    return true;
}

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

static const String kw_then = { "THEN", 4 };
static const String kw_or = { "OR", 2 };
static const String kw_to = { "to", 2 };
static const String kw_at_keyword = { "at", 2 };
static const String kw_len = { "len", 3 };
static const String kw_find = { "find", 4 };
static const String kw_find_re = { "find:re", 7 };
static const String kw_find_bin = { "find:bin", 8 };
static const String kw_skip = { "skip", 4 };
static const String kw_take = { "take", 4 };
static const String kw_until = { "until", 5 };
static const String kw_until_re = { "until:re", 8 };
static const String kw_until_bin = { "until:bin", 9 };
static const String kw_label = { "label", 5 };
static const String kw_view = { "view", 4 };
static const String kw_clear = { "clear", 5 };
static const String kw_print = { "print", 5 };
static const String kw_echo = { "echo", 4 };
static const String kw_fail = { "fail", 4 };
static const String kw_cursor = { "cursor", 6 };
static const String kw_bof = { "BOF", 3 };
static const String kw_eof = { "EOF", 3 };
static const String kw_match_start = { "match-start", 11 };
static const String kw_match_end = { "match-end", 9 };
static const String kw_line_start = { "line-start", 10 };
static const String kw_line_end = { "line-end", 8 };

typedef struct LabelTable {
    i32 count;
    char names[MAX_LABELS][MAX_LABEL_LEN + 1];
} LabelTable;

static inline bool is_keyword(String token, const String* kw)
{
    return string_eq(token, *kw);
}
static enum Err parse_op(const String* tokens, i32* idx, i32 token_count, Op* op, Program* prg,
    LabelTable* labels,
    char* str_pool, size_t* str_pool_off, size_t str_pool_cap);
static enum Err parse_loc_expr(const String* tokens, i32* idx, i32 token_count, LocExpr* loc, Program* prg, LabelTable* labels);
static enum Err parse_at_expr(const String* tokens, i32* idx, i32 token_count, LocExpr* at);
static enum Err parse_offset(String token, i64* offset, Unit* unit);
static i32 find_or_add_label(Program* prg, LabelTable* labels, String name);
static bool is_label_name_valid(String name);
static bool has_empty_quantified_group(const char* pattern, i32 len);

// Helper function to check if a location expression contains a valid label name
static bool loc_expr_contains_label(const String* token)
{
    const char* offset_start = find_inline_offset_start(token->bytes);
    if (offset_start) {
        // Extract base part
        char base[256];
        size_t base_len = (size_t)(offset_start - token->bytes);
        if (base_len >= sizeof(base)) {
            return false;
        }
        memcpy(base, token->bytes, base_len);
        base[base_len] = '\0';

        String base_str = { base, (i32)base_len };
        return is_label_name_valid(base_str);
    } // No offset - check the whole token
    return is_label_name_valid(*token);
}

// Count total number of counter-using quantifiers in a pattern
// Counters are allocated permanently (never released), so we count total usage
// Counter-using quantifiers are: {n}, {n,}, {n,m} where n >= 2
// Returns -1 on error, otherwise count
static i32 count_counter_quantifiers_in_pattern(String pattern)
{
    i32 counter_count = 0;
    bool in_escape = false;
    bool in_charclass = false;

    for (i32 i = 0; i < pattern.len; i++) {
        char c = pattern.bytes[i];

        if (in_escape) {
            in_escape = false;
            continue;
        }

        if (c == '\\') {
            in_escape = true;
            continue;
        }

        if (in_charclass) {
            if (c == ']') {
                in_charclass = false;
            }
            continue;
        }

        if (c == '[') {
            in_charclass = true;
            continue;
        }

        // Check for counter-using quantifiers: {n}, {n,}, {n,m}
        // Skip {0} and {1} as they don't allocate counters
        if (c == '{') {
            i32 j = i + 1;
            i32 min_val = 0;
            i32 max_val = 0;
            bool has_min = false;
            bool has_max = false;

            // Parse minimum value
            while (j < pattern.len && pattern.bytes[j] >= '0' && pattern.bytes[j] <= '9') {
                min_val = min_val * 10 + (pattern.bytes[j] - '0');
                has_min = true;
                j++;
            }

            if (j < pattern.len && pattern.bytes[j] == ',') {
                j++; // skip comma
                // Parse maximum value if present
                while (j < pattern.len && pattern.bytes[j] >= '0' && pattern.bytes[j] <= '9') {
                    max_val = max_val * 10 + (pattern.bytes[j] - '0');
                    has_max = true;
                    j++;
                }
                if (!has_max) {
                    max_val = -1; // unbounded
                }
            } else if (has_min) {
                max_val = min_val; // exact count
            }

            // Skip to closing brace
            while (j < pattern.len && pattern.bytes[j] != '}') {
                j++;
            }
            if (j < pattern.len) {
                j++; // skip '}'
            }

            // Only count if it will actually use a counter (not {0} or {1})
            if (has_min && min_val > 1) {
                counter_count++;
            } else if (has_min && min_val == 0 && max_val > 1) {
                // {0,m} where m > 1 uses counter
                counter_count++;
            } else if (has_min && min_val == 1 && (max_val > 1 || max_val == -1)) {
                // {1,m} where m > 1 or {1,} uses counter
                counter_count++;
            }

            i = j - 1; // -1 because loop will increment
        }
    }

    return counter_count;
}

// Estimate instruction count for regex pattern using the formula from REGEX_IMPLEMENTATION_PLAN.md:
// nins_est ≤ 2A + 3·Alt + 2·Qu + 6·Qc + max(16, Alt + Qc + Qu)
// Where:
//   A = number of atoms (literals, classes, dot, anchors)
//   Alt = number of | occurrences
//   Qu = number of simple quantifiers (? * +)
//   Qc = number of counter quantifiers ({n} {n,} {n,m})
static i32 estimate_regex_instructions(String pattern)
{
    i32 atom_count = 0;
    i32 alt_count = 0;
    i32 simple_quant_count = 0;
    i32 counter_quant_count = 0;
    i32 max_paren_depth = 0;
    i32 quantified_group_count = 0;

    bool in_escape = false;
    bool in_charclass = false;
    i32 paren_depth = 0;

    for (i32 i = 0; i < pattern.len; i++) {
        char c = pattern.bytes[i];

        if (in_escape) {
            in_escape = false;
            // Escaped characters are atoms (except special escapes which become classes)
            if (c == 'd' || c == 'D' || c == 'w' || c == 'W' || c == 's' || c == 'S') {
                // These become character classes, but count as atoms for instruction purposes
                atom_count++;
            } else {
                // Regular escaped literal
                atom_count++;
            }
            continue;
        }

        if (c == '\\') {
            in_escape = true;
            continue;
        }

        if (in_charclass) {
            if (c == ']') {
                in_charclass = false;
            }
            continue;
        }

        if (c == '[') {
            in_charclass = true;
            atom_count++; // Character class is an atom
            continue;
        }

        if (c == '.') {
            atom_count++; // Dot is an atom
            continue;
        }

        if (c == '^' || c == '$') {
            atom_count++; // Anchors are atoms (epsilon atoms)
            continue;
        }

        if (c == '(') {
            paren_depth++;
            if (paren_depth > max_paren_depth) {
                max_paren_depth = paren_depth;
            }
            continue;
        }

        if (c == ')') {
            if (paren_depth > 0) {
                paren_depth--;

                // Check if this closing paren is followed by a quantifier
                i32 j = i + 1;
                if (j < pattern.len) {
                    char next = pattern.bytes[j];
                    if (next == '*' || next == '+' || next == '?' || next == '{') {
                        quantified_group_count++;
                    }
                }
            }
            continue;
        }

        if (c == '|') {
            alt_count++;
            continue;
        }

        // Check for quantifiers: *, +, ?, {n,m}
        if (c == '*' || c == '+' || c == '?') {
            simple_quant_count++;
            continue;
        }

        if (c == '{') {
            // Parse to see if this is a counter-using quantifier
            i32 j = i + 1;
            bool has_digits = false;

            // Skip digits
            while (j < pattern.len && pattern.bytes[j] >= '0' && pattern.bytes[j] <= '9') {
                has_digits = true;
                j++;
            }

            if (has_digits) {
                counter_quant_count++;
            }

            // Skip rest of quantifier
            while (j < pattern.len && pattern.bytes[j] != '}') {
                j++;
            }
            if (j < pattern.len) {
                j++; // skip '}'
            }
            i = j - 1; // -1 because loop will increment
            continue;
        }

        // Regular literal character
        atom_count++;
    }

    // Apply formula: 2A + 3·Alt + 2·Qu + 6·Qc + max(16, Alt + Qc + Qu)
    i32 base_margin = (alt_count + counter_quant_count + simple_quant_count);
    if (base_margin < 16) {
        base_margin = 16;
    }

    i32 base_estimate = 2 * atom_count + 3 * alt_count + 2 * simple_quant_count + 6 * counter_quant_count + base_margin;

    // Apply nesting depth multiplier for deeply nested patterns
    // Each level of nesting roughly doubles the instruction overhead due to
    // group overhead, state management, and instruction wiring
    i32 nesting_multiplier = 1;
    if (max_paren_depth >= 4) {
        // Very deep nesting (4+ levels): 4x multiplier
        nesting_multiplier = 4;
    } else if (max_paren_depth == 3) {
        // Deep nesting (3 levels): 3x multiplier
        nesting_multiplier = 3;
    } else if (max_paren_depth == 2) {
        // Moderate nesting (2 levels): 2x multiplier
        nesting_multiplier = 2;
    }

    // Additional overhead for quantified groups - each adds significant instruction overhead
    i32 quantified_group_overhead = quantified_group_count * 10;

    // Combine: base estimate × nesting + group overhead
    i32 estimate = base_estimate * nesting_multiplier + quantified_group_overhead;

    // Add a generous flat safety margin
    estimate = estimate + 200;

    return estimate;
}

enum Err parse_preflight(i32 token_count, const String* tokens, const char* in_path, ParsePlan* plan, const char** in_path_out)
{
    error_detail_reset();
    memset(plan, 0, sizeof(*plan));

    if (token_count < 1) {
        error_detail_set(E_PARSE, -1, "expected at least one operation");
        return E_PARSE;
    }

    *in_path_out = in_path;

    i32 idx = 0;
    while (idx < token_count) {
        plan->clause_count++; // Count each clause as we process it
        while (idx < token_count && !is_keyword(tokens[idx], &kw_then) && !is_keyword(tokens[idx], &kw_or)) {
            const String cmd_tok = tokens[idx];
            plan->total_ops++;

            if (is_keyword(cmd_tok, &kw_find)) {
                idx++;
                if (idx < token_count && is_keyword(tokens[idx], &kw_to)) {
                    idx++;
                    if (idx < token_count) {
                        // Check if this location expression contains a valid label name
                        if (loc_expr_contains_label(&tokens[idx])) {
                            /* no-op: we don't size anything from names in preflight */
                        }
                        idx++;
                    }
                    if (idx < token_count) {
                        char first = string_first(tokens[idx]);
                        if (first == '+' || first == '-') {
                            idx++;
                        }
                    }
                }
                if (idx < token_count) {
                    plan->needle_count++;
                    size_t escaped_len = calculate_escaped_string_length(tokens[idx]);
                    plan->needle_bytes += escaped_len;
                    idx++;
                }
            } else if (is_keyword(cmd_tok, &kw_find_re)) {
                idx++;
                if (idx < token_count && is_keyword(tokens[idx], &kw_to)) {
                    idx++;
                    if (idx < token_count) {
                        // Check if this location expression contains a valid label name
                        if (loc_expr_contains_label(&tokens[idx])) {
                            /* no-op: we don't size anything from names in preflight */
                        }
                        idx++;
                    }
                    if (idx < token_count) {
                        char first = string_first(tokens[idx]);
                        if (first == '+' || first == '-') {
                            idx++;
                        }
                    }
                }
                if (idx < token_count) {
                    const String pat_tok = tokens[idx];
                    const char* pat = pat_tok.bytes;

                    plan->sum_findr_ops++;
                    i32 est = estimate_regex_instructions(pat_tok);
                    plan->re_ins_estimate += est;
                    if (est > plan->re_ins_estimate_max) {
                        plan->re_ins_estimate_max = est;
                    }
                    // Count character classes, accounting for group quantifiers
                    // Group quantifiers like (...)+ duplicate the group content
                    i32 base_classes = 0;
                    i32 group_depth = 0;
                    i32 group_start_classes[16] = {0};  // Track classes at each group level
                    i32 max_depth = 0;

                    for (i32 pi = 0; pi < pat_tok.len; ++pi) {
                        char c = pat[pi];

                        if (c == '\\' && pi + 1 < pat_tok.len) {
                            char next = pat[pi + 1];
                            if (string_char_in_set(next, "dDwWsS")) {
                                base_classes++;
                            }
                            pi++;  // Skip next char
                            continue;
                        }

                        if (c == '(') {
                            if (group_depth < 16) {
                                group_start_classes[group_depth] = base_classes;
                            }
                            group_depth++;
                            if (group_depth > max_depth) max_depth = group_depth;
                        } else if (c == ')' && group_depth > 0) {
                            group_depth--;
                            // Check for quantifier after group
                            if (pi + 1 < pat_tok.len) {
                                char next = pat[pi + 1];
                                if (next == '*' || next == '+' || next == '?') {
                                    // +/* emit group content 2x (min copies + loop copy)
                                    // ? emits 1x
                                    i32 multiplier = (next == '?') ? 0 : 1;
                                    if (group_depth < 16) {
                                        i32 classes_in_group = base_classes - group_start_classes[group_depth];
                                        base_classes += classes_in_group * multiplier;
                                    }
                                }
                            }
                        } else if (c == '[' && group_depth == 0) {
                            // Only count '[' outside groups for base estimate
                            base_classes++;
                        } else if (c == '[') {
                            base_classes++;
                        }
                    }

                    plan->re_classes_estimate += base_classes;
                    // Add safety margin for nested groups and alternations
                    // Use generous multiplier for complex patterns
                    if (max_depth > 0) {
                        i32 margin = (base_classes * max_depth) > (max_depth * 8)
                            ? (base_classes * max_depth)
                            : (max_depth * 8);
                        plan->re_classes_estimate += margin;
                    }
                    plan->needle_count++;
                    size_t escaped_len = calculate_escaped_string_length(pat_tok);
                    plan->needle_bytes += escaped_len;
                    idx++;
                }
            } else if (is_keyword(cmd_tok, &kw_find_bin)) {
                idx++;
                if (idx < token_count && is_keyword(tokens[idx], &kw_to)) {
                    idx++;
                    if (idx < token_count) {
                        // Check if this location expression contains a valid label name
                        if (loc_expr_contains_label(&tokens[idx])) {
                            /* no-op: we don't size anything from names in preflight */
                        }
                        idx++;
                    }
                    if (idx < token_count) {
                        char first = string_first(tokens[idx]);
                        if (first == '+' || first == '-') {
                            idx++;
                        }
                    }
                }
                if (idx < token_count) {
                    const String hex_tok = tokens[idx];
                    size_t hex_digits = 0;
                    for (i32 pi = 0; pi < hex_tok.len; ++pi) {
                        unsigned char c = (unsigned char)hex_tok.bytes[pi];
                        if (!isspace(c)) {
                            hex_digits++;
                        }
                    }
                    plan->needle_count++;
                    plan->needle_bytes += hex_digits / 2;
                    idx++;
                }
            } else if (is_keyword(cmd_tok, &kw_skip)) {
                idx++;
                if (idx < token_count && is_keyword(tokens[idx], &kw_to)) {
                    idx++; // skip "to"
                    if (idx < token_count) {
                        // Check if this location expression contains a valid label name
                        if (loc_expr_contains_label(&tokens[idx])) {
                            /* no-op: we don't size anything from names in preflight */
                        }
                        idx++;
                    }
                    if (idx < token_count) {
                        char first = string_first(tokens[idx]);
                        if (first == '+' || first == '-') {
                            idx++;
                        }
                    }
                } else if (idx < token_count) {
                    idx++; // skip offset
                }
            } else if (is_keyword(cmd_tok, &kw_take)) {
                idx++;
                if (idx < token_count) {
                    const String next_tok = tokens[idx];
                    if (is_keyword(next_tok, &kw_to)) {
                        plan->sum_take_ops++;
                        idx++;
                        if (idx < token_count) {
                            // Check if this location expression contains a valid label name
                            if (loc_expr_contains_label(&tokens[idx])) {
                                /* no-op: we don't size anything from names in preflight */
                            }
                            idx++;
                        }
                        if (idx < token_count) {
                            char first = string_first(tokens[idx]);
                            if (first == '+' || first == '-') {
                                idx++;
                            }
                        }
                    } else if (is_keyword(next_tok, &kw_until_re)) {
                        plan->sum_take_ops++;
                        idx++;
                        if (idx < token_count) {
                            const String pat_tok = tokens[idx];
                            const char* pat = pat_tok.bytes;

                            // Validate counter quantifier usage before compilation
                            i32 counter_count = count_counter_quantifiers_in_pattern(pat_tok);
                            if (counter_count > MAX_RE_COUNTERS) {
                                error_detail_set(E_CAPACITY, idx,
                                    "regex: too many quantified groups in pattern (found %d, max %d); reduce nesting or use simpler quantifiers",
                                    counter_count, MAX_RE_COUNTERS);
                                return E_CAPACITY;
                            }

                            plan->sum_findr_ops++;
                            i32 est = estimate_regex_instructions(pat_tok);
                            plan->re_ins_estimate += est;
                            if (est > plan->re_ins_estimate_max) {
                                plan->re_ins_estimate_max = est;
                            }

                            // Count character classes, accounting for group quantifiers
                            // (Same logic as find:re)
                            i32 base_classes = 0;
                            i32 group_depth = 0;
                            i32 group_start_classes[16] = {0};
                            i32 max_depth = 0;

                            for (i32 pi = 0; pi < pat_tok.len; ++pi) {
                                char c = pat[pi];

                                if (c == '\\' && pi + 1 < pat_tok.len) {
                                    char next = pat[pi + 1];
                                    if (string_char_in_set(next, "dDwWsS")) {
                                        base_classes++;
                                    }
                                    pi++;
                                    continue;
                                }

                                if (c == '(') {
                                    if (group_depth < 16) {
                                        group_start_classes[group_depth] = base_classes;
                                    }
                                    group_depth++;
                                    if (group_depth > max_depth) max_depth = group_depth;
                                } else if (c == ')' && group_depth > 0) {
                                    group_depth--;
                                    if (pi + 1 < pat_tok.len) {
                                        char next = pat[pi + 1];
                                        if (next == '*' || next == '+' || next == '?') {
                                            i32 multiplier = (next == '?') ? 0 : 1;
                                            if (group_depth < 16) {
                                                i32 classes_in_group = base_classes - group_start_classes[group_depth];
                                                base_classes += classes_in_group * multiplier;
                                            }
                                        }
                                    }
                                } else if (c == '[') {
                                    base_classes++;
                                }
                            }

                            plan->re_classes_estimate += base_classes;
                            // Add safety margin for nested groups and alternations
                            if (max_depth > 0) {
                                i32 margin = (base_classes * max_depth) > (max_depth * 8)
                                    ? (base_classes * max_depth)
                                    : (max_depth * 8);
                                plan->re_classes_estimate += margin;
                            }

                            plan->needle_count++;
                            plan->needle_bytes += calculate_escaped_string_length(pat_tok);
                            idx++;
                        }
                        if (idx < token_count && is_keyword(tokens[idx], &kw_at_keyword)) {
                            idx++;
                            if (idx < token_count) {
                                idx++;
                            }
                            if (idx < token_count) {
                                char first = string_first(tokens[idx]);
                                if (first == '+' || first == '-') {
                                    idx++;
                                }
                            }
                        }
                    } else if (is_keyword(next_tok, &kw_until_bin)) {
                        plan->sum_take_ops++;
                        idx++;
                        if (idx < token_count) {
                            const String hex_tok = tokens[idx];
                            size_t hex_digits = 0;
                            for (i32 pi = 0; pi < hex_tok.len; ++pi) {
                                unsigned char c = (unsigned char)hex_tok.bytes[pi];
                                if (!isspace(c)) {
                                    hex_digits++;
                                }
                            }
                            plan->needle_count++;
                            plan->needle_bytes += hex_digits / 2;
                            idx++;
                        }
                        if (idx < token_count && is_keyword(tokens[idx], &kw_at_keyword)) {
                            idx++;
                            if (idx < token_count) {
                                idx++;
                            }
                            if (idx < token_count) {
                                char first = string_first(tokens[idx]);
                                if (first == '+' || first == '-') {
                                    idx++;
                                }
                            }
                        }
                    } else if (is_keyword(next_tok, &kw_until)) {
                        plan->sum_take_ops++;
                        idx++;
                        if (idx < token_count) {
                            plan->needle_count++;
                            plan->needle_bytes += calculate_escaped_string_length(tokens[idx]);
                            idx++;
                        }
                        if (idx < token_count && is_keyword(tokens[idx], &kw_at_keyword)) {
                            idx++;
                            if (idx < token_count) {
                                idx++;
                            }
                            if (idx < token_count) {
                                char first = string_first(tokens[idx]);
                                if (first == '+' || first == '-') {
                                    idx++;
                                }
                            }
                        }
                    } else {
                        plan->sum_take_ops++;
                        if (is_keyword(next_tok, &kw_len)) {
                            idx++;
                            if (idx < token_count) {
                                idx++;
                            }
                        } else {
                            idx++;
                        }
                    }
                }
            } else if (is_keyword(cmd_tok, &kw_label)) {
                plan->sum_label_ops++;
                idx++;
                if (idx < token_count) {
                    // Check if this is a valid label name
                    if (is_label_name_valid(tokens[idx])) {
                        /* no-op: we don't size anything from names in preflight */
                    }
                    idx++;
                }
            } else if (is_keyword(cmd_tok, &kw_view)) {
                idx++;
                if (idx < token_count) {
                    // Check if this location expression contains a valid label name
                    if (loc_expr_contains_label(&tokens[idx])) {
                        /* no-op: we don't size anything from names in preflight */
                    }
                    idx++;
                }
                if (idx < token_count) {
                    char first = string_first(tokens[idx]);
                    if (first == '+' || first == '-') {
                        idx++;
                    }
                }
                if (idx < token_count) {
                    // Check if this location expression contains a valid label name
                    if (loc_expr_contains_label(&tokens[idx])) {
                        /* no-op: we don't size anything from names in preflight */
                    }
                    idx++;
                }
                if (idx < token_count) {
                    char first = string_first(tokens[idx]);
                    if (first == '+' || first == '-') {
                        idx++;
                    }
                }
            } else if (is_keyword(cmd_tok, &kw_clear)) {
                idx++;
                if (idx < token_count) {
                    idx++;
                }
            } else if (is_keyword(cmd_tok, &kw_print) || is_keyword(cmd_tok, &kw_echo)) {
                idx++;
                if (idx < token_count) {
                    size_t stored_len = 0;
                    i32 segments = 0;
                    i32 marks = 0;
                    if (!compute_print_stats(tokens[idx], &stored_len, &segments, &marks)) {
                        error_detail_set(E_PARSE, idx, "invalid escape in print literal");
                        return E_PARSE;
                    }
                    plan->needle_count++;
                    plan->needle_bytes += stored_len;
                    plan->sum_take_ops += segments + marks;
                    plan->sum_inline_lits += marks;
                    idx++;
                }
            } else if (is_keyword(cmd_tok, &kw_fail)) {
                idx++;
                if (idx < token_count) {
                    plan->needle_count++;
                    plan->needle_bytes += calculate_escaped_string_length(tokens[idx]);
                    idx++;
                }
            } else {
                error_detail_set(E_PARSE, idx, "unknown operation '%.*s'", cmd_tok.len, cmd_tok.bytes);
                return E_PARSE;
            }
        }

        if (idx < token_count && (is_keyword(tokens[idx], &kw_then) || is_keyword(tokens[idx], &kw_or))) {
            idx++;
        }
    }

    return E_OK;
}

// Check if regex pattern contains empty alternative in quantified group
// Patterns like (|a)*, (a|)*, (||)+ cause exponential expansion
static bool has_empty_quantified_group(const char* pattern, i32 len)
{
    i32 depth = 0;
    bool in_group = false;
    bool group_has_empty_alt = false;

    for (i32 i = 0; i < len; i++) {
        char c = pattern[i];

        if (c == '\\' && i + 1 < len) {
            i++; // Skip escaped character
            continue;
        }

        if (c == '(') {
            depth++;
            in_group = true;
            group_has_empty_alt = false;
        } else if (c == ')' && depth > 0) {
            depth--;
            // Check if group is followed by quantifier
            if (i + 1 < len && group_has_empty_alt) {
                char next = pattern[i + 1];
                if (next == '*' || next == '+' || next == '?') {
                    return true;
                }
                if (next == '{') {
                    return true; // {n,m} quantifier
                }
            }
            in_group = (depth > 0);
            group_has_empty_alt = false;
        } else if (c == '|' && in_group) {
            // Check if this is an empty alternative
            // Empty if: |(  or  ||  or  |)
            if (i + 1 < len) {
                char next = pattern[i + 1];
                if (next == '|' || next == ')') {
                    group_has_empty_alt = true;
                }
            }
            // Check previous character for (|
            if (i > 0 && pattern[i - 1] == '(') {
                group_has_empty_alt = true;
            }
        }
    }

    return false;
}

enum Err parse_build(i32 token_count, const String* tokens, const char* in_path, Program* prg, const char** in_path_out,
    Clause* clauses_buf, Op* ops_buf, char* str_pool, size_t str_pool_cap)
{
    error_detail_reset();
    memset(prg, 0, sizeof(*prg));

    if (token_count < 1) {
        error_detail_set(E_PARSE, -1, "expected at least one operation");
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
    LabelTable labels = { 0 };

    // Parse clauses separated by "THEN"
    i32 idx = 0;
    i32 op_cursor = 0;

    while (idx < token_count) {
        Clause* clause = &prg->clauses[prg->clause_count];
        clause->ops = ops_buf + op_cursor;
        clause->op_count = 0;
        clause->link = LINK_NONE; // Default to no link

        // Parse operations in this clause
        i32 clause_start = idx;
        while (idx < token_count && !is_keyword(tokens[idx], &kw_then) && !is_keyword(tokens[idx], &kw_or)) {
            String cmd_tok = tokens[idx];
            idx++;

            // Skip command-specific tokens
            if (is_keyword(cmd_tok, &kw_find) || is_keyword(cmd_tok, &kw_find_re) || is_keyword(cmd_tok, &kw_find_bin)) {
                if (idx < token_count && is_keyword(tokens[idx], &kw_to)) {
                    idx++;
                    if (idx < token_count) {
                        idx++; // skip location
                    }
                    if (idx < token_count && (TOK_FIRST(tokens, idx) == '+' || TOK_FIRST(tokens, idx) == '-')) {
                        idx++; // skip offset
                    }
                }
                skip_one_token(&idx, token_count);
            } else if (is_keyword(cmd_tok, &kw_skip)) {
                idx++;
                if (idx < token_count && is_keyword(tokens[idx], &kw_to)) {
                    idx++; // skip "to"
                    skip_one_token(&idx, token_count); // skip location
                    if (idx < token_count && (TOK_FIRST(tokens, idx) == '+' || TOK_FIRST(tokens, idx) == '-')) {
                        idx++; // skip offset
                    }
                } else if (idx < token_count) {
                    idx++; // skip offset
                }
            } else if (is_keyword(cmd_tok, &kw_take)) {
                if (idx < token_count) {
                    const String next_tok = tokens[idx];
                    if (is_keyword(next_tok, &kw_to)) {
                        idx++;
                        skip_one_token(&idx, token_count);
                        if (idx < token_count && (TOK_FIRST(tokens, idx) == '+' || TOK_FIRST(tokens, idx) == '-')) {
                            idx++; // skip offset
                        }
                    } else if (is_keyword(next_tok, &kw_until) || is_keyword(next_tok, &kw_until_re) || is_keyword(next_tok, &kw_until_bin)) {
                        idx++;
                        skip_one_token(&idx, token_count);
                        if (idx < token_count && is_keyword(tokens[idx], &kw_at_keyword)) {
                            idx++;
                            if (idx < token_count) {
                                idx++; // skip location
                            }
                            if (idx < token_count && (TOK_FIRST(tokens, idx) == '+' || TOK_FIRST(tokens, idx) == '-')) {
                                idx++; // skip offset
                            }
                        }
                    } else {
                        if (is_keyword(next_tok, &kw_len)) {
                            idx++;
                            if (idx < token_count) {
                                idx++;
                            }
                        } else {
                            idx++;
                        }
                    }
                }
            } else if (is_keyword(cmd_tok, &kw_label)) {
                if (idx < token_count) {
                    idx++; // skip name
                }
            } else if (is_keyword(cmd_tok, &kw_view)) {
                if (idx < token_count) {
                    idx++; // skip first location
                }
                if (idx < token_count && (TOK_FIRST(tokens, idx) == '+' || TOK_FIRST(tokens, idx) == '-')) {
                    idx++; // skip offset
                }
                if (idx < token_count) {
                    idx++; // skip second location
                }
                if (idx < token_count && (TOK_FIRST(tokens, idx) == '+' || TOK_FIRST(tokens, idx) == '-')) {
                    idx++; // skip offset
                }
            } else if (is_keyword(cmd_tok, &kw_clear)) {
                skip_optional_token(&idx, token_count);
            } else if (is_keyword(cmd_tok, &kw_print) || is_keyword(cmd_tok, &kw_echo)) {
                skip_one_token(&idx, token_count);
            } else if (is_keyword(cmd_tok, &kw_fail)) {
                if (idx < token_count) {
                    idx++;
                }
            }
        }

        // 3 Reset idx to clause start and parse for real
        idx = clause_start;
        while (idx < token_count && !is_keyword(tokens[idx], &kw_then) && !is_keyword(tokens[idx], &kw_or)) {
            Op* op = &clause->ops[clause->op_count];
            enum Err err = parse_op(tokens, &idx, token_count, op, prg, &labels, str_pool, &str_pool_off, str_pool_cap);
            if (err != E_OK) {
                return err;
            }
            clause->op_count++;
        }

        prg->clause_count++;
        op_cursor += clause->op_count; // Use actual parsed count, not pre-count

        // Check for link keywords
        if (idx < token_count) {
            if (is_keyword(tokens[idx], &kw_or)) {
                clause->link = LINK_OR;
                idx++;
            } else if (is_keyword(tokens[idx], &kw_then)) {
                clause->link = LINK_THEN;
                idx++;
            }
        }
    }

    prg->name_count = labels.count;

    // Check for trailing link operators (OR, THEN, AND without following clause)
    if (prg->clause_count > 0) {
        const Clause* last_clause = &prg->clauses[prg->clause_count - 1];
        if (last_clause->link != LINK_NONE) {
            if (token_count > 0) {
                const String tail_tok = tokens[token_count - 1];
                error_detail_set(E_PARSE, token_count - 1, "dangling '%.*s' without following clause", tail_tok.len, tail_tok.bytes);
            } else {
                error_detail_set(E_PARSE, -1, "dangling clause link without target");
            }
            return E_PARSE; // Trailing OR/THEN is a parse error
        }
    }

    return E_OK;
}

static i32 find_or_add_label(Program* prg, LabelTable* labels, String name)
{
    if (!labels) {
        return -1;
    }

    for (i32 i = 0; i < labels->count; i++) {
        if (string_eq_cstr(name, labels->names[i])) {
            return i;
        }
    }

    if (labels->count >= MAX_LABELS) {
        return -1;
    }

    if (!string_copy_to_buffer(name, labels->names[labels->count], sizeof(labels->names[0]))) {
        return -1;
    }

    i32 idx = labels->count++;
    if (prg) {
        prg->name_count = labels->count;
    }
    return idx;
}

static enum Err parse_op(const String* tokens, i32* idx, i32 token_count, Op* op, Program* prg, LabelTable* labels,
    char* str_pool, size_t* str_pool_off, size_t str_pool_cap)
{
    if (*idx >= token_count) {
        error_detail_set(E_PARSE, token_count, "unexpected end of input while reading operation");
        return E_PARSE;
    }

    i32 cmd_idx = *idx;
    const String cmd_tok = tokens[*idx];
    (*idx)++;

    /************************************************************
     * SEARCH OPERATIONS
     ************************************************************/
    if (is_keyword(cmd_tok, &kw_find)) {
        op->kind = OP_FIND;

        if (*idx < token_count && is_keyword(tokens[*idx], &kw_to)) {
            (*idx)++;
            enum Err err = parse_loc_expr(tokens, idx, token_count, &op->u.find.to, prg, labels);
            if (err != E_OK) {
                return err;
            }
        } else {
            // Default to EOF
            op->u.find.to.base = LOC_EOF;
            op->u.find.to.name_idx = -1;
            op->u.find.to.offset = 0;
            op->u.find.to.unit = UNIT_BYTES;
        }

        // Parse needle
        if (*idx >= token_count) {
            error_detail_set(E_PARSE, cmd_idx, "missing needle for 'find'");
            return E_PARSE;
        }
        const String needle_tok = tokens[*idx];
        (*idx)++;

        if (needle_tok.len == 0) {
            return E_BAD_NEEDLE;
        }

        if (needle_tok.len > MAX_PATTERN_LENGTH) {
            error_detail_set(E_PARSE, *idx - 1, "pattern too long (max %d bytes)", MAX_PATTERN_LENGTH);
            return E_PARSE;
        }

        enum Err err = E_OK;
        op->u.find.needle = parse_string_to_bytes(needle_tok, str_pool, str_pool_off, str_pool_cap, &err, NULL);
        if (err != E_OK) {
            return err;
        }

    } else if (is_keyword(cmd_tok, &kw_find_re)) {
        op->kind = OP_FIND_RE;

        if (*idx < token_count && is_keyword(tokens[*idx], &kw_to)) {
            (*idx)++;
            enum Err err = parse_loc_expr(tokens, idx, token_count, &op->u.findr.to, prg, labels);
            if (err != E_OK) {
                return err;
            }
        } else {
            op->u.findr.to.base = LOC_EOF;
            op->u.findr.to.name_idx = -1;
            op->u.findr.to.offset = 0;
            op->u.findr.to.unit = UNIT_BYTES;
        }
        if (*idx >= token_count) {
            error_detail_set(E_PARSE, cmd_idx, "missing pattern for 'find:re'");
            return E_PARSE;
        }
        const String pat_tok = tokens[*idx];
        (*idx)++;

        if (pat_tok.len > MAX_PATTERN_LENGTH) {
            error_detail_set(E_PARSE, *idx - 1, "pattern too long (max %d bytes)", MAX_PATTERN_LENGTH);
            return E_PARSE;
        }

        enum Err err = E_OK;
        op->u.findr.pattern = parse_string_to_bytes(pat_tok, str_pool, str_pool_off, str_pool_cap, &err, NULL);
        if (err != E_OK) {
            return err;
        }

        // Check for patterns that cause exponential expansion
        if (has_empty_quantified_group(op->u.findr.pattern.bytes, op->u.findr.pattern.len)) {
            error_detail_set(E_PARSE, *idx - 1, "regex pattern contains empty alternative in quantified group (e.g. '(|a)*')");
            return E_PARSE;
        }

        op->u.findr.prog = NULL;

    } else if (is_keyword(cmd_tok, &kw_find_bin)) {
        op->kind = OP_FIND_BIN;

        if (*idx < token_count && is_keyword(tokens[*idx], &kw_to)) {
            (*idx)++;
            enum Err err = parse_loc_expr(tokens, idx, token_count, &op->u.findbin.to, prg, labels);
            if (err != E_OK) {
                return err;
            }
        } else {
            // Default to EOF
            op->u.findbin.to.base = LOC_EOF;
            op->u.findbin.to.name_idx = -1;
            op->u.findbin.to.offset = 0;
            op->u.findbin.to.unit = UNIT_BYTES;
        }

        // Parse hex string
        if (*idx >= token_count) {
            error_detail_set(E_PARSE, cmd_idx, "missing hex bytes for 'find:bin'");
            return E_PARSE;
        }
        const String hex_tok = tokens[*idx];
        (*idx)++;

        if (hex_tok.len == 0) {
            return E_BAD_NEEDLE;
        }

        if (hex_tok.len > MAX_PATTERN_LENGTH) {
            error_detail_set(E_PARSE, *idx - 1, "pattern too long (max %d bytes)", MAX_PATTERN_LENGTH);
            return E_PARSE;
        }

        enum Err err = E_OK;
        op->u.findbin.needle = parse_hex_to_bytes(hex_tok, str_pool, str_pool_off, str_pool_cap, &err);
        if (err != E_OK) {
            return err;
        }

        /************************************************************
         * MOVEMENT OPERATIONS
         ************************************************************/
    } else if (is_keyword(cmd_tok, &kw_skip)) {
        op->kind = OP_SKIP;

        if (*idx >= token_count) {
            error_detail_set(E_PARSE, cmd_idx, "missing target for 'skip'");
            return E_PARSE;
        }

        // Check if this is "skip to <location>" or "skip <offset><unit>"
        if (is_keyword(tokens[*idx], &kw_to)) {
            (*idx)++; // consume "to"
            op->u.skip.is_location = true;
            enum Err err = parse_loc_expr(tokens, idx, token_count, &op->u.skip.to_location.to, prg, labels);
            if (err != E_OK) {
                return err;
            }
        } else {
            op->u.skip.is_location = false;
            i32 offset_idx = *idx;
            enum Err err = parse_offset(tokens[*idx], &op->u.skip.by_offset.offset, &op->u.skip.by_offset.unit);
            if (err != E_OK) {
                error_detail_set(E_PARSE, offset_idx, "invalid offset '%.*s' for 'skip'", tokens[offset_idx].len, tokens[offset_idx].bytes);
                return err;
            }
            (*idx)++;
        }

        /************************************************************
         * EXTRACTION OPERATIONS
         ************************************************************/
    } else if (is_keyword(cmd_tok, &kw_take)) {
        if (*idx >= token_count) {
            error_detail_set(E_PARSE, cmd_idx, "missing argument for 'take'");
            return E_PARSE;
        }

        const String next_tok = tokens[*idx];
        if (is_keyword(next_tok, &kw_to)) {
            op->kind = OP_TAKE_TO;
            (*idx)++;
            enum Err err = parse_loc_expr(tokens, idx, token_count, &op->u.take_to.to, prg, labels);
            if (err != E_OK) {
                return err;
            }
        } else if (is_keyword(next_tok, &kw_until_re)) {
            op->kind = OP_TAKE_UNTIL_RE;
            (*idx)++;

            // Parse pattern
            if (*idx >= token_count) {
                error_detail_set(E_PARSE, cmd_idx, "missing pattern for 'take until:re'");
                return E_PARSE;
            }
            const String pattern_tok = tokens[*idx];
            (*idx)++;

            if (pattern_tok.len == 0) {
                return E_BAD_NEEDLE;
            }

            if (pattern_tok.len > MAX_PATTERN_LENGTH) {
                error_detail_set(E_PARSE, *idx - 1, "pattern too long (max %d bytes)", MAX_PATTERN_LENGTH);
                return E_PARSE;
            }

            enum Err err = E_OK;
            op->u.take_until_re.pattern = parse_string_to_bytes(pattern_tok, str_pool, str_pool_off, str_pool_cap, &err, NULL);
            if (err != E_OK) {
                return err;
            }

            // Check for patterns that cause exponential expansion
            if (has_empty_quantified_group(op->u.take_until_re.pattern.bytes, op->u.take_until_re.pattern.len)) {
                error_detail_set(E_PARSE, *idx - 1, "regex pattern contains empty alternative in quantified group (e.g. '(|a)*')");
                return E_PARSE;
            }

            // Parse "at" expression if present
            if (*idx < token_count && is_keyword(tokens[*idx], &kw_at_keyword)) {
                (*idx)++;
                op->u.take_until_re.has_at = true;
                enum Err err2 = parse_at_expr(tokens, idx, token_count, &op->u.take_until_re.at);
                if (err2 != E_OK) {
                    return err2;
                }
            } else {
                op->u.take_until_re.has_at = false;
            }
            op->u.take_until_re.prog = NULL;
        } else if (is_keyword(next_tok, &kw_until_bin)) {
            op->kind = OP_TAKE_UNTIL_BIN;
            (*idx)++;

            // Parse hex string
            if (*idx >= token_count) {
                error_detail_set(E_PARSE, cmd_idx, "missing hex bytes for 'take until:bin'");
                return E_PARSE;
            }
            const String hex_tok = tokens[*idx];
            (*idx)++;

            if (hex_tok.len == 0) {
                return E_BAD_NEEDLE;
            }

            if (hex_tok.len > MAX_PATTERN_LENGTH) {
                error_detail_set(E_PARSE, *idx - 1, "pattern too long (max %d bytes)", MAX_PATTERN_LENGTH);
                return E_PARSE;
            }

            enum Err err = E_OK;
            op->u.take_until_bin.needle = parse_hex_to_bytes(hex_tok, str_pool, str_pool_off, str_pool_cap, &err);
            if (err != E_OK) {
                return err;
            }

            // Parse "at" expression if present
            if (*idx < token_count && is_keyword(tokens[*idx], &kw_at_keyword)) {
                (*idx)++;
                op->u.take_until_bin.has_at = true;
                enum Err err2 = parse_at_expr(tokens, idx, token_count, &op->u.take_until_bin.at);
                if (err2 != E_OK) {
                    return err2;
                }
            } else {
                op->u.take_until_bin.has_at = false;
            }
        } else if (is_keyword(next_tok, &kw_until)) {
            op->kind = OP_TAKE_UNTIL;
            (*idx)++;

            // Parse needle
            if (*idx >= token_count) {
                error_detail_set(E_PARSE, cmd_idx, "missing needle for 'take until'");
                return E_PARSE;
            }
            const String needle_tok = tokens[*idx];
            (*idx)++;

            if (needle_tok.len == 0) {
                return E_BAD_NEEDLE;
            }

            if (needle_tok.len > MAX_PATTERN_LENGTH) {
                error_detail_set(E_PARSE, *idx - 1, "pattern too long (max %d bytes)", MAX_PATTERN_LENGTH);
                return E_PARSE;
            }

            enum Err err = E_OK;
            op->u.take_until.needle = parse_string_to_bytes(needle_tok, str_pool, str_pool_off, str_pool_cap, &err, NULL);
            if (err != E_OK) {
                return err;
            }

            // Parse "at" expression if present
            if (*idx < token_count && is_keyword(tokens[*idx], &kw_at_keyword)) {
                (*idx)++;
                op->u.take_until.has_at = true;
                enum Err err2 = parse_at_expr(tokens, idx, token_count, &op->u.take_until.at);
                if (err2 != E_OK) {
                    return err2;
                }
            } else {
                op->u.take_until.has_at = false;
            }
        } else {
            op->kind = OP_TAKE_LEN;
            if (is_keyword(next_tok, &kw_len)) {
                (*idx)++;
                if (*idx >= token_count) {
                    error_detail_set(E_PARSE, cmd_idx, "missing length value for 'take len'");
                    return E_PARSE;
                }
            }
            enum Err err = parse_offset(tokens[*idx], &op->u.take_len.offset, &op->u.take_len.unit);
            if (err != E_OK) {
                const char* ctx = is_keyword(next_tok, &kw_len) ? "take len" : "take";
                error_detail_set(E_PARSE, *idx, "invalid offset '%.*s' for '%s'", tokens[*idx].len, tokens[*idx].bytes, ctx);
                return err;
            }
            (*idx)++;
        }

        /************************************************************
         * CONTROL OPERATIONS
         ************************************************************/
    } else if (is_keyword(cmd_tok, &kw_label)) {
        op->kind = OP_LABEL;

        if (*idx >= token_count) {
            error_detail_set(E_PARSE, cmd_idx, "missing label name for 'label'");
            return E_PARSE;
        }
        String name_tok = tokens[*idx];
        (*idx)++;

        if (!is_label_name_valid(name_tok)) {
            return E_LABEL_FMT;
        }

        i32 name_idx = find_or_add_label(prg, labels, name_tok);
        if (name_idx < 0) {
            return E_CAPACITY;
        }
        op->u.label.name_idx = name_idx;

        /************************************************************
         * VIEW OPERATIONS
         ************************************************************/
    } else if (is_keyword(cmd_tok, &kw_view)) {
        op->kind = OP_VIEWSET;

        if (*idx >= token_count) {
            error_detail_set(E_PARSE, cmd_idx, "missing start location for 'view'");
            return E_PARSE;
        }
        enum Err err = parse_loc_expr(tokens, idx, token_count, &op->u.viewset.a, prg, labels);
        if (err != E_OK) {
            return err;
        }

        if (*idx >= token_count) {
            error_detail_set(E_PARSE, cmd_idx, "missing end location for 'view'");
            return E_PARSE;
        }
        err = parse_loc_expr(tokens, idx, token_count, &op->u.viewset.b, prg, labels);
        if (err != E_OK) {
            return err;
        }

    } else if (is_keyword(cmd_tok, &kw_clear)) {
        // Parse second token to determine what to clear
        if (*idx >= token_count) {
            error_detail_set(E_PARSE, cmd_idx, "missing target for 'clear'");
            return E_PARSE;
        }

        i32 target_idx = *idx;
        String target_tok = tokens[*idx];
        (*idx)++;

        if (is_keyword(target_tok, &kw_view)) {
            op->kind = OP_VIEWCLEAR;
            // No additional parsing needed
        } else {
            // Future: clear <LABEL> - for now, error
            error_detail_set(E_PARSE, target_idx, "unsupported clear target '%.*s'", target_tok.len, target_tok.bytes);
            return E_PARSE;
        }

        /************************************************************
         * OUTPUT/UTILITY OPERATIONS
         ************************************************************/
    } else if (is_keyword(cmd_tok, &kw_print) || is_keyword(cmd_tok, &kw_echo)) {
        op->kind = OP_PRINT;

        // Parse string
        if (*idx >= token_count) {
            error_detail_set(E_PARSE, cmd_idx, "missing string for '%.*s'", cmd_tok.len, cmd_tok.bytes);
            return E_PARSE;
        }
        const String str_tok = tokens[*idx];
        (*idx)++;

        i32 segments = 0;
        if (!compute_print_stats(str_tok, NULL, &segments, NULL)) {
            error_detail_set(E_PARSE, cmd_idx, "invalid escape in print literal");
            return E_PARSE;
        }

        enum Err err = E_OK;
        i32 parsed_marks = 0;
        op->u.print.string = parse_string_to_bytes(str_tok, str_pool, str_pool_off, str_pool_cap, &err, &parsed_marks);
        if (err != E_OK) {
            return err;
        }
        op->u.print.cursor_marks = parsed_marks;
        op->u.print.literal_segments = segments;

    } else if (is_keyword(cmd_tok, &kw_fail)) {
        op->kind = OP_FAIL;

        // Parse message
        if (*idx >= token_count) {
            error_detail_set(E_PARSE, cmd_idx, "missing message for 'fail'");
            return E_PARSE;
        }
        const String message_tok = tokens[*idx];
        (*idx)++;

        enum Err err = E_OK;
        op->u.fail.message = parse_string_to_bytes(message_tok, str_pool, str_pool_off, str_pool_cap, &err, NULL);
        if (err != E_OK) {
            return err;
        }

    } else {
        error_detail_set(E_PARSE, cmd_idx, "unknown operation '%.*s'", cmd_tok.len, cmd_tok.bytes);
        return E_PARSE;
    }

    return E_OK;
}

static enum Err parse_loc_expr(const String* tokens, i32* idx, i32 token_count, LocExpr* loc, Program* prg, LabelTable* labels)
{
    if (*idx >= token_count) {
        error_detail_set(E_PARSE, token_count, "expected location expression");
        return E_PARSE;
    }

    // Initialize defaults
    loc->name_idx = -1;

    i32 loc_idx = *idx;
    String token_tok = tokens[*idx];
    const char* token = token_tok.bytes;
    (*idx)++;

    const char* offset_start = find_inline_offset_start(token);
    String base_tok;
    if (offset_start) {
        // Parse base part
        size_t base_len = (size_t)(offset_start - token);
        if (base_len == 0) {
            error_detail_set(E_PARSE, loc_idx, "location '%.*s' missing base before offset", token_tok.len, token_tok.bytes);
            return E_PARSE;
        }
        if (base_len > INT32_MAX) {
            error_detail_set(E_PARSE, loc_idx, "location '%.*s' base is too long", token_tok.len, token_tok.bytes);
            return E_PARSE;
        }

        // Parse offset part
        i32 offset_len = (i32)(token_tok.len - (offset_start - token_tok.bytes));
        String offset_str = { offset_start, offset_len };
        enum Err err = parse_offset(offset_str, &loc->offset, &loc->unit);
        if (err != E_OK) {
            error_detail_set(E_PARSE, loc_idx, "invalid offset '%.*s' in location '%.*s'", offset_str.len, offset_str.bytes, token_tok.len, token_tok.bytes);
            return err;
        }

        // Reuse the original token buffer for the base substring
        base_tok.bytes = token_tok.bytes;
        base_tok.len = (i32)base_len;
    } else {
        // No offset - use original token
        base_tok = token_tok;
        loc->offset = 0;
        loc->unit = UNIT_BYTES; // default unit
    }

    // Parse base location
    if (is_keyword(base_tok, &kw_cursor)) {
        loc->base = LOC_CURSOR;
    } else if (is_keyword(base_tok, &kw_bof)) {
        loc->base = LOC_BOF;
    } else if (is_keyword(base_tok, &kw_eof)) {
        loc->base = LOC_EOF;
    } else if (is_keyword(base_tok, &kw_match_start)) {
        loc->base = LOC_MATCH_START;
    } else if (is_keyword(base_tok, &kw_match_end)) {
        loc->base = LOC_MATCH_END;
    } else if (is_keyword(base_tok, &kw_line_start)) {
        loc->base = LOC_LINE_START;
    } else if (is_keyword(base_tok, &kw_line_end)) {
        loc->base = LOC_LINE_END;
    } else if (is_label_name_valid(base_tok)) {
        loc->base = LOC_NAME;
        i32 name_idx = find_or_add_label(prg, labels, base_tok);
        if (name_idx < 0) {
            return E_CAPACITY;
        }
        loc->name_idx = name_idx;
    } else {
        error_detail_set(E_PARSE, loc_idx, "unknown location '%.*s'", token_tok.len, token_tok.bytes);
        return E_PARSE;
    }

    // Support detached offset as next token (e.g., "BOF +100b")
    if (*idx < token_count) {
        i64 offset_tmp;
        Unit unit_tmp;
        enum Err off_err = parse_offset(tokens[*idx], &offset_tmp, &unit_tmp);
        if (off_err == E_OK) {
            loc->offset = offset_tmp;
            loc->unit = unit_tmp;
            (*idx)++; // consume detached offset token
        }
    }

    return E_OK;
}

static enum Err parse_at_expr(const String* tokens, i32* idx, i32 token_count, LocExpr* at)
{
    if (*idx >= token_count) {
        error_detail_set(E_PARSE, token_count, "expected location after 'at'");
        return E_PARSE;
    }

    i32 at_idx = *idx;
    String token_tok = tokens[*idx];
    const char* token = token_tok.bytes;
    (*idx)++;

    const char* offset_start = find_inline_offset_start(token);
    String base_tok;
    if (offset_start) {
        // Parse base part
        size_t base_len = (size_t)(offset_start - token);
        if (base_len == 0) {
            error_detail_set(E_PARSE, at_idx, "location '%.*s' missing base before offset", token_tok.len, token_tok.bytes);
            return E_PARSE;
        }
        if (base_len > INT32_MAX) {
            error_detail_set(E_PARSE, at_idx, "location '%.*s' base is too long", token_tok.len, token_tok.bytes);
            return E_PARSE;
        }

        // Parse offset part
        // Create String for offset part - compute length directly
        i32 offset_len = (i32)(token_tok.len - (offset_start - token_tok.bytes));
        String offset_str = { offset_start, offset_len };
        enum Err err = parse_offset(offset_str, &at->offset, &at->unit);
        if (err != E_OK) {
            error_detail_set(E_PARSE, at_idx, "invalid offset '%.*s' in location '%.*s'", offset_str.len, offset_str.bytes, token_tok.len, token_tok.bytes);
            return err;
        }

        // Reuse the original token buffer for the base substring
        base_tok.bytes = token_tok.bytes;
        base_tok.len = (i32)base_len;
    } else {
        // No offset - use original token
        base_tok = token_tok;
        at->offset = 0;
        at->unit = UNIT_BYTES; // default unit
    }

    // Parse base location
    if (is_keyword(base_tok, &kw_match_start)) {
        at->base = LOC_MATCH_START;
    } else if (is_keyword(base_tok, &kw_match_end)) {
        at->base = LOC_MATCH_END;
    } else if (is_keyword(base_tok, &kw_line_start)) {
        at->base = LOC_LINE_START;
    } else if (is_keyword(base_tok, &kw_line_end)) {
        at->base = LOC_LINE_END;
    } else {
        error_detail_set(E_PARSE, at_idx, "unknown 'at' location '%.*s'", token_tok.len, token_tok.bytes);
        return E_PARSE;
    }

    // AtExpr never uses named labels, so set name_idx to -1
    at->name_idx = -1;

    // Support detached offset as next token (e.g., "line-start -2l")
    if (*idx < token_count) {
        i64 offset_tmp;
        Unit unit_tmp;
        enum Err off_err = parse_offset(tokens[*idx], &offset_tmp, &unit_tmp);
        if (off_err == E_OK) {
            at->offset = offset_tmp;
            at->unit = unit_tmp;
            (*idx)++; // consume
        }
    }

    return E_OK;
}

static enum Err parse_offset(String token, i64* offset, Unit* unit)
{
    if (token.len <= 0) {
        return E_PARSE;
    }

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
    if (!isdigit(*p)) {
        return E_PARSE;
    }

    u64 num = 0;
    while (isdigit(*p)) {
        u64 new_num = num * 10 + (u64)(*p - '0');
        if (new_num < num) {
            return E_PARSE; // Overflow
        }
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
    if (p != token.bytes + token.len) {
        return E_PARSE; // Extra characters
    }

    // Validate character unit limits
    if (*unit == UNIT_CHARS && num > INT_MAX) {
        return E_PARSE; // Character count exceeds INT_MAX
    }

    // Convert to signed and apply sign
    if (sign < 0) {
        u64 limit = (u64)INT64_MAX + 1;
        if (num > limit) {
            return E_PARSE; // Overflow
        }
        if (num == limit) {
            *offset = INT64_MIN;
        } else {
            *offset = -(i64)num;
        }
    } else {
        if (num > (u64)INT64_MAX) {
            return E_PARSE; // Overflow
        }
        *offset = (i64)num;
    }
    return E_OK;
}

static bool is_label_name_valid(String name)
{
    return string_is_valid_label(name);
}

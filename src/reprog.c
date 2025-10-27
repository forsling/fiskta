#include "reprog.h"
#include "iosearch.h"
#include "error.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/****************************
 * CHARACTER CLASS UTILITIES
 ****************************/

static inline void cls_clear(ReClass* c) { memset(c->bits, 0, sizeof c->bits); }
static inline void cls_set(ReClass* c, unsigned char ch) { c->bits[ch >> 3] |= (unsigned char)(1U << (ch & 7)); }
static inline void cls_set_range(ReClass* c, unsigned char a, unsigned char b)
{
    if (a > b) {
        unsigned char t = a;
        a = b;
        b = t;
    }
    for (unsigned v = a;; ++v) {
        cls_set(c, (unsigned char)v);
        if (v == b) {
            break;
        }
    }
}
static inline void cls_set_ws(ReClass* c)
{
    cls_set(c, ' ');
    cls_set(c, '\t');
    cls_set(c, '\n');
    cls_set(c, '\r');
    cls_set(c, '\v');
    cls_set(c, '\f');
}
static inline void cls_set_digit(ReClass* c) { cls_set_range(c, '0', '9'); }
static inline void cls_set_word(ReClass* c)
{
    cls_set_range(c, '0', '9');
    cls_set_range(c, 'A', 'Z');
    cls_set_range(c, 'a', 'z');
    cls_set(c, '_');
}

/**********************
 * INSTRUCTION BUILDER
 **********************/

typedef struct {
    ReProg* out;
    ReInst* ins;
    int nins, ins_cap;
    ReClass* cls;
    int ncls, cls_cap;
    int next_counter_id; // For allocating counter IDs to quantified groups
    unsigned char has_lazy; // 1 if any lazy quantifier seen
    String pattern; // Pattern being compiled (for error messages)
} ReB;

static enum Err emit_inst(ReB* b, ReOp op, int x, int y, unsigned char ch, int cls_idx, int* out_idx)
{
    if (b->nins >= b->ins_cap) {
        error_detail_set(E_CAPACITY, -1,
            "regex: pattern too complex (needs %d+ instructions, max %d); reduce alternations/quantifiers",
            b->nins + 1, b->ins_cap);
        return E_CAPACITY;
    }
    int idx = b->nins++;
    b->ins[idx].op = op;
    b->ins[idx].x = x;
    b->ins[idx].y = y;
    b->ins[idx].ch = ch;
    b->ins[idx].cls_idx = cls_idx;
    if (out_idx) {
        *out_idx = idx;
    }
    return E_OK;
}

static enum Err emit_class(ReB* b, const ReClass* src, int* idx_out)
{
    if (b->ncls >= b->cls_cap) {
        error_detail_set(E_CAPACITY, -1,
            "regex: too many character classes (needs %d+, max %d); simplify pattern",
            b->ncls + 1, b->cls_cap);
        return E_CAPACITY;
    }
    b->cls[b->ncls] = *src;
    *idx_out = b->ncls++;
    return E_OK;
}

/*************************
 * CHARACTER CLASS PARSER
 *************************/

// Parse a character class: pattern points at first char AFTER '['; returns index AFTER ']'
static enum Err parse_char_class(ReB* b, String pat, int* i_inout, int* out_cls_idx)
{
    int i = *i_inout;
    ReClass cls;
    cls_clear(&cls);
    int negated = 0;

    if (pat.bytes[i] == ']') {
        return E_PARSE; // empty
    }

    // Check for negation
    if (pat.bytes[i] == '^') {
        negated = 1;
        i++;
    }

    while (i < pat.len && pat.bytes[i] != ']') {
        unsigned char a;
        if (pat.bytes[i] == '\\') {
            ++i;
            if (i >= pat.len) {
                return E_PARSE;
            }
            switch (pat.bytes[i]) {
            case 'd':
                cls_set_digit(&cls);
                break;
            case 'D': { // non-digits: set all then clear digits
                for (int v = 0; v < 256; ++v) {
                    cls_set(&cls, (unsigned char)v);
                }
                ReClass d;
                cls_clear(&d);
                cls_set_digit(&d);
                for (int bit = 0; bit < 32; ++bit) {
                    cls.bits[bit] &= (unsigned char)~d.bits[bit];
                }
            } break;
            case 'w':
                cls_set_word(&cls);
                break;
            case 'W': {
                for (int v = 0; v < 256; ++v) {
                    cls_set(&cls, (unsigned char)v);
                }
                ReClass w;
                cls_clear(&w);
                cls_set_word(&w);
                for (int bit = 0; bit < 32; ++bit) {
                    cls.bits[bit] &= (unsigned char)~w.bits[bit];
                }
            } break;
            case 's':
                cls_set_ws(&cls);
                break;
            case 'S': {
                for (int v = 0; v < 256; ++v) {
                    cls_set(&cls, (unsigned char)v);
                }
                ReClass ws;
                cls_clear(&ws);
                cls_set_ws(&ws);
                for (int bit = 0; bit < 32; ++bit) {
                    cls.bits[bit] &= (unsigned char)~ws.bits[bit];
                }
            } break;
            default:
                cls_set(&cls, (unsigned char)pat.bytes[i]);
                break;
            }
            ++i;
        } else {
            a = (unsigned char)pat.bytes[i++];
            if (i < pat.len && pat.bytes[i] == '-' && i + 1 < pat.len && pat.bytes[i + 1] != ']') {
                unsigned char bch;
                ++i;
                if (pat.bytes[i] == '\\') {
                    ++i;
                    if (i >= pat.len) {
                        return E_PARSE;
                    }
                    bch = (unsigned char)pat.bytes[i++];
                } else {
                    bch = (unsigned char)pat.bytes[i++];
                }
                cls_set_range(&cls, a, bch);
            } else {
                cls_set(&cls, a);
            }
        }
    }
    if (i >= pat.len || pat.bytes[i] != ']') {
        return E_PARSE;
    }
    ++i;

    // Apply negation if needed
    if (negated) {
        ReClass negated_cls;
        cls_clear(&negated_cls);
        for (int v = 0; v < 256; ++v) {
            cls_set(&negated_cls, (unsigned char)v);
        }
        for (int bit = 0; bit < 32; ++bit) {
            negated_cls.bits[bit] &= (unsigned char)~cls.bits[bit];
        }
        cls = negated_cls;
    }

    int cls_idx;
    enum Err e = emit_class(b, &cls, &cls_idx);
    if (e != E_OK) {
        return e;
    }
    *i_inout = i;
    *out_cls_idx = cls_idx;
    return E_OK;
}

/**********************
 * PATTERN COMPILATION
 **********************/

static enum Err compile_atom(ReB* b, String pat, int* i_inout, bool* out_nullable);

// Syntactic nullability analysis - determines if pattern can match empty string
// without actually compiling it. Returns true if pattern is nullable.
// This is used to reject pathological patterns like (a*)* before compilation.
static bool is_pattern_nullable(String pat, int len);

// Syntactic nullability check implementation
// Add recursion depth guard to prevent stack overflow
static bool is_pattern_nullable_impl(String pat, int len, int depth)
{
    // Guard against stack overflow from deeply nested patterns
    if (depth > 100) {
        return false; // Treat as non-nullable to avoid crash
    }

    if (len == 0) {
        return true; // Empty pattern is nullable
    }

    // Check for alternation at top level - pattern is nullable if ANY branch is nullable
    int paren_depth = 0;
    bool has_alt = false;
    int alt_start = 0;
    for (int j = 0; j < len; ++j) {
        if (pat.bytes[j] == '\\') {
            ++j; // Skip escaped char
            continue;
        }
        if (pat.bytes[j] == '(') {
            ++paren_depth;
        } else if (pat.bytes[j] == ')') {
            --paren_depth;
        } else if (pat.bytes[j] == '|' && paren_depth == 0) {
            has_alt = true;
            // Check this alternative
            String alt = (String) { pat.bytes + alt_start, j - alt_start };
            if (is_pattern_nullable_impl(alt, j - alt_start, depth + 1)) {
                return true; // At least one alt is nullable
            }
            alt_start = j + 1;
        }
    }

    // If we found alternation, check final alternative and return
    if (has_alt) {
        String alt = (String) { pat.bytes + alt_start, len - alt_start };
        return is_pattern_nullable_impl(alt, len - alt_start, depth + 1);
    }

    // No alternation at top level - check concatenation
    // ALL atoms must be nullable for concat to be nullable
    int i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)pat.bytes[i];

        // Anchors are nullable
        if (c == '^' || c == '$') {
            ++i;
            continue;
        }

        // Escaped characters
        if (c == '\\') {
            ++i;
            if (i >= len) {
                return false; // Malformed, treat as non-nullable
            }
            // unsigned char ec = (unsigned char)pat.bytes[i];  // unused
            ++i;
            // Character classes like \d, \w, \s are NOT nullable (they match chars)
            // Check for quantifier
            if (i < len) {
                c = (unsigned char)pat.bytes[i];
                if (c == '*' || c == '?' || c == '{') {
                    // Quantifier - could be nullable
                    if (c == '*' || c == '?') {
                        ++i; // These make the atom nullable, continue
                        continue;
                    }
                    // Parse {min,max}
                    if (c == '{') {
                        ++i;
                        int min_count = 0;
                        while (i < len && pat.bytes[i] >= '0' && pat.bytes[i] <= '9') {
                            min_count = min_count * 10 + (pat.bytes[i] - '0');
                            ++i;
                        }
                        if (i < len && pat.bytes[i] == ',') {
                            ++i;
                            while (i < len && pat.bytes[i] >= '0' && pat.bytes[i] <= '9') {
                                ++i;
                            }
                        }
                        if (i < len && pat.bytes[i] == '}') {
                            ++i;
                        }
                        // If min_count is 0, this is nullable
                        if (min_count == 0) {
                            continue;
                        }
                    }
                }
            }
            // No quantifier or non-nullable quantifier - this atom is not nullable
            return false;
        }

        // Character classes [...]
        if (c == '[') {
            int j = i + 1;
            if (j < len && pat.bytes[j] == '^') {
                ++j;
            }
            while (j < len && pat.bytes[j] != ']') {
                if (pat.bytes[j] == '\\') {
                    ++j;
                }
                ++j;
            }
            if (j < len) {
                ++j; // Skip ]
            }
            // Character class is not nullable unless followed by *, ?, or {0...}
            if (j < len) {
                c = (unsigned char)pat.bytes[j];
                if (c == '*' || c == '?') {
                    i = j + 1;
                    continue; // Nullable
                }
                if (c == '{') {
                    // Parse quantifier
                    ++j;
                    int min_count = 0;
                    while (j < len && pat.bytes[j] >= '0' && pat.bytes[j] <= '9') {
                        min_count = min_count * 10 + (pat.bytes[j] - '0');
                        ++j;
                    }
                    if (j < len && pat.bytes[j] == ',') {
                        ++j;
                        while (j < len && pat.bytes[j] >= '0' && pat.bytes[j] <= '9') {
                            ++j;
                        }
                    }
                    if (j < len && pat.bytes[j] == '}') {
                        ++j;
                    }
                    if (min_count == 0) {
                        i = j;
                        continue; // Nullable
                    }
                }
            }
            return false; // Character class without nullable quantifier
        }

        // Groups (...)
        if (c == '(') {
            // Find matching )
            int group_depth = 1;
            int j = i + 1;
            while (j < len && group_depth > 0) {
                if (pat.bytes[j] == '\\') {
                    ++j;
                } else if (pat.bytes[j] == '(') {
                    ++group_depth;
                } else if (pat.bytes[j] == ')') {
                    --group_depth;
                }
                ++j;
            }
            // Group content is pat[i+1 .. j-1)
            int inner_len = j - i - 2;
            bool group_nullable = false;
            if (inner_len >= 0) {
                String inner = (String) { pat.bytes + i + 1, inner_len };
                group_nullable = is_pattern_nullable_impl(inner, inner_len, depth + 1);
            }

            // Check for quantifier after group
            if (j < len) {
                c = (unsigned char)pat.bytes[j];
                if (c == '*' || c == '?') {
                    i = j + 1;
                    continue; // Nullable regardless of group content
                }
                if (c == '{') {
                    // Parse quantifier
                    ++j;
                    int min_count = 0;
                    while (j < len && pat.bytes[j] >= '0' && pat.bytes[j] <= '9') {
                        min_count = min_count * 10 + (pat.bytes[j] - '0');
                        ++j;
                    }
                    if (j < len && pat.bytes[j] == ',') {
                        ++j;
                        while (j < len && pat.bytes[j] >= '0' && pat.bytes[j] <= '9') {
                            ++j;
                        }
                    }
                    if (j < len && pat.bytes[j] == '}') {
                        ++j;
                    }
                    if (min_count == 0) {
                        i = j;
                        continue; // Nullable
                    }
                    // min_count >= 1: nullable only if group itself is nullable
                    if (!group_nullable) {
                        return false;
                    }
                    i = j;
                    continue;
                }
            }

            // No quantifier - check if group itself is nullable
            if (!group_nullable) {
                return false;
            }
            i = j;
            continue;
        }

        // Literal character
        // Check for quantifier
        if (i + 1 < len) {
            c = (unsigned char)pat.bytes[i + 1];
            if (c == '*' || c == '?') {
                i += 2;
                continue; // Nullable
            }
            if (c == '{') {
                int j = i + 2;
                int min_count = 0;
                while (j < len && pat.bytes[j] >= '0' && pat.bytes[j] <= '9') {
                    min_count = min_count * 10 + (pat.bytes[j] - '0');
                    ++j;
                }
                if (j < len && pat.bytes[j] == ',') {
                    ++j;
                    while (j < len && pat.bytes[j] >= '0' && pat.bytes[j] <= '9') {
                        ++j;
                    }
                }
                if (j < len && pat.bytes[j] == '}') {
                    ++j;
                }
                if (min_count == 0) {
                    i = j;
                    continue; // Nullable
                }
            }
        }
        // Literal without nullable quantifier
        return false;
    }

    // All atoms in concatenation were nullable
    return true;
}

// Wrapper for is_pattern_nullable_impl with initial depth=0
static bool is_pattern_nullable(String pat, int len)
{
    return is_pattern_nullable_impl(pat, len, 0);
}

// Compiles pat[0..len) into `b` without emitting RI_MATCH.
// Uses N-1 splits so there is no epsilon path that skips all alts.
// If out_nullable is non-NULL, sets *out_nullable to true if pattern can match empty string.
static enum Err compile_alt_sequence(ReB* b, String pat, int len, bool* out_nullable)
{
    // 1) Collect top-level alternatives (respect escapes/parentheses)
    int depth = 0;
    int nalt = 1;
    for (int j = 0; j < len; ++j) {
        if (pat.bytes[j] == '\\') {
            if (j + 1 < len) {
                ++j;
            }
            continue;
        }
        if (pat.bytes[j] == '(') {
            ++depth;
        } else if (pat.bytes[j] == ')') {
            --depth;
        } else if (pat.bytes[j] == '|' && depth == 0) {
            ++nalt;
        }
    }

    // Single alt: compile linearly and return
    if (nalt == 1) {
        int i = 0;
        String tmp_bytes = { pat.bytes, len };
        // Concatenation is nullable only if ALL atoms are nullable
        bool sequence_nullable = true;
        while (i < len) {
            bool atom_nullable = false;
            enum Err e = compile_atom(b, tmp_bytes, &i, &atom_nullable);
            if (e != E_OK) {
                return e;
            }
            if (!atom_nullable) {
                sequence_nullable = false;
            }
        }
        if (out_nullable) {
            *out_nullable = sequence_nullable;
        }
        return E_OK;
    }

    // Prevent stack overflow with too many alternatives
    if (nalt > MAX_ALTS) {
        return E_PARSE; // Too many alternations
    }

    // 2) Record (lo,len) for each alt using fixed-size arrays
    int lo_arr[MAX_ALTS];
    int alen_arr[MAX_ALTS];
    int split_pc_arr[MAX_ALTS];
    int alt_start_pc_arr[MAX_ALTS];
    int jmp_pc_arr[MAX_ALTS];

    int* lo = lo_arr;
    int* alen = alen_arr;
    int* split_pc = split_pc_arr;
    int* alt_start_pc = alt_start_pc_arr;
    int* jmp_pc = jmp_pc_arr;
    enum Err err = E_OK;

    int k = 0;
    int start = 0;
    depth = 0;
    for (int j = 0; j < len; ++j) {
        if (pat.bytes[j] == '\\') {
            if (j + 1 < len) {
                ++j;
            }
            continue;
        }
        if (pat.bytes[j] == '(') {
            ++depth;
        } else if (pat.bytes[j] == ')') {
            --depth;
        } else if (pat.bytes[j] == '|' && depth == 0) {
            lo[k] = start;
            alen[k] = j - start;
            ++k;
            start = j + 1;
        }
    }
    lo[k] = start;
    alen[k] = len - start; /* k == nalt-1 */

    // 3) Emit N-1 splits + all alt bodies
    enum Err e;
    // Track if any alternative is nullable (alternation is nullable if ANY alt is nullable)
    bool any_alt_nullable = false;

    // Emit a split before each of the first N-1 alts, then their bodies
    for (int i = 0; i < nalt - 1; ++i) {
        e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &split_pc[i]);
        if (e != E_OK) {
            err = e;
            goto cleanup;
        }

        alt_start_pc[i] = b->nins;
        // compile alt i - each alt is a sequence, nullable only if all atoms nullable
        int pi = 0;
        String frag_bytes = { pat.bytes + lo[i], alen[i] };
        bool this_alt_nullable = true;
        while (pi < alen[i]) {
            bool atom_nullable = false;
            e = compile_atom(b, frag_bytes, &pi, &atom_nullable);
            if (e != E_OK) {
                err = e;
                goto cleanup;
            }
            if (!atom_nullable) {
                this_alt_nullable = false;
            }
        }
        if (this_alt_nullable) {
            any_alt_nullable = true;
        }

        e = emit_inst(b, RI_JMP, -1, 0, 0, -1, &jmp_pc[i]);
        if (e != E_OK) {
            err = e;
            goto cleanup;
        }
    }

    // Last alternative (no leading split)
    alt_start_pc[nalt - 1] = b->nins;
    int pi = 0;
    String last_bytes = { pat.bytes + lo[nalt - 1], alen[nalt - 1] };
    bool last_alt_nullable = true;
    while (pi < alen[nalt - 1]) {
        bool atom_nullable = false;
        e = compile_atom(b, last_bytes, &pi, &atom_nullable);
        if (e != E_OK) {
            err = e;
            goto cleanup;
        }
        if (!atom_nullable) {
            last_alt_nullable = false;
        }
    }
    if (last_alt_nullable) {
        any_alt_nullable = true;
    }
    e = emit_inst(b, RI_JMP, -1, 0, 0, -1, &jmp_pc[nalt - 1]);
    if (e != E_OK) {
        err = e;
        goto cleanup;
    }

    // 4) Continuation point and patching
    int cont = b->nins;
    for (int i = 0; i < nalt; ++i) {
        b->ins[jmp_pc[i]].x = cont;
    }

    for (int i = 0; i < nalt - 1; ++i) {
        b->ins[split_pc[i]].x = alt_start_pc[i];
        b->ins[split_pc[i]].y = (i + 1 < nalt - 1) ? split_pc[i + 1] : alt_start_pc[nalt - 1];
    }

    // Set nullable output if caller requested it
    if (out_nullable) {
        *out_nullable = any_alt_nullable;
    }

    err = E_OK;

cleanup:
    return err;
}

// Parse quantifier at position i, updating i_inout to position after quantifier
// Returns parsed min/max counts, whether a quantifier was found, and if it's lazy
static enum Err parse_quantifier(String pat, int* i_inout, int* min_count, int* max_count, bool* is_quantified, bool* is_lazy)
{
    int i = *i_inout;
    *is_quantified = false;
    *is_lazy = false;
    *min_count = 1;
    *max_count = 1;

    if (i >= pat.len) {
        return E_OK;
    }

    char q = pat.bytes[i];

    if (q == '?') {
        *min_count = 0;
        *max_count = 1;
        *is_quantified = true;
        i++;
    } else if (q == '*') {
        *min_count = 0;
        *max_count = -1;
        *is_quantified = true;
        i++;
    } else if (q == '+') {
        *min_count = 1;
        *max_count = -1;
        *is_quantified = true;
        i++;
    } else if (q == '{') {
        // Parse {n,m} quantifier
        i++; // skip '{'
        if (i >= pat.len || !isdigit(pat.bytes[i])) {
            return E_PARSE;
        }

        // Parse minimum count
        *min_count = 0;
        while (i < pat.len && isdigit(pat.bytes[i])) {
            // Check for overflow before multiplication
            // Safe limit: (INT_MAX - 9) / 10 to ensure count * 10 + digit fits in int
            if (*min_count > (INT_MAX - 9) / 10) {
                return E_PARSE; // quantifier too large
            }
            *min_count = *min_count * 10 + (pat.bytes[i] - '0');
            i++;
        }

        if (i >= pat.len) {
            return E_PARSE; // missing closing '}'
        }
        if (pat.bytes[i] == '}') {
            // {n} - exactly n times
            *max_count = *min_count;
            i++;
        } else if (pat.bytes[i] == ',') {
            i++; // skip ','
            if (i >= pat.len) {
                return E_PARSE; // missing closing '}'
            }
            if (pat.bytes[i] == '}') {
                // {n,} - n or more times
                *max_count = -1; // unlimited
                i++;
            } else if (isdigit(pat.bytes[i])) {
                // {n,m} - between n and m times
                *max_count = 0;
                while (i < pat.len && isdigit(pat.bytes[i])) {
                    // Check for overflow before multiplication
                    if (*max_count > (INT_MAX - 9) / 10) {
                        return E_PARSE; // quantifier too large
                    }
                    *max_count = *max_count * 10 + (pat.bytes[i] - '0');
                    i++;
                }
                if (i >= pat.len || pat.bytes[i] != '}') {
                    return E_PARSE;
                }
                i++;
            } else {
                return E_PARSE;
            }
        } else {
            return E_PARSE;
        }
        *is_quantified = true;
    }

    // Validate quantifier bounds
    if (*is_quantified) {
        if (*max_count > 0 && *min_count > *max_count) {
            return E_PARSE;
        }
        // Check for lazy suffix '?'
        if (i < pat.len && pat.bytes[i] == '?') {
            *is_lazy = true;
            i++;
        }
    }

    *i_inout = i;
    return E_OK;
}

// Compile a single regex atom (+ optional quantifier)
// If out_nullable is non-NULL, sets *out_nullable to true if atom can match empty string.
static enum Err compile_atom(ReB* b, String pat, int* i_inout, bool* out_nullable)
{
    int i = *i_inout;
    if (i >= pat.len) {
        return E_PARSE;
    }

    // Initialize nullable to false by default (most atoms consume input)
    bool nullable = false;

    // Identify atom without emitting yet if quantifier needs ordering
    enum { A_CHAR,
        A_ANY,
        A_CLASS,
        A_BOL,
        A_EOL } ak;
    unsigned char ch = 0;
    int cls_idx = -1;

    // --- Unescaped grouping '(' ... ')' (handles nested alternation) ---
    if (pat.bytes[i] == '(') {
        int j = i + 1;
        int depth = 1;
        while (j < pat.len) {
            if (pat.bytes[j] == '\\') {
                if (j + 1 < pat.len) {
                    j += 2;
                    continue;
                }
                return E_PARSE;
            }
            if (pat.bytes[j] == '(') {
                depth++;
            } else if (pat.bytes[j] == ')') {
                depth--;
                if (depth == 0) {
                    break;
                }
            }
            j++;
        }
        if (j >= pat.len || pat.bytes[j] != ')') {
            return E_PARSE; // unmatched '('
        }

        int inner_lo = i + 1;
        int inner_len = j - inner_lo;

        if (inner_len == 0) {
            // Empty group - skip past ) and any quantifier
            int k = j + 1;
            int min_count, max_count;
            bool is_quantified, is_lazy;
            enum Err e = parse_quantifier(pat, &k, &min_count, &max_count, &is_quantified, &is_lazy);
            if (e != E_OK) {
                return e;
            }
            *i_inout = k;
            // Empty group is always nullable
            if (out_nullable) {
                *out_nullable = true;
            }
            return E_OK; // epsilon group
        }

        // Parse quantifier after the closing )
        int k = j + 1;
        int min_count, max_count;
        bool is_quantified, is_lazy;
        enum Err e = parse_quantifier(pat, &k, &min_count, &max_count, &is_quantified, &is_lazy);
        if (e != E_OK) {
            return e;
        }
        if (is_lazy) {
            b->has_lazy = 1;
        }

        String inner = (String) { pat.bytes + inner_lo, inner_len };

        if (!is_quantified) {
            // No quantifier - just compile the group and propagate its nullability
            bool group_nullable = false;
            e = compile_alt_sequence(b, inner, inner_len, &group_nullable);
            if (e != E_OK) {
                return e;
            }
            *i_inout = k;
            if (out_nullable) {
                *out_nullable = group_nullable;
            }
            return E_OK;
        }

        // For quantified groups, check if the result would be nullable
        // Quantifier-level nullability: {0}, {0,n}, *, ? always make result nullable
        // regardless of inner pattern
        bool quantifier_makes_nullable = (min_count == 0);

        // Use syntactic nullable analysis to check inner pattern
        // This correctly handles nested {0} patterns without compilation
        bool inner_nullable = is_pattern_nullable(inner, inner_len);

        // Reject unbounded or very large quantifiers on nullable patterns
        // Unbounded means max_count == -1 (infinite upper bound)
        // Very large means max_count > 1000 (effectively unbounded for nullable patterns)
        // Examples: (a*)*, (a*)+, (a*){5,}, (\W?){10,}, ((x{0})){0,999999}
        if (inner_nullable && (max_count == -1 || max_count > 1000)) {
            error_detail_set(E_PARSE, -1,
                "regex: unbounded or very large quantifier on nullable pattern (can match empty string repeatedly)");
            return E_PARSE;
        }

        // Handle group quantifiers with counter-based approach
        // This achieves true 0% capacity errors by avoiding expansion

        // Special case: {0} - emit nothing (no counter needed)
        if (max_count == 0) {
            *i_inout = k;
            if (out_nullable) {
                *out_nullable = quantifier_makes_nullable; // Always true for {0}
            }
            return E_OK;
        }

        // Special case: {1} - compile group once (no counter needed)
        if (min_count == 1 && max_count == 1) {
            e = compile_alt_sequence(b, inner, inner_len, NULL);
            if (e != E_OK) {
                return e;
            }
            *i_inout = k;
            // {1} preserves inner nullability (but we'd need to check it)
            // Since we don't have it and {1} with min=1, result is not nullable from quantifier
            if (out_nullable) {
                *out_nullable = false; // {1} doesn't make nullable
            }
            return E_OK;
        }

        if (min_count == 0 && max_count == 1) {
            // ? quantifier - split(take, skip) for greedy; split(skip, take) for lazy
            int split_pc;
            // ch: bit0=repeat(1), bit1=lazy(if is_lazy)
            e = emit_inst(b, RI_SPLIT, -1, -1, 0x01 | (is_lazy ? 0x02 : 0x00), -1, &split_pc);
            if (e != E_OK) {
                return e;
            }
            int group_start = b->nins;
            e = compile_alt_sequence(b, inner, inner_len, NULL);
            if (e != E_OK) {
                return e;
            }
            if (is_lazy) {
                b->ins[split_pc].x = b->nins; // skip first -> lazy
                b->ins[split_pc].y = group_start; // take second
            } else {
                b->ins[split_pc].x = group_start; // take first -> greedy
                b->ins[split_pc].y = b->nins; // skip second
            }
            if (out_nullable) {
                *out_nullable = quantifier_makes_nullable; // Always true for ?
            }
        } else if (max_count == -1) {
            // {n,} or + or * - use counter-based loop (no pre-expansion)
            // Note: nullable patterns already rejected above
            if (min_count == 0) {
                // * quantifier - split(loop, stop) for greedy; split(stop, loop) for lazy
                int split_pc;
                e = emit_inst(b, RI_SPLIT, -1, -1, 0x01 | (is_lazy ? 0x02 : 0x00), -1, &split_pc);
                if (e != E_OK) {
                    return e;
                }
                int group_start = b->nins;
                e = compile_alt_sequence(b, inner, inner_len, NULL);
                if (e != E_OK) {
                    return e;
                }
                int jmp_pc;
                e = emit_inst(b, RI_JMP, split_pc, 0, 0, -1, &jmp_pc);
                if (e != E_OK) {
                    return e;
                }
                if (is_lazy) {
                    b->ins[split_pc].x = b->nins; // stop first -> lazy
                    b->ins[split_pc].y = group_start; // loop second
                } else {
                    b->ins[split_pc].x = group_start; // loop first -> greedy
                    b->ins[split_pc].y = b->nins; // stop second
                }
                if (out_nullable) {
                    *out_nullable = quantifier_makes_nullable; // True for *
                }
            } else if (min_count == 1) {
                // + quantifier - split(loop, stop) for greedy; split(stop, loop) for lazy
                e = compile_alt_sequence(b, inner, inner_len, NULL);
                if (e != E_OK) {
                    return e;
                }
                int split_pc;
                // ch: bit0=repeat(1), bit1=lazy(if is_lazy)
                e = emit_inst(b, RI_SPLIT, -1, -1, 0x01 | (is_lazy ? 0x02 : 0x00), -1, &split_pc);
                if (e != E_OK) {
                    return e;
                }
                int group_start = b->nins;
                e = compile_alt_sequence(b, inner, inner_len, NULL);
                if (e != E_OK) {
                    return e;
                }
                int jmp_pc;
                e = emit_inst(b, RI_JMP, split_pc, 0, 0, -1, &jmp_pc);
                if (e != E_OK) {
                    return e;
                }
                if (is_lazy) {
                    b->ins[split_pc].x = b->nins; // stop first -> lazy
                    b->ins[split_pc].y = group_start; // loop second
                } else {
                    b->ins[split_pc].x = group_start; // loop first -> greedy
                    b->ins[split_pc].y = b->nins; // stop second
                }
                if (out_nullable) {
                    *out_nullable = quantifier_makes_nullable; // False for +
                }
            } else {
                // {n,} where n >= 2 - use counter-based loop
                int counter_id = b->next_counter_id++;
                if (counter_id >= MAX_RE_COUNTERS) {
                    error_detail_set(E_CAPACITY, -1,
                        "regex: too many quantified groups in pattern (max %d); reduce nesting or use simpler quantifiers",
                        MAX_RE_COUNTERS);
                    return E_CAPACITY;
                }

                // COUNTER_RESET: initialize counter to 0
                e = emit_inst(b, RI_COUNTER_RESET, counter_id, 0, 0, -1, NULL);
                if (e != E_OK) {
                    return e;
                }

                // Loop: pattern, INC, SPLIT->(loop | CHECK_MIN+exit)
                int loop_start = b->nins;

                // Emit the pattern once
                e = compile_alt_sequence(b, inner, inner_len, NULL);
                if (e != E_OK) {
                    return e;
                }

                // Increment counter after successful match
                e = emit_inst(b, RI_COUNTER_INC, counter_id, 0, 0, -1, NULL);
                if (e != E_OK) {
                    return e;
                }
                if (out_nullable) {
                    *out_nullable = quantifier_makes_nullable; // False for {n,} where n>=2
                }

                // SPLIT: split(loop, exit) for greedy; split(exit, loop) for lazy
                int split_pc;
                // ch: bit0=repeat(1), bit1=lazy(if is_lazy)
                e = emit_inst(b, RI_SPLIT, -1, -1, 0x01 | (is_lazy ? 0x02 : 0x00), -1, &split_pc);
                if (e != E_OK) {
                    return e;
                }

                // First branch: loop unconditionally (no max limit)
                int loop_branch = b->nins;
                e = emit_inst(b, RI_JMP, loop_start, 0, 0, -1, NULL);
                if (e != E_OK) {
                    return e;
                }

                // Second branch: check minimum before exiting
                int exit_branch = b->nins;
                e = emit_inst(b, RI_COUNTER_CHECK_MIN, counter_id, min_count, 0, -1, NULL);
                if (e != E_OK) {
                    return e;
                }

                // Wire up SPLIT
                if (is_lazy) {
                    b->ins[split_pc].x = exit_branch; // lazy: try to exit first
                    b->ins[split_pc].y = loop_branch; // or loop
                } else {
                    b->ins[split_pc].x = loop_branch; // greedy: try to loop first
                    b->ins[split_pc].y = exit_branch; // or exit
                }
            }
        } else if (max_count > 1) {
            // Bounded {n,m} or {n} - use counter-based loop
            // These are safe even with nullable patterns (bounded)
            int counter_id = b->next_counter_id++;
            if (counter_id >= MAX_RE_COUNTERS) {
                error_detail_set(E_CAPACITY, -1,
                    "regex: too many quantified groups in pattern (max %d); reduce nesting or use simpler quantifiers",
                    MAX_RE_COUNTERS);
                return E_CAPACITY;
            }

            // COUNTER_RESET: initialize counter to 0
            e = emit_inst(b, RI_COUNTER_RESET, counter_id, 0, 0, -1, NULL);
            if (e != E_OK) {
                return e;
            }

            // Loop: pattern, INC, SPLIT->(CHECK+JMP | continue)
            int loop_start = b->nins;

            // Emit the pattern once
            e = compile_alt_sequence(b, inner, inner_len, NULL);
            if (e != E_OK) {
                return e;
            }

            // Increment counter after successful match
            e = emit_inst(b, RI_COUNTER_INC, counter_id, 0, 0, -1, NULL);
            if (e != E_OK) {
                return e;
            }
            // Bounded quantifiers: nullable if min_count == 0
            if (out_nullable) {
                *out_nullable = quantifier_makes_nullable;
            }

            // For exact count: use SPLIT to decide between looping and exiting
            if (max_count == min_count) {
                // SPLIT: split(loop, exit) for greedy; split(exit, loop) for lazy
                int split_pc;
                // ch: bit0=repeat(1), bit1=lazy(if is_lazy)
                e = emit_inst(b, RI_SPLIT, -1, -1, 0x01 | (is_lazy ? 0x02 : 0x00), -1, &split_pc);
                if (e != E_OK) {
                    return e;
                }

                // First branch: check if we can loop, then jump
                int check_branch = b->nins;
                e = emit_inst(b, RI_COUNTER_CHECK, counter_id, max_count, 0, -1, NULL);
                if (e != E_OK) {
                    return e;
                }
                e = emit_inst(b, RI_JMP, loop_start, 0, 0, -1, NULL);
                if (e != E_OK) {
                    return e;
                }

                // Second branch: check minimum before exit
                int exit_branch = b->nins;
                e = emit_inst(b, RI_COUNTER_CHECK_MIN, counter_id, min_count, 0, -1, NULL);
                if (e != E_OK) {
                    return e;
                }

                // Wire up SPLIT
                if (is_lazy) {
                    b->ins[split_pc].x = exit_branch; // lazy: try to exit first
                    b->ins[split_pc].y = check_branch; // or loop
                } else {
                    b->ins[split_pc].x = check_branch; // greedy: try to loop first
                    b->ins[split_pc].y = exit_branch; // or exit
                }
            } else {
                // Range {n,m} where n < m
                // SPLIT: split(loop, exit) for greedy; split(exit, loop) for lazy
                int split_pc;
                // ch: bit0=repeat(1), bit1=lazy(if is_lazy)
                e = emit_inst(b, RI_SPLIT, -1, -1, 0x01 | (is_lazy ? 0x02 : 0x00), -1, &split_pc);
                if (e != E_OK) {
                    return e;
                }

                // First branch: check if we can loop, then jump
                int check_branch = b->nins;
                e = emit_inst(b, RI_COUNTER_CHECK, counter_id, max_count, 0, -1, NULL);
                if (e != E_OK) {
                    return e;
                }
                e = emit_inst(b, RI_JMP, loop_start, 0, 0, -1, NULL);
                if (e != E_OK) {
                    return e;
                }

                // Second branch: check minimum before exit
                int exit_branch = b->nins;
                e = emit_inst(b, RI_COUNTER_CHECK_MIN, counter_id, min_count, 0, -1, NULL);
                if (e != E_OK) {
                    return e;
                }

                // Wire up SPLIT
                if (is_lazy) {
                    b->ins[split_pc].x = exit_branch; // lazy: try to exit first
                    b->ins[split_pc].y = check_branch; // or loop
                } else {
                    b->ins[split_pc].x = check_branch; // greedy: try to loop first
                    b->ins[split_pc].y = exit_branch; // or exit
                }
            }
        }

        *i_inout = k;
        return E_OK;
    } else if (pat.bytes[i] == '^') {
        ak = A_BOL;
        nullable = true; // BOL is nullable (doesn't consume input)
        i++;
    } else if (pat.bytes[i] == '$') {
        ak = A_EOL;
        nullable = true; // EOL is nullable (doesn't consume input)
        i++;
    } else if (pat.bytes[i] == '.') {
        ak = A_ANY;
        // A_ANY is not nullable (consumes exactly one char)
        i++;
    } else if (pat.bytes[i] == '[') {
        i++;
        enum Err e = parse_char_class(b, pat, &i, &cls_idx);
        if (e != E_OK) {
            return e;
        }
        ak = A_CLASS;
    } else if (pat.bytes[i] == '\\') {
        i++;
        if (i >= pat.len) {
            return E_PARSE;
        }
        switch (pat.bytes[i]) {
        case 'd': {
            ReClass c;
            cls_clear(&c);
            cls_set_digit(&c);
            enum Err e = emit_class(b, &c, &cls_idx);
            if (e != E_OK) {
                return e;
            }
            ak = A_CLASS;
        } break;
        case 'D': {
            ReClass c;
            cls_clear(&c);
            for (int v = 0; v < 256; ++v) {
                cls_set(&c, (unsigned char)v);
            }
            ReClass d;
            cls_clear(&d);
            cls_set_digit(&d);
            for (int b2 = 0; b2 < 32; ++b2) {
                c.bits[b2] &= (unsigned char)~d.bits[b2];
            }
            enum Err e = emit_class(b, &c, &cls_idx);
            if (e != E_OK) {
                return e;
            }
            ak = A_CLASS;
        } break;
        case 'w': {
            ReClass c;
            cls_clear(&c);
            cls_set_word(&c);
            enum Err e = emit_class(b, &c, &cls_idx);
            if (e != E_OK) {
                return e;
            }
            ak = A_CLASS;
        } break;
        case 'W': {
            ReClass c;
            cls_clear(&c);
            for (int v = 0; v < 256; ++v) {
                cls_set(&c, (unsigned char)v);
            }
            ReClass w;
            cls_clear(&w);
            cls_set_word(&w);
            for (int b2 = 0; b2 < 32; ++b2) {
                c.bits[b2] &= (unsigned char)~w.bits[b2];
            }
            enum Err e = emit_class(b, &c, &cls_idx);
            if (e != E_OK) {
                return e;
            }
            ak = A_CLASS;
        } break;
        case 's': {
            ReClass c;
            cls_clear(&c);
            cls_set_ws(&c);
            enum Err e = emit_class(b, &c, &cls_idx);
            if (e != E_OK) {
                return e;
            }
            ak = A_CLASS;
        } break;
        case 'S': {
            ReClass c;
            cls_clear(&c);
            for (int v = 0; v < 256; ++v) {
                cls_set(&c, (unsigned char)v);
            }
            ReClass ws;
            cls_clear(&ws);
            cls_set_ws(&ws);
            for (int b2 = 0; b2 < 32; ++b2) {
                c.bits[b2] &= (unsigned char)~ws.bits[b2];
            }
            enum Err e = emit_class(b, &c, &cls_idx);
            if (e != E_OK) {
                return e;
            }
            ak = A_CLASS;
        } break;
        case 'n':
            ch = '\n';
            ak = A_CHAR;
            break;
        case 't':
            ch = '\t';
            ak = A_CHAR;
            break;
        case 'r':
            ch = '\r';
            ak = A_CHAR;
            break;
        case 'f':
            ch = '\f';
            ak = A_CHAR;
            break;
        case 'v':
            ch = '\v';
            ak = A_CHAR;
            break;
        case '0':
            ch = '\0';
            ak = A_CHAR;
            break;
        default:
            ch = (unsigned char)pat.bytes[i];
            ak = A_CHAR;
            break;
        }
        i++;
    } else {
        ch = (unsigned char)pat.bytes[i++];
        ak = A_CHAR;
    }

    // Parse quantifier using shared helper
    int min_count, max_count;
    bool is_quantified, is_lazy;
    enum Err e = parse_quantifier(pat, &i, &min_count, &max_count, &is_quantified, &is_lazy);
    if (e != E_OK) {
        return e;
    }
    if (is_lazy) {
        b->has_lazy = 1;
    }

    // Emit sequence based on (ak, q)
    if (!is_quantified) {
        // No quantifier - emit single atom
        switch (ak) {
        case A_CHAR:
            e = emit_inst(b, RI_CHAR, 0, 0, ch, -1, NULL);
            break;
        case A_ANY:
            e = emit_inst(b, RI_ANY, 0, 0, 0, -1, NULL);
            break;
        case A_CLASS:
            e = emit_inst(b, RI_CLASS, 0, 0, 0, cls_idx, NULL);
            break;
        case A_BOL:
            e = emit_inst(b, RI_BOL, 0, 0, 0, -1, NULL);
            break;
        case A_EOL:
            e = emit_inst(b, RI_EOL, 0, 0, 0, -1, NULL);
            break;
        }
        if (e != E_OK) {
            return e;
        }
        // nullable already set correctly above
    } else {
        // Handle quantified patterns
        if (ak == A_BOL || ak == A_EOL) {
            return E_PARSE; // 4 anchors can't be quantified
        }

        if (min_count == 0 && max_count == 1) {
            // ? quantifier - split(take, cont) for greedy; split(cont, take) for lazy
            int idx_split;
            int idx_atom;
            // ch: bit0=repeat(1), bit1=lazy(if is_lazy)
            e = emit_inst(b, RI_SPLIT, -1, -1, 0x01 | (is_lazy ? 0x02 : 0x00), -1, &idx_split);
            if (e != E_OK) {
                return e;
            }
            switch (ak) {
            case A_CHAR:
                e = emit_inst(b, RI_CHAR, 0, 0, ch, -1, &idx_atom);
                break;
            case A_ANY:
                e = emit_inst(b, RI_ANY, 0, 0, 0, -1, &idx_atom);
                break;
            case A_CLASS:
                e = emit_inst(b, RI_CLASS, 0, 0, 0, cls_idx, &idx_atom);
                break;
            default:
                return E_PARSE;
            }
            if (e != E_OK) {
                return e;
            }
            if (is_lazy) {
                b->ins[idx_split].x = b->nins; // skip first -> lazy
                b->ins[idx_split].y = idx_atom; // take second
            } else {
                b->ins[idx_split].x = idx_atom; // take first -> greedy
                b->ins[idx_split].y = b->nins; // skip second
            }
            // ? makes result nullable
            nullable = true;
        } else if (min_count == 0 && max_count == -1) {
            // * quantifier - split(loop, stop) for greedy; split(stop, loop) for lazy
            int idx_split;
            int idx_atom;
            int idx_jmp;
            // ch: bit0=repeat(1), bit1=lazy(if is_lazy)
            e = emit_inst(b, RI_SPLIT, -1, -1, 0x01 | (is_lazy ? 0x02 : 0x00), -1, &idx_split);
            if (e != E_OK) {
                return e;
            }
            switch (ak) {
            case A_CHAR:
                e = emit_inst(b, RI_CHAR, 0, 0, ch, -1, &idx_atom);
                break;
            case A_ANY:
                e = emit_inst(b, RI_ANY, 0, 0, 0, -1, &idx_atom);
                break;
            case A_CLASS:
                e = emit_inst(b, RI_CLASS, 0, 0, 0, cls_idx, &idx_atom);
                break;
            default:
                return E_PARSE;
            }
            if (e != E_OK) {
                return e;
            }
            e = emit_inst(b, RI_JMP, idx_split, 0, 0, -1, &idx_jmp);
            if (e != E_OK) {
                return e;
            }
            if (is_lazy) {
                b->ins[idx_split].x = b->nins; // stop first -> lazy
                b->ins[idx_split].y = idx_atom; // loop second
            } else {
                b->ins[idx_split].x = idx_atom; // loop first -> greedy
                b->ins[idx_split].y = b->nins; // stop second
            }
            // * makes result nullable
            nullable = true;
        } else if (min_count == 1 && max_count == -1) {
            // + quantifier - split(loop, stop) for greedy; split(stop, loop) for lazy
            int idx_atom;
            int idx_split;
            switch (ak) {
            case A_CHAR:
                e = emit_inst(b, RI_CHAR, 0, 0, ch, -1, &idx_atom);
                break;
            case A_ANY:
                e = emit_inst(b, RI_ANY, 0, 0, 0, -1, &idx_atom);
                break;
            case A_CLASS:
                e = emit_inst(b, RI_CLASS, 0, 0, 0, cls_idx, &idx_atom);
                break;
            default:
                return E_PARSE;
            }
            if (e != E_OK) {
                return e;
            }
            // ch: bit0=repeat(1), bit1=lazy(if is_lazy)
            e = emit_inst(b, RI_SPLIT, -1, -1, 0x01 | (is_lazy ? 0x02 : 0x00), -1, &idx_split);
            if (e != E_OK) {
                return e;
            }
            if (is_lazy) {
                b->ins[idx_split].x = b->nins; // stop first -> lazy
                b->ins[idx_split].y = idx_atom; // loop second
            } else {
                b->ins[idx_split].x = idx_atom; // loop first -> greedy
                b->ins[idx_split].y = b->nins; // stop second
            }
            // + doesn't make result nullable (requires at least one match)
            nullable = false;
        } else {
            // {n,m} quantifier - use counter-based loop to avoid expansion
            // This handles patterns like \d{100} or [a-z]{50,100} without
            // emitting hundreds of instructions

            // Special case: {0} - emit nothing (correctness fix)
            if (max_count == 0) {
                // Pattern matches zero occurrences, so skip atom entirely
                *i_inout = i;
                // {0} makes result nullable
                if (out_nullable) {
                    *out_nullable = true;
                }
                return E_OK;
            }

            // Special case: {1} - emit once without loop or counter
            if (min_count == 1 && max_count == 1) {
                switch (ak) {
                case A_CHAR:
                    e = emit_inst(b, RI_CHAR, 0, 0, ch, -1, NULL);
                    break;
                case A_ANY:
                    e = emit_inst(b, RI_ANY, 0, 0, 0, -1, NULL);
                    break;
                case A_CLASS:
                    e = emit_inst(b, RI_CLASS, 0, 0, 0, cls_idx, NULL);
                    break;
                default:
                    return E_PARSE;
                }
                if (e != E_OK) {
                    return e;
                }
                *i_inout = i;
                // {1} doesn't make result nullable
                nullable = false;
                if (out_nullable) {
                    *out_nullable = nullable;
                }
                return E_OK;
            }
            // Bounded quantifiers: nullable if min_count == 0
            nullable = (min_count == 0);

            // All other cases use counter-based loops
            int counter_id = b->next_counter_id++;
            if (counter_id >= MAX_RE_COUNTERS) {
                error_detail_set(E_CAPACITY, -1,
                    "regex: too many quantified groups in pattern (max %d); reduce nesting or use simpler quantifiers",
                    MAX_RE_COUNTERS);
                return E_CAPACITY;
            }

            // COUNTER_RESET: initialize counter to 0
            e = emit_inst(b, RI_COUNTER_RESET, counter_id, 0, 0, -1, NULL);
            if (e != E_OK) {
                return e;
            }

            // Loop: atom, INC, SPLIT->(CHECK+JMP | continue)
            int loop_start = b->nins;

            // Emit the atom once
            switch (ak) {
            case A_CHAR:
                e = emit_inst(b, RI_CHAR, 0, 0, ch, -1, NULL);
                break;
            case A_ANY:
                e = emit_inst(b, RI_ANY, 0, 0, 0, -1, NULL);
                break;
            case A_CLASS:
                e = emit_inst(b, RI_CLASS, 0, 0, 0, cls_idx, NULL);
                break;
            default:
                return E_PARSE;
            }
            if (e != E_OK) {
                return e;
            }

            // Increment counter after successful match
            e = emit_inst(b, RI_COUNTER_INC, counter_id, 0, 0, -1, NULL);
            if (e != E_OK) {
                return e;
            }

            if (max_count == -1) {
                // Unbounded: {n,} - loop forever (no max limit)
                // SPLIT: split(loop, exit) for greedy; split(exit, loop) for lazy
                int split_pc;
                // ch: bit0=repeat(1), bit1=lazy(if is_lazy)
                e = emit_inst(b, RI_SPLIT, -1, -1, 0x01 | (is_lazy ? 0x02 : 0x00), -1, &split_pc);
                if (e != E_OK) {
                    return e;
                }

                // First branch: loop unconditionally (no max limit)
                int loop_branch = b->nins;
                e = emit_inst(b, RI_JMP, loop_start, 0, 0, -1, NULL);
                if (e != E_OK) {
                    return e;
                }

                // Second branch: check minimum before exiting
                int exit_branch = b->nins;
                e = emit_inst(b, RI_COUNTER_CHECK_MIN, counter_id, min_count, 0, -1, NULL);
                if (e != E_OK) {
                    return e;
                }

                // Wire up SPLIT
                if (is_lazy) {
                    b->ins[split_pc].x = exit_branch; // lazy: try to exit first
                    b->ins[split_pc].y = loop_branch; // or loop
                } else {
                    b->ins[split_pc].x = loop_branch; // greedy: try to loop first
                    b->ins[split_pc].y = exit_branch; // or exit
                }
            } else if (max_count == min_count) {
                // Exact count: {n} - loop exactly n times
                // SPLIT: split(loop, exit) for greedy; split(exit, loop) for lazy
                int split_pc;
                // ch: bit0=repeat(1), bit1=lazy(if is_lazy)
                e = emit_inst(b, RI_SPLIT, -1, -1, 0x01 | (is_lazy ? 0x02 : 0x00), -1, &split_pc);
                if (e != E_OK) {
                    return e;
                }

                // First branch: check if we can loop, then jump
                int check_branch = b->nins;
                e = emit_inst(b, RI_COUNTER_CHECK, counter_id, max_count, 0, -1, NULL);
                if (e != E_OK) {
                    return e;
                }
                e = emit_inst(b, RI_JMP, loop_start, 0, 0, -1, NULL);
                if (e != E_OK) {
                    return e;
                }

                // Second branch: exit (must meet exact count)
                int exit_branch = b->nins;
                e = emit_inst(b, RI_COUNTER_CHECK_MIN, counter_id, min_count, 0, -1, NULL);
                if (e != E_OK) {
                    return e;
                }

                // Wire up SPLIT
                if (is_lazy) {
                    b->ins[split_pc].x = exit_branch; // lazy: try to exit first
                    b->ins[split_pc].y = check_branch; // or loop
                } else {
                    b->ins[split_pc].x = check_branch; // greedy: try to loop first
                    b->ins[split_pc].y = exit_branch; // or exit
                }
            } else {
                // Range: {n,m} - loop between n and m times
                // SPLIT: split(loop, exit) for greedy; split(exit, loop) for lazy
                int split_pc;
                // ch: bit0=repeat(1), bit1=lazy(if is_lazy)
                e = emit_inst(b, RI_SPLIT, -1, -1, 0x01 | (is_lazy ? 0x02 : 0x00), -1, &split_pc);
                if (e != E_OK) {
                    return e;
                }

                // First branch: check max, then jump back
                int check_branch = b->nins;
                e = emit_inst(b, RI_COUNTER_CHECK, counter_id, max_count, 0, -1, NULL);
                if (e != E_OK) {
                    return e;
                }
                e = emit_inst(b, RI_JMP, loop_start, 0, 0, -1, NULL);
                if (e != E_OK) {
                    return e;
                }

                // Second branch: check minimum and continue
                int exit_branch = b->nins;
                e = emit_inst(b, RI_COUNTER_CHECK_MIN, counter_id, min_count, 0, -1, NULL);
                if (e != E_OK) {
                    return e;
                }

                // Wire up SPLIT
                if (is_lazy) {
                    b->ins[split_pc].x = exit_branch; // lazy: try to exit first
                    b->ins[split_pc].y = check_branch; // or loop
                } else {
                    b->ins[split_pc].x = check_branch; // greedy: try to loop first
                    b->ins[split_pc].y = exit_branch; // or exit
                }
            }
        }
    }

    *i_inout = i;
    // Set nullable output if caller requested it
    if (out_nullable) {
        *out_nullable = nullable;
    }
    return E_OK;
}

/*************
 * PUBLIC API
 *************/


enum Err re_compile_into(String pattern,
    ReProg* out,
    ReInst* ins_base, int ins_cap, int* ins_used,
    ReClass* cls_base, int cls_cap, int* cls_used)
{
    if (getenv("FISKTA_TRACE_COMPILE")) {
        fprintf(stderr, "TRACE COMPILE begin pattern='%.*s'\n", pattern.len, pattern.bytes);
    }
    ReB b = { 0 };
    b.out = out;
    b.pattern = pattern;
    // Start writing at current pool offsets
    int ins_start = (ins_used && *ins_used >= 0) ? *ins_used : 0;
    int cls_start = (cls_used && *cls_used >= 0) ? *cls_used : 0;
    if (ins_start > ins_cap || cls_start > cls_cap) {
        return E_CAPACITY;
    }
    b.ins = ins_base + ins_start;
    b.ins_cap = ins_cap - ins_start;
    b.cls = cls_base + cls_start;
    b.cls_cap = cls_cap - cls_start;
    b.nins = 0;
    b.ncls = 0;

    if (!pattern.bytes || pattern.len == 0) {
        return E_BAD_NEEDLE;
    }

    // Always use the proven single-pass emitter (with nested alternation via compile_alt_sequence)
    // First, check top-level alternation
    int depth = 0;
    int has_bar = 0;
    for (int j = 0; j < pattern.len; ++j) {
        if (pattern.bytes[j] == '\\') {
            if (j + 1 < pattern.len) {
                j++;
            }
            continue;
        }
        if (pattern.bytes[j] == '(') {
            depth++;
        } else if (pattern.bytes[j] == ')') {
            depth--;
        } else if (pattern.bytes[j] == '|' && depth == 0) {
            has_bar = 1;
            break;
        }
    }

    if (!has_bar) {
        int i = 0;
        while (i < pattern.len) {
            enum Err e = compile_atom(&b, pattern, &i, NULL);
            if (e != E_OK) {
                return e;
            }
        }
    } else {
        enum Err e2 = compile_alt_sequence(&b, pattern, pattern.len, NULL);
        if (e2 != E_OK) {
            return e2;
        }
    }

    // Emit final match instruction
    enum Err e = emit_inst(&b, RI_MATCH, 0, 0, 0, -1, NULL);
    if (e != E_OK) {
        return e;
    }

    out->ins = b.ins;
    out->nins = b.nins;
    out->classes = b.cls;
    out->nclasses = b.ncls;
    out->counter_count = b.next_counter_id;
    out->has_lazy = b.has_lazy;

    if (ins_used) {
        *ins_used = ins_start + b.nins;
    }
    if (cls_used) {
        *cls_used = cls_start + b.ncls;
    }
    if (getenv("FISKTA_TRACE_COMPILE")) {
        fprintf(stderr, "TRACE COMPILE pattern='%.*s' nins=%d\n", pattern.len, pattern.bytes, b.nins);
    }
    return E_OK;
}

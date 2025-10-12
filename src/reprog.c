#include "reprog.h"
#include <ctype.h>
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
} ReB;

static enum Err emit_inst(ReB* b, ReOp op, int x, int y, unsigned char ch, int cls_idx, int* out_idx)
{
    if (b->nins >= b->ins_cap) {
        return E_OOM;
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
        return E_OOM;
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

static enum Err compile_atom(ReB* b, String pat, int* i_inout);

// Compiles pat[0..len) into `b` without emitting RI_MATCH.
// Uses N-1 splits so there is no epsilon path that skips all alts.
static enum Err compile_alt_sequence(ReB* b, String pat, int len)
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
        }
        else if (pat.bytes[j] == ')') {
            --depth;
        } else if (pat.bytes[j] == '|' && depth == 0) {
            ++nalt;
}
    }

    // Single alt: compile linearly and return
    if (nalt == 1) {
        int i = 0;
        String tmp_bytes = { pat.bytes, len };
        while (i < len) {
            enum Err e = compile_atom(b, tmp_bytes, &i);
            if (e != E_OK) {
                return e;
            }
        }
        return E_OK;
    }

    // 2) Record (lo,len) for each alt
    int* lo = NULL;
    int* alen = NULL;
    int* split_pc = NULL;
    int* alt_start_pc = NULL;
    int* jmp_pc = NULL;
    enum Err err = E_OK;

    lo = (int*)malloc((size_t)nalt * sizeof(int));
    alen = (int*)malloc((size_t)nalt * sizeof(int));
    split_pc = (int*)malloc((size_t)(nalt - 1) * sizeof(int));
    alt_start_pc = (int*)malloc((size_t)nalt * sizeof(int));
    jmp_pc = (int*)malloc((size_t)nalt * sizeof(int));
    if (!lo || !alen || !split_pc || !alt_start_pc || !jmp_pc) {
        err = E_OOM;
        goto cleanup;
    }

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
        }
        else if (pat.bytes[j] == ')') {
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
    // Emit a split before each of the first N-1 alts, then their bodies
    for (int i = 0; i < nalt - 1; ++i) {
        e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &split_pc[i]);
        if (e != E_OK) {
            err = e;
            goto cleanup;
        }

        alt_start_pc[i] = b->nins;
        // compile alt i
        int pi = 0;
        String frag_bytes = { pat.bytes + lo[i], alen[i] };
        while (pi < alen[i]) {
            e = compile_atom(b, frag_bytes, &pi);
            if (e != E_OK) {
                err = e;
                goto cleanup;
            }
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
    while (pi < alen[nalt - 1]) {
        e = compile_atom(b, last_bytes, &pi);
        if (e != E_OK) {
            err = e;
            goto cleanup;
        }
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

    err = E_OK;

cleanup:
    free(lo);
    free(alen);
    free(split_pc);
    free(alt_start_pc);
    free(jmp_pc);
    return err;
}

// Compile a single regex atom (+ optional quantifier)
static enum Err compile_atom(ReB* b, String pat, int* i_inout)
{
    int i = *i_inout;
    if (i >= pat.len) {
        return E_PARSE;
    }

    // Identify atom without emitting yet if quantifier needs ordering
    enum { A_CHAR,
        A_ANY,
        A_CLASS,
        A_BOL,
        A_EOL } ak;
    unsigned char ch = 0;
    int cls_idx = -1;

    if (pat.bytes[i] == '^') {
        ak = A_BOL;
        i++;
    } else if (pat.bytes[i] == '$') {
        ak = A_EOL;
        i++;
    } else if (pat.bytes[i] == '.') {
        ak = A_ANY;
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

    // --- Grouping atom '(' ... ')' with nested alternation ---
    if (pat.bytes[i - 1] == '(') {
        int j = i;
        int depth = 1;
        while (j < pat.len) {
            if (pat.bytes[j] == '\\') {
                if (j + 1 < pat.len) {
                    j += 2;
                }
                else {
                    return E_PARSE;
                }
                continue;
            }
            if (pat.bytes[j] == '(') {
                depth++;
            }
            else if (pat.bytes[j] == ')') {
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

        int inner_lo = i;
        int inner_len = j - inner_lo;
        char q = (j + 1 < pat.len) ? pat.bytes[j + 1] : 0; // quantifier char (if present)

        // Handle empty group: epsilon (no-op)
        if (inner_len == 0) {
            i = (q ? j + 2 : j + 1);
            *i_inout = i;
            return E_OK; // epsilon contributes nothing
        }

        enum Err e;
        int split_pc;
        int group_entry;
        int cont;
        int jmp_pc;

        // Use canonical Thompson constructions based on quantifier
        if (q == '?') {
            // 4 ( ... )? - pre-split: split(take, skip)
            e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &split_pc);
            if (e != E_OK) {
                return e;
            }

            group_entry = b->nins;
            String inner_bytes = { pat.bytes + inner_lo, inner_len };
            e = compile_alt_sequence(b, inner_bytes, inner_len);
            if (e != E_OK) {
                return e;
            }
            cont = b->nins;

            b->ins[split_pc].x = group_entry; // take
            b->ins[split_pc].y = cont; // skip

            i = j + 2; // past '?'
        } else if (q == '*') {
            // 4 ( ... )* - pre-split + back-jump: split(take, next), body, jmp split
            e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &split_pc);
            if (e != E_OK) {
                return e;
            }

            group_entry = b->nins;
            String inner_bytes = { pat.bytes + inner_lo, inner_len };
            e = compile_alt_sequence(b, inner_bytes, inner_len);
            if (e != E_OK) {
                return e;
            }

            e = emit_inst(b, RI_JMP, split_pc, 0, 0, -1, &jmp_pc);
            if (e != E_OK) {
                return e;
            }

            cont = b->nins;
            b->ins[split_pc].x = group_entry; // take one more (greedy)
            b->ins[split_pc].y = cont; // or stop here

            i = j + 2; // past '*'
        } else if (q == '+') {
            // ( ... )+ - post-split loop: body, split(start, next)
            // Use the same pattern as single-atom +: atom, then split(atom, next)
            group_entry = b->nins;
            String inner_bytes = { pat.bytes + inner_lo, inner_len };
            e = compile_alt_sequence(b, inner_bytes, inner_len);
            if (e != E_OK) {
                return e;
            }

            e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &split_pc);
            if (e != E_OK) {
                return e;
            }
            b->ins[split_pc].x = group_entry; // loop back to group start
            b->ins[split_pc].y = b->nins; // fallthrough to next instruction

            i = j + 2; // past '+'
        } else {
            // No quantifier - just compile the body
            String inner_bytes = { pat.bytes + inner_lo, inner_len };
            e = compile_alt_sequence(b, inner_bytes, inner_len);
            if (e != E_OK) {
                return e;
            }
            i = j + 1;
        }

        *i_inout = i;
        return E_OK;
    }

    // Look for quantifier (bounded)
    char q = (i < pat.len) ? pat.bytes[i] : 0;
    int min_count = 1;
    int max_count = 1; // default for single atom
    int is_quantified = 0;

    if (q == '*' || q == '+' || q == '?') {
        i++;
        is_quantified = 1;
        switch (q) {
        case '*':
            min_count = 0;
            max_count = -1;
            break; // unlimited
        case '+':
            min_count = 1;
            max_count = -1;
            break; // unlimited
        case '?':
            min_count = 0;
            max_count = 1;
            break;
        default:
            break;
        }
    } else if (q == '{') {
        // Parse {n,m} quantifier
        i++; // skip '{'
        if (i >= pat.len || !isdigit(pat.bytes[i])) {
            return E_PARSE;
        }

        // Parse minimum count
        min_count = 0;
        while (i < pat.len && isdigit(pat.bytes[i])) {
            min_count = min_count * 10 + (pat.bytes[i] - '0');
            i++;
        }

        if (i >= pat.len || pat.bytes[i] == '}') {
            // {n} - exactly n times
            max_count = min_count;
            i++;
        } else if (pat.bytes[i] == ',') {
            i++; // skip ','
            if (i >= pat.len || pat.bytes[i] == '}') {
                // {n,} - n or more times
                max_count = -1; // unlimited
                i++;
            } else if (isdigit(pat.bytes[i])) {
                // {n,m} - between n and m times
                max_count = 0;
                while (i < pat.len && isdigit(pat.bytes[i])) {
                    max_count = max_count * 10 + (pat.bytes[i] - '0');
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
        is_quantified = 1;
    }

    // Emit sequence based on (ak, q)
    enum Err e = E_OK;
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
    } else {
        // Handle quantified patterns
        if (ak == A_BOL || ak == A_EOL) {
            return E_PARSE; // 4 anchors can't be quantified
        }

        if (min_count == 0 && max_count == 1) {
            // ? quantifier - greedy: split(take, cont), atom
            int idx_split;
            int idx_atom;
            e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &idx_split);
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
            b->ins[idx_split].x = idx_atom; // take first -> greedy
            b->ins[idx_split].y = b->nins; // continue after atom
        } else if (min_count == 0 && max_count == -1) {
            // * quantifier - greedy: split(loop, cont), atom, jmp split
            int idx_split;
            int idx_atom;
            int idx_jmp;
            e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &idx_split);
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
            b->ins[idx_split].x = idx_atom; // loop first -> greedy
            b->ins[idx_split].y = b->nins; // continue after jmp
        } else if (min_count == 1 && max_count == -1) {
            // + quantifier - atom, split(loop, cont)
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
            e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &idx_split);
            if (e != E_OK) {
                return e;
            }
            b->ins[idx_split].x = idx_atom; // loop back to atom
            b->ins[idx_split].y = b->nins; // fallthrough to next instruction
        } else {
            // {n,m} quantifier - more complex NFA structure needed
            // For now, implement a simple approach: emit min_count atoms, then add optional ones

            // Emit minimum required atoms
            for (int j = 0; j < min_count; j++) {
                int idx_atom;
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
            }

            if (max_count == -1) {
                // unlimited tail == greedy '*'
                int sp;
                int at;
                int jmp;
                e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &sp);
                if (e != E_OK) {
                    return e;
                }
                switch (ak) {
                case A_CHAR:
                    e = emit_inst(b, RI_CHAR, 0, 0, ch, -1, &at);
                    break;
                case A_ANY:
                    e = emit_inst(b, RI_ANY, 0, 0, 0, -1, &at);
                    break;
                case A_CLASS:
                    e = emit_inst(b, RI_CLASS, 0, 0, 0, cls_idx, &at);
                    break;
                default:
                    return E_PARSE;
                }
                if (e != E_OK) {
                    return e;
                }
                e = emit_inst(b, RI_JMP, sp, 0, 0, -1, &jmp);
                if (e != E_OK) {
                    return e;
                }
                b->ins[sp].x = at;
                b->ins[sp].y = b->nins;
            } else if (max_count > min_count) {
                int opt = max_count - min_count;
                for (int k = 0; k < opt; ++k) {
                    int sp;
                    int at;
                    e = emit_inst(b, RI_SPLIT, 0, 0, 0, -1, &sp);
                    if (e != E_OK) {
                        return e;
                    }
                    switch (ak) {
                    case A_CHAR:
                        e = emit_inst(b, RI_CHAR, 0, 0, ch, -1, &at);
                        break;
                    case A_ANY:
                        e = emit_inst(b, RI_ANY, 0, 0, 0, -1, &at);
                        break;
                    case A_CLASS:
                        e = emit_inst(b, RI_CLASS, 0, 0, 0, cls_idx, &at);
                        break;
                    default:
                        return E_PARSE;
                    }
                    if (e != E_OK) {
                        return e;
                    }
                    b->ins[sp].x = at; // take one more (greedy)
                    b->ins[sp].y = b->nins; // or stop here
                }
            }
        }
    }

    *i_inout = i;
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
    ReB b = { 0 };
    b.out = out;
    // Start writing at current pool offsets
    int ins_start = (ins_used && *ins_used >= 0) ? *ins_used : 0;
    int cls_start = (cls_used && *cls_used >= 0) ? *cls_used : 0;
    if (ins_start > ins_cap || cls_start > cls_cap) {
        return E_OOM;
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
        }
        else if (pattern.bytes[j] == ')') {
            depth--;
        } else if (pattern.bytes[j] == '|' && depth == 0) {
            has_bar = 1;
            break;
        }
    }

    if (!has_bar) {
        int i = 0;
        while (i < pattern.len) {
            enum Err e = compile_atom(&b, pattern, &i);
            if (e != E_OK) {
                return e;
            }
        }
    } else {
        enum Err e2 = compile_alt_sequence(&b, pattern, pattern.len);
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

    if (ins_used) *ins_used = ins_start + b.nins;
    if (cls_used) *cls_used = cls_start + b.ncls;
    return E_OK;
}

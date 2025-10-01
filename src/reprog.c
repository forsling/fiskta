#include "reprog.h"
#include <alloca.h>
#include <ctype.h>
#include <string.h>

static inline void cls_clear(ReClass* c) { memset(c->bits, 0, sizeof c->bits); }
static inline void cls_set(ReClass* c, unsigned char ch) { c->bits[ch >> 3] |= (unsigned char)(1u << (ch & 7)); }
static inline void cls_set_range(ReClass* c, unsigned char a, unsigned char b)
{
    if (a > b) {
        unsigned char t = a;
        a = b;
        b = t;
    }
    for (unsigned v = a;; ++v) {
        cls_set(c, (unsigned char)v);
        if (v == b)
            break;
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

typedef struct {
    ReProg* out;
    ReInst* ins;
    int nins, ins_cap;
    ReClass* cls;
    int ncls, cls_cap;
} ReB;

static enum Err emit_inst(ReB* b, ReOp op, int x, int y, unsigned char ch, int cls_idx, int* out_idx)
{
    if (b->nins >= b->ins_cap)
        return E_OOM;
    int idx = b->nins++;
    b->ins[idx].op = op;
    b->ins[idx].x = x;
    b->ins[idx].y = y;
    b->ins[idx].ch = ch;
    b->ins[idx].cls_idx = cls_idx;
    if (out_idx)
        *out_idx = idx;
    return E_OK;
}

static enum Err new_class(ReB* b, const ReClass* src, int* idx_out)
{
    if (b->ncls >= b->cls_cap)
        return E_OOM;
    b->cls[b->ncls] = *src;
    *idx_out = b->ncls++;
    return E_OK;
}

static int peek(const char* s, int i) { return (unsigned char)s[i]; }

// Parse a character class: pattern points at first char AFTER '['; returns index AFTER ']'
static enum Err parse_class(ReB* b, String pat, int* i_inout, int* out_cls_idx)
{
    int i = *i_inout;
    ReClass cls;
    cls_clear(&cls);
    int negated = 0;

    if (pat.p[i] == ']')
        return E_PARSE; // empty

    // Check for negation
    if (pat.p[i] == '^') {
        negated = 1;
        i++;
    }

    while (i < pat.n && pat.p[i] != ']') {
        unsigned char a;
        if (pat.p[i] == '\\') {
            ++i;
            if (i >= pat.n)
                return E_PARSE;
            switch (pat.p[i]) {
            case 'd':
                cls_set_digit(&cls);
                break;
            case 'D': { // non-digits: set all then clear digits
                for (int v = 0; v < 256; ++v)
                    cls_set(&cls, (unsigned char)v);
                ReClass d;
                cls_clear(&d);
                cls_set_digit(&d);
                for (int b = 0; b < 32; ++b)
                    cls.bits[b] &= (unsigned char)~d.bits[b];
            } break;
            case 'w':
                cls_set_word(&cls);
                break;
            case 'W': {
                for (int v = 0; v < 256; ++v)
                    cls_set(&cls, (unsigned char)v);
                ReClass w;
                cls_clear(&w);
                cls_set_word(&w);
                for (int b = 0; b < 32; ++b)
                    cls.bits[b] &= (unsigned char)~w.bits[b];
            } break;
            case 's':
                cls_set_ws(&cls);
                break;
            case 'S': {
                for (int v = 0; v < 256; ++v)
                    cls_set(&cls, (unsigned char)v);
                ReClass ws;
                cls_clear(&ws);
                cls_set_ws(&ws);
                for (int b = 0; b < 32; ++b)
                    cls.bits[b] &= (unsigned char)~ws.bits[b];
            } break;
            default:
                cls_set(&cls, (unsigned char)pat.p[i]);
                break;
            }
            ++i;
        } else {
            a = (unsigned char)pat.p[i++];
            if (i < pat.n && pat.p[i] == '-' && i + 1 < pat.n && pat.p[i + 1] != ']') {
                unsigned char bch;
                ++i;
                if (pat.p[i] == '\\') {
                    ++i;
                    if (i >= pat.n)
                        return E_PARSE;
                    bch = (unsigned char)pat.p[i++];
                } else {
                    bch = (unsigned char)pat.p[i++];
                }
                cls_set_range(&cls, a, bch);
            } else {
                cls_set(&cls, a);
            }
        }
    }
    if (i >= pat.n || pat.p[i] != ']')
        return E_PARSE;
    ++i;

    // Apply negation if needed
    if (negated) {
        ReClass negated_cls;
        for (int v = 0; v < 256; ++v)
            cls_set(&negated_cls, (unsigned char)v);
        for (int b = 0; b < 32; ++b)
            negated_cls.bits[b] &= (unsigned char)~cls.bits[b];
        cls = negated_cls;
    }

    int cls_idx;
    enum Err e = new_class(b, &cls, &cls_idx);
    if (e != E_OK)
        return e;
    *i_inout = i;
    *out_cls_idx = cls_idx;
    return E_OK;
}

static enum Err compile_piece(ReB* b, String pat, int* i_inout);

// Compiles pat[0..len) into `b` without emitting RI_MATCH.
// Uses N-1 splits so there is no epsilon path that skips all alts.
static enum Err compile_subpattern(ReB* b, String pat, int len)
{
    // 1) Collect top-level alternatives (respect escapes/parentheses)
    int depth = 0, nalt = 1;
    for (int j = 0; j < len; ++j) {
        if (pat.p[j] == '\\') {
            if (j + 1 < len)
                ++j;
            continue;
        }
        if (pat.p[j] == '(')
            ++depth;
        else if (pat.p[j] == ')')
            --depth;
        else if (pat.p[j] == '|' && depth == 0)
            ++nalt;
    }

    // Single alt: compile linearly and return
    if (nalt == 1) {
        char* tmp = (char*)alloca((size_t)len + 1);
        memcpy(tmp, pat.p, (size_t)len);
        tmp[len] = '\0';
        int i = 0;
        String tmp_bytes = {tmp, len};
        while (i < len) {
            enum Err e = compile_piece(b, tmp_bytes, &i);
            if (e != E_OK)
                return e;
        }
        return E_OK;
    }

    // 2) Record (lo,len) for each alt
    int* lo = (int*)alloca((size_t)nalt * sizeof(int));
    int* alen = (int*)alloca((size_t)nalt * sizeof(int));
    int k = 0, start = 0;
    depth = 0;
    for (int j = 0; j < len; ++j) {
        if (pat.p[j] == '\\') {
            if (j + 1 < len)
                ++j;
            continue;
        }
        if (pat.p[j] == '(')
            ++depth;
        else if (pat.p[j] == ')')
            --depth;
        else if (pat.p[j] == '|' && depth == 0) {
            lo[k] = start;
            alen[k] = j - start;
            ++k;
            start = j + 1;
        }
    }
    lo[k] = start;
    alen[k] = len - start; /* k == nalt-1 */

    // 3) Emit N-1 splits + all alt bodies
    int* split_pc = (int*)alloca((size_t)(nalt - 1) * sizeof(int));
    int* alt_start_pc = (int*)alloca((size_t)nalt * sizeof(int));
    int* jmp_pc = (int*)alloca((size_t)nalt * sizeof(int));

    enum Err e;
    // Emit a split before each of the first N-1 alts, then their bodies
    for (int i = 0; i < nalt - 1; ++i) {
        e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &split_pc[i]);
        if (e != E_OK)
            return e;

        alt_start_pc[i] = b->nins;
        // compile alt i
        char* frag = (char*)alloca((size_t)alen[i] + 1);
        memcpy(frag, pat.p + lo[i], (size_t)alen[i]);
        frag[alen[i]] = '\0';
        int pi = 0;
        String frag_bytes = {frag, alen[i]};
        while (pi < alen[i]) {
            e = compile_piece(b, frag_bytes, &pi);
            if (e != E_OK)
                return e;
        }

        e = emit_inst(b, RI_JMP, -1, 0, 0, -1, &jmp_pc[i]);
        if (e != E_OK)
            return e;
    }

    // Last alternative (no leading split)
    alt_start_pc[nalt - 1] = b->nins;
    char* last = (char*)alloca((size_t)alen[nalt - 1] + 1);
        memcpy(last, pat.p + lo[nalt - 1], (size_t)alen[nalt - 1]);
    last[alen[nalt - 1]] = '\0';
    int pi = 0;
    String last_bytes = {last, alen[nalt - 1]};
    while (pi < alen[nalt - 1]) {
        e = compile_piece(b, last_bytes, &pi);
        if (e != E_OK)
            return e;
    }
    e = emit_inst(b, RI_JMP, -1, 0, 0, -1, &jmp_pc[nalt - 1]);
    if (e != E_OK)
        return e;

    // 4) Continuation point and patching
    int cont = b->nins;
    for (int i = 0; i < nalt; ++i)
        b->ins[jmp_pc[i]].x = cont;

    for (int i = 0; i < nalt - 1; ++i) {
        b->ins[split_pc[i]].x = alt_start_pc[i];
        b->ins[split_pc[i]].y = (i + 1 < nalt - 1) ? split_pc[i + 1] : alt_start_pc[nalt - 1];
    }

    return E_OK;
}

// Compile a single regex piece (atom + optional quantifier)
static enum Err compile_piece(ReB* b, String pat, int* i_inout)
{
    int i = *i_inout;
    if (i >= pat.n)
        return E_PARSE;

    // Identify atom without emitting yet if quantifier needs ordering
    enum { A_CHAR,
        A_ANY,
        A_CLASS,
        A_BOL,
        A_EOL } ak;
    unsigned char ch = 0;
    int cls_idx = -1;

    if (pat.p[i] == '^') {
        ak = A_BOL;
        i++;
    } else if (pat.p[i] == '$') {
        ak = A_EOL;
        i++;
    } else if (pat.p[i] == '.') {
        ak = A_ANY;
        i++;
    } else if (pat.p[i] == '[') {
        i++;
        enum Err e = parse_class(b, pat, &i, &cls_idx);
        if (e != E_OK)
            return e;
        ak = A_CLASS;
    } else if (pat.p[i] == '\\') {
        i++;
        if (i >= pat.n)
            return E_PARSE;
        switch (pat.p[i]) {
        case 'd': {
            ReClass c;
            cls_clear(&c);
            cls_set_digit(&c);
            enum Err e = new_class(b, &c, &cls_idx);
            if (e != E_OK)
                return e;
            ak = A_CLASS;
        } break;
        case 'D': {
            ReClass c;
            for (int v = 0; v < 256; ++v)
                cls_set(&c, (unsigned char)v);
            ReClass d;
            cls_clear(&d);
            cls_set_digit(&d);
            for (int b2 = 0; b2 < 32; ++b2)
                c.bits[b2] &= (unsigned char)~d.bits[b2];
            enum Err e = new_class(b, &c, &cls_idx);
            if (e != E_OK)
                return e;
            ak = A_CLASS;
        } break;
        case 'w': {
            ReClass c;
            cls_clear(&c);
            cls_set_word(&c);
            enum Err e = new_class(b, &c, &cls_idx);
            if (e != E_OK)
                return e;
            ak = A_CLASS;
        } break;
        case 'W': {
            ReClass c;
            for (int v = 0; v < 256; ++v)
                cls_set(&c, (unsigned char)v);
            ReClass w;
            cls_clear(&w);
            cls_set_word(&w);
            for (int b2 = 0; b2 < 32; ++b2)
                c.bits[b2] &= (unsigned char)~w.bits[b2];
            enum Err e = new_class(b, &c, &cls_idx);
            if (e != E_OK)
                return e;
            ak = A_CLASS;
        } break;
        case 's': {
            ReClass c;
            cls_clear(&c);
            cls_set_ws(&c);
            enum Err e = new_class(b, &c, &cls_idx);
            if (e != E_OK)
                return e;
            ak = A_CLASS;
        } break;
        case 'S': {
            ReClass c;
            for (int v = 0; v < 256; ++v)
                cls_set(&c, (unsigned char)v);
            ReClass w;
            cls_clear(&w);
            cls_set_ws(&w);
            for (int b2 = 0; b2 < 32; ++b2)
                c.bits[b2] &= (unsigned char)~w.bits[b2];
            enum Err e = new_class(b, &c, &cls_idx);
            if (e != E_OK)
                return e;
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
            ch = (unsigned char)pat.p[i];
            ak = A_CHAR;
            break;
        }
        i++;
    } else {
        ch = (unsigned char)pat.p[i++];
        ak = A_CHAR;
    }

    // --- NEW: grouping atom '(' ... ')' with nested alternation ---
    if (pat.p[i - 1] == '(') {
        int start_i = i - 1;
        int j = i, depth = 1;
        while (j < pat.n) {
            if (pat.p[j] == '\\') {
                if (j + 1 < pat.n)
                    j += 2;
                else
                    return E_PARSE;
                continue;
            }
            if (pat.p[j] == '(')
                depth++;
            else if (pat.p[j] == ')') {
                depth--;
                if (depth == 0) {
                    break;
                }
            }
            j++;
        }
        if (j >= pat.n || pat.p[j] != ')')
            return E_PARSE; // unmatched '('

        int inner_lo = i;
        int inner_len = j - inner_lo;
        char q = pat.p[j + 1]; // quantifier char (if present)

        // Handle empty group
        if (inner_len == 0) {
            // Empty group: compile a dead epsilon (never proceeds)
            int dead_pc;
            enum Err e2 = emit_inst(b, RI_JMP, 0, 0, 0, -1, &dead_pc);
            if (e2 != E_OK)
                return e2;
            b->ins[dead_pc].x = dead_pc;
            i = j + 1;
            *i_inout = i;
            return E_OK;
        }

        enum Err e;
        int split_pc, group_entry, cont, jmp_pc;

        // Use canonical Thompson constructions based on quantifier
        if (q == '?') {
            // 4 ( ... )? - pre-split: split(take, skip)
            e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &split_pc);
            if (e != E_OK)
                return e;

            group_entry = b->nins;
            String inner_bytes = {pat.p + inner_lo, inner_len};
            e = compile_subpattern(b, inner_bytes, inner_len);
            if (e != E_OK)
                return e;
            cont = b->nins;

            b->ins[split_pc].x = group_entry; // take
            b->ins[split_pc].y = cont; // skip

            i = j + 2; // past '?'
        } else if (q == '*') {
            // 4 ( ... )* - pre-split + back-jump: split(take, next), body, jmp split
            e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &split_pc);
            if (e != E_OK)
                return e;

            group_entry = b->nins;
            String inner_bytes = {pat.p + inner_lo, inner_len};
            e = compile_subpattern(b, inner_bytes, inner_len);
            if (e != E_OK)
                return e;

            e = emit_inst(b, RI_JMP, split_pc, 0, 0, -1, &jmp_pc);
            if (e != E_OK)
                return e;

            cont = b->nins;
            b->ins[split_pc].x = group_entry; // take one more (greedy)
            b->ins[split_pc].y = cont; // or stop here

            i = j + 2; // past '*'
        } else if (q == '+') {
            // ( ... )+ - post-split loop: body, split(start, next)
            // Use the same pattern as single-atom +: atom, then split(atom, next)
            group_entry = b->nins;
            String inner_bytes = {pat.p + inner_lo, inner_len};
            e = compile_subpattern(b, inner_bytes, inner_len);
            if (e != E_OK)
                return e;

            e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &split_pc);
            if (e != E_OK)
                return e;
            b->ins[split_pc].x = group_entry; // loop back to group start
            b->ins[split_pc].y = b->nins; // fallthrough to next instruction

            i = j + 2; // past '+'
        } else {
            // No quantifier - just compile the body
            String inner_bytes = {pat.p + inner_lo, inner_len};
            e = compile_subpattern(b, inner_bytes, inner_len);
            if (e != E_OK)
                return e;
            i = j + 1;
        }

        *i_inout = i;
        return E_OK;
    }

    // Look for quantifier
    char q = pat.p[i];
    int min_count = 1, max_count = 1; // default for single atom
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
        }
    } else if (q == '{') {
        // Parse {n,m} quantifier
        i++; // skip '{'
        if (i >= pat.n || !isdigit(pat.p[i]))
            return E_PARSE;

        // Parse minimum count
        min_count = 0;
        while (i < pat.n && isdigit(pat.p[i])) {
            min_count = min_count * 10 + (pat.p[i] - '0');
            i++;
        }

        if (i >= pat.n || pat.p[i] == '}') {
            // {n} - exactly n times
            max_count = min_count;
            i++;
        } else if (pat.p[i] == ',') {
            i++; // skip ','
            if (i >= pat.n || pat.p[i] == '}') {
                // {n,} - n or more times
                max_count = -1; // unlimited
                i++;
            } else if (isdigit(pat.p[i])) {
                // {n,m} - between n and m times
                max_count = 0;
                while (i < pat.n && isdigit(pat.p[i])) {
                    max_count = max_count * 10 + (pat.p[i] - '0');
                    i++;
                }
                if (i >= pat.n || pat.p[i] != '}')
                    return E_PARSE;
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
    int idx_atom, idx_split, idx_jmp;
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
        if (e != E_OK)
            return e;
    } else {
        // Handle quantified patterns
        if (ak == A_BOL || ak == A_EOL)
            return E_PARSE; // 4 anchors can't be quantified

        if (min_count == 0 && max_count == 1) {
            // ? quantifier - greedy: split(take, cont), atom
            int idx_split, idx_atom;
            e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &idx_split);
            if (e != E_OK)
                return e;
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
            if (e != E_OK)
                return e;
            b->ins[idx_split].x = idx_atom; // take first -> greedy
            b->ins[idx_split].y = b->nins; // continue after atom
        } else if (min_count == 0 && max_count == -1) {
            // * quantifier - greedy: split(loop, cont), atom, jmp split
            int idx_split, idx_atom, idx_jmp;
            e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &idx_split);
            if (e != E_OK)
                return e;
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
            if (e != E_OK)
                return e;
            e = emit_inst(b, RI_JMP, idx_split, 0, 0, -1, &idx_jmp);
            if (e != E_OK)
                return e;
            b->ins[idx_split].x = idx_atom; // loop first -> greedy
            b->ins[idx_split].y = b->nins; // continue after jmp
        } else if (min_count == 1 && max_count == -1) {
            // + quantifier - atom, split(loop, cont)
            int idx_atom, idx_split;
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
            if (e != E_OK)
                return e;
            e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &idx_split);
            if (e != E_OK)
                return e;
            b->ins[idx_split].x = idx_atom; // loop back to atom
            b->ins[idx_split].y = b->nins; // fallthrough to next instruction
        } else {
            // {n,m} quantifier - more complex NFA structure needed
            // For now, implement a simple approach: emit min_count atoms, then add optional ones
            int first_atom = -1;

            // Emit minimum required atoms
            for (int i = 0; i < min_count; i++) {
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
                if (e != E_OK)
                    return e;
                if (i == 0)
                    first_atom = idx_atom;
            }

            if (max_count == -1) {
                // unlimited tail == greedy '*'
                int sp, at, jmp;
                e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &sp);
                if (e != E_OK)
                    return e;
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
                if (e != E_OK)
                    return e;
                e = emit_inst(b, RI_JMP, sp, 0, 0, -1, &jmp);
                if (e != E_OK)
                    return e;
                b->ins[sp].x = at;
                b->ins[sp].y = b->nins;
            } else if (max_count > min_count) {
                int opt = max_count - min_count;
                for (int k = 0; k < opt; ++k) {
                    int sp, at;
                    e = emit_inst(b, RI_SPLIT, 0, 0, 0, -1, &sp);
                    if (e != E_OK)
                        return e;
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
                    if (e != E_OK)
                        return e;
                    b->ins[sp].x = at; // take one more (greedy)
                    b->ins[sp].y = b->nins; // or stop here
                }
            }
        }
    }

    *i_inout = i;
    return E_OK;
}

enum Err re_compile_into(String pattern,
    ReProg* out,
    ReInst* ins_base, int ins_cap, int* ins_used,
    ReClass* cls_base, int cls_cap, int* cls_used)
{
    ReB b = { 0 };
    b.out = out;
    b.ins = ins_base;
    b.ins_cap = ins_cap;
    b.cls = cls_base;
    b.cls_cap = cls_cap;

    if (!pattern.p || pattern.n == 0)
        return E_BAD_NEEDLE;

    // Always use the proven single-pass emitter (with nested alternation via compile_subpattern)
    // First, check top-level alternation
    int depth = 0, has_bar = 0;
    for (int j = 0; j < pattern.n; ++j) {
        if (pattern.p[j] == '\\') {
            if (j + 1 < pattern.n)
                j++;
            continue;
        }
        if (pattern.p[j] == '(')
            depth++;
        else if (pattern.p[j] == ')')
            depth--;
        else if (pattern.p[j] == '|' && depth == 0) {
            has_bar = 1;
            break;
        }
    }

    if (!has_bar) {
        int i = 0;
        while (i < pattern.n) {
            enum Err e = compile_piece(&b, pattern, &i);
            if (e != E_OK)
                return e;
        }
    } else {
        enum Err e2 = compile_subpattern(&b, pattern, pattern.n);
        if (e2 != E_OK)
            return e2;
    }

    // Emit final match instruction
    enum Err e = emit_inst(&b, RI_MATCH, 0, 0, 0, -1, NULL);
    if (e != E_OK)
        return e;

    out->ins = b.ins;
    out->nins = b.nins;
    out->classes = b.cls;
    out->nclasses = b.ncls;

    *ins_used = b.nins;
    *cls_used = b.ncls;
    return E_OK;
}

// reprog.c
#include "reprog.h"
#include <string.h>
#include <ctype.h>
#include <alloca.h>

static inline void cls_clear(ReClass* c){ memset(c->bits, 0, sizeof c->bits); }
static inline void cls_set(ReClass* c, unsigned char ch){ c->bits[ch>>3] |= (unsigned char)(1u << (ch & 7)); }
static inline void cls_set_range(ReClass* c, unsigned char a, unsigned char b){
    if (a > b){ unsigned char t=a; a=b; b=t; }
    for (unsigned v=a;;++v){ cls_set(c,(unsigned char)v); if (v==b) break; }
}
static inline void cls_set_ws(ReClass* c){ cls_set(c,' '); cls_set(c,'\t'); cls_set(c,'\n'); cls_set(c,'\r'); cls_set(c,'\v'); cls_set(c,'\f'); }
static inline void cls_set_digit(ReClass* c){ cls_set_range(c,'0','9'); }
static inline void cls_set_word(ReClass* c){
    cls_set_range(c,'0','9'); cls_set_range(c,'A','Z'); cls_set_range(c,'a','z'); cls_set(c,'_');
}

typedef struct {
    ReProg*  out;
    ReInst*  ins;  int nins, ins_cap;
    ReClass* cls;  int ncls, cls_cap;
} ReB;

static enum Err emit_inst(ReB* b, ReOp op, int x, int y, unsigned char ch, int cls_idx, int* out_idx){
    if (b->nins >= b->ins_cap) return E_OOM;
    int idx = b->nins++;
    b->ins[idx].op = op;
    b->ins[idx].x = x;
    b->ins[idx].y = y;
    b->ins[idx].ch = ch;
    b->ins[idx].cls_idx = cls_idx;
    if (out_idx) *out_idx = idx;
    return E_OK;
}

static enum Err new_class(ReB* b, const ReClass* src, int* idx_out){
    if (b->ncls >= b->cls_cap) return E_OOM;
    b->cls[b->ncls] = *src;
    *idx_out = b->ncls++;
    return E_OK;
}

static int peek(const char* s, int i){ return (unsigned char)s[i]; }

// Parse a character class: pattern points at first char AFTER '['; returns index AFTER ']'
static enum Err parse_class(ReB* b, const char* pat, int* i_inout, int* out_cls_idx){
    int i = *i_inout;
    ReClass cls; cls_clear(&cls);
    int negated = 0;

    if (pat[i] == ']') return E_PARSE; // empty

    // Check for negation
    if (pat[i] == '^') {
        negated = 1;
        i++;
    }

    while (pat[i] && pat[i] != ']'){
        unsigned char a;
        if (pat[i] == '\\'){
            ++i; if (!pat[i]) return E_PARSE;
            switch (pat[i]){
                case 'd': cls_set_digit(&cls); break;
                case 'D': { // non-digits: set all then clear digits
                    for (int v=0; v<256; ++v) cls_set(&cls,(unsigned char)v);
                    ReClass d; cls_clear(&d); cls_set_digit(&d);
                    for (int b=0;b<32;++b) cls.bits[b] &= (unsigned char)~d.bits[b];
                } break;
                case 'w': cls_set_word(&cls); break;
                case 'W': {
                    for (int v=0; v<256; ++v) cls_set(&cls,(unsigned char)v);
                    ReClass w; cls_clear(&w); cls_set_word(&w);
                    for (int b=0;b<32;++b) cls.bits[b] &= (unsigned char)~w.bits[b];
                } break;
                case 's': cls_set_ws(&cls); break;
                case 'S': {
                    for (int v=0; v<256; ++v) cls_set(&cls,(unsigned char)v);
                    ReClass ws; cls_clear(&ws); cls_set_ws(&ws);
                    for (int b=0;b<32;++b) cls.bits[b] &= (unsigned char)~ws.bits[b];
                } break;
                default:  cls_set(&cls, (unsigned char)pat[i]); break;
            }
            ++i;
        } else {
            a = (unsigned char)pat[i++];
            if (pat[i] == '-' && pat[i+1] && pat[i+1] != ']'){
                unsigned char bch;
                ++i;
                if (pat[i] == '\\'){ ++i; if (!pat[i]) return E_PARSE; bch = (unsigned char)pat[i++]; }
                else { bch = (unsigned char)pat[i++]; }
                cls_set_range(&cls, a, bch);
            } else {
                cls_set(&cls, a);
            }
        }
    }
    if (pat[i] != ']') return E_PARSE;
    ++i;

    // Apply negation if needed
    if (negated) {
        ReClass negated_cls;
        for (int v=0; v<256; ++v) cls_set(&negated_cls,(unsigned char)v);
        for (int b=0;b<32;++b) negated_cls.bits[b] &= (unsigned char)~cls.bits[b];
        cls = negated_cls;
    }

    int cls_idx;
    enum Err e = new_class(b, &cls, &cls_idx); if (e != E_OK) return e;
    *i_inout = i;
    *out_cls_idx = cls_idx;
    return E_OK;
}

// Parse one atom + optional quantifier, emit code
static enum Err compile_piece(ReB* b, const char* pat, int* i_inout){
    int i = *i_inout;
    if (!pat[i]) return E_PARSE;

    // Identify atom without emitting yet if quantifier needs ordering
    enum { A_CHAR, A_ANY, A_CLASS, A_BOL, A_EOL } ak;
    unsigned char ch = 0;
    int cls_idx = -1;

    if (pat[i] == '^'){ ak = A_BOL; i++; }
    else if (pat[i] == '$'){ ak = A_EOL; i++; }
    else if (pat[i] == '.'){ ak = A_ANY; i++; }
    else if (pat[i] == '['){
        i++;
        enum Err e = parse_class(b, pat, &i, &cls_idx); if (e != E_OK) return e;
        ak = A_CLASS;
    }
    else if (pat[i] == '\\'){
        i++; if (!pat[i]) return E_PARSE;
        switch (pat[i]){
            case 'd': { ReClass c; cls_clear(&c); cls_set_digit(&c); enum Err e=new_class(b,&c,&cls_idx); if (e!=E_OK) return e; ak=A_CLASS; } break;
            case 'D': { ReClass c; for (int v=0; v<256; ++v) cls_set(&c,(unsigned char)v); ReClass d; cls_clear(&d); cls_set_digit(&d);
                        for (int b2=0;b2<32;++b2) c.bits[b2] &= (unsigned char)~d.bits[b2];
                        enum Err e=new_class(b,&c,&cls_idx); if (e!=E_OK) return e; ak=A_CLASS; } break;
            case 'w': { ReClass c; cls_clear(&c); cls_set_word(&c); enum Err e=new_class(b,&c,&cls_idx); if (e!=E_OK) return e; ak=A_CLASS; } break;
            case 'W': { ReClass c; for (int v=0; v<256; ++v) cls_set(&c,(unsigned char)v); ReClass w; cls_clear(&w); cls_set_word(&w);
                        for (int b2=0;b2<32;++b2) c.bits[b2] &= (unsigned char)~w.bits[b2];
                        enum Err e=new_class(b,&c,&cls_idx); if (e!=E_OK) return e; ak=A_CLASS; } break;
            case 's': { ReClass c; cls_clear(&c); cls_set_ws(&c); enum Err e=new_class(b,&c,&cls_idx); if (e!=E_OK) return e; ak=A_CLASS; } break;
            case 'S': { ReClass c; for (int v=0; v<256; ++v) cls_set(&c,(unsigned char)v); ReClass w; cls_clear(&w); cls_set_ws(&w);
                        for (int b2=0;b2<32;++b2) c.bits[b2] &= (unsigned char)~w.bits[b2];
                        enum Err e=new_class(b,&c,&cls_idx); if (e!=E_OK) return e; ak=A_CLASS; } break;
            case 'n': ch = '\n'; ak = A_CHAR; break;
            case 't': ch = '\t'; ak = A_CHAR; break;
            case 'r': ch = '\r'; ak = A_CHAR; break;
            case 'f': ch = '\f'; ak = A_CHAR; break;
            case 'v': ch = '\v'; ak = A_CHAR; break;
            case '0': ch = '\0'; ak = A_CHAR; break;
            default: ch = (unsigned char)pat[i]; ak = A_CHAR; break;
        }
        i++;
    }
    else {
        ch = (unsigned char)pat[i++];
        ak = A_CHAR;
    }

    // Look for quantifier
    char q = pat[i];
    if (q=='*' || q=='+' || q=='?') i++;

    // Emit sequence based on (ak, q)
    enum Err e = E_OK;
    int idx_atom, idx_split, idx_jmp;
    switch (q){
        case '*': // greedy: split(loop, cont), atom, jmp split
            if (ak==A_BOL || ak==A_EOL) return E_PARSE;
            e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &idx_split); if (e!=E_OK) return e;
            // atom
            switch (ak){
                case A_CHAR:  e = emit_inst(b, RI_CHAR, 0,0,ch,-1,&idx_atom); break;
                case A_ANY:   e = emit_inst(b, RI_ANY, 0,0,0,-1,&idx_atom); break;
                case A_CLASS: e = emit_inst(b, RI_CLASS,0,0,0,cls_idx,&idx_atom); break;
                default: return E_PARSE;
            }
            if (e!=E_OK) return e;
            e = emit_inst(b, RI_JMP, idx_split, 0,0,-1,&idx_jmp); if (e!=E_OK) return e;
            // patch split
            b->ins[idx_split].x = idx_atom;       // loop first -> greedy
            b->ins[idx_split].y = b->nins;        // continue after jmp
            break;

        case '+': // atom, split(loop, cont)
            if (ak==A_BOL || ak==A_EOL) return E_PARSE;
            switch (ak){
                case A_CHAR:  e = emit_inst(b, RI_CHAR,0,0,ch,-1,&idx_atom); break;
                case A_ANY:   e = emit_inst(b, RI_ANY,0,0,0,-1,&idx_atom); break;
                case A_CLASS: e = emit_inst(b, RI_CLASS,0,0,0,cls_idx,&idx_atom); break;
                default: return E_PARSE;
            }
            if (e!=E_OK) return e;
            // split with loop-first greedy; y = cont = split+1
            e = emit_inst(b, RI_SPLIT, idx_atom, b->nins+1, 0, -1, &idx_split); if (e!=E_OK) return e;
            break;

        case '?': // non-greedy: split(cont, take), atom
            if (ak==A_BOL || ak==A_EOL) return E_PARSE;
            e = emit_inst(b, RI_SPLIT, b->nins+2, -1, 0,-1, &idx_split); if (e!=E_OK) return e; // x=cont (next after atom)
            switch (ak){
                case A_CHAR:  e = emit_inst(b, RI_CHAR,0,0,ch,-1,&idx_atom); break;
                case A_ANY:   e = emit_inst(b, RI_ANY,0,0,0,-1,&idx_atom); break;
                case A_CLASS: e = emit_inst(b, RI_CLASS,0,0,0,cls_idx,&idx_atom); break;
                default: return E_PARSE;
            }
            if (e!=E_OK) return e;
            b->ins[idx_split].y = idx_atom; // take path second (ordered -> non-greedy)
            break;

        default: // no quantifier
            switch (ak){
                case A_CHAR:  e = emit_inst(b, RI_CHAR,0,0,ch,-1,NULL); break;
                case A_ANY:   e = emit_inst(b, RI_ANY,0,0,0,-1,NULL); break;
                case A_CLASS: e = emit_inst(b, RI_CLASS,0,0,0,cls_idx,NULL); break;
                case A_BOL:   e = emit_inst(b, RI_BOL,0,0,0,-1,NULL); break;
                case A_EOL:   e = emit_inst(b, RI_EOL,0,0,0,-1,NULL); break;
            }
            if (e != E_OK) return e;
            break;
    }

    *i_inout = i;
    return E_OK;
}

enum Err re_compile_into(const char* pattern,
                         ReProg* out,
                         ReInst* ins_base,  int ins_cap,  int* ins_used,
                         ReClass* cls_base, int cls_cap, int* cls_used)
{
    ReB b = {0};
    b.out     = out;
    b.ins     = ins_base;
    b.ins_cap = ins_cap;
    b.cls     = cls_base;
    b.cls_cap = cls_cap;

    int i = 0;
    if (!pattern || !pattern[0]) return E_BAD_NEEDLE;

    // Parse alternation: find all | characters
    int alt_count = 1;
    for (int j = 0; pattern[j]; j++) {
        if (pattern[j] == '|') alt_count++;
    }

    if (alt_count == 1) {
        // No alternation, compile normally
        while (pattern[i]){
            enum Err e = compile_piece(&b, pattern, &i);
            if (e != E_OK) return e;
        }
    } else {
        // Handle alternation - create proper NFA structure
        int split_pc = -1;
        int match_pc = -1;

        // First, emit the split instruction
        enum Err e = emit_inst(&b, RI_SPLIT, -1, -1, 0, -1, &split_pc);
        if (e != E_OK) return e;

        int alt_start = 0;
        int alt_idx = 0;

        for (int j = 0; j <= strlen(pattern); j++) {
            if (pattern[j] == '|' || pattern[j] == '\0') {
                // Compile this alternative
                char* alt_pattern = (char*)alloca((size_t)(j - alt_start + 1));
                strncpy(alt_pattern, pattern + alt_start, (size_t)(j - alt_start));
                alt_pattern[j - alt_start] = '\0';

                int alt_start_pc = b.nins;
                int alt_i = 0;
                while (alt_pattern[alt_i]) {
                    enum Err e = compile_piece(&b, alt_pattern, &alt_i);
                    if (e != E_OK) return e;
                }

                // Patch the split to point to this alternative
                if (alt_idx == 0) {
                    b.ins[split_pc].x = alt_start_pc;
                } else {
                    b.ins[split_pc].y = alt_start_pc;
                }

                // Add JMP to MATCH after this alternative
                enum Err e = emit_inst(&b, RI_JMP, -1, 0, 0, -1, NULL);
                if (e != E_OK) return e;

                alt_start = j + 1;
                alt_idx++;
            }
        }

        // Set the continuation point for the split
        match_pc = b.nins;

        // Patch all JMP instructions to point to MATCH
        for (int k = split_pc + 1; k < b.nins; k++) {
            if (b.ins[k].op == RI_JMP && b.ins[k].x == -1) {
                b.ins[k].x = match_pc;
            }
        }
    }

    enum Err e = emit_inst(&b, RI_MATCH, 0,0,0,-1,NULL);
    if (e != E_OK) return e;

    out->ins      = b.ins;
    out->nins     = b.nins;
    out->classes  = b.cls;
    out->nclasses = b.ncls;

    *ins_used = b.nins;
    *cls_used = b.ncls;
    return E_OK;
}

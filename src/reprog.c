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

// Recursive descent parser structures
typedef struct {
    const char* pat;
    int i; // current index
} ReParser;

// Thompson construction fragment for proper quantifier handling
typedef struct {
    int start;     // start instruction
    int* outs;     // list of out instructions to patch
    int nouts;     // number of out instructions
} Frag;

static int peekp(ReParser* p) { return (unsigned char)p->pat[p->i]; }
static int getp(ReParser* p)  { return (unsigned char)p->pat[p->i++]; }
static int eop(ReParser* p)  { return p->pat[p->i] == '\0'; }

// Thompson construction helpers
static void patch(Frag* frag, int target) {
    for (int i = 0; i < frag->nouts; i++) {
        frag->outs[i] = target;
    }
}

static Frag append(Frag* a, Frag* b) {
    // Append b's outs to a's outs
    Frag result = { .start = a->start, .outs = a->outs, .nouts = a->nouts + b->nouts };
    // Note: This is simplified - in practice we'd need proper memory management
    return result;
}

// Forward declarations for recursive descent parser
static enum Err compile_alt(ReB* b, ReParser* p);
static enum Err compile_concat(ReB* b, ReParser* p);
static enum Err compile_quant(ReB* b, ReParser* p);
static enum Err compile_atom(ReB* b, ReParser* p);

// Legacy function - now just calls the new parser
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
    int min_count = 1, max_count = 1; // default for single atom
    int is_quantified = 0;

    if (q=='*' || q=='+' || q=='?') {
        i++;
        is_quantified = 1;
        switch (q) {
            case '*': min_count = 0; max_count = -1; break; // unlimited
            case '+': min_count = 1; max_count = -1; break; // unlimited
            case '?': min_count = 0; max_count = 1; break;
        }
    } else if (q == '{') {
        // Parse {n,m} quantifier
        i++; // skip '{'
        if (!pat[i] || !isdigit(pat[i])) return E_PARSE;

        // Parse minimum count
        min_count = 0;
        while (pat[i] && isdigit(pat[i])) {
            min_count = min_count * 10 + (pat[i] - '0');
            i++;
        }

        if (pat[i] == '}') {
            // {n} - exactly n times
            max_count = min_count;
            i++;
        } else if (pat[i] == ',') {
            i++; // skip ','
            if (pat[i] == '}') {
                // {n,} - n or more times
                max_count = -1; // unlimited
                i++;
            } else if (isdigit(pat[i])) {
                // {n,m} - between n and m times
                max_count = 0;
                while (pat[i] && isdigit(pat[i])) {
                    max_count = max_count * 10 + (pat[i] - '0');
                    i++;
                }
                if (pat[i] != '}') return E_PARSE;
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
        switch (ak){
            case A_CHAR:  e = emit_inst(b, RI_CHAR,0,0,ch,-1,NULL); break;
            case A_ANY:   e = emit_inst(b, RI_ANY,0,0,0,-1,NULL); break;
            case A_CLASS: e = emit_inst(b, RI_CLASS,0,0,0,cls_idx,NULL); break;
            case A_BOL:   e = emit_inst(b, RI_BOL,0,0,0,-1,NULL); break;
            case A_EOL:   e = emit_inst(b, RI_EOL,0,0,0,-1,NULL); break;
        }
        if (e!=E_OK) return e;
    } else {
        // Handle quantified patterns
        if (ak==A_BOL || ak==A_EOL) return E_PARSE; // anchors can't be quantified

        if (min_count == 0 && max_count == 1) {
            // ? quantifier - greedy: split(take, cont), atom
            int idx_split, idx_atom;
            e = emit_inst(b, RI_SPLIT, -1, -1, 0,-1, &idx_split); if (e!=E_OK) return e;
            switch (ak){
                case A_CHAR:  e = emit_inst(b, RI_CHAR,0,0,ch,-1,&idx_atom); break;
                case A_ANY:   e = emit_inst(b, RI_ANY,0,0,0,-1,&idx_atom); break;
                case A_CLASS: e = emit_inst(b, RI_CLASS,0,0,0,cls_idx,&idx_atom); break;
                default: return E_PARSE;
            }
            if (e!=E_OK) return e;
            b->ins[idx_split].x = idx_atom; // take first -> greedy
            b->ins[idx_split].y = b->nins;  // continue after atom
        } else if (min_count == 0 && max_count == -1) {
            // * quantifier - greedy: split(loop, cont), atom, jmp split
            int idx_split, idx_atom, idx_jmp;
            e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &idx_split); if (e!=E_OK) return e;
            switch (ak){
                case A_CHAR:  e = emit_inst(b, RI_CHAR, 0,0,ch,-1,&idx_atom); break;
                case A_ANY:   e = emit_inst(b, RI_ANY, 0,0,0,-1,&idx_atom); break;
                case A_CLASS: e = emit_inst(b, RI_CLASS,0,0,0,cls_idx,&idx_atom); break;
                default: return E_PARSE;
            }
            if (e!=E_OK) return e;
            e = emit_inst(b, RI_JMP, idx_split, 0,0,-1,&idx_jmp); if (e!=E_OK) return e;
            b->ins[idx_split].x = idx_atom;       // loop first -> greedy
            b->ins[idx_split].y = b->nins;        // continue after jmp
        } else if (min_count == 1 && max_count == -1) {
            // + quantifier - atom, split(loop, cont)
            int idx_atom, idx_split;
            switch (ak){
                case A_CHAR:  e = emit_inst(b, RI_CHAR,0,0,ch,-1,&idx_atom); break;
                case A_ANY:   e = emit_inst(b, RI_ANY,0,0,0,-1,&idx_atom); break;
                case A_CLASS: e = emit_inst(b, RI_CLASS,0,0,0,cls_idx,&idx_atom); break;
                default: return E_PARSE;
            }
            if (e!=E_OK) return e;
            e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &idx_split); if (e!=E_OK) return e;
            b->ins[idx_split].x = idx_atom; // loop back to atom
            b->ins[idx_split].y = b->nins;  // fallthrough to next instruction
        } else {
            // {n,m} quantifier - more complex NFA structure needed
            // For now, implement a simple approach: emit min_count atoms, then add optional ones
            int first_atom = -1;

            // Emit minimum required atoms
            for (int i = 0; i < min_count; i++) {
                int idx_atom;
                switch (ak){
                    case A_CHAR:  e = emit_inst(b, RI_CHAR,0,0,ch,-1,&idx_atom); break;
                    case A_ANY:   e = emit_inst(b, RI_ANY,0,0,0,-1,&idx_atom); break;
                    case A_CLASS: e = emit_inst(b, RI_CLASS,0,0,0,cls_idx,&idx_atom); break;
                    default: return E_PARSE;
                }
                if (e!=E_OK) return e;
                if (i == 0) first_atom = idx_atom;
            }

            if (max_count == -1) {
                // unlimited tail == greedy '*'
                int sp, at, jmp;
                e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &sp); if (e!=E_OK) return e;
                switch (ak){ case A_CHAR: e=emit_inst(b,RI_CHAR,0,0,ch,-1,&at); break;
                             case A_ANY:  e=emit_inst(b,RI_ANY ,0,0,0 ,-1,&at); break;
                             case A_CLASS:e=emit_inst(b,RI_CLASS,0,0,0,cls_idx,&at); break;
                             default: return E_PARSE; } if (e!=E_OK) return e;
                e = emit_inst(b, RI_JMP, sp, 0, 0, -1, &jmp); if (e!=E_OK) return e;
                b->ins[sp].x = at; b->ins[sp].y = b->nins;
            } else if (max_count > min_count) {
                int opt = max_count - min_count;
                for (int k = 0; k < opt; ++k) {
                    int sp, at;
                    e = emit_inst(b, RI_SPLIT, 0, 0, 0, -1, &sp); if (e!=E_OK) return e;
                    switch (ak){ case A_CHAR: e=emit_inst(b,RI_CHAR,0,0,ch,-1,&at); break;
                                 case A_ANY:  e=emit_inst(b,RI_ANY ,0,0,0 ,-1,&at); break;
                                 case A_CLASS:e=emit_inst(b,RI_CLASS,0,0,0,cls_idx,&at); break;
                                 default: return E_PARSE; } if (e!=E_OK) return e;
                    b->ins[sp].x = at;      // take one more (greedy)
                    b->ins[sp].y = b->nins; // or stop here
                }
            }
        }
    }

    *i_inout = i;
    return E_OK;
}

// Recursive descent parser implementation
// Grammar: regex := concat ('|' concat)*

// alt := concat ('|' concat)*
static enum Err compile_alt(ReB* b, ReParser* p) {
    // Compile first alternative
    enum Err e = compile_concat(b, p);
    if (e != E_OK) return e;
    
    // Handle additional alternatives
    int jmp_stack_cap = 16, jmp_n = 0;
    int* jmp_from = (int*)alloca((size_t)jmp_stack_cap * sizeof(int));
    int last_split_pc = -1;
    
    while (peekp(p) == '|') {
        getp(p); // consume '|'
        
        // Emit split: prefer current alternative (x), fallthrough to next (y)
        int split_pc;
        e = emit_inst(b, RI_SPLIT, -1, -1, 0, -1, &split_pc);
        if (e != E_OK) return e;
        last_split_pc = split_pc;
        
        // Compile next alternative
        int alt_start = b->nins;
        e = compile_concat(b, p);
        if (e != E_OK) return e;
        
        // Emit jump to continuation
        int jmp_pc;
        e = emit_inst(b, RI_JMP, -1, 0, 0, -1, &jmp_pc);
        if (e != E_OK) return e;
        
        // Store jump for later patching
        if (jmp_n == jmp_stack_cap) {
            jmp_stack_cap *= 2;
            jmp_from = (int*)alloca((size_t)jmp_stack_cap * sizeof(int));
        }
        jmp_from[jmp_n++] = jmp_pc;
        
        // Patch split: x = alt_start, y = next alternative (or continuation if this is the last)
        b->ins[split_pc].x = alt_start;
        // We'll patch the y field after we know if there are more alternatives
    }
    
    // Now patch all the split y fields
    int split_idx = 0;
    int temp_jmp_n = 0;
    int temp_last_split = last_split_pc;
    
    // Re-scan to patch split y fields correctly
    p->i = 0; // Reset parser position
    e = compile_concat(b, p); // Compile first alternative again
    if (e != E_OK) return e;
    
    while (peekp(p) == '|') {
        getp(p); // consume '|'
        
        // Find the split instruction for this alternative
        // This is a simplified approach - we'll patch the last split's y field
        if (peekp(p) == '|' || eop(p) || peekp(p) == ')') {
            // This is the last alternative, patch the previous split's y field
            if (temp_last_split >= 0) {
                b->ins[temp_last_split].y = b->nins; // point to continuation
            }
        } else {
            // There are more alternatives, patch to point to next alternative
            if (temp_last_split >= 0) {
                b->ins[temp_last_split].y = b->nins; // point to next alternative
            }
        }
        
        // Compile next alternative
        int alt_start = b->nins;
        e = compile_concat(b, p);
        if (e != E_OK) return e;
        
        // Emit jump to continuation
        int jmp_pc;
        e = emit_inst(b, RI_JMP, -1, 0, 0, -1, &jmp_pc);
        if (e != E_OK) return e;
        
        temp_jmp_n++;
    }
    
    // Patch all jumps to continuation point
    int cont = b->nins;
    for (int t = 0; t < jmp_n; ++t) {
        b->ins[jmp_from[t]].x = cont + 1; // +1 because RI_MATCH will be at cont
    }
    
    // Patch the last split's fallthrough to a dead-end epsilon that goes nowhere:
    // RI_JMP to itself. Epsilon-closure sees it once (seen[] stops the loop) and
    // it yields no consuming threads and no MATCH path.
    if (last_split_pc >= 0) {
        int dead_pc;
        enum Err e2 = emit_inst(b, RI_JMP, 0, 0, 0, -1, &dead_pc);
        if (e2 != E_OK) return e2;
        b->ins[dead_pc].x = dead_pc;   // self-loop
        b->ins[last_split_pc].y = dead_pc;
    }
    
    return E_OK;
}

// concat := quant+
static enum Err compile_concat(ReB* b, ReParser* p) {
    // Stop on: ')', '|', or end
    while (!eop(p) && peekp(p) != ')' && peekp(p) != '|') {
        enum Err e = compile_quant(b, p);
        if (e != E_OK) return e;
    }
    return E_OK;
}

// quant := atom quantifier?
static enum Err compile_quant(ReB* b, ReParser* p) {
    // Compile the atom first
    enum Err e = compile_atom(b, p);
    if (e != E_OK) return e;

    // Check for quantifier immediately after
    if (eop(p) || peekp(p) == ')' || peekp(p) == '|') {
        return E_OK; // no quantifier
    }

    char q = peekp(p);
    if (q == '*' || q == '+' || q == '?' || q == '{') {
        // We have a quantifier, but we've already compiled the atom
        // This is a limitation of the current approach - we need to handle
        // quantifiers at the atom level, not after compilation
        // For now, return an error to indicate this needs proper implementation
        return E_PARSE;
    }

    return E_OK;
}

// atom := literal | '.' | '^' | '$' | '['class']' | '(' alt ')'
static enum Err compile_atom(ReB* b, ReParser* p) {
    if (eop(p)) return E_PARSE;

    int c = peekp(p);

    if (c == '(') {
        getp(p); // consume '('
        enum Err e = compile_alt(b, p);
        if (e != E_OK) return e;
        if (peekp(p) != ')') return E_PARSE;
        getp(p); // consume ')'
        return E_OK;
    }

    if (c == '^') {
        getp(p);
        return emit_inst(b, RI_BOL, 0, 0, 0, -1, NULL);
    }

    if (c == '$') {
        getp(p);
        return emit_inst(b, RI_EOL, 0, 0, 0, -1, NULL);
    }

    if (c == '.') {
        getp(p);
        return emit_inst(b, RI_ANY, 0, 0, 0, -1, NULL);
    }

    if (c == '[') {
        getp(p); // consume '['
        int cls_idx;
        int i = p->i;
        enum Err e = parse_class(b, p->pat, &i, &cls_idx);
        if (e != E_OK) return e;
        p->i = i; // update parser position
        return emit_inst(b, RI_CLASS, 0, 0, 0, cls_idx, NULL);
    }

    if (c == '\\') {
        getp(p); // consume '\'
        if (eop(p)) return E_PARSE;
        c = getp(p);

        unsigned char ch = 0;
        int cls_idx = -1;

        switch (c) {
            case 'd': {
                ReClass cls; cls_clear(&cls); cls_set_digit(&cls);
                enum Err e = new_class(b, &cls, &cls_idx);
                if (e != E_OK) return e;
                return emit_inst(b, RI_CLASS, 0, 0, 0, cls_idx, NULL);
            }
            case 'D': {
                ReClass cls;
                for (int v = 0; v < 256; ++v) cls_set(&cls, (unsigned char)v);
                ReClass d; cls_clear(&d); cls_set_digit(&d);
                for (int b2 = 0; b2 < 32; ++b2) cls.bits[b2] &= (unsigned char)~d.bits[b2];
                enum Err e = new_class(b, &cls, &cls_idx);
                if (e != E_OK) return e;
                return emit_inst(b, RI_CLASS, 0, 0, 0, cls_idx, NULL);
            }
            case 'w': {
                ReClass cls; cls_clear(&cls); cls_set_word(&cls);
                enum Err e = new_class(b, &cls, &cls_idx);
                if (e != E_OK) return e;
                return emit_inst(b, RI_CLASS, 0, 0, 0, cls_idx, NULL);
            }
            case 'W': {
                ReClass cls;
                for (int v = 0; v < 256; ++v) cls_set(&cls, (unsigned char)v);
                ReClass w; cls_clear(&w); cls_set_word(&w);
                for (int b2 = 0; b2 < 32; ++b2) cls.bits[b2] &= (unsigned char)~w.bits[b2];
                enum Err e = new_class(b, &cls, &cls_idx);
                if (e != E_OK) return e;
                return emit_inst(b, RI_CLASS, 0, 0, 0, cls_idx, NULL);
            }
            case 's': {
                ReClass cls; cls_clear(&cls); cls_set_ws(&cls);
                enum Err e = new_class(b, &cls, &cls_idx);
                if (e != E_OK) return e;
                return emit_inst(b, RI_CLASS, 0, 0, 0, cls_idx, NULL);
            }
            case 'S': {
                ReClass cls;
                for (int v = 0; v < 256; ++v) cls_set(&cls, (unsigned char)v);
                ReClass ws; cls_clear(&ws); cls_set_ws(&ws);
                for (int b2 = 0; b2 < 32; ++b2) cls.bits[b2] &= (unsigned char)~ws.bits[b2];
                enum Err e = new_class(b, &cls, &cls_idx);
                if (e != E_OK) return e;
                return emit_inst(b, RI_CLASS, 0, 0, 0, cls_idx, NULL);
            }
            case 'n': ch = '\n'; break;
            case 't': ch = '\t'; break;
            case 'r': ch = '\r'; break;
            case 'f': ch = '\f'; break;
            case 'v': ch = '\v'; break;
            case '0': ch = '\0'; break;
            default: ch = (unsigned char)c; break;
        }

        return emit_inst(b, RI_CHAR, 0, 0, ch, -1, NULL);
    }

    // Literal character
    unsigned char ch = (unsigned char)getp(p);
    return emit_inst(b, RI_CHAR, 0, 0, ch, -1, NULL);
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

    if (!pattern || !pattern[0]) return E_BAD_NEEDLE;

    // Check if pattern has quantifiers - if so, use old parser
    int has_quantifiers = 0;
    int paren_depth = 0;
    for (int i = 0; pattern[i]; i++) {
        if (pattern[i] == '(') paren_depth++;
        else if (pattern[i] == ')') paren_depth--;
        else if (pattern[i] == '*' || pattern[i] == '+' || pattern[i] == '?' || pattern[i] == '{') {
            has_quantifiers = 1;
            break;
        }
    }

    if (has_quantifiers) {
        // Use old parser for patterns with quantifiers
        int i = 0;

        // Parse alternation: find all | characters not inside parentheses
        int alt_count = 1;
        paren_depth = 0;
        for (int j = 0; pattern[j]; j++) {
            if (pattern[j] == '(') paren_depth++;
            else if (pattern[j] == ')') paren_depth--;
            else if (pattern[j] == '|' && paren_depth == 0) alt_count++;
        }

        if (alt_count == 1) {
            // No alternation, compile normally
            while (pattern[i]){
                enum Err e = compile_piece(&b, pattern, &i);
                if (e != E_OK) return e;
            }
        } else {
            int jlen = (int)strlen(pattern);
            int alt_start = 0;
            int jmp_stack_cap = 16, jmp_n = 0;
            int* jmp_from = (int*)alloca((size_t)jmp_stack_cap * sizeof(int));
            int last_split_pc = -1;
            paren_depth = 0;
            for (int j = 0; j <= jlen; ++j) {
                if (j < jlen) {
                    if (pattern[j] == '(') paren_depth++;
                    else if (pattern[j] == ')') paren_depth--;
                }
                if ((pattern[j] == '|' && paren_depth == 0) || pattern[j] == '\0') {
                    // split: prefer taking this alternative (x), else fallthrough (y)
                    int split_pc; enum Err e = emit_inst(&b, RI_SPLIT, 0, 0, 0, -1, &split_pc); if (e!=E_OK) return e;
                    last_split_pc = split_pc;
                    int alt_i = 0;
                    char* alt = (char*)alloca((size_t)(j - alt_start + 1));
                    memcpy(alt, pattern + alt_start, (size_t)(j - alt_start));
                    alt[j - alt_start] = '\0';
                    int alt_start_pc = b.nins;
                    while (alt[alt_i]) { e = compile_piece(&b, alt, &alt_i); if (e!=E_OK) return e; }
                    int jmp_pc; e = emit_inst(&b, RI_JMP, -1, 0, 0, -1, &jmp_pc); if (e!=E_OK) return e;
                    if (jmp_n == jmp_stack_cap) { /* simple grow */ jmp_stack_cap *= 2; jmp_from = (int*)alloca((size_t)jmp_stack_cap * sizeof(int)); }
                    jmp_from[jmp_n++] = jmp_pc;
                    b.ins[split_pc].x = alt_start_pc; // into alt body
                    b.ins[split_pc].y = b.nins;       // fall through to next split/alt
                    alt_start = j + 1;
                }
            }
            int cont = b.nins;
            for (int t = 0; t < jmp_n; ++t) b.ins[jmp_from[t]].x = cont + 1; // +1 because RI_MATCH will be at cont

            // Patch the last split's fallthrough to a dead-end epsilon that goes nowhere:
            // RI_JMP to itself. Epsilon-closure sees it once (seen[] stops the loop) and
            // it yields no consuming threads and no MATCH path.
            if (last_split_pc >= 0) {
                int dead_pc;
                enum Err e2 = emit_inst(&b, RI_JMP, 0, 0, 0, -1, &dead_pc); if (e2 != E_OK) return e2;
                b.ins[dead_pc].x = dead_pc;   // self-loop
                b.ins[last_split_pc].y = dead_pc;
            }
        }
    } else {
        // Use new recursive descent parser for patterns without quantifiers
        ReParser p = { .pat = pattern, .i = 0 };
        enum Err e = compile_alt(&b, &p);
        if (e != E_OK) return e;

        // Ensure we consumed the entire pattern
        if (!eop(&p)) return E_PARSE;
    }

    // Emit final match instruction
    enum Err e = emit_inst(&b, RI_MATCH, 0, 0, 0, -1, NULL);
    if (e != E_OK) return e;

    out->ins      = b.ins;
    out->nins     = b.nins;
    out->classes  = b.cls;
    out->nclasses = b.ncls;

    *ins_used = b.nins;
    *cls_used = b.ncls;
    return E_OK;
}

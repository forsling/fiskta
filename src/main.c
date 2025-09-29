// main.c
#include "arena.h"
#include "fiskta.h"
#include "iosearch.h"
#include "reprog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alloca.h>

typedef struct {
    i32 clause_count;
    i32 total_ops;
    i32 sum_take_ops;
    i32 sum_label_ops;
    i32 needle_count;
    size_t needle_bytes;
    i32   sum_findr_ops;
    i32   re_ins_estimate;
    i32   re_classes_estimate;
    i32   re_ins_estimate_max; // NEW: for regex scratch sizing
} ParsePlan;

// Minimal ops-string splitter for quoted CLI usage
static i32 split_ops_string(const char* s, char** out, i32 max_tokens)
{
    static char buf[4096]; // one-shot scratch; fine for our CLI
    i32 n = 0;
    size_t off = 0;

    while (*s && n < max_tokens) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;

        if (s[0] == ':' && s[1] == ':') { // "::" token
            out[n++] = (char*)"::";
            s += 2;
            continue;
        }

        const char* start = s;
        while (*s && *s != ' ' && *s != '\t') {
            if (s[0] == ':' && s[1] == ':')
                break;
            s++;
        }
        size_t len = (size_t)(s - start);
        if (len) {
            if (off + len + 1 >= sizeof(buf))
                return -1; // return error instead of silent truncation
            memcpy(buf + off, start, len);
            buf[off + len] = '\0';
            out[n++] = buf + off;
            off += len + 1;
        }
    }
    return n;
}

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

// parse_program and parse_free removed - use parse_build with arena allocation instead
enum Err engine_run(const Program*, const char*, FILE*);
enum Err parse_preflight(i32 token_count, char** tokens, const char* in_path, ParsePlan* plan, const char** in_path_out);
enum Err parse_build(i32 token_count, char** tokens, const char* in_path, Program* prg, const char** in_path_out,
    Clause* clauses_buf, Op* ops_buf,
    char* str_pool, size_t str_pool_cap);
enum Err io_open(File* io, const char* path,
    unsigned char* search_buf, size_t search_buf_cap);

static const char* err_str(enum Err e) {
    switch (e) {
    case E_OK:
        return "ok";
    case E_PARSE:
        return "parse error";
    case E_BAD_NEEDLE:
        return "empty needle";
    case E_LOC_RESOLVE:
        return "location not resolvable";
    case E_NO_MATCH:
        return "no match in window";
    case E_LABEL_FMT:
        return "bad label (A-Z, _ or -, ≤16)";
    case E_IO:
        return "I/O error";
    case E_OOM:
        return "out of memory";
    default:
        return "unknown error";
    }
}

static void die(enum Err e, const char* msg)
{
    if (msg)
        fprintf(stderr, "fiskta: %s (%s)\n", msg, err_str(e));
    else
        fprintf(stderr, "fiskta: %s\n", err_str(e));
    exit(e == E_OK ? 0 : 2);
}

// Arena alignment helper with overflow protection
static size_t safe_align(size_t x, size_t align) {
    size_t aligned = a_align(x, align);
    if (aligned < x) { // overflow check
        die(E_OOM, "arena alignment overflow");
        return 0; // unreachable
    }
    return aligned;
}

// Normalize "no match" into one code
static inline enum Err normalize_no_match(enum Err e){
    switch (e){
        case E_NO_MATCH:
        /* Add any aliases your engine might use for "no match": */
        #ifdef E_REGEX_NO_MATCH
        case E_REGEX_NO_MATCH:
        #endif
        #ifdef E_WINDOW_NO_MATCH
        case E_WINDOW_NO_MATCH:
        #endif
        #ifdef E_SEARCH_NOT_FOUND
        case E_SEARCH_NOT_FOUND:
        #endif
            return E_NO_MATCH;
        default:
            return e;
    }
}

// Complete VM state reset for clause execution
static void vm_reset_full(VM* vm)
{
    if (!vm) return;
    vm->cursor = 0;
    vm->last_match.valid = false;
    vm->last_match.start = 0;
    vm->last_match.end = 0;
    vm->view.active = false;
    vm->view.lo = 0;
    vm->view.hi = 0;
    vm->gen_counter = 0;

    // Reset all labels
    for (int i = 0; i < 32; i++) {
        vm->labels[i].in_use = false;
        vm->labels[i].name_idx = 0;
        vm->labels[i].pos = 0;
        vm->labels[i].gen = 0;
    }
}

static void print_usage(void)
{
    printf("fiskta (FInd SKip TAke) Text Extraction Tool\n");
    printf("\n");
    printf("USAGE:\n");
    printf("  fiskta [options] <operations> [file|-]\n");
    printf("\n");
    printf("OPERATIONS:\n");
    printf("  take <n><unit>              Extract n units from current position\n");
    printf("  skip <n><unit>              Move cursor n units forward (no output)\n");
    printf("  find [to <location>] <string>\n");
    printf("                              Search within [min(cursor,L), max(cursor,L)),\n");
    printf("                              default L=EOF; picks match closest to cursor\n");
    printf("  findr [to <location>] <regex>\n");
    printf("                              Search using regular expressions within\n");
    printf("                              [min(cursor,L), max(cursor,L)); supports\n");
    printf("                              character classes, quantifiers, anchors\n");
    printf("  take to <location>          Order-normalized: emits [min(cursor,L), max(cursor,L));\n");
    printf("                              cursor moves to the high end\n");
    printf("  take until <string> [at <location>]\n");
    printf("                              Forward-only: emits [cursor, B) where B is derived\n");
    printf("                              from the match; cursor moves only if B > cursor\n");
    printf("  label <name>                Mark current position with label\n");
    printf("  goto <location>             Jump to labeled position\n");
    printf("  viewset <L1> <L2>           Limit all ops to [min(L1,L2), max(L1,L2))\n");
    printf("  viewclear                   Clear view; return to full file\n");
    printf("\n");
    printf("UNITS:\n");
    printf("  b                           Bytes\n");
    printf("  l                           Lines (LF only, CR treated as bytes)\n");
    printf("  c                           UTF-8 code points (never splits sequences)\n");
    printf("\n");
    printf("REGEX SYNTAX:\n");
    printf("  Character Classes: \\d (digits), \\w (word), \\s (space), [a-z], [^0-9]\n");
    printf("  Quantifiers: * (0+), + (1+), ? (0-1), {n} (exactly n), {n,m} (n to m)\n");
    printf("  Grouping: ( ... ) (group subpatterns), (a|b)+ (quantified groups)\n");
    printf("  Anchors: ^ (line start), $ (line end)\n");
    printf("  Alternation: | (OR)\n");
    printf("  Escape: \\n, \\t, \\r, \\f, \\v, \\0\n");
    printf("  Special: . (any char except newline)\n");
    printf("\n");
    printf("LABELS:\n");
    printf("  NAME                        UPPERCASE, ≤16 chars, [A-Z_-]\n");
    printf("\n");
    printf("LOCATIONS:\n");
    printf("  cursor                      Current cursor position\n");
    printf("  BOF                         Beginning of file\n");
    printf("  EOF                         End of file\n");
    printf("  match-start                 Start of last match\n");
    printf("  match-end                   End of last match\n");
    printf("  line-start                  Start of current line\n");
    printf("  line-end                    End of current line\n");
    printf("  <label>                     Named label position\n");
    printf("  Note: line-start/line-end are relative to the cursor here; in \"at\" (for\n");
    printf("  \"take until\") they're relative to the last match.\n");
    printf("\n");
    printf("OFFSETS:\n");
    printf("  <location> +<n><unit>       n units after location\n");
    printf("  <location> -<n><unit>       n units before location\n");
    printf("                              (inline offsets like BOF+100b are allowed)\n");
    printf("\n");
    printf("EXAMPLES:\n");
    printf("  fiskta take 10b file.txt                    # Extract first 10 bytes\n");
    printf("  fiskta take 3l file.txt                     # Extract first 3 lines\n");
    printf("  fiskta find \"ERROR\" take to match-start file.txt  # Extract to ERROR (excludes ERROR)\n");
    printf("  fiskta findr \"\\\\d{3}-\\\\d{3}-\\\\d{4}\" take +12b file.txt  # Find phone numbers\n");
    printf("  fiskta findr \"^ERROR\" take to line-end file.txt  # Find ERROR at line start\n");
    printf("  fiskta take to BOF+100b file.txt          # Extract from BOF+100b\n");
    printf("  fiskta skip 5b take 10b file.txt           # Skip 5, take 10\n");
    printf("  fiskta take until \"---\" file.txt          # Extract until \"---\"\n");
    printf("  fiskta take until \"END\" at line-start file.txt  # Extract until start of END's line\n");
    printf("  echo \"Hello\" | fiskta take 5b -          # Process stdin\n");
    printf("\n");
    printf("CLAUSES:\n");
    printf("  Separate operations with '::'. Each clause executes independently.\n");
    printf("  If a clause fails, subsequent clauses still execute.\n");
    printf("  Command succeeds if ANY clause succeeds.\n");
    printf("\n");
    printf("OPTIONS:\n");
    printf("  -h, --help                  Show this help message\n");
    printf("  -v, --version               Show version information\n");
    printf("\n");
    printf("For more information, see the README.md file.\n");
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    // Handle help and version options
    if (argc == 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
            printf("fiskta (FInd SKip TAke) v2.2\n");
            return 0;
        }
    }

    if (argc < 3) {
        fprintf(stderr, "Usage: fiskta [options] <operations> [file|-]\n");
        fprintf(stderr, "Try 'fiskta --help' for more information.\n");
        return 2;
    }

    // 1) Preflight
    ParsePlan plan = { 0 };
    const char* path = NULL;

    // Build the token view the parser expects: argv[1..argc-1] if no file specified, argv[1..argc-2] if file specified
    char** tokens = argv + 1;
    i32 token_count = argc - 1;
    const char* in_path = "-"; // Default to stdin

    // Check if last argument is a file path (not an operation)
    // Improved heuristic: only treat as file if it contains path separators, is "-", or actually exists
    if (argc > 2) {
        const char* last_arg = argv[argc - 1];
        bool treat_as_file = false;
        
        if (strcmp(last_arg, "-") == 0 || strchr(last_arg, '/') != NULL || strchr(last_arg, '\\') != NULL) {
            treat_as_file = true;
        } else {
            // Check if it actually exists as a file
            FILE* test_file = fopen(last_arg, "rb");
            if (test_file) {
                fclose(test_file);
                treat_as_file = true;
            }
        }
        
        if (treat_as_file) {
            in_path = last_arg;
            token_count = argc - 2;
        }
    }

    // If user passed a single ops string, split it.
    char* splitv[256];
    if (token_count == 1 && strchr(tokens[0], ' ')) {
        i32 n = split_ops_string(tokens[0], splitv, (i32)(sizeof splitv / sizeof splitv[0]));
        if (n == -1) {
            fprintf(stderr, "fiskta: operations string too long (max 4096 bytes)\n");
            return 2;
        }
        if (n > 0) {
            tokens = splitv;
            token_count = n;
            // Don't add input path to tokens - it's passed separately
        }
    }

    enum Err e = parse_preflight(token_count, tokens, in_path, &plan, &path);
    if (e != E_OK) {
        die(e, "parse preflight");
        return 2;
    }

    // 2) Compute sizes
    const size_t search_buf_cap = (FW_WIN > (BK_BLK + OVERLAP_MAX)) ? (size_t)FW_WIN : (size_t)(BK_BLK + OVERLAP_MAX);
    const size_t ops_bytes = (size_t)plan.total_ops * sizeof(Op);
    const size_t clauses_bytes = (size_t)plan.clause_count * sizeof(Clause);
    const size_t str_pool_bytes = plan.needle_bytes + (size_t)plan.needle_count; // include NULs
    // Per-clause ranges/labels are stack-allocated during execution
    // Regex pools
    const size_t re_prog_bytes = (size_t)plan.sum_findr_ops * sizeof(ReProg);
    const size_t re_ins_bytes  = (size_t)plan.re_ins_estimate * sizeof(ReInst);
    const size_t re_cls_bytes  = (size_t)plan.re_classes_estimate * sizeof(ReClass);
    // Regex runtime scratch (arena-backed):
    // Choose per-run thread capacity as ~4x max nins (like old logic), min 32.
    int re_threads_cap = plan.re_ins_estimate_max > 0 ? 4 * plan.re_ins_estimate_max : 32;
    if (re_threads_cap < 32) re_threads_cap = 32;
    const size_t re_threads_bytes = (size_t)re_threads_cap * sizeof(ReThread);

    // 3) One allocation
    size_t search_buf_size = safe_align(search_buf_cap, alignof(unsigned char));
    size_t clauses_size = safe_align(clauses_bytes, alignof(Clause));
    size_t ops_size = safe_align(ops_bytes, alignof(Op));
    size_t re_prog_size = safe_align(re_prog_bytes, alignof(ReProg));
    size_t re_ins_size  = safe_align(re_ins_bytes,  alignof(ReInst));
    size_t re_cls_size  = safe_align(re_cls_bytes,  alignof(ReClass));
    size_t str_pool_size = safe_align(str_pool_bytes, alignof(char));
    // Two thread buffers + two seen arrays sized to max estimated nins
    size_t re_seen_bytes_each = (size_t)(plan.re_ins_estimate_max > 0 ? plan.re_ins_estimate_max : 32);
    size_t re_seen_size = safe_align(re_seen_bytes_each, 1) * 2;
    size_t re_thrbufs_size = safe_align(re_threads_bytes, alignof(ReThread)) * 2;

    size_t total = search_buf_size + clauses_size + ops_size
                 + re_prog_size + re_ins_size + re_cls_size
                 + str_pool_size + re_thrbufs_size + re_seen_size + 64; // small cushion

    void* block = malloc(total);
    if (!block) {
        die(E_OOM, "arena alloc");
        return 2;
    }
    Arena A;
    arena_init(&A, block, total);

    // 4) Carve slices
    unsigned char* search_buf = arena_alloc(&A, search_buf_cap, alignof(unsigned char));
    Clause* clauses_buf = arena_alloc(&A, clauses_bytes, alignof(Clause));
    Op* ops_buf = arena_alloc(&A, ops_bytes, alignof(Op));
    ReThread* re_curr_thr = arena_alloc(&A, re_threads_bytes, alignof(ReThread));
    ReThread* re_next_thr = arena_alloc(&A, re_threads_bytes, alignof(ReThread));
    unsigned char* seen_curr = arena_alloc(&A, re_seen_bytes_each, 1);
    unsigned char* seen_next = arena_alloc(&A, re_seen_bytes_each, 1);
    ReProg* re_progs = arena_alloc(&A, re_prog_bytes, alignof(ReProg));
    ReInst*  re_ins  = arena_alloc(&A, re_ins_bytes,  alignof(ReInst));
    ReClass* re_cls  = arena_alloc(&A, re_cls_bytes,  alignof(ReClass));
    char* str_pool = arena_alloc(&A, str_pool_bytes, alignof(char));
    if (!search_buf || !clauses_buf || !ops_buf
        || !re_curr_thr || !re_next_thr || !seen_curr || !seen_next
        || !re_progs || !re_ins || !re_cls || !str_pool) {
        die(E_OOM, "arena carve");
        free(block);
        return 2;
    }

    // 5) Parse into preallocated storage
    Program prg = { 0 };
    e = parse_build(token_count, tokens, in_path, &prg, &path, clauses_buf, ops_buf,
        str_pool, str_pool_bytes);
    if (e != E_OK) {
        die(e, "parse build");
        free(block);
        return 2;
    }

    // 5.5) Compile regex programs
    i32 re_prog_idx = 0, re_ins_idx = 0, re_cls_idx = 0;
    for (i32 ci = 0; ci < prg.clause_count; ++ci) {
        Clause* clause = &prg.clauses[ci];
        for (i32 i = 0; i < clause->op_count; ++i) {
            Op* op = &clause->ops[i];
            if (op->kind == OP_FINDR) {
                ReProg* prog = &re_progs[re_prog_idx++];
                enum Err err = re_compile_into(op->u.findr.pattern, prog,
                    re_ins + re_ins_idx, (i32)(re_ins_bytes/sizeof(ReInst)) - re_ins_idx, &re_ins_idx,
                    re_cls + re_cls_idx, (i32)(re_cls_bytes/sizeof(ReClass)) - re_cls_idx, &re_cls_idx);
                if (err != E_OK) {
                    die(err, "regex compile");
                    free(block);
                    return 2;
                }
                op->u.findr.prog = prog;
            }
        }
    }

    // 6) Open I/O with arena-backed buffers
    File io = { 0 };
    e = io_open(&io, path, search_buf, search_buf_cap);
    if (e != E_OK) {
        die(e, "I/O open");
        free(block);
        return 2;
    }
    // Provide regex scratch (seen arrays sized by max-estimated nins)
    io_set_regex_scratch(&io, re_curr_thr, re_next_thr, re_threads_cap,
                         seen_curr, seen_next, (size_t)re_seen_bytes_each);

    // 7) Run engine using precomputed scratch pools with short-circuit behavior
    VM vm = { 0 };
    vm.cursor = 0;
    vm.last_match.valid = false;
    vm.gen_counter = 0;

    i32 ok = 0;
    enum Err last_err = E_OK;

    for (i32 ci = 0; ci < prg.clause_count; ++ci) {
        i32 rc=0, lc=0;
        clause_caps(&prg.clauses[ci], &rc, &lc);
        Range*      r_tmp  = rc ? alloca((size_t)rc * sizeof *r_tmp) : NULL;
        LabelWrite* lw_tmp = lc ? alloca((size_t)lc * sizeof *lw_tmp) : NULL;
        e = execute_clause_with_scratch(&prg.clauses[ci], &prg, &io, &vm, stdout,
                                        r_tmp, rc, lw_tmp, lc);
        if (e == E_OK)
            ok++;
        else
            last_err = e;
    }
    io_close(&io);
    free(block);

    if (ok == 0) {
        fprintf(stderr, "fiskta: execution error (%s)\n", err_str(last_err));
        return 2;
    }
    return 0;
}

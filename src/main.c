#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "arena.h"
#include "fiskta.h"
#include "iosearch.h"
#include "parse_plan.h"
#include "reprog.h"
#include "util.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>

#ifndef FISKTA_VERSION
#define FISKTA_VERSION "dev"
#endif

// Helper for overflow-safe size arithmetic
static int add_ovf(size_t a, size_t b, size_t* out)
{
    if (SIZE_MAX - a < b)
        return 1;
    *out = a + b;
    return 0;
}

// Quote-aware ops-string splitter for CLI usage
// Note: one-shot scratch; tokens invalidated after next call
static i32 split_ops_string(const char* s, char** out, i32 max_tokens)
{
    static char buf[4096];
    size_t boff = 0;
    i32 ntok = 0;

    enum { S_WS, S_TOKEN, S_SQ, S_DQ } st = S_WS;
    const char* p = s;

    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (st == S_WS) {
            if (c == ' ' || c == '\t') { p++; continue; }
            if (c == '\'') { st = S_SQ; p++; if (boff+1 >= sizeof buf) return -1; out[ntok] = &buf[boff]; continue; }
            if (c == '"')  { st = S_DQ; p++; if (boff+1 >= sizeof buf) return -1; out[ntok] = &buf[boff]; continue; }
            // start token
            st = S_TOKEN;
            if (ntok >= max_tokens) return -1;
            out[ntok] = &buf[boff];
            continue;
        } else if (st == S_TOKEN) {
            if (c == ' ' || c == '\t') {
                buf[boff++] = '\0'; ntok++; st = S_WS; p++; continue;
            }
            if (c == '\'') { st = S_SQ; p++; continue; }
            if (c == '"')  { st = S_DQ; p++; continue; }
            if (c == '\\' && p[1]) {
                unsigned char next = (unsigned char)p[1];
                if (next == ' ' || next == '\t' || next == '\\' || next == '\'' || next == '"') {
                    if (boff+1 >= sizeof buf) return -1;
                    buf[boff++] = (char)next; p += 2; continue;
                }
            }
            if (boff+1 >= sizeof buf) return -1;
            buf[boff++] = (char)c; p++; continue;
        } else if (st == S_SQ) { // single quotes: no escapes
            if (c == '\'') { st = S_TOKEN; p++; continue; }
            if (boff+1 >= sizeof buf) return -1;
            buf[boff++] = (char)c; p++; continue;
        } else { // S_DQ
            if (c == '"') { st = S_TOKEN; p++; continue; }
            if (c == '\\' && p[1]) {
                unsigned char esc = (unsigned char)p[1];
                if (esc == '"' || esc == '\\') {
                    if (boff+1 >= sizeof buf) return -1;
                    buf[boff++] = (char)esc; p += 2; continue;
                }
            }
            if (boff+1 >= sizeof buf) return -1;
            buf[boff++] = (char)c; p++; continue;
        }
    }

    if (st == S_TOKEN || st == S_SQ || st == S_DQ) {
        buf[boff++] = '\0';
        if (ntok < max_tokens) ntok++;
    }
    return ntok;
}

static int read_command_stream_line(char* buf, size_t cap)
{
    size_t total = 0;
    int ch;
    bool overflow = false;

    while ((ch = fgetc(stdin)) != EOF) {
        if (ch == '\n')
            break;
        if (total + 1 >= cap) {
            overflow = true;
            continue;
        }
        buf[total++] = (char)ch;
    }

    if (ch == EOF && total == 0 && !overflow)
        return -1; // EOF with no data

    if (overflow) {
        buf[0] = '\0';
        return -2;
    }

    while (total > 0 && buf[total - 1] == '\r')
        total--;

    buf[total] = '\0';
    return (int)total;
}

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

enum Err engine_run(const Program*, const char*, FILE*);
enum Err parse_preflight(i32 token_count, char** tokens, const char* in_path, ParsePlan* plan, const char** in_path_out);
enum Err parse_build(i32 token_count, char** tokens, const char* in_path, Program* prg, const char** in_path_out,
    Clause* clauses_buf, Op* ops_buf,
    char* str_pool, size_t str_pool_cap);
enum Err io_open(File* io, const char* path,
    unsigned char* search_buf, size_t search_buf_cap);
void commit_labels(VM* vm, const LabelWrite* label_writes, i32 label_count);
enum Err io_emit(File* io, i64 start, i64 end, FILE* out);

static const char* err_str(enum Err e)
{
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
        return "bad label (A-Z, _ or -, <16)";
    case E_IO:
        return "I/O error";
    case E_OOM:
        return "out of memory";
    default:
        return "unknown error";
    }
}

static void print_err(enum Err e, const char* msg)
{
    if (msg)
        fprintf(stderr, "fiskta: %s (%s)\n", msg, err_str(e));
    else
        fprintf(stderr, "fiskta: %s\n", err_str(e));
}

static size_t align_or_die(size_t x, size_t align)
{
    size_t aligned = safe_align(x, align);
    if (aligned == SIZE_MAX) {
        print_err(E_OOM, "arena alignment overflow");
        exit(4);
    }
    return aligned;
}

#ifdef _WIN32
#include <windows.h>
#if !defined(__MINGW32__) && !defined(__MINGW64__)
#define fseeko _fseeki64
#define ftello _ftelli64
#endif
#endif

typedef enum {
    WINDOW_POLICY_DELTA,
    WINDOW_POLICY_RESCAN,
    WINDOW_POLICY_CURSOR
} WindowPolicy;

enum {
    CMD_STREAM_BUF_CAP = 4096,
    CMD_STREAM_MAX_TOKENS = 1024,
    CMD_STREAM_MAX_OPS = 2048,
    CMD_STREAM_MAX_CLAUSES = 1024,
    CMD_STREAM_MAX_NEEDLE_BYTES = CMD_STREAM_BUF_CAP,
    CMD_STREAM_MAX_FINDR = CMD_STREAM_MAX_OPS,
    CMD_STREAM_MAX_TAKE = CMD_STREAM_MAX_OPS,
    CMD_STREAM_MAX_LABEL = CMD_STREAM_MAX_OPS,
    CMD_STREAM_MAX_RE_INS_TOTAL = (CMD_STREAM_MAX_NEEDLE_BYTES * 4) + (CMD_STREAM_MAX_FINDR * 8),
    CMD_STREAM_MAX_RE_INS_SINGLE = (CMD_STREAM_MAX_NEEDLE_BYTES * 4) + 8,
    CMD_STREAM_MAX_RE_CLASSES = CMD_STREAM_MAX_NEEDLE_BYTES
};

static void sleep_msec(int msec)
{
    if (msec <= 0)
        return;
#ifdef _WIN32
    Sleep((DWORD)msec);
#else
    struct timespec req;
    req.tv_sec = msec / 1000;
    req.tv_nsec = (long)(msec % 1000) * 1000000L;
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
    }
#endif
}

static uint64_t now_millis(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

static void refresh_file_size(File* io)
{
    if (!io || !io->f)
        return;
    if (fseeko(io->f, 0, SEEK_END) == 0) {
        off_t pos = ftello(io->f);
        if (pos >= 0)
            io->size = (i64)pos;
        if (fseeko(io->f, 0, SEEK_SET) != 0) {
            clearerr(io->f);
        }
    } else {
        clearerr(io->f);
    }
}

static int parse_nonneg_option(const char* value, const char* opt_name, int* out)
{
    if (!value || !opt_name || !out)
        return 1;

    char* end = NULL;
    long v = strtol(value, &end, 10);
    if (value[0] == '\0' || (end && *end != '\0') || v < 0 || v > INT_MAX) {
        fprintf(stderr, "fiskta: %s expects a non-negative integer (milliseconds)\n", opt_name);
        return 1;
    }
    *out = (int)v;
    return 0;
}

static int parse_window_policy_option(const char* value, WindowPolicy* out)
{
    if (!value || !out)
        return 1;
    if (strcmp(value, "delta") == 0) {
        *out = WINDOW_POLICY_DELTA;
        return 0;
    }
    if (strcmp(value, "rescan") == 0) {
        *out = WINDOW_POLICY_RESCAN;
        return 0;
    }
    if (strcmp(value, "cursor") == 0) {
        *out = WINDOW_POLICY_CURSOR;
        return 0;
    }
    fprintf(stderr, "fiskta: --window-policy expects one of: delta, rescan, cursor\n");
    return 1;
}

static int parse_idle_timeout_option(const char* value, int* out)
{
    if (!value || !out)
        return 1;
    if (strcmp(value, "none") == 0 || strcmp(value, "off") == 0 || strcmp(value, "-1") == 0) {
        *out = -1;
        return 0;
    }
    return parse_nonneg_option(value, "--idle-timeout", out);
}

static int run_program_once(const Program* prg, File* io, VM* vm,
    Range* clause_ranges, LabelWrite* clause_labels,
    enum Err* last_err_out,
    i64 window_lo, i64 window_hi)
{
    if (last_err_out)
        *last_err_out = E_OK;

    io_reset_full(io);

    VM local_vm;
    VM* vm_exec = vm ? vm : &local_vm;
    if (!vm) {
        memset(vm_exec, 0, sizeof(*vm_exec));
        for (i32 i = 0; i < 128; i++)
            vm_exec->label_pos[i] = -1;
    }

    i64 lo = clamp64(window_lo, 0, io_size(io));
    i64 hi = clamp64(window_hi, 0, io_size(io));
    if (vm_exec->cursor < lo)
        vm_exec->cursor = lo;
    if (vm_exec->cursor > hi)
        vm_exec->cursor = hi;
    vm_exec->view.active = true;
    vm_exec->view.lo = lo;
    vm_exec->view.hi = hi;
    vm_exec->last_match.valid = false;

    i32 ok = 0;
    enum Err last_err = E_OK;
    i32 last_failed_clause = -1;
    StagedResult result;
    bool and_chain_failed = false;
    i32 and_chain_failed_at = -1;
    bool clauses_succeeded_after_and_failure = false;

    for (i32 ci = 0; ci < prg->clause_count; ++ci) {
        i32 rc = 0, lc = 0;
        clause_caps(&prg->clauses[ci], &rc, &lc);
        Range* r_tmp = (rc > 0) ? clause_ranges : NULL;
        LabelWrite* lw_tmp = (lc > 0) ? clause_labels : NULL;

        enum Err e = execute_clause_stage_only(&prg->clauses[ci], io, vm_exec,
            r_tmp, rc, lw_tmp, lc, &result);
        if (e == E_OK) {
            // Commit staged ranges to output
            for (i32 i = 0; i < result.range_count; i++) {
                if (result.ranges[i].kind == RANGE_FILE) {
                    e = io_emit(io, result.ranges[i].file.start, result.ranges[i].file.end, stdout);
                } else {
                    // RANGE_LIT: write literal bytes
                    if ((size_t)fwrite(result.ranges[i].lit.bytes, 1, (size_t)result.ranges[i].lit.len, stdout) != (size_t)result.ranges[i].lit.len)
                        e = E_IO;
                }
                if (e != E_OK)
                    break;
            }
            if (e == E_OK) {
                // Commit staged VM state
                commit_labels(vm_exec, result.label_writes, result.label_count);
                vm_exec->cursor = result.staged_vm.cursor;
                vm_exec->last_match = result.staged_vm.last_match;
                vm_exec->view = result.staged_vm.view;
            }
        }

        // Handle links - the clause's link tells us what to do next
        if (e == E_OK) {
            ok++;
            if (and_chain_failed) {
                clauses_succeeded_after_and_failure = true;
            }
            // Success: check this clause's link
            if (prg->clauses[ci].link == LINK_OR) {
                // This clause succeeded and links with OR
                // Skip all remaining clauses in the OR chain
                while (ci + 1 < prg->clause_count && prg->clauses[ci].link == LINK_OR) {
                    ci++;  // Skip the OR alternative
                }
            }
            // For LINK_AND, LINK_THEN, or LINK_NONE: just continue to next clause
        } else {
            last_err = e;
            last_failed_clause = ci;

            bool drop_and_chain = false;
            if (ci > 0 && prg->clauses[ci - 1].link == LINK_AND)
                drop_and_chain = true;
            if (prg->clauses[ci].link == LINK_AND)
                drop_and_chain = true;

            if (drop_and_chain) {
                and_chain_failed = true;
                if (and_chain_failed_at == -1)
                    and_chain_failed_at = ci;
                while (ci + 1 < prg->clause_count && prg->clauses[ci].link == LINK_AND)
                    ci++;
                continue;
            }
            // For LINK_OR, LINK_THEN, or LINK_NONE: continue to next clause
        }
    }

    if (last_err_out)
        *last_err_out = last_err;

    // Determine return value for exit code calculation:
    // - Positive: number of successful clauses
    // - -1: No clauses succeeded (return last failed clause index)
    // - -2 - N: AND chain failed at clause (N + 2)
    if (and_chain_failed && !clauses_succeeded_after_and_failure) {
        return -2 - and_chain_failed_at;  // AND chain failure
    } else if (ok == 0 && last_failed_clause >= 0) {
        return -2 - last_failed_clause;  // All clauses failed
    } else {
        return ok;  // Success (at least one clause succeeded)
    }
}

static void print_usage(void)
{
    printf("fiskta (FInd SKip TAke) Text Extraction Tool v%s\n", FISKTA_VERSION);
    printf("\n");
    printf("USAGE:\n");
    printf("  fiskta [options] <operations>\n");
    printf("  (use --input <path> to select input; defaults to stdin)\n");
    printf("\n");
    printf("EXAMPLES:\n");
    printf("  Take first 10 bytes of a file:\n");
    printf("    fiskta --input file.txt take 10b\n");
    printf("\n");
    printf("  Take lines 2-4 from a file:\n");
    printf("    fiskta --input file.txt skip 1l take 3l\n");
    printf("\n");
    printf("  Take from the first STATUS to EOF:\n");
    printf("    fiskta --input file.txt find \"STATUS\" take to EOF\n");
    printf("\n");
    printf("  Take five lines around the first WARN:\n");
    printf("    fiskta --input logs.txt findr \"^WARN\" take -2l take 3l\n");
    printf("\n");
    printf("  Take until the start of the END section:\n");
    printf("    fiskta --input config.txt take until \"END\" at line-start\n");
    printf("\n");
    printf("  Take from section1 up to section2:\n");
    printf("    fiskta --input file.txt find section1 label s1 find section2 take to section1\n");
    printf("\n");
    printf("  Process stdin:\n");
    printf("    echo \"Hello\" | fiskta take 5b\n");
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
    printf("  sleep <duration>            Pause execution; duration suffix ms or s (e.g., 500ms, 1s)\n");
    printf("  print <string>              Emit literal bytes (alias: echo)\n");
    printf("                              Supports escape sequences: \\n \\t \\r \\0 \\\\ \\xHH\n");
    printf("                              Participates in clause atomicity\n");
    printf("\n");
    printf("UNITS:\n");
    printf("  b                           Bytes\n");
    printf("  l                           Lines (LF only, CR treated as bytes)\n");
    printf("  c                           UTF-8 code points (never splits sequences)\n");
    printf("\n");
    printf("LABELS:\n");
    printf("  NAME                        UPPERCASE, <16 chars, [A-Z0-9_-] (first must be A-Z)\n");
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
    printf("REGEX SYNTAX:\n");
    printf("  Character Classes: \\d (digits), \\w (word), \\s (space), [a-z], [^0-9]\n");
    printf("  Quantifiers: * (0+), + (1+), ? (0-1), {n} (exactly n), {n,m} (n to m)\n");
    printf("  Grouping: ( ... ) (group subpatterns), (a|b)+ (quantified groups)\n");
    printf("  Anchors: ^ (line start), $ (line end)\n");
    printf("  Alternation: | (OR)\n");
    printf("  Escape: \\n, \\t, \\r, \\f, \\v, \\0\n");
    printf("  Special: . (any char except newline)\n");
    printf("\n");
    printf("CLAUSES AND LOGICAL OPERATORS:\n");
    printf("  Operations are grouped into clauses connected by logical operators:\n");
    printf("    THEN    Sequential execution (always runs next clause)\n");
    printf("    AND     Both clauses must succeed (short-circuits on failure)\n");
    printf("    OR      First success wins (short-circuits on success)\n");
    printf("\n");
    printf("  Within a clause: all ops must succeed or the clause fails atomically.\n");
    printf("  On Failure: clause rolls back (no output or label changes).\n");
    printf("  On Success: emits staged output, commits labels, updates cursor and last-match.\n");
    printf("\n");
    printf("  Evaluation is strictly left-to-right with these rules:\n");
    printf("    1. THEN always executes the next clause (regardless of previous result)\n");
    printf("    2. AND creates a chain - if any clause in an AND chain fails, skip\n");
    printf("       remaining clauses in that chain and continue after it\n");
    printf("    3. OR short-circuits - if a clause succeeds, skip all remaining OR\n");
    printf("       alternatives in that chain\n");
    printf("    4. THEN acts as a \"chain breaker\" - clauses after THEN always run,\n");
    printf("       even if previous AND chains failed\n");
    printf("\n");
    printf("  Examples:\n");
    printf("    find abc THEN take +3b               # Sequential: always do both\n");
    printf("    take 10b AND find xyz AND take 5b    # Partial output: gets first 10b even if xyz not found\n");
    printf("    find abc OR find xyz                 # Alternative: try abc, or try xyz\n");
    printf("    take 100b THEN skip 1b               # Mixed: skip runs even if take fails\n");
    printf("\n");
    printf("EXIT CODES:\n");
    printf("  0               Success (at least one clause succeeded)\n");
    printf("  1               I/O error (file not found, permission denied, etc.)\n");
    printf("  2               Parse error (invalid syntax, unknown operation)\n");
    printf("  3               Regex error (invalid regex pattern)\n");
    printf("  4               Resource limit (program too large, out of memory)\n");
    printf("  10+             Clause N failed (exit code = 10 + clause index)\n");
    printf("\n");
    printf("  Example: exit code 11 means clause 1 (second clause) failed\n");
    printf("  Scripts can extract the failing clause: clause_index=$((exit_code - 10))\n");
    printf("\n");
    printf("OPTIONS:\n");
    printf("  -i, --input <path>          Read input from path (default: stdin)\n");
    printf("  -c, --commands <string|-|file> Provide operations as a single string ( '-' or file path allowed)\n");
    printf("      --                      Treat subsequent arguments as operations\n");
    printf("      --loop <ms>             Re-run the program every ms (0 disables looping)\n");
    printf("      --idle-timeout <ms>     Stop looping after ms with no input growth\n");
    printf("      --window-policy <p>     Loop window policy: delta | rescan | cursor (default: cursor)\n");
    printf("  -h, --help                  Show this help message\n");
    printf("  -v, --version               Show version information\n");
    printf("\n");
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    const char* input_path = "-";
    const char* command_arg = NULL;
    const char* command_file = NULL;
    bool input_explicit = false;
    bool commands_from_stdin = false;
    int loop_ms = 0;
    int idle_timeout_ms = -1;
    WindowPolicy window_policy = WINDOW_POLICY_CURSOR;

    int argi = 1;
    while (argi < argc) {
        const char* arg = argv[argi];
        if (strcmp(arg, "--") == 0) {
            argi++;
            break;
        }
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage();
            return 0;
        }
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--version") == 0) {
            printf("fiskta (FInd SKip TAke) v%s\n", FISKTA_VERSION);
            return 0;
        }
        if (strcmp(arg, "-i") == 0 || strcmp(arg, "--input") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --input requires a path\n");
                return 2;
            }
            input_path = argv[argi + 1];
            input_explicit = true;
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--input=", 8) == 0) {
            if (arg[8] == '\0') {
                fprintf(stderr, "fiskta: --input requires a path\n");
                return 2;
            }
            input_path = arg + 8;
            input_explicit = true;
            argi++;
            continue;
        }
        if (strcmp(arg, "--loop") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --loop requires a value\n");
                return 2;
            }
            if (parse_nonneg_option(argv[argi + 1], "--loop", &loop_ms) != 0)
                return 2;
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--loop=", 7) == 0) {
            if (parse_nonneg_option(arg + 7, "--loop", &loop_ms) != 0)
                return 2;
            argi++;
            continue;
        }
        if (strcmp(arg, "--idle-timeout") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --idle-timeout requires a value\n");
                return 2;
            }
            if (parse_idle_timeout_option(argv[argi + 1], &idle_timeout_ms) != 0)
                return 2;
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--idle-timeout=", 15) == 0) {
            if (parse_idle_timeout_option(arg + 15, &idle_timeout_ms) != 0)
                return 2;
            argi++;
            continue;
        }
        if (strcmp(arg, "--window-policy") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --window-policy requires a value\n");
                return 2;
            }
            if (parse_window_policy_option(argv[argi + 1], &window_policy) != 0)
                return 2;
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--window-policy=", 16) == 0) {
            if (parse_window_policy_option(arg + 16, &window_policy) != 0)
                return 2;
            argi++;
            continue;
        }
        if (strcmp(arg, "-c") == 0 || strcmp(arg, "--commands") == 0) {
            if (command_arg || command_file || commands_from_stdin) {
                fprintf(stderr, "fiskta: --commands specified multiple times\n");
                return 2;
            }
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --commands requires a string\n");
                return 2;
            }
            const char* value = argv[argi + 1];
            if (strcmp(value, "-") == 0) {
                commands_from_stdin = true;
            } else {
                FILE* test = fopen(value, "rb");
                if (test) {
                    fclose(test);
                    command_file = value;
                } else {
                    command_arg = value;
                }
            }
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--commands=", 11) == 0) {
            if (command_arg || command_file || commands_from_stdin) {
                fprintf(stderr, "fiskta: --commands specified multiple times\n");
                return 2;
            }
            if (arg[11] == '\0') {
                fprintf(stderr, "fiskta: --commands requires a string\n");
                return 2;
            }
            const char* value = arg + 11;
            if (strcmp(value, "-") == 0) {
                commands_from_stdin = true;
            } else {
                FILE* test = fopen(value, "rb");
                if (test) {
                    fclose(test);
                    command_file = value;
                } else {
                    command_arg = value;
                }
            }
            argi++;
            continue;
        }
        if (arg[0] == '-') {
            if (arg[1] == '\0' || isdigit((unsigned char)arg[1])) {
                break;
            }
            fprintf(stderr, "fiskta: unknown option %s\n", arg);
            return 2;
        }
        break;
    }

    int ops_index = argi;
    char** tokens = NULL;
    i32 token_count = 0;
    char* splitv[CMD_STREAM_MAX_TOKENS];

    char stdin_commands_buf[CMD_STREAM_BUF_CAP];

    bool streaming_mode = commands_from_stdin;

    if (streaming_mode) {
        if (ops_index < argc) {
            fprintf(stderr, "fiskta: --commands cannot be combined with positional operations\n");
            return 2;
        }
        if (!input_explicit || strcmp(input_path, "-") == 0) {
            fprintf(stderr, "fiskta: -c - requires --input pointing to a file\n");
            return 2;
        }
        if (loop_ms > 0) {
            fprintf(stderr, "fiskta: --loop is not supported with -c - command streams\n");
            return 2;
        }
        if (idle_timeout_ms >= 0) {
            fprintf(stderr, "fiskta: --idle-timeout is not supported with -c - command streams\n");
            return 2;
        }
        if (window_policy != WINDOW_POLICY_CURSOR) {
            fprintf(stderr, "fiskta: --window-policy cannot be combined with -c - command streams\n");
            return 2;
        }
    } else if (command_file) {
        if (ops_index < argc) {
            fprintf(stderr, "fiskta: --commands cannot be combined with positional operations\n");
            return 2;
        }
        FILE* cf = fopen(command_file, "rb");
        if (!cf) {
            fprintf(stderr, "fiskta: unable to open commands file %s\n", command_file);
            return 2;
        }
        size_t total = fread(stdin_commands_buf, 1, sizeof(stdin_commands_buf) - 1, cf);
        if (ferror(cf)) {
            fclose(cf);
            fprintf(stderr, "fiskta: error reading commands file %s\n", command_file);
            return 2;
        }
        if (!feof(cf)) {
            fclose(cf);
            fprintf(stderr, "fiskta: operations file too long (max 4096 bytes)\n");
            return 2;
        }
        fclose(cf);
        stdin_commands_buf[total] = '\0';
        if (total == 0) {
            fprintf(stderr, "fiskta: empty commands file\n");
            return 2;
        }
        for (size_t i = 0; i < total; ++i) {
            if (stdin_commands_buf[i] == '\n' || stdin_commands_buf[i] == '\r')
                stdin_commands_buf[i] = ' ';
        }
        i32 n = split_ops_string(stdin_commands_buf, splitv, (i32)(sizeof splitv / sizeof splitv[0]));
        if (n == -1) {
            fprintf(stderr, "fiskta: operations string too long (max 4096 bytes)\n");
            return 2;
        }
        if (n <= 0) {
            fprintf(stderr, "fiskta: empty command string\n");
            return 2;
        }
        tokens = splitv;
        token_count = n;
    } else if (command_arg) {
        if (ops_index < argc) {
            fprintf(stderr, "fiskta: --commands cannot be combined with positional operations\n");
            return 2;
        }
        i32 n = split_ops_string(command_arg, splitv, (i32)(sizeof splitv / sizeof splitv[0]));
        if (n == -1) {
            fprintf(stderr, "fiskta: operations string too long (max 4096 bytes)\n");
            return 2;
        }
        if (n <= 0) {
            fprintf(stderr, "fiskta: empty command string\n");
            return 2;
        }
        tokens = splitv;
        token_count = n;
    } else {
        token_count = (i32)(argc - ops_index);
        if (token_count <= 0) {
            fprintf(stderr, "fiskta: missing operations\n");
            fprintf(stderr, "Try 'fiskta --help' for more information.\n");
            return 2;
        }
        tokens = argv + ops_index;
        if (token_count == 1 && strchr(tokens[0], ' ')) {
            i32 n = split_ops_string(tokens[0], splitv, (i32)(sizeof splitv / sizeof splitv[0]));
            if (n == -1) {
                fprintf(stderr, "fiskta: operations string too long (max 4096 bytes)\n");
                return 2;
            }
            if (n <= 0) {
                fprintf(stderr, "fiskta: empty operations string\n");
                return 2;
            }
            tokens = splitv;
            token_count = n;
        }
    }

    // 1) Preflight or establish streaming bounds
    ParsePlan plan = { 0 };
    const char* path = NULL;

    enum Err e = E_OK;
    if (!streaming_mode) {
        e = parse_preflight(token_count, tokens, input_path, &plan, &path);
        if (e != E_OK) {
            print_err(e, "parse preflight");
            return 2;  // Exit code 2: Parse error
        }
    } else {
        path = input_path;
        plan.total_ops = CMD_STREAM_MAX_OPS;
        plan.clause_count = CMD_STREAM_MAX_CLAUSES;
        plan.needle_bytes = CMD_STREAM_MAX_NEEDLE_BYTES;
        plan.sum_take_ops = CMD_STREAM_MAX_TAKE;
        plan.sum_label_ops = CMD_STREAM_MAX_LABEL;
        plan.sum_findr_ops = CMD_STREAM_MAX_FINDR;
        plan.re_ins_estimate = CMD_STREAM_MAX_RE_INS_TOTAL;
        plan.re_ins_estimate_max = CMD_STREAM_MAX_RE_INS_SINGLE;
        plan.re_classes_estimate = CMD_STREAM_MAX_RE_CLASSES;
    }

    // 2) Compute sizes
    const size_t search_buf_cap = (FW_WIN > (BK_BLK + OVERLAP_MAX)) ? (size_t)FW_WIN : (size_t)(BK_BLK + OVERLAP_MAX);
    const size_t ops_bytes = (size_t)plan.total_ops * sizeof(Op);
    const size_t clauses_bytes = (size_t)plan.clause_count * sizeof(Clause);
    const size_t str_pool_bytes = plan.needle_bytes;

    const size_t re_prog_bytes = (size_t)plan.sum_findr_ops * sizeof(ReProg);
    const size_t re_ins_bytes = (size_t)plan.re_ins_estimate * sizeof(ReInst);
    const size_t re_cls_bytes = (size_t)plan.re_classes_estimate * sizeof(ReClass);

    // Choose per-run thread capacity as ~4x max nins (like old logic), min 32.
    int re_threads_cap = plan.re_ins_estimate_max > 0 ? 4 * plan.re_ins_estimate_max : 32;
    if (re_threads_cap < 32)
        re_threads_cap = 32;
    const size_t re_threads_bytes = (size_t)re_threads_cap * sizeof(ReThread);

    // 3) Allocation
    size_t search_buf_size = align_or_die(search_buf_cap, alignof(unsigned char));
    size_t clauses_size = align_or_die(clauses_bytes, alignof(Clause));
    size_t ops_size = align_or_die(ops_bytes, alignof(Op));
    size_t re_prog_size = align_or_die(re_prog_bytes, alignof(ReProg));
    size_t re_ins_size = align_or_die(re_ins_bytes, alignof(ReInst));
    size_t re_cls_size = align_or_die(re_cls_bytes, alignof(ReClass));
    size_t str_pool_size = align_or_die(str_pool_bytes, alignof(char));
    // Two thread buffers + two seen arrays sized to max estimated nins
    size_t re_seen_bytes_each = (size_t)(plan.re_ins_estimate_max > 0 ? plan.re_ins_estimate_max : 32);
    size_t re_seen_size = safe_align(re_seen_bytes_each, 1) * 2;
    size_t re_thrbufs_size = align_or_die(re_threads_bytes, alignof(ReThread)) * 2;

    size_t ranges_bytes = (plan.sum_take_ops > 0) ? align_or_die((size_t)plan.sum_take_ops * sizeof(Range), alignof(Range)) : 0;
    size_t labels_bytes = (plan.sum_label_ops > 0) ? align_or_die((size_t)plan.sum_label_ops * sizeof(LabelWrite), alignof(LabelWrite)) : 0;

    size_t total = search_buf_size;
    if (add_ovf(total, clauses_size, &total) ||
        add_ovf(total, ops_size, &total) ||
        add_ovf(total, re_prog_size, &total) ||
        add_ovf(total, re_ins_size, &total) ||
        add_ovf(total, re_cls_size, &total) ||
        add_ovf(total, str_pool_size, &total) ||
        add_ovf(total, re_thrbufs_size, &total) ||
        add_ovf(total, re_seen_size, &total) ||
        add_ovf(total, ranges_bytes, &total) ||
        add_ovf(total, labels_bytes, &total) ||
        add_ovf(total, 64, &total)) { // 3 small cushion
        print_err(E_OOM, "arena size overflow");
        return 4;  // Exit code 4: Resource limit
    }

    void* block = malloc(total);
    if (!block) {
        print_err(E_OOM, "arena alloc");
        return 4;  // Exit code 4: Resource limit
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
    ReInst* re_ins = arena_alloc(&A, re_ins_bytes, alignof(ReInst));
    ReClass* re_cls = arena_alloc(&A, re_cls_bytes, alignof(ReClass));
    char* str_pool = arena_alloc(&A, str_pool_bytes, alignof(char));
    Range* clause_ranges = (plan.sum_take_ops > 0) ? arena_alloc(&A, (size_t)plan.sum_take_ops * sizeof(Range), alignof(Range)) : NULL;
    LabelWrite* clause_labels = (plan.sum_label_ops > 0) ? arena_alloc(&A, (size_t)plan.sum_label_ops * sizeof(LabelWrite), alignof(LabelWrite)) : NULL;

    if (!search_buf || !clauses_buf || !ops_buf
        || !re_curr_thr || !re_next_thr || !seen_curr || !seen_next
        || !re_progs || !re_ins || !re_cls || !str_pool
        || (plan.sum_take_ops > 0 && !clause_ranges)
        || (plan.sum_label_ops > 0 && !clause_labels)) {
        print_err(E_OOM, "arena carve");
        free(block);
        return 4;  // Exit code 4: Resource limit
    }

    // 5) Prepare program storage; parse immediately for one-shot mode
    Program prg = { 0 };
    if (!streaming_mode) {
        e = parse_build(token_count, tokens, input_path, &prg, &path, clauses_buf, ops_buf,
            str_pool, str_pool_bytes);
        if (e != E_OK) {
            print_err(e, "parse build");
            free(block);
            return 2;  // Exit code 2: Parse error
        }

        // Compile regex programs once
        i32 re_prog_idx = 0, re_ins_idx = 0, re_cls_idx = 0;
        for (i32 ci = 0; ci < prg.clause_count; ++ci) {
            Clause* clause = &prg.clauses[ci];
            for (i32 i = 0; i < clause->op_count; ++i) {
                Op* op = &clause->ops[i];
                if (op->kind == OP_FINDR) {
                    ReProg* prog = &re_progs[re_prog_idx++];
                    enum Err err = re_compile_into(op->u.findr.pattern, prog,
                        re_ins + re_ins_idx, (i32)(re_ins_bytes / sizeof(ReInst)) - re_ins_idx, &re_ins_idx,
                        re_cls + re_cls_idx, (i32)(re_cls_bytes / sizeof(ReClass)) - re_cls_idx, &re_cls_idx);
                    if (err != E_OK) {
                        // Regex compilation error
                        if (err == E_PARSE || err == E_BAD_NEEDLE) {
                            print_err(err, "regex compile");
                            free(block);
                            return 3;  // Exit code 3: Regex error
                        } else {
                            // Resource error (E_OOM)
                            print_err(err, "regex compile");
                            free(block);
                            return 4;  // Exit code 4: Resource limit
                        }
                    }
                    op->u.findr.prog = prog;
                }
            }
        }
    }

    // 6) Open I/O with arena-backed buffers
    File io = { 0 };
    e = io_open(&io, path, search_buf, search_buf_cap);
    if (e != E_OK) {
        free(block);
        print_err(e, "I/O open");
        return 1;  // Exit code 1: I/O error
    }

    io_set_regex_scratch(&io, re_curr_thr, re_next_thr, re_threads_cap,
        seen_curr, seen_next, (size_t)re_seen_bytes_each);

    // 7) Execute programs using precomputed scratch buffers
    int exit_code = 0;

    if (!streaming_mode) {
        refresh_file_size(&io);
        i64 window_start = 0;
        i64 last_size = io_size(&io);
        uint64_t last_change_ms = now_millis();
        VM saved_vm;
        memset(&saved_vm, 0, sizeof(saved_vm));
        for (i32 i = 0; i < 128; i++)
            saved_vm.label_pos[i] = -1;
        bool have_saved_vm = false;
        const int loop_enabled = (loop_ms > 0);
        if (!loop_enabled)
            idle_timeout_ms = -1;

        for (;;) {
            refresh_file_size(&io);
            i64 window_end = io_size(&io);
            uint64_t now_ms = now_millis();
            if (window_end != last_size) {
                last_size = window_end;
                last_change_ms = now_ms;
            }
            if (window_start > window_end)
                window_start = window_end;

            i64 effective_start = window_start;
            if (window_policy == WINDOW_POLICY_RESCAN)
                effective_start = 0;
            else if (window_policy == WINDOW_POLICY_CURSOR && have_saved_vm)
                effective_start = clamp64(saved_vm.cursor, 0, window_end);

            if (loop_enabled && window_policy == WINDOW_POLICY_DELTA && effective_start >= window_end) {
                if (idle_timeout_ms >= 0 && (now_ms - last_change_ms) >= (uint64_t)idle_timeout_ms)
                    break;
                sleep_msec(loop_ms);
                continue;
            }

            enum Err last_err = E_OK;
            i64 prev_cursor = have_saved_vm ? saved_vm.cursor : effective_start;
            VM* vm_ptr = (window_policy == WINDOW_POLICY_CURSOR) ? &saved_vm : NULL;
            int ok = run_program_once(&prg, &io, vm_ptr, clause_ranges, clause_labels, &last_err,
                effective_start, window_end);

            // Handle exit codes
            if (ok > 0) {
                // Success - at least one clause succeeded
            } else if (ok == 0) {
                // I/O error during execution
                fprintf(stderr, "fiskta: I/O error (%s)\n", err_str(last_err));
                exit_code = 1;
                goto cleanup;
            } else {
                // ok is (-2 - clause_index), extract the clause index
                i32 failed_clause = (-ok) - 2;
                exit_code = 10 + failed_clause;
                goto cleanup;
            }

            fflush(stdout);

            if (!loop_enabled)
                break;

            if (window_policy == WINDOW_POLICY_CURSOR) {
                have_saved_vm = true;
                window_start = clamp64(saved_vm.cursor, 0, window_end);
                if (saved_vm.cursor != prev_cursor)
                    last_change_ms = now_ms;
            } else if (window_policy == WINDOW_POLICY_DELTA) {
                window_start = window_end;
            } else {
                window_start = 0;
            }

            now_ms = now_millis();
            if (idle_timeout_ms >= 0 && (now_ms - last_change_ms) >= (uint64_t)idle_timeout_ms)
                break;

            sleep_msec(loop_ms);
        }
    } else {
        for (;;) {
            int len = read_command_stream_line(stdin_commands_buf, sizeof(stdin_commands_buf));
            if (len == -1)
                break; // EOF, normal exit
            if (len == -2) {
                fprintf(stderr, "fiskta: operations string too long (max %d bytes)\n", CMD_STREAM_BUF_CAP);
                exit_code = 4;  // Exit code 4: Resource limit
                goto cleanup;
            }
            if (len == 0) {
                fprintf(stderr, "fiskta: empty command string\n");
                continue;
            }

            i32 n = split_ops_string(stdin_commands_buf, splitv, (i32)(sizeof splitv / sizeof splitv[0]));
            if (n == -1 || n <= 0) {
                fprintf(stderr, "fiskta: command stream token limit exceeded (max %d tokens)\n", CMD_STREAM_MAX_TOKENS);
                continue;
            }

            ParsePlan line_plan = { 0 };
            const char* dummy_path = NULL;
            enum Err perr = parse_preflight(n, splitv, input_path, &line_plan, &dummy_path);
            if (perr != E_OK) {
                fprintf(stderr, "fiskta: command stream parse error (%s)\n", err_str(perr));
                continue;
            }

            if (line_plan.total_ops > CMD_STREAM_MAX_OPS ||
                line_plan.clause_count > CMD_STREAM_MAX_CLAUSES ||
                line_plan.needle_bytes > CMD_STREAM_MAX_NEEDLE_BYTES ||
                line_plan.sum_take_ops > CMD_STREAM_MAX_TAKE ||
                line_plan.sum_label_ops > CMD_STREAM_MAX_LABEL ||
                line_plan.sum_findr_ops > CMD_STREAM_MAX_FINDR ||
                line_plan.re_ins_estimate > CMD_STREAM_MAX_RE_INS_TOTAL ||
                line_plan.re_ins_estimate_max > CMD_STREAM_MAX_RE_INS_SINGLE ||
                line_plan.re_classes_estimate > CMD_STREAM_MAX_RE_CLASSES) {
                fprintf(stderr, "fiskta: command stream exceeds built-in limits\n");
                continue;
            }

            perr = parse_build(n, splitv, input_path, &prg, &dummy_path, clauses_buf, ops_buf,
                str_pool, str_pool_bytes);
            if (perr != E_OK) {
                fprintf(stderr, "fiskta: command stream parse build error (%s)\n", err_str(perr));
                continue;
            }

            i32 re_prog_idx = 0, re_ins_idx = 0, re_cls_idx = 0;
            bool regex_ok = true;
            for (i32 ci = 0; ci < prg.clause_count && regex_ok; ++ci) {
                Clause* clause = &prg.clauses[ci];
                for (i32 i = 0; i < clause->op_count; ++i) {
                    Op* op = &clause->ops[i];
                    if (op->kind == OP_FINDR) {
                        ReProg* prog = &re_progs[re_prog_idx++];
                        enum Err err = re_compile_into(op->u.findr.pattern, prog,
                            re_ins + re_ins_idx, (i32)(re_ins_bytes / sizeof(ReInst)) - re_ins_idx, &re_ins_idx,
                            re_cls + re_cls_idx, (i32)(re_cls_bytes / sizeof(ReClass)) - re_cls_idx, &re_cls_idx);
                        if (err != E_OK) {
                            fprintf(stderr, "fiskta: regex compile error (%s)\n", err_str(err));
                            regex_ok = false;
                            break;
                        }
                        op->u.findr.prog = prog;
                    }
                }
            }
            if (!regex_ok)
                continue;

            refresh_file_size(&io);
            enum Err last_err = E_OK;
            int ok = run_program_once(&prg, &io, NULL, clause_ranges, clause_labels, &last_err,
                0, io_size(&io));

            // Handle exit codes (for streaming mode, only I/O errors terminate)
            if (ok > 0) {
                // Success - at least one clause succeeded
            } else if (ok == 0) {
                // I/O error during execution
                fprintf(stderr, "fiskta: command stream I/O error (%s)\n", err_str(last_err));
                exit_code = 1;
                goto cleanup;
            } else {
                // Execution failure - in streaming mode we just continue
                // (clauses can fail without terminating the stream)
            }

            fflush(stdout);
        }
    }

cleanup:
    io_close(&io);
    free(block);
    return exit_code;
}

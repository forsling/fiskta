#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "fiskta.h"
#include "iosearch.h"
#include "parse_plan.h"
#include "reprog.h"
#include "util.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#ifndef FISKTA_VERSION
#define FISKTA_VERSION "dev"
#endif

// Quote-aware ops-string splitter for CLI usage
// Note: one-shot scratch; tokens invalidated after next call
static i32 split_ops_string(const char* s, char** out, i32 max_tokens)
{
    static char buf[4096];
    size_t boff = 0;
    i32 ntok = 0;

    enum { S_WS,
        S_TOKEN,
        S_SQ,
        S_DQ } st
        = S_WS;
    const char* p = s;

    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (st == S_WS) {
            if (c == ' ' || c == '\t') {
                p++;
                continue;
            }
            if (c == '\'' || c == '"') {
                if (ntok >= max_tokens)
                    return -1; // NEW
                if (boff >= sizeof buf)
                    return -1; // NEW
                out[ntok] = &buf[boff];
                st = (c == '\'') ? S_SQ : S_DQ;
                p++;
                continue;
            }
            // start token, reprocess this char in S_TOKEN
            if (ntok >= max_tokens)
                return -1; // NEW
            if (boff >= sizeof buf)
                return -1; // NEW
            out[ntok] = &buf[boff];
            st = S_TOKEN;
            continue;
        } else if (st == S_TOKEN) {
            if (c == ' ' || c == '\t') {
                if (boff >= sizeof buf)
                    return -1; // NUL safety
                buf[boff++] = '\0';
                ntok++;
                st = S_WS;
                p++;
                continue;
            }
            if (c == '\'') {
                st = S_SQ;
                p++;
                continue;
            }
            if (c == '"') {
                st = S_DQ;
                p++;
                continue;
            }
            if (c == '\\' && p[1]) {
                unsigned char next = (unsigned char)p[1];
                if (next == ' ' || next == '\t' || next == '\\' || next == '\'' || next == '"') {
                    if (boff >= sizeof buf)
                        return -1;
                    buf[boff++] = (char)next;
                    p += 2;
                    continue;
                }
            }
            if (boff >= sizeof buf)
                return -1;
            buf[boff++] = (char)c;
            p++;
            continue;
        } else if (st == S_SQ) {
            if (c == '\'') {
                st = S_TOKEN;
                p++;
                continue;
            }
            if (boff >= sizeof buf)
                return -1;
            buf[boff++] = (char)c;
            p++;
            continue;
        } else { // S_DQ
            if (c == '"') {
                st = S_TOKEN;
                p++;
                continue;
            }
            if (c == '\\' && p[1]) {
                unsigned char esc = (unsigned char)p[1];
                if (esc == '"' || esc == '\\') {
                    if (boff >= sizeof buf)
                        return -1;
                    buf[boff++] = (char)esc;
                    p += 2;
                    continue;
                }
            }
            if (boff >= sizeof buf)
                return -1;
            buf[boff++] = (char)c;
            p++;
            continue;
        }
    }

    if (st == S_TOKEN || st == S_SQ || st == S_DQ) {
        if (boff >= sizeof buf)
            return -1; // NEW
        buf[boff++] = '\0';
        if (ntok < max_tokens)
            ntok++;
        else
            return -1; // NEW
    }
    return ntok;
}

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

enum Err engine_run(const Program*, const char*, FILE*);
enum Err parse_preflight(i32 token_count, const String* tokens, const char* in_path, ParsePlan* plan, const char** in_path_out);
enum Err parse_build(i32 token_count, const String* tokens, const char* in_path, Program* prg, const char** in_path_out,
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
    case E_BAD_HEX:
        return "invalid hex string";
    case E_LOC_RESOLVE:
        return "location not resolvable";
    case E_NO_MATCH:
        return "no match in window";
    case E_FAIL_OP:
        return "fail operation";
    case E_LABEL_FMT:
        return "bad label (A-Z0-9_-; first A-Z; <16)";
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
    LOOP_VIEW_DELTA,
    LOOP_VIEW_RESCAN,
    LOOP_VIEW_CURSOR
} LoopViewPolicy;

enum {
    MAX_TOKENS = 1024,
    MAX_OPS = 2048,
    MAX_CLAUSES = 1024,
    MAX_NEEDLE_BYTES = 4096,
    MAX_FINDR = MAX_OPS,
    MAX_TAKE = MAX_OPS,
    MAX_LABEL = MAX_OPS,
    MAX_RE_INS_TOTAL = (MAX_NEEDLE_BYTES * 4) + (MAX_FINDR * 8),
    MAX_RE_INS_SINGLE = (MAX_NEEDLE_BYTES * 4) + 8,
    MAX_RE_CLASSES = MAX_NEEDLE_BYTES
};

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

#include <sys/stat.h>

static void refresh_file_size(File* io)
{
    if (!io || !io->f)
        return;

    int fd = fileno(io->f);
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        // regular file: we can ask for size without disturbing FILE* position
        io->size = (i64)st.st_size;
    }
    // else: leave io->size as-is (your iosearch layer should track growth)
}

static int parse_loop_view_option(const char* value, LoopViewPolicy* out)
{
    if (!value || !out)
        return 1;
    if (strcmp(value, "delta") == 0) {
        *out = LOOP_VIEW_DELTA;
        return 0;
    }
    if (strcmp(value, "rescan") == 0) {
        *out = LOOP_VIEW_RESCAN;
        return 0;
    }
    if (strcmp(value, "cursor") == 0) {
        *out = LOOP_VIEW_CURSOR;
        return 0;
    }
    fprintf(stderr, "fiskta: --loop-view expects one of: delta, rescan, cursor\n");
    return 1;
}

static int parse_time_option(const char* value, const char* opt_name, int* out)
{
    if (!value || !opt_name || !out)
        return 1;

    // Parse: 0, 100ms, 5s, 2m, 1h (suffix required for non-zero values)
    char* end = NULL;
    long v = strtol(value, &end, 10);
    if (value[0] == '\0' || v < 0 || v > INT_MAX) {
        fprintf(stderr, "fiskta: %s expects a non-negative integer with suffix (ms|s|m|h)\n", opt_name);
        return 1;
    }

    // Allow bare "0" or "0" with any valid suffix
    if (v == 0) {
        if (!end || *end == '\0'
            || strcmp(end, "ms") == 0
            || strcmp(end, "s") == 0
            || strcmp(end, "m") == 0
            || strcmp(end, "h") == 0) {
            *out = 0;
            return 0;
        }
        // Invalid suffix after 0
        fprintf(stderr, "fiskta: %s invalid suffix '%s' (valid: ms, s, m, h)\n", opt_name, end);
        return 1;
    }

    // For non-zero values, require suffix
    if (!end || *end == '\0') {
        fprintf(stderr, "fiskta: %s requires a suffix (ms|s|m|h) for non-zero values\n", opt_name);
        return 1;
    }

    int multiplier = 1;
    if (strcmp(end, "ms") == 0) {
        multiplier = 1;
    } else if (strcmp(end, "s") == 0) {
        multiplier = 1000;
    } else if (strcmp(end, "m") == 0) {
        multiplier = 60000;
    } else if (strcmp(end, "h") == 0) {
        multiplier = 3600000;
    } else {
        fprintf(stderr, "fiskta: %s invalid suffix '%s' (valid: ms, s, m, h)\n", opt_name, end);
        return 1;
    }

    if (v > INT_MAX / multiplier) {
        fprintf(stderr, "fiskta: %s value too large\n", opt_name);
        return 1;
    }

    *out = (int)(v * multiplier);
    return 0;
}

static int parse_loop_timeout_option(const char* value, int* out)
{
    if (!value || !out)
        return 1;
    if (strcmp(value, "none") == 0 || strcmp(value, "off") == 0 || strcmp(value, "-1") == 0) {
        *out = -1;
        return 0;
    }
    return parse_time_option(value, "--loop-timeout", out);
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
        for (i32 i = 0; i < MAX_LABELS; i++)
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

    for (i32 ci = 0; ci < prg->clause_count; ++ci) {
        i32 rc = 0, lc = 0;
        clause_caps(&prg->clauses[ci], &rc, &lc);
        Range* r_tmp = (rc > 0) ? clause_ranges : NULL;
        LabelWrite* lw_tmp = (lc > 0) ? clause_labels : NULL;

        enum Err e = stage_clause(&prg->clauses[ci], io, vm_exec,
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
            // Success: check this clause's link
            if (prg->clauses[ci].link == LINK_OR) {
                // This clause succeeded and links with OR
                // Skip all remaining clauses in the OR chain
                while (ci + 1 < prg->clause_count && prg->clauses[ci].link == LINK_OR) {
                    ci++; // Skip the OR alternative
                }
            }
            // For LINK_THEN or LINK_NONE: just continue to next clause
        } else {
            last_err = e;
            last_failed_clause = ci;
            // For LINK_OR, LINK_THEN, or LINK_NONE: continue to next clause
        }
    }

    if (last_err_out)
        *last_err_out = last_err;

    // Determine return value for exit code calculation:
    // - Positive: number of successful clauses
    // - -2 - N: All clauses failed, last failure at clause (N + 2)
    if (ok == 0 && last_failed_clause >= 0) {
        return -2 - last_failed_clause; // All clauses failed
    } else {
        return ok; // Success (at least one clause succeeded)
    }
}

static void print_usage(void)
{
    printf("(fi)nd (sk)ip (ta)ke v%s\n", FISKTA_VERSION);
    printf("\n");
    printf("USAGE:\n");
    printf("  fiskta [options] <operations>\n");
    printf("  (use --input <path> to select input; defaults to stdin)\n");
    printf("\n");
    printf("OPERATIONS:\n");
    printf("  take <n><unit>              Extract n units from current position\n");
    printf("  skip <n><unit>              Move cursor n units (no output); negative moves backward\n");
    printf("  find [to <location>] <string>\n");
    printf("                              Search towards location (default: EOF)\n");
    printf("                              for <string>, move cursor to closest match\n");
    printf("  find:re [to <location>] <regex>  Regex enabled find\n");
    printf("  find:bin [to <location>] <hex-string>\n");
    printf("                              Find binary pattern (hex: DEADBEEF or DE AD BE EF)\n");
    printf("                              Case-insensitive, whitespace ignored\n");
    printf("  take to <location>          Order-normalized: emits [min(cursor,L), max(cursor,L));\n");
    printf("                              cursor moves to the high end\n");
    printf("  take until <string> [at match-start|match-end|line-start|line-end]\n");
    printf("                              Search forward, extract to specified position\n");
    printf("                              Default: match-start (excludes pattern)\n");
    printf("                              match-end includes; line-* relative to match\n");
    printf("  take until:re <regex> [at match-start|match-end|line-start|line-end]\n");
    printf("                              Same as take until but with regex pattern support\n");
    printf("  take until:bin <hex-string> [at match-start|match-end|line-start|line-end]\n");
    printf("                              Same as take until but with binary pattern (hex format)\n");
    printf("  label <name>                Mark current position with label\n");
    printf("  goto <location>             Jump to labeled position\n");
    printf("  view <L1> <L2>              Limit all ops to [min(L1,L2), max(L1,L2))\n");
    printf("  clear view                  Clear view; return to full file\n");
    printf("  print <string>              Emit literal bytes (alias: echo)\n");
    printf("                              Supports escape sequences: \\n \\t \\r \\0 \\\\ \\xHH\n");
    printf("                              Participates in clause atomicity\n");
    printf("  fail <message>              Write message to stderr and fail clause\n");
    printf("                              Message written immediately (not staged)\n");
    printf("                              Useful with OR for error messages\n");
    printf("\n");
    printf("UNITS:\n");
    printf("  b                           Bytes\n");
    printf("  l                           Lines\n");
    printf("  c                           UTF-8 code points\n");
    printf("\n");
    printf("LABELS:\n");
    printf("  NAME                        Uppercase, <16 chars, [A-Z0-9_-], starts with letter\n");
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
    printf("  Character Classes: \\d (digits), \\D (non-digits), \\w (word), \\W (non-word),\n");
    printf("                     \\s (space), \\S (non-space), [a-z], [^0-9]\n");
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
    printf("    OR      First success wins (short-circuits on success)\n");
    printf("\n");
    printf("  Within a clause: all ops must succeed or the clause fails atomically.\n");
    printf("  On Failure: clause rolls back (no output or state changes).\n");
    printf("  On Success: emits staged output, commits labels, updates cursor and last-match.\n");
    printf("\n");
    printf("EXIT CODES:\n");
    printf("  0               Success (at least one clause succeeded)\n");
    printf("  1               I/O error (file not found, permission denied, etc.)\n");
    printf("  2               Parse error (invalid syntax, unknown operation)\n");
    printf("  3               Regex error (invalid regex pattern)\n");
    printf("  4               Resource limit (program too large, out of memory)\n");
    printf("  10+             All clauses failed (exit code = 10 + index of last failed clause)\n");
    printf("\n");
    printf("OPTIONS:\n");
    printf("  -i, --input <path>          Read input from path (default: stdin)\n");
    printf("  -c, --commands <string|file>  Provide operations as a single string or file path\n");
    printf("      --                      Treat subsequent arguments as operations\n");
    printf("  -l, --loop [<number><ms|s|m|h>] Re-run fiskta program on input with optional delay\n");
    printf("  -k, --ignore-loop-failures  Continue looping on iteration failure\n");
    printf("  -t, --loop-timeout <number><ms|s|m|h>\n");
    printf("                              Stop looping after time value with no input growth\n");
    printf("  -w, --loop-view <policy>    View change policy: cursor (default) | delta | rescan\n");
    printf("                                cursor:  Continue from last cursor position\n");
    printf("                                delta:   Only process new data since last run\n");
    printf("                                rescan:  Re-scan entire file each iteration\n");
    printf("  -h, --help                  Show this help message\n");
    printf("      --examples              Show comprehensive usage examples\n");
    printf("  -v, --version               Show version information\n");
    printf("\n");
}

static void print_examples(void)
{
    printf("(fi)nd (sk)ip (ta)ke v%s\n", FISKTA_VERSION);
    printf("\n");
    printf("COMPREHENSIVE EXAMPLES:\n");
    printf("\n");
    printf("BASIC EXTRACTION:\n");
    printf("  # Take first 10 bytes\n");
    printf("  fiskta --input file.txt take 10b\n");
    printf("\n");
    printf("  # Take lines 2-4\n");
    printf("  fiskta --input file.txt skip 1l take 3l\n");
    printf("\n");
    printf("  # Take 20 utf-8 characters from a specific line\n");
    printf("  fiskta -i file.txt skip 12l take 20c\n");
    printf("\n");
    printf("SEARCH AND EXTRACT:\n");
    printf("  # Find pattern and take rest of line\n");
    printf("  fiskta --input logs.txt find \"ERROR:\" take to line-end\n");
    printf("\n");
    printf("  # Find with regex and take context (5 lines surrounding match)\n");
    printf("  fiskta --input logs.txt find:re \"^WARN\" take -2l take 3l\n");
    printf("\n");
    printf("  # Extract between delimiters\n");
    printf("  fiskta --input config.txt find \"[\" skip 1b take until \"]\"\n");
    printf("\n");
    printf("CONDITIONAL EXTRACTION:\n");
    printf("  # Extract only if pattern found\n");
    printf("  fiskta --input auth.log find \"login success\" find \"user=\" skip 5b take until \" \"\n");
    printf("\n");
    printf("  # Try multiple patterns (OR)\n");
    printf("  fiskta --input logs.txt find \"ERROR:\" take to line-end OR find \"WARN:\" take to line-end\n");
    printf("\n");
    printf("  # Fail with error message if pattern not found\n");
    printf("  fiskta --input config.txt find \"database\" OR fail \"Config missing database section\\n\"\n");
    printf("\n");
    printf("  # Sequential operations (THEN)\n");
    printf("  fiskta --input data.txt find \"header\" THEN take 5l THEN find \"footer\"\n");
    printf("\n");
    printf("ADVANCED PATTERNS:\n");
    printf("  # Extract clamped to specific section\n");
    printf("  # After view has been set, all operations are clampted to bounds\n");
    printf("  fiskta --input config.txt find \"[database]\" label START find \"[\" label END view START END ...\n");
    printf("\n");
    printf("  # Extract all occurrences (loop)\n");
    printf("  fiskta -i contacts.txt --loop 1ms find:re \"[A-Za-z0-9._%%+-]+@[A-Za-z0-9.-]+\" take to match-end print \"\\n\" \n");
    printf("\n");
    printf("  # Extract with regex until\n");
    printf("  fiskta --input data.txt take until:re \"\\\\d+\" at match-end\n");
    printf("\n");
    printf("  # Extract until binary pattern\n");
    printf("  fiskta --input data.bin take until:bin \"DEADBEEF\" at match-end\n");
    printf("\n");
    printf("BINARY DATA:\n");
    printf("  # Extract binary data at a specific location in file\n");
    printf("  fiskta --input image.bin find:bin \"AD D3 B4 02\" take 82b\n");
    printf("\n");
    printf("  # Detect PNG file header\n");
    printf("  fiskta --input image.bin find:bin \"89 50 4E 47 0D 0A 1A 0A\" or fail \"Not a PNG file\"\n");
    printf("\n");
    printf("  # Detect file type by magic number\n");
    printf("  fiskta --i mystery.dat find:bin \"504B0304\" print \"Type 1\" OR find:bin \"ac0a3c\" print \"Type 2\" \\ \n");
    printf("         OR find:bin \"ac0a3c\" print \"Type 2\" OR find:bin \"FFD8FFE0\" print \"Type 3\"\n");
    printf("\n");
    printf("STREAMING AND MONITORING:\n");
    printf("  # Process data in chunks (default cursor mode - maintain cursor position across iterations)\n");
    printf("  fiskta --loop 100ms --loop-view cursor --input data.txt take 1000b\n");
    printf("\n");
    printf("  # Monitor log file for errors (delta mode - only process new data)\n");
    printf("  fiskta --loop 1s --loop-view delta --input service.log find \"ERROR\" take to line-end\n");
    printf("\n");
    printf("  # Monitor changing file content (rescan mode - re-scan entire file each time)\n");
    printf("  fiskta --loop 2m --loop-view rescan --input status.txt find \"READY\"\n");
    printf("\n");
    printf("  # Process stdin\n");
    printf("  echo \"Hello world\" | fiskta find \"world\" take to match-end\n");
    printf("\n");
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    /***********************
     * CLI ARGUMENT PARSING
     ***********************/
    const char* input_path = "-";
    const char* command_arg = NULL;
    const char* command_file = NULL;
    int loop_ms = 0;
    bool loop_enabled = false;
    bool ignore_loop_failures = false;
    int idle_timeout_ms = -1;
    LoopViewPolicy loop_view_policy = LOOP_VIEW_CURSOR;

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
        if (strcmp(arg, "--examples") == 0) {
            print_examples();
            return 0;
        }
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--version") == 0) {
            printf("fiskta - (fi)nd (sk)ip (ta)ke v%s\n", FISKTA_VERSION);
            return 0;
        }
        if (strcmp(arg, "-i") == 0 || strcmp(arg, "--input") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --input requires a path\n");
                return 2;
            }
            input_path = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--input=", 8) == 0) {
            if (arg[8] == '\0') {
                fprintf(stderr, "fiskta: --input requires a path\n");
                return 2;
            }
            input_path = arg + 8;
            argi++;
            continue;
        }
        if (strcmp(arg, "-l") == 0 || strcmp(arg, "--loop") == 0) {
            loop_enabled = true;
            // Check if next arg looks like a time value (starts with digit)
            if (argi + 1 < argc && isdigit((unsigned char)argv[argi + 1][0])) {
                if (parse_time_option(argv[argi + 1], "--loop", &loop_ms) != 0)
                    return 2;
                argi += 2;
            } else {
                // No value provided, default to 0ms
                loop_ms = 0;
                argi++;
            }
            continue;
        }
        if (strncmp(arg, "--loop=", 7) == 0) {
            loop_enabled = true;
            if (parse_time_option(arg + 7, "--loop", &loop_ms) != 0)
                return 2;
            argi++;
            continue;
        }
        if (strcmp(arg, "-t") == 0 || strcmp(arg, "--loop-timeout") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --loop-timeout requires a value\n");
                return 2;
            }
            if (parse_loop_timeout_option(argv[argi + 1], &idle_timeout_ms) != 0)
                return 2;
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--loop-timeout=", 15) == 0) {
            if (parse_loop_timeout_option(arg + 15, &idle_timeout_ms) != 0)
                return 2;
            argi++;
            continue;
        }
        if (strcmp(arg, "-w") == 0 || strcmp(arg, "--loop-view") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --loop-view requires a value\n");
                return 2;
            }
            if (parse_loop_view_option(argv[argi + 1], &loop_view_policy) != 0)
                return 2;
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--loop-view=", 12) == 0) {
            if (parse_loop_view_option(arg + 12, &loop_view_policy) != 0)
                return 2;
            argi++;
            continue;
        }
        if (strcmp(arg, "-k") == 0 || strcmp(arg, "--ignore-loop-failures") == 0) {
            ignore_loop_failures = true;
            argi++;
            continue;
        }
        if (strcmp(arg, "-c") == 0 || strcmp(arg, "--commands") == 0) {
            if (command_arg || command_file) {
                fprintf(stderr, "fiskta: --commands specified multiple times\n");
                return 2;
            }
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --commands requires a string\n");
                return 2;
            }
            const char* value = argv[argi + 1];
            FILE* test = fopen(value, "rb");
            if (test) {
                fclose(test);
                command_file = value;
            } else {
                command_arg = value;
            }
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--commands=", 11) == 0) {
            if (command_arg || command_file) {
                fprintf(stderr, "fiskta: --commands specified multiple times\n");
                return 2;
            }
            if (arg[11] == '\0') {
                fprintf(stderr, "fiskta: --commands requires a string\n");
                return 2;
            }
            const char* value = arg + 11;
            FILE* test = fopen(value, "rb");
            if (test) {
                fclose(test);
                command_file = value;
            } else {
                command_arg = value;
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

    /**************************
     * OPERATION TOKEN PARSING
     **************************/
    int ops_index = argi;
    char** tokens = NULL;
    i32 token_count = 0;
    char* splitv[MAX_TOKENS];
    char file_content_buf[MAX_NEEDLE_BYTES];
    String tokens_view[MAX_TOKENS];

    if (command_file) {
        if (ops_index < argc) {
            fprintf(stderr, "fiskta: --commands cannot be combined with positional operations\n");
            return 2;
        }
        FILE* cf = fopen(command_file, "rb");
        if (!cf) {
            fprintf(stderr, "fiskta: unable to open commands file %s\n", command_file);
            return 2;
        }
        size_t total = fread(file_content_buf, 1, sizeof(file_content_buf) - 1, cf);
        if (ferror(cf)) {
            fclose(cf);
            fprintf(stderr, "fiskta: error reading commands file %s\n", command_file);
            return 2;
        }
        if (!feof(cf)) {
            fclose(cf);
            fprintf(stderr, "fiskta: operations file too long (max %d bytes)\n", MAX_NEEDLE_BYTES);
            return 2;
        }
        fclose(cf);
        file_content_buf[total] = '\0';
        if (total == 0) {
            fprintf(stderr, "fiskta: empty commands file\n");
            return 2;
        }
        for (size_t i = 0; i < total; ++i) {
            if (file_content_buf[i] == '\n' || file_content_buf[i] == '\r')
                file_content_buf[i] = ' ';
        }
        i32 n = split_ops_string(file_content_buf, splitv, (i32)(sizeof splitv / sizeof splitv[0]));
        if (n == -1) {
            fprintf(stderr, "fiskta: operations string too long (max %d bytes)\n", MAX_NEEDLE_BYTES);
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
            fprintf(stderr, "fiskta: operations string too long (max %d bytes)\n", MAX_NEEDLE_BYTES);
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
            i32 n = split_ops_string_optimized(tokens[0], tokens_view, MAX_TOKENS);
            if (n == -1) {
                fprintf(stderr, "fiskta: operations string too long (max %d bytes)\n", MAX_NEEDLE_BYTES);
                return 2;
            }
            if (n <= 0) {
                fprintf(stderr, "fiskta: empty operations string\n");
                return 2;
            }
            token_count = n;
            // tokens_view is already populated, skip the conversion step
            goto skip_conversion;
        }
    }

    /************************************************************
     * PHASE 1: PREFLIGHT PARSE
     * Analyze operations to determine memory requirements
     ************************************************************/
    convert_tokens_to_strings(tokens, token_count, tokens_view);
skip_conversion:

    ParsePlan plan = (ParsePlan) { 0 };
    const char* path = NULL;
    enum Err e = parse_preflight(token_count, tokens_view, input_path, &plan, &path);
    if (e != E_OK) {
        print_err(e, "parse preflight");
        return 2; // Exit code 2: Parse error
    }

    /************************************************************
     * PHASE 2: COMPUTE ARENA SIZES
     * Calculate total memory needed for all data structures
     ************************************************************/
    const size_t search_buf_cap = (FW_WIN > (BK_BLK + OVERLAP_MAX)) ? (size_t)FW_WIN : (size_t)(BK_BLK + OVERLAP_MAX);
    const size_t ops_bytes = (size_t)plan.total_ops * sizeof(Op);
    const size_t clauses_bytes = (size_t)plan.clause_count * sizeof(Clause);
    const size_t str_pool_bytes = plan.needle_bytes;

    const size_t re_prog_bytes = (size_t)plan.sum_findr_ops * sizeof(ReProg);
    const size_t re_ins_bytes = (size_t)plan.re_ins_estimate * sizeof(ReInst);
    const size_t re_cls_bytes = (size_t)plan.re_classes_estimate * sizeof(ReClass);

    // Choose per-run thread capacity as ~2x max nins, min 32.
    int re_threads_cap = plan.re_ins_estimate_max > 0 ? 2 * plan.re_ins_estimate_max : 32;
    if (re_threads_cap < 32)
        re_threads_cap = 32;
    const size_t re_threads_bytes = (size_t)re_threads_cap * sizeof(ReThread);

    /************************************************************
     * PHASE 3: ARENA ALLOCATION
     * Allocate single memory block and compute aligned offsets
     ************************************************************/
    size_t search_buf_size = align_or_die(search_buf_cap, alignof(unsigned char));
    size_t clauses_size = align_or_die(clauses_bytes, alignof(Clause));
    size_t ops_size = align_or_die(ops_bytes, alignof(Op));
    size_t re_prog_size = align_or_die(re_prog_bytes, alignof(ReProg));
    size_t re_ins_size = align_or_die(re_ins_bytes, alignof(ReInst));
    size_t re_cls_size = align_or_die(re_cls_bytes, alignof(ReClass));
    size_t str_pool_size = align_or_die(str_pool_bytes, alignof(char));
    // Two thread buffers + two seen arrays sized to max estimated nins
    size_t re_seen_bytes_each = (size_t)(plan.re_ins_estimate_max > 0 ? plan.re_ins_estimate_max : 32);
    size_t re_seen_size;
    if (add_overflow(re_seen_bytes_each, re_seen_bytes_each, &re_seen_size)) {
        print_err(E_OOM, "regex 'seen' size overflow");
        return 4;
    }
    size_t re_thrbufs_size = align_or_die(re_threads_bytes, alignof(ReThread)) * 2;

    size_t ranges_bytes = (plan.sum_take_ops > 0) ? align_or_die((size_t)plan.sum_take_ops * sizeof(Range), alignof(Range)) : 0;
    size_t labels_bytes = (plan.sum_label_ops > 0) ? align_or_die((size_t)plan.sum_label_ops * sizeof(LabelWrite), alignof(LabelWrite)) : 0;

    size_t total = search_buf_size;
    if (add_overflow(total, clauses_size, &total) || add_overflow(total, ops_size, &total) || add_overflow(total, re_prog_size, &total) || add_overflow(total, re_ins_size, &total) || add_overflow(total, re_cls_size, &total) || add_overflow(total, str_pool_size, &total) || add_overflow(total, re_thrbufs_size, &total) || add_overflow(total, re_seen_size, &total) || add_overflow(total, ranges_bytes, &total) || add_overflow(total, labels_bytes, &total) || add_overflow(total, 64, &total)) { // 3 small cushion
        print_err(E_OOM, "arena size overflow");
        return 4; // Exit code 4: Resource limit
    }

    void* block = malloc(total);
    if (!block) {
        print_err(E_OOM, "arena alloc");
        return 4; // Exit code 4: Resource limit
    }
    Arena A;
    arena_init(&A, block, total);

    /************************************************************
     * PHASE 4: CARVE ARENA SLICES
     * Partition the memory block into specific buffers
     ************************************************************/
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
        return 4; // Exit code 4: Resource limit
    }

    /************************************************************
     * PHASE 5: BUILD PROGRAM
     * Parse operations into executable program structure
     ************************************************************/
    Program prg = (Program) { 0 };
    e = parse_build(token_count, tokens_view, input_path, &prg, &path,
        clauses_buf, ops_buf, str_pool, str_pool_bytes);
    if (e != E_OK) {
        print_err(e, "parse build");
        free(block);
        return 2; // Exit code 2: Parse error
    }
    if (prg.clause_count == 0) {
        print_err(E_PARSE, "no operations parsed");
        free(block);
        return 2;
    }

    // Compile all regex patterns upfront
    i32 re_prog_idx = 0, re_ins_idx = 0, re_cls_idx = 0;
    for (i32 ci = 0; ci < prg.clause_count; ++ci) {
        Clause* clause = &prg.clauses[ci];
        for (i32 i = 0; i < clause->op_count; ++i) {
            Op* op = &clause->ops[i];
            if (op->kind == OP_FIND_RE) {
                ReProg* prog = &re_progs[re_prog_idx++];
                enum Err err = re_compile_into(op->u.findr.pattern, prog,
                    re_ins + re_ins_idx, (i32)(re_ins_bytes / sizeof(ReInst)) - re_ins_idx, &re_ins_idx,
                    re_cls + re_cls_idx, (i32)(re_cls_bytes / sizeof(ReClass)) - re_cls_idx, &re_cls_idx);
                if (err != E_OK) {
                    print_err(err, "regex compile");
                    free(block);
                    return (err == E_PARSE || err == E_BAD_NEEDLE) ? 3 : 4;
                }
                op->u.findr.prog = prog;
            } else if (op->kind == OP_TAKE_UNTIL_RE) {
                ReProg* prog = &re_progs[re_prog_idx++];
                enum Err err = re_compile_into(op->u.take_until_re.pattern, prog,
                    re_ins + re_ins_idx, (i32)(re_ins_bytes / sizeof(ReInst)) - re_ins_idx, &re_ins_idx,
                    re_cls + re_cls_idx, (i32)(re_cls_bytes / sizeof(ReClass)) - re_cls_idx, &re_cls_idx);
                if (err != E_OK) {
                    print_err(err, "regex compile");
                    free(block);
                    return (err == E_PARSE || err == E_BAD_NEEDLE) ? 3 : 4;
                }
                op->u.take_until_re.prog = prog;
            }
        }
    }

    /********************************************
     * PHASE 6: OPEN FILE I/O
     * Initialize file handle and search buffers
     ********************************************/
    File io = { 0 };
    e = io_open(&io, path, search_buf, search_buf_cap);
    if (e != E_OK) {
        free(block);
        print_err(e, "I/O open");
        return 1; // Exit code 1: I/O error
    }

    io_set_regex_scratch(&io, re_curr_thr, re_next_thr, re_threads_cap,
        seen_curr, seen_next, (size_t)re_seen_bytes_each);

    /*****************************************************
     * PHASE 7: EXECUTE PROGRAM
     * Run operations with optional looping for streaming
     *****************************************************/
    int exit_code = 0;

    // Single execution mode (no looping)
    refresh_file_size(&io);
    i64 window_start = 0;
    i64 last_size = io_size(&io);
    uint64_t last_change_ms = now_millis();
    VM saved_vm;
    memset(&saved_vm, 0, sizeof(saved_vm));
    for (i32 i = 0; i < MAX_LABELS; i++)
        saved_vm.label_pos[i] = -1;
    bool have_saved_vm = false;
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
        if (loop_view_policy == LOOP_VIEW_RESCAN)
            effective_start = 0;
        else if (loop_view_policy == LOOP_VIEW_CURSOR && have_saved_vm)
            effective_start = clamp64(saved_vm.cursor, 0, window_end);

        if (loop_enabled && loop_view_policy == LOOP_VIEW_DELTA && effective_start >= window_end) {
            if (idle_timeout_ms >= 0 && (now_ms - last_change_ms) >= (uint64_t)idle_timeout_ms)
                break;
            sleep_msec(loop_ms);
            continue;
        }

        enum Err last_err = E_OK;
        i64 prev_cursor = have_saved_vm ? saved_vm.cursor : effective_start;
        VM* vm_ptr = (loop_view_policy == LOOP_VIEW_CURSOR) ? &saved_vm : NULL;
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
            i32 failed_clause = (-ok) - 2;
            // If looping with ignore-failures, continue to next iteration
            if (loop_enabled && ignore_loop_failures) {
                // Continue looping despite failure
            } else {
                fprintf(stderr, "fiskta: clause %d failed (%s)\n", failed_clause, err_str(last_err));
                exit_code = 10 + failed_clause;
                goto cleanup;
            }
        }

        fflush(stdout);

        if (!loop_enabled)
            break;

        if (loop_view_policy == LOOP_VIEW_CURSOR) {
            have_saved_vm = true;
            window_start = clamp64(saved_vm.cursor, 0, window_end);
            if (saved_vm.cursor != prev_cursor)
                last_change_ms = now_ms;
        } else if (loop_view_policy == LOOP_VIEW_DELTA) {
            window_start = window_end;
        } else {
            window_start = 0;
        }

        now_ms = now_millis();
        if (idle_timeout_ms >= 0 && (now_ms - last_change_ms) >= (uint64_t)idle_timeout_ms)
            break;

        sleep_msec(loop_ms);
    }

cleanup:
    io_close(&io);
    free(block);
    return exit_code;
}

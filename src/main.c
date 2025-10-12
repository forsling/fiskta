#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cli_help.h"
#include "fiskta.h"
#include "iosearch.h"
#include "reprog.h"
#include "util.h"
#include <ctype.h>
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
                if (ntok >= max_tokens) {
                    return -1;
                }
                if (boff >= sizeof buf) {
                    return -1;
                }
                out[ntok] = &buf[boff];
                st = (c == '\'') ? S_SQ : S_DQ;
                p++;
                continue;
            }
            // start token, reprocess this char in S_TOKEN
            if (ntok >= max_tokens) {
                return -1;
            }
            if (boff >= sizeof buf) {
                return -1;
            }
            out[ntok] = &buf[boff];
            st = S_TOKEN;
            continue;
        }
        if (st == S_TOKEN) {
            if (c == ' ' || c == '\t') {
                if (boff >= sizeof buf) {
                    return -1; // NUL safety
                }
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
                    if (boff >= sizeof buf) {
                        return -1;
                    }
                    buf[boff++] = (char)next;
                    p += 2;
                    continue;
                }
            }
            if (boff >= sizeof buf) {
                return -1;
            }
            buf[boff++] = (char)c;
            p++;
            continue;
        }
        if (st == S_SQ) {
            if (c == '\'') {
                st = S_TOKEN;
                p++;
                continue;
            }
            if (boff >= sizeof buf) {
                return -1;
            }
            buf[boff++] = (char)c;
            p++;
            continue;
        }
        if (st == S_DQ) {
            if (c == '"') {
                st = S_TOKEN;
                p++;
                continue;
            }
            if (c == '\\' && p[1]) {
                unsigned char esc = (unsigned char)p[1];
                if (esc == '"' || esc == '\\') {
                    if (boff >= sizeof buf) {
                        return -1;
                    }
                    buf[boff++] = (char)esc;
                    p += 2;
                    continue;
                }
            }
            if (boff >= sizeof buf) {
                return -1;
            }
            buf[boff++] = (char)c;
            p++;
            continue;
        }
    }

    if (st == S_TOKEN || st == S_SQ || st == S_DQ) {
        if (boff >= sizeof buf) {
            return -1;
        }
        buf[boff++] = '\0';
        if (ntok < max_tokens) {
            ntok++;
        } else {
            return -1;
        }
    }
    return ntok;
}

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

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
    if (msg) {
        fprintf(stderr, "fiskta: %s (%s)\n", msg, err_str(e));
    } else {
        fprintf(stderr, "fiskta: %s\n", err_str(e));
    }
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
    LOOP_MODE_FOLLOW, // --follow, -f: only new data (delta)
    LOOP_MODE_MONITOR, // --monitor, -m: restart from BOF (rescan)
    LOOP_MODE_CONTINUE // --continue, -c: resume from cursor (default)
} LoopMode;

typedef struct {
    const char* input_path;
    const char* ops_arg;
    const char* ops_file;
    int loop_ms;
    bool loop_enabled;
    bool ignore_loop_failures;
    int idle_timeout_ms;
    int exec_timeout_ms;
    LoopMode loop_mode;
} CliOptions;

typedef struct {
    String* tokens;
    i32 token_count;
    bool tokens_need_conversion;
} Operations;

typedef struct {
    int exit_code;
    int exit_reason;
    int last_ok;
    i64 data_start;
    i64 last_size;
    u64 start_ms;
    u64 last_change_ms;
    VM saved_vm;
    bool have_saved_vm;
} LoopState;

static int parse_time_option(const char* value, const char* opt_name, int* out);
static int load_ops_from_cli_options(const CliOptions* opts, int ops_index, int argc, char** argv, Operations* out);
static int parse_until_idle_option(const char* value, int* out);

static bool parse_cli_args(int argc, char** argv, CliOptions* out, int* ops_index, int* exit_code_out)
{
    if (!out || !ops_index || !exit_code_out) {
        return false;
    }

    CliOptions opt = {
        .input_path = "-",
        .ops_arg = NULL,
        .ops_file = NULL,
        .loop_ms = 0,
        .loop_enabled = false,
        .ignore_loop_failures = false,
        .idle_timeout_ms = -1,
        .exec_timeout_ms = -1,
        .loop_mode = LOOP_MODE_CONTINUE
    };

    *exit_code_out = -1;

    int argi = 1;
    while (argi < argc) {
        const char* arg = argv[argi];
        if (strcmp(arg, "--") == 0) {
            argi++;
            break;
        }
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage();
            *exit_code_out = 0;
            return false;
        }
        if (strcmp(arg, "--examples") == 0) {
            print_examples();
            *exit_code_out = 0;
            return false;
        }
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--version") == 0) {
            printf("fiskta - (fi)nd (sk)ip (ta)ke v%s\n", FISKTA_VERSION);
            *exit_code_out = 0;
            return false;
        }
        if (strcmp(arg, "-i") == 0 || strcmp(arg, "--input") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --input requires a path\n");
                *exit_code_out = 2;
                return false;
            }
            opt.input_path = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--input=", 8) == 0) {
            if (arg[8] == '\0') {
                fprintf(stderr, "fiskta: --input requires a path\n");
                *exit_code_out = 2;
                return false;
            }
            opt.input_path = arg + 8;
            argi++;
            continue;
        }
        if (strcmp(arg, "--every") == 0) {
            opt.loop_enabled = true;
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --every requires a time value\n");
                *exit_code_out = 2;
                return false;
            }
            if (parse_time_option(argv[argi + 1], "--every", &opt.loop_ms) != 0) {
                *exit_code_out = 2;
                return false;
            }
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--every=", 8) == 0) {
            opt.loop_enabled = true;
            if (parse_time_option(arg + 8, "--every", &opt.loop_ms) != 0) {
                *exit_code_out = 2;
                return false;
            }
            argi++;
            continue;
        }
        if (strcmp(arg, "--until-idle") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --until-idle requires a value\n");
                *exit_code_out = 2;
                return false;
            }
            if (parse_until_idle_option(argv[argi + 1], &opt.idle_timeout_ms) != 0) {
                *exit_code_out = 2;
                return false;
            }
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--until-idle=", 13) == 0) {
            if (parse_until_idle_option(arg + 13, &opt.idle_timeout_ms) != 0) {
                *exit_code_out = 2;
                return false;
            }
            argi++;
            continue;
        }
        if (strcmp(arg, "--for") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --for requires a value\n");
                *exit_code_out = 2;
                return false;
            }
            if (parse_time_option(argv[argi + 1], "--for", &opt.exec_timeout_ms) != 0) {
                *exit_code_out = 2;
                return false;
            }
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--for=", 6) == 0) {
            if (parse_time_option(arg + 6, "--for", &opt.exec_timeout_ms) != 0) {
                *exit_code_out = 2;
                return false;
            }
            argi++;
            continue;
        }
        if (strcmp(arg, "-m") == 0 || strcmp(arg, "--monitor") == 0) {
            opt.loop_mode = LOOP_MODE_MONITOR;
            opt.loop_enabled = true;
            argi++;
            continue;
        }
        if (strcmp(arg, "-c") == 0 || strcmp(arg, "--continue") == 0) {
            opt.loop_mode = LOOP_MODE_CONTINUE;
            opt.loop_enabled = true;
            argi++;
            continue;
        }
        if (strcmp(arg, "-f") == 0 || strcmp(arg, "--follow") == 0) {
            opt.loop_mode = LOOP_MODE_FOLLOW;
            opt.loop_enabled = true;
            argi++;
            continue;
        }
        if (strcmp(arg, "-k") == 0 || strcmp(arg, "--ignore-failures") == 0) {
            opt.ignore_loop_failures = true;
            argi++;
            continue;
        }
        if (strcmp(arg, "--ops") == 0) {
            if (opt.ops_arg || opt.ops_file) {
                fprintf(stderr, "fiskta: --ops specified multiple times\n");
                *exit_code_out = 2;
                return false;
            }
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --ops requires a string\n");
                *exit_code_out = 2;
                return false;
            }
            const char* value = argv[argi + 1];
            FILE* test = fopen(value, "rb");
            if (test) {
                fclose(test);
                opt.ops_file = value;
            } else {
                opt.ops_arg = value;
            }
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--ops=", 6) == 0) {
            if (opt.ops_arg || opt.ops_file) {
                fprintf(stderr, "fiskta: --ops specified multiple times\n");
                *exit_code_out = 2;
                return false;
            }
            if (arg[6] == '\0') {
                fprintf(stderr, "fiskta: --ops requires a string\n");
                *exit_code_out = 2;
                return false;
            }
            const char* value = arg + 6;
            FILE* test = fopen(value, "rb");
            if (test) {
                fclose(test);
                opt.ops_file = value;
            } else {
                opt.ops_arg = value;
            }
            argi++;
            continue;
        }
        if (arg[0] == '-') {
            if (arg[1] == '\0' || isdigit((unsigned char)arg[1])) {
                break;
            }
            fprintf(stderr, "fiskta: unknown option %s\n", arg);
            *exit_code_out = 2;
            return false;
        }
        break;
    }

    *out = opt;
    *ops_index = argi;
    return true;
}

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

static u64 now_millis(void)
{
#ifdef _WIN32
    return (u64)GetTickCount64();
#else
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (u64)ts.tv_sec * 1000ULL + (u64)ts.tv_nsec / 1000000ULL;
#endif
}

#include <sys/stat.h>

static void refresh_file_size(File* io)
{
    if (!io || !io->f) {
        return;
    }

    int fd = fileno(io->f);
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        // Regular file: we can refresh size without disturbing FILE* position
        io->size = (i64)st.st_size;
    }
}

static int parse_time_option(const char* value, const char* opt_name, int* out)
{
    if (!value || !opt_name || !out) {
        return 1;
    }

    char* end = NULL;
    long v = strtol(value, &end, 10);
    if (value[0] == '\0' || v < 0 || v > INT_MAX) {
        fprintf(stderr, "fiskta: %s expects a non-negative integer with suffix (ms|s|m|h)\n", opt_name);
        return 1;
    }

    if (v == 0) {
        if (!end || *end == '\0'
            || strcmp(end, "ms") == 0
            || strcmp(end, "s") == 0
            || strcmp(end, "m") == 0
            || strcmp(end, "h") == 0) {
            *out = 0;
            return 0;
        }
        fprintf(stderr, "fiskta: %s invalid suffix '%s' (valid: ms, s, m, h)\n", opt_name, end);
        return 1;
    }

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

static int parse_until_idle_option(const char* value, int* out)
{
    if (!value || !out) {
        return 1;
    }
    if (strcmp(value, "none") == 0 || strcmp(value, "off") == 0 || strcmp(value, "-1") == 0) {
        *out = -1;
        return 0;
    }
    return parse_time_option(value, "--until-idle", out);
}

// Load operations from CLI options (--ops string, --ops-file, or positional args)
// Returns exit code on error, 0 on success
static int load_ops_from_cli_options(const CliOptions* opts, int ops_index, int argc, char** argv, Operations* out)
{
    if (!opts || !out) {
        return 2;
    }

    // Static buffers for operations loading
    static char* splitv[MAX_TOKENS];
    static char file_content_buf[MAX_NEEDLE_BYTES];
    static String tokens_view[MAX_TOKENS];

    const char* command_file = opts->ops_file;
    const char* command_arg = opts->ops_arg;

    if (command_file) {
        // Load operations from file
        if (ops_index < argc) {
            fprintf(stderr, "fiskta: --ops cannot be combined with positional operations\n");
            return 2;
        }

        FILE* cf = fopen(command_file, "rb");
        if (!cf) {
            fprintf(stderr, "fiskta: unable to open ops file %s\n", command_file);
            return 2;
        }

        size_t total = fread(file_content_buf, 1, sizeof(file_content_buf) - 1, cf);
        if (ferror(cf)) {
            fclose(cf);
            fprintf(stderr, "fiskta: error reading ops file %s\n", command_file);
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
            fprintf(stderr, "fiskta: empty ops file\n");
            return 2;
        }

        // Replace newlines with spaces
        for (size_t i = 0; i < total; ++i) {
            if (file_content_buf[i] == '\n' || file_content_buf[i] == '\r') {
                file_content_buf[i] = ' ';
            }
        }

        i32 n = split_ops_string(file_content_buf, splitv, (i32)(sizeof splitv / sizeof splitv[0]));
        if (n == -1) {
            fprintf(stderr, "fiskta: operations string too long (max %d bytes)\n", MAX_NEEDLE_BYTES);
            return 2;
        }
        if (n <= 0) {
            fprintf(stderr, "fiskta: empty ops string\n");
            return 2;
        }

        out->tokens = tokens_view;
        out->token_count = n;
        out->tokens_need_conversion = true;
        convert_tokens_to_strings(splitv, n, tokens_view);

    } else if (command_arg) {
        // Load operations from --ops string
        if (ops_index < argc) {
            fprintf(stderr, "fiskta: --ops cannot be combined with positional operations\n");
            return 2;
        }

        i32 n = split_ops_string(command_arg, splitv, (i32)(sizeof splitv / sizeof splitv[0]));
        if (n == -1) {
            fprintf(stderr, "fiskta: operations string too long (max %d bytes)\n", MAX_NEEDLE_BYTES);
            return 2;
        }
        if (n <= 0) {
            fprintf(stderr, "fiskta: empty ops string\n");
            return 2;
        }

        out->tokens = tokens_view;
        out->token_count = n;
        out->tokens_need_conversion = true;
        convert_tokens_to_strings(splitv, n, tokens_view);

    } else {
        // Load operations from positional arguments
        i32 token_count = (i32)(argc - ops_index);
        if (token_count <= 0) {
            fprintf(stderr, "fiskta: missing operations\n");
            fprintf(stderr, "Try 'fiskta --help' for more information.\n");
            return 2;
        }

        char** tokens = argv + ops_index;
        if (token_count == 1 && strchr(tokens[0], ' ')) {
            // Single token with spaces - use optimized tokenizer
            i32 n = tokenize_ops_string(tokens[0], tokens_view, MAX_TOKENS);
            if (n == -1) {
                fprintf(stderr, "fiskta: operations string too long (max %d bytes)\n", MAX_NEEDLE_BYTES);
                return 2;
            }
            if (n <= 0) {
                fprintf(stderr, "fiskta: empty operations string\n");
                return 2;
            }
            out->tokens = tokens_view;
            out->token_count = n;
            out->tokens_need_conversion = false; // Already converted
        } else {
            // Multiple tokens or single token without spaces
            out->tokens = tokens_view;
            out->token_count = token_count;
            out->tokens_need_conversion = true;
            convert_tokens_to_strings(tokens, token_count, tokens_view);
        }
    }

    return 0; // Success
}

// Initialize loop state with default values
static void init_loop_state(LoopState* state, bool loop_enabled, int idle_timeout_ms)
{
    (void)idle_timeout_ms; // Unused parameter

    if (!state) return;

    state->exit_code = 0;
    state->exit_reason = 0; // EXIT_REASON_NORMAL
    state->last_ok = 1; // Default success
    state->data_start = 0;
    state->last_size = 0;
    state->start_ms = now_millis();
    state->last_change_ms = state->start_ms;

    // Initialize saved VM
    memset(&state->saved_vm, 0, sizeof(state->saved_vm));
    for (i32 i = 0; i < MAX_LABELS; i++) {
        state->saved_vm.label_pos[i] = -1;
    }
    state->have_saved_vm = false;

    // Disable idle timeout if looping is disabled
    if (!loop_enabled) {
        // idle_timeout_ms is passed by value, so we can't modify it here
        // This will be handled in the main function
    }
}

static int execute_program_iteration(const Program* prg, File* io, VM* vm,
    Range* clause_ranges, LabelWrite* clause_labels,
    enum Err* last_err_out,
    i64 data_lo, i64 data_hi)
{
    if (last_err_out) {
        *last_err_out = E_OK;
    }

    io_reset_full(io);

    VM local_vm;
    VM* vm_exec = vm ? vm : &local_vm;
    if (!vm) {
        memset(vm_exec, 0, sizeof(*vm_exec));
        for (i32 i = 0; i < MAX_LABELS; i++) {
            vm_exec->label_pos[i] = -1;
        }
    }

    i64 lo = clamp64(data_lo, 0, io_size(io));
    i64 hi = clamp64(data_hi, 0, io_size(io));
    if (vm_exec->cursor < lo) {
        vm_exec->cursor = lo;
    }
    if (vm_exec->cursor > hi) {
        vm_exec->cursor = hi;
    }
    vm_exec->view.active = true;
    vm_exec->view.lo = lo;
    vm_exec->view.hi = hi;
    vm_exec->last_match.valid = false;

    i32 ok = 0;
    enum Err last_err = E_OK;
    i32 last_failed_clause = -1;
    StagedResult result;

    for (i32 ci = 0; ci < prg->clause_count; ++ci) {
        i32 rc = 0;
        i32 lc = 0;
        clause_caps(&prg->clauses[ci], &rc, &lc);
        Range* r_tmp = (rc > 0) ? clause_ranges : NULL;
        LabelWrite* lw_tmp = (lc > 0) ? clause_labels : NULL;

        enum Err e = stage_clause(&prg->clauses[ci], io, vm_exec,
            r_tmp, rc, lw_tmp, lc, &result);
        if (e == E_OK) {
            // Commit staged ranges to stdout / file as appropriate
            for (i32 i = 0; i < result.range_count; i++) {
                if (result.ranges[i].kind == RANGE_FILE) {
                    e = io_emit(io, result.ranges[i].file.start, result.ranges[i].file.end, stdout);
                } else {
                    // RANGE_LIT: write literal bytes
                    if ((size_t)fwrite(result.ranges[i].lit.bytes, 1, (size_t)result.ranges[i].lit.len, stdout) != (size_t)result.ranges[i].lit.len) {
                        e = E_IO;
                    }
                }
                if (e != E_OK) {
                    break;
                }
            }
            if (e == E_OK) {
                // Commit staged VM state now that I/O succeeded
                commit_labels(vm_exec, result.label_writes, result.label_count);
                vm_exec->cursor = result.staged_vm.cursor;
                vm_exec->last_match = result.staged_vm.last_match;
                vm_exec->view = result.staged_vm.view;
            }
        }

        if (e == E_OK) {
            ok++;
            if (prg->clauses[ci].link == LINK_OR) {
                // Skip remaining alternatives in this OR-chain once one succeeds
                while (ci + 1 < prg->clause_count && prg->clauses[ci].link == LINK_OR) {
                    ci++;
                }
            }
        } else {
            last_err = e;
            last_failed_clause = ci;
        }
    }

    if (last_err_out) {
        *last_err_out = last_err;
    }
    if (ok == 0 && last_failed_clause >= 0) {
        return -2 - last_failed_clause;
    }
    return ok;
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    /***********************
     * CLI ARGUMENT PARSING
     ***********************/
    CliOptions cli_opts;
    int ops_index = 0;
    int parse_exit = -1;
    if (!parse_cli_args(argc, argv, &cli_opts, &ops_index, &parse_exit)) {
        return (parse_exit >= 0) ? parse_exit : 0;
    }

    const char* input_path = cli_opts.input_path;
    int loop_ms = cli_opts.loop_ms;
    bool loop_enabled = cli_opts.loop_enabled;
    bool ignore_loop_failures = cli_opts.ignore_loop_failures;
    int idle_timeout_ms = cli_opts.idle_timeout_ms;
    int exec_timeout_ms = cli_opts.exec_timeout_ms;
    LoopMode loop_mode = cli_opts.loop_mode;

    /**************************
     * OPERATION TOKEN PARSING
     **************************/
    Operations ops;
    int ops_result = load_ops_from_cli_options(&cli_opts, ops_index, argc, argv, &ops);
    if (ops_result != 0) {
        return ops_result;
    }

    /************************************************************
     * PHASE 1: PREFLIGHT PARSE
     * Analyze operations to determine memory requirements
     *************************************************************/
    ParsePlan plan = (ParsePlan) { 0 };
    const char* path = NULL;
    enum Err e = parse_preflight(ops.token_count, ops.tokens, input_path, &plan, &path);
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
    if (re_threads_cap < 32) {
        re_threads_cap = 32;
    }
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
    Arena arena;
    arena_init(&arena, block, total);

    /************************************************************
     * PHASE 4: CARVE ARENA SLICES
     * Partition the memory block into specific buffers
     ************************************************************/
    unsigned char* search_buf = arena_alloc(&arena, search_buf_cap, alignof(unsigned char));
    Clause* clauses_buf = arena_alloc(&arena, clauses_bytes, alignof(Clause));
    Op* ops_buf = arena_alloc(&arena, ops_bytes, alignof(Op));
    ReThread* re_curr_thr = arena_alloc(&arena, re_threads_bytes, alignof(ReThread));
    ReThread* re_next_thr = arena_alloc(&arena, re_threads_bytes, alignof(ReThread));
    unsigned char* seen_curr = arena_alloc(&arena, re_seen_bytes_each, 1);
    unsigned char* seen_next = arena_alloc(&arena, re_seen_bytes_each, 1);
    ReProg* re_progs = arena_alloc(&arena, re_prog_bytes, alignof(ReProg));
    ReInst* re_ins = arena_alloc(&arena, re_ins_bytes, alignof(ReInst));
    ReClass* re_cls = arena_alloc(&arena, re_cls_bytes, alignof(ReClass));
    char* str_pool = arena_alloc(&arena, str_pool_bytes, alignof(char));
    Range* clause_ranges = (plan.sum_take_ops > 0) ? arena_alloc(&arena, (size_t)plan.sum_take_ops * sizeof(Range), alignof(Range)) : NULL;
    LabelWrite* clause_labels = (plan.sum_label_ops > 0) ? arena_alloc(&arena, (size_t)plan.sum_label_ops * sizeof(LabelWrite), alignof(LabelWrite)) : NULL;

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
    e = parse_build(ops.token_count, ops.tokens, input_path, &prg, &path,
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
    i32 re_prog_idx = 0;
    i32 re_ins_idx = 0;
    i32 re_cls_idx = 0;
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
        seen_curr, seen_next, re_seen_bytes_each);

    /*****************************************************
     * PHASE 7: EXECUTE PROGRAM
     * Run operations with optional looping for streaming
     *****************************************************/
    enum {
        EXIT_REASON_NORMAL = 0,
        EXIT_REASON_EXEC_TIMEOUT = 5
    };

    // Initialize loop state
    LoopState loop_state;
    init_loop_state(&loop_state, loop_enabled, idle_timeout_ms);

    // Disable idle timeout if looping is disabled
    if (!loop_enabled) {
        idle_timeout_ms = -1;
    }

    // Initialize file size tracking
    refresh_file_size(&io);
    loop_state.last_size = io_size(&io);

    for (;;) {
        refresh_file_size(&io);
        i64 data_end = io_size(&io);
        u64 now_ms = now_millis();

        // Check execution timeout (works in both loop and non-loop modes)
        if (exec_timeout_ms >= 0 && (now_ms - loop_state.start_ms) >= (u64)exec_timeout_ms) {
            loop_state.exit_reason = EXIT_REASON_EXEC_TIMEOUT;
            break;
        }

        if (data_end != loop_state.last_size) {
            loop_state.last_size = data_end;
            loop_state.last_change_ms = now_ms;
        }
        if (loop_state.data_start > data_end) {
            loop_state.data_start = data_end;
        }

        i64 effective_start = loop_state.data_start;
        if (loop_mode == LOOP_MODE_MONITOR) {
            effective_start = 0;
        } else if (loop_mode == LOOP_MODE_CONTINUE && loop_state.have_saved_vm) {
            effective_start = clamp64(loop_state.saved_vm.cursor, 0, data_end);
        }

        if (loop_enabled && loop_mode == LOOP_MODE_FOLLOW && effective_start >= data_end) {
            if (idle_timeout_ms >= 0 && (now_ms - loop_state.last_change_ms) >= (u64)idle_timeout_ms) {
                // Idle timeout is considered normal exit (success) since it's the expected behavior
                break;
            }
            sleep_msec(loop_ms);
            continue;
        }

        enum Err last_err = E_OK;
        i64 prev_cursor = loop_state.have_saved_vm ? loop_state.saved_vm.cursor : effective_start;
        VM* vm_ptr = (loop_mode == LOOP_MODE_CONTINUE) ? &loop_state.saved_vm : NULL;
        int ok = execute_program_iteration(&prg, &io, vm_ptr, clause_ranges, clause_labels, &last_err,
            effective_start, data_end);

        // Save last iteration result for exit code determination
        loop_state.last_ok = ok;

        // Handle exit codes
        if (ok > 0) {
            // Success - at least one clause succeeded
        } else if (ok == 0) {
            // I/O error during execution
            fprintf(stderr, "fiskta: I/O error (%s)\n", err_str(last_err));
            loop_state.exit_code = 1;
            goto cleanup;
        } else {
            i32 failed_clause = (-ok) - 2;
            if (ignore_loop_failures) {
                if (!loop_enabled) {
                    // Non-iterative usage: exit silently with success
                    loop_state.exit_code = 0;
                    goto cleanup;
                }
                // Iterative mode: suppress clause failure and keep looping
                loop_state.last_ok = 1;
            } else {
                // Normal error handling: print error and exit with 10+N code
                fprintf(stderr, "fiskta: clause %d failed (%s)\n", failed_clause, err_str(last_err));
                loop_state.exit_code = 10 + failed_clause;
                goto cleanup;
            }
        }

        fflush(stdout);

        if (!loop_enabled) {
            break;
        }

        if (loop_mode == LOOP_MODE_CONTINUE) {
            loop_state.have_saved_vm = true;
            loop_state.data_start = clamp64(loop_state.saved_vm.cursor, 0, data_end);
            if (loop_state.saved_vm.cursor != prev_cursor) {
                loop_state.last_change_ms = now_ms;
            }
        } else if (loop_mode == LOOP_MODE_FOLLOW) {
            loop_state.data_start = data_end;
        } else {
            loop_state.data_start = 0;
        }

        now_ms = now_millis();
        if (idle_timeout_ms >= 0 && (now_ms - loop_state.last_change_ms) >= (u64)idle_timeout_ms) {
            // Idle timeout is considered normal exit (success) since it's the expected behavior
            break;
        }
        if (exec_timeout_ms >= 0 && (now_ms - loop_state.start_ms) >= (u64)exec_timeout_ms) {
            loop_state.exit_reason = EXIT_REASON_EXEC_TIMEOUT;
            break;
        }

        sleep_msec(loop_ms);
    }

cleanup:
    io_close(&io);
    free(block);

    // Exit code priority:
    // 1. If we have a non-zero exit_code set during execution (goto cleanup from error), use it
    // 2. Otherwise, determine exit code from last iteration + timeout state
    if (loop_state.exit_code != 0) {
        return loop_state.exit_code;
    }

    // Compute exit code from last iteration
    int last_iter_code = 0;
    if (loop_state.last_ok > 0) {
        last_iter_code = 0; // Success
    } else if (loop_state.last_ok == 0) {
        last_iter_code = 1; // I/O error
    } else {
        i32 failed_clause = (-loop_state.last_ok) - 2;
        last_iter_code = 10 + failed_clause; // Clause failure
    }

    // If we have an execution timeout (--for), use exit code 5 unless last iteration failed
    if (loop_state.exit_reason == EXIT_REASON_EXEC_TIMEOUT) {
        return loop_state.exit_reason; // Timeout takes priority once triggered
    }

    // Normal exit or --until-idle timeout: use last iteration's result
    return last_iter_code;
}

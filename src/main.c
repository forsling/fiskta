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
    i32 loop_ms;
    bool loop_enabled;
    bool ignore_loop_failures;
    i32 idle_timeout_ms;
    i32 exec_timeout_ms;
    LoopMode loop_mode;
} CliOptions;

typedef struct {
    String* tokens;
    i32 token_count;
    bool tokens_need_conversion;
} Operations;

// Sentinel: means "no saved VM yet"
#define VM_CURSOR_UNSET ((i64)-1)

typedef enum {
    ITER_OK,
    ITER_PROGRAM_FAIL,
    ITER_IO_ERROR
} IterStatus;

typedef struct {
    IterStatus status;
    enum Err last_err;
    i32 emitted_ranges;
} IterResult;

typedef struct {
    bool enabled;
    LoopMode mode;
    i32 loop_ms, idle_timeout_ms, exec_timeout_ms;
    u64 t0_ms, last_activity_ms;
    i64 baseline;     // FOLLOW: last processed end; CONTINUE: unused; MONITOR: 0
    i64 last_size;    // last observed file size
    VM vm;            // CONTINUE state (vm.cursor == VM_CURSOR_UNSET => none)
    IterResult last_result;
    int exit_code;
    int exit_reason;  // 0 normal, 5 exec timeout
} LoopState;

static int parse_time_option(const char* value, const char* opt_name, i32* out);
static int load_ops_from_cli_options(const CliOptions* opts, int ops_index, int argc, char** argv, Operations* out);
static int parse_until_idle_option(const char* value, i32* out);

// Loop context helper functions
static void loop_init(LoopState* state, const CliOptions* opt, File* io);
static void loop_compute_window(LoopState* state, File* io, i64* lo, i64* hi);
static bool loop_should_wait_or_stop(LoopState* state, bool no_new_data, int* out_exit_reason);
static void loop_commit(LoopState* state, i64 data_hi, IterResult result, bool ignore_fail);

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

static int parse_time_option(const char* value, const char* opt_name, i32* out)
{
    if (!value || !opt_name || !out) {
        return 1;
    }

    const unsigned char* p = (const unsigned char*)value;
    if (*p == '\0') {
        fprintf(stderr, "fiskta: %s expects a non-negative integer with suffix (ms|s|m|h)\n", opt_name);
        return 1;
    }

    i32 base = 0;
    while (*p >= '0' && *p <= '9') {
        int digit = (int)(*p - '0');
        if (base > INT_MAX / 10 || (base == INT_MAX / 10 && digit > (INT_MAX % 10))) {
            fprintf(stderr, "fiskta: %s value too large\n", opt_name);
            return 1;
        }
        base = base * 10 + digit;
        p++;
    }

    if (p == (const unsigned char*)value) {
        fprintf(stderr, "fiskta: %s expects a non-negative integer with suffix (ms|s|m|h)\n", opt_name);
        return 1;
    }

    const char* suffix = (const char*)p;
    if (*suffix != '\0' && strcmp(suffix, "ms") != 0 && strcmp(suffix, "s") != 0 &&
        strcmp(suffix, "m") != 0 && strcmp(suffix, "h") != 0) {
        fprintf(stderr, "fiskta: %s invalid suffix '%s' (valid: ms, s, m, h)\n", opt_name, suffix);
        return 1;
    }

    if (base == 0) {
        if (*suffix == '\0' || strcmp(suffix, "ms") == 0 || strcmp(suffix, "s") == 0
            || strcmp(suffix, "m") == 0 || strcmp(suffix, "h") == 0) {
            *out = 0;
            return 0;
        }
        return 1;
    }

    if (*suffix == '\0') {
        fprintf(stderr, "fiskta: %s requires a suffix (ms|s|m|h) for non-zero values\n", opt_name);
        return 1;
    }

    i32 multiplier = 1;
    if (strcmp(suffix, "ms") == 0) {
        multiplier = 1;
    } else if (strcmp(suffix, "s") == 0) {
        multiplier = 1000;
    } else if (strcmp(suffix, "m") == 0) {
        multiplier = 60000;
    } else if (strcmp(suffix, "h") == 0) {
        multiplier = 3600000;
    }

    if (base > 0 && base > INT_MAX / multiplier) {
        fprintf(stderr, "fiskta: %s value too large\n", opt_name);
        return 1;
    }

    *out = base * multiplier;
    return 0;
}

static int parse_until_idle_option(const char* value, i32* out)
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

        i32 n = tokenize_ops_string(file_content_buf, tokens_view, MAX_TOKENS);
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
        out->tokens_need_conversion = false;

    } else if (command_arg) {
        // Load operations from --ops string
        if (ops_index < argc) {
            fprintf(stderr, "fiskta: --ops cannot be combined with positional operations\n");
            return 2;
        }

        i32 n = tokenize_ops_string(command_arg, tokens_view, MAX_TOKENS);
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
        out->tokens_need_conversion = false;

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

// Initialize loop context with default values
static void loop_init(LoopState* state, const CliOptions* opt, File* io)
{
    if (!state || !opt) { return;
}

    memset(state, 0, sizeof *state);
    state->enabled = opt->loop_enabled;
    state->mode = opt->loop_mode;
    state->loop_ms = opt->loop_ms;
    state->idle_timeout_ms = opt->idle_timeout_ms;
    state->exec_timeout_ms = opt->exec_timeout_ms;
    state->t0_ms = state->last_activity_ms = now_millis();
    state->exit_code = 0;
    state->exit_reason = 0;
    state->last_result = (IterResult) {
        .status = ITER_OK,
        .last_err = E_OK,
        .emitted_ranges = 0
    };

    refresh_file_size(io);
    state->last_size = io_size(io);

    state->vm.cursor = VM_CURSOR_UNSET; // replaces have_saved_vm
    for (i32 i = 0; i < MAX_LABELS; i++) { state->vm.label_pos[i] = -1;
}

    // FOLLOW: tail semantics—start at EOF
    state->baseline = (state->enabled && state->mode == LOOP_MODE_FOLLOW) ? state->last_size : 0;
}

// Compute the scan window for this iteration
static void loop_compute_window(LoopState* state, File* io, i64* lo, i64* hi)
{
    refresh_file_size(io);
    i64 size = io_size(io);
    if (size != state->last_size) {
        state->last_size = size;
        state->last_activity_ms = now_millis(); // data arrived/truncated
    }
    if (state->mode == LOOP_MODE_FOLLOW && size < state->baseline) {
        state->baseline = size; // file shrank: restart tail at new EOF
    }
    *hi = size;

    switch (state->mode) {
        case LOOP_MODE_MONITOR: *lo = 0; break;
        case LOOP_MODE_FOLLOW:  *lo = state->baseline; break;
        case LOOP_MODE_CONTINUE:
            if (state->vm.cursor != VM_CURSOR_UNSET) {
                *lo = clamp64(state->vm.cursor, 0, size);
            } else {
                *lo = 0;
}
            break;
        default: *lo = 0; break; // Should never happen, but prevents uninitialized warning
    }
    if (*lo > *hi) { *lo = *hi; // truncation safety
}
}

// Determine if we should wait or stop
static bool loop_should_wait_or_stop(LoopState* state, bool no_new_data, int* out_exit_reason)
{
    if (out_exit_reason) {
        *out_exit_reason = 0; // default: normal stop
    }
    u64 now = now_millis();
    if (state->exec_timeout_ms >= 0 && now - state->t0_ms >= (u64)state->exec_timeout_ms) {
        if (out_exit_reason) {
            *out_exit_reason = 5; // EXEC TIMEOUT
        }
        return false; // stop
    }
    if (!state->enabled) { return false; // one-shot
}

    if (no_new_data) {
        if (state->idle_timeout_ms >= 0 && now - state->last_activity_ms >= (u64)state->idle_timeout_ms) {
            if (out_exit_reason) {
                *out_exit_reason = 0; // normal (idle)
            }
            return false; // stop
        }
        sleep_msec(state->loop_ms);
        return true; // wait & continue
    }
    return false; // have work, go execute
}

// Apply results back to state
static void loop_commit(LoopState* state, i64 data_hi, IterResult result, bool ignore_fail)
{
    state->last_result = result;

    switch (result.status) {
    case ITER_OK:
        // success: mark activity and bump baselines
        state->last_activity_ms = now_millis();
        if (state->mode == LOOP_MODE_CONTINUE) {
            if (state->vm.cursor != VM_CURSOR_UNSET) {
                // baseline becomes new cursor for next pass
                state->baseline = clamp64(state->vm.cursor, 0, data_hi);
            }
        } else if (state->mode == LOOP_MODE_FOLLOW) {
            state->baseline = data_hi; // tail at EOF
        } else { /* MONITOR: stays 0 */ }
        state->exit_code = 0;
        break;
    case ITER_PROGRAM_FAIL:
        if (ignore_fail && state->enabled) {
            // treat as success to keep looping
            state->last_result.status = ITER_OK;
            state->last_result.last_err = E_OK;
            state->last_result.emitted_ranges = 0;
            state->exit_code = 0;
        } else {
            state->exit_code = FISKTA_EXIT_PROGRAM_FAIL; // program failed (no clause succeeded)
        }
        break;
    case ITER_IO_ERROR:
        state->exit_code = 1;
        break;
    }
}

static IterResult execute_program_iteration(const Program* prg, File* io, VM* vm,
    Range* clause_ranges, LabelWrite* clause_labels,
    i64 data_lo, i64 data_hi)
{
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

    bool any_success = false;
    enum Err last_err = E_OK;
    IterResult iter_result = {
        .status = ITER_OK,
        .last_err = E_OK,
        .emitted_ranges = 0
    };

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
                iter_result.emitted_ranges++;
            }
            if (e == E_OK) {
                // Commit staged VM state now that I/O succeeded
                commit_labels(vm_exec, result.label_writes, result.label_count);
                vm_exec->cursor = result.staged_vm.cursor;
                vm_exec->last_match = result.staged_vm.last_match;
                vm_exec->view = result.staged_vm.view;
                any_success = true;
            }
        }

        if (e == E_OK) {
            if (prg->clauses[ci].link == LINK_OR) {
                // Skip remaining alternatives in this OR-chain once one succeeds
                while (ci + 1 < prg->clause_count && prg->clauses[ci].link == LINK_OR) {
                    ci++;
                }
            }
        } else {
            last_err = e;
            if (e == E_IO) {
                iter_result.status = ITER_IO_ERROR;
                iter_result.last_err = E_IO;
                break;
            }
        }
    }

    if (iter_result.status == ITER_IO_ERROR) {
        return iter_result;
    }
    if (any_success) {
        iter_result.status = ITER_OK;
        iter_result.last_err = E_OK;
    } else {
        iter_result.status = ITER_PROGRAM_FAIL;
        iter_result.last_err = (last_err != E_OK) ? last_err : E_FAIL_OP;
    }
    return iter_result;
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
    bool ignore_loop_failures = cli_opts.ignore_loop_failures;

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
    LoopState loop_state;
    loop_init(&loop_state, &cli_opts, &io);

    for (;;) {
        // Handle --for timeout even if no iteration has executed yet
        int reason = 0;
        (void)loop_should_wait_or_stop(&loop_state, /*no_new_data=*/false, &reason);
        if (reason == 5) {
            loop_state.exit_reason = reason;
            break;
        }

        i64 lo;
        i64 hi;
        loop_compute_window(&loop_state, &io, &lo, &hi);

        // FOLLOW/CONTINUE: nothing new? maybe wait/stop
        if (loop_state.enabled && loop_state.idle_timeout_ms != 0 &&
            (loop_state.mode == LOOP_MODE_FOLLOW || loop_state.mode == LOOP_MODE_CONTINUE) && lo >= hi) {
            reason = 0;
            if (loop_should_wait_or_stop(&loop_state, /*no_new_data=*/true, &reason)) {
                continue; // just slept; try again
            }
            loop_state.exit_reason = reason; // 0 => idle stop, 5 => exec timeout
            break;
        }

        // CONTINUE passes saved VM; other modes run with ephemeral VM
        VM* vm_ptr = (loop_state.mode == LOOP_MODE_CONTINUE) ? &loop_state.vm : NULL;
        IterResult iteration = execute_program_iteration(&prg, &io, vm_ptr,
                                                         clause_ranges, clause_labels,
                                                         lo, hi);

        loop_commit(&loop_state, hi, iteration, ignore_loop_failures);
        fflush(stdout);

        if (!loop_state.enabled || loop_state.exit_code) {
            break;
        }

        // Special case: --until-idle 0 => stop after a single iteration (all modes).
        if (loop_state.idle_timeout_ms == 0) {
            loop_state.exit_reason = 0; // normal stop
            break;
        }

        // Throttle non-FOLLOW modes between passes; FOLLOW sleeps only when there's no new data.
        if (loop_state.mode != LOOP_MODE_FOLLOW) {
            if (loop_state.loop_ms > 0) {
                sleep_msec(loop_state.loop_ms);
            }
            // still honor --for (exec timeout)
            reason = 0;
            (void)loop_should_wait_or_stop(&loop_state, /*no_new_data=*/false, &reason); // only checks exec timeout here
            if ((loop_state.exit_reason = reason) != 0) {
                break;
            }
        }
    }

    io_close(&io);
    free(block);

    if (loop_state.exit_code) { return loop_state.exit_code;
}
    if (loop_state.exit_reason == 5) { return 5; // --for timeout
}

    // Otherwise evaluate last iteration outcome
    switch (loop_state.last_result.status) {
    case ITER_OK: return 0;
    case ITER_IO_ERROR: return 1;
    case ITER_PROGRAM_FAIL: return FISKTA_EXIT_PROGRAM_FAIL;
    default: return 1;
    }
}

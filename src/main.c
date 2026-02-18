#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cli_help.h"
#include "fiskta.h"
#include "fiskta_types.h"
#include "util.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FISKTA_VERSION
#define FISKTA_VERSION "dev"
#endif

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

// Error printing for CLI
static void print_err(enum FisktaErr e)
{
    i32 position = fiskta_error_position();
    const char* message = fiskta_error_message();

    fprintf(stderr, "fiskta: %s", fiskta_err_str(e));

    if (message && fiskta_error_code() == e) {
        if (position >= 0) {
            fprintf(stderr, ": %s (token %d)", message, position + 1);
        } else {
            fprintf(stderr, ": %s", message);
        }
    }

    fputc('\n', stderr);
}

typedef struct {
    FisktaString* tokens;
    i32 token_count;
} Operations;

// Forward declarations
static int parse_time_option(const char* value, const char* opt_name, i32* out);
static int load_ops_from_cli_options(const char* ops_arg, const char* ops_file, int ops_index, int argc, char** argv, Operations* out);
static int parse_until_idle_option(const char* value, i32* out);

static bool parse_cli_args(int argc, char** argv,
    FisktaRuntimeConfig* config_out,
    const char** input_path_out,
    const char** ops_arg_out,
    const char** ops_file_out,
    int* ops_index,
    int* exit_code_out)
{
    if (!config_out || !input_path_out || !ops_arg_out || !ops_file_out || !ops_index || !exit_code_out) {
        return false;
    }

    FisktaRuntimeConfig cfg = {
        .loop_ms = 0,
        .loop_enabled = false,
        .ignore_loop_failures = false,
        .idle_timeout_ms = -1,
        .exec_timeout_ms = -1
    };
    const char* input_path = "-";
    const char* ops_arg = NULL;
    const char* ops_file = NULL;

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
            *exit_code_out = FISKTA_EXIT_OK;
            return false;
        }
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--version") == 0) {
            printf("fiskta - (fi)nd (sk)ip (ta)ke v%s\n", FISKTA_VERSION);
            *exit_code_out = FISKTA_EXIT_OK;
            return false;
        }
        if (strcmp(arg, "-i") == 0 || strcmp(arg, "--input") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --input requires a path\n");
                *exit_code_out = FISKTA_EXIT_USAGE;
                return false;
            }
            input_path = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--input=", 8) == 0) {
            if (arg[8] == '\0') {
                fprintf(stderr, "fiskta: --input requires a path\n");
                *exit_code_out = FISKTA_EXIT_USAGE;
                return false;
            }
            input_path = arg + 8;
            argi++;
            continue;
        }
        if (strcmp(arg, "-u") == 0 || strcmp(arg, "--until-idle") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: -u/--until-idle requires a value\n");
                *exit_code_out = FISKTA_EXIT_USAGE;
                return false;
            }
            if (parse_until_idle_option(argv[argi + 1], &cfg.idle_timeout_ms) != 0) {
                *exit_code_out = FISKTA_EXIT_USAGE;
                return false;
            }
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--until-idle=", 13) == 0) {
            if (parse_until_idle_option(arg + 13, &cfg.idle_timeout_ms) != 0) {
                *exit_code_out = FISKTA_EXIT_USAGE;
                return false;
            }
            argi++;
            continue;
        }
        if (strncmp(arg, "-u", 2) == 0 && arg[2] != '\0') {
            const char* value = arg + 2;
            if (value[0] == '=') {
                value++;
            }
            if (value[0] == '\0') {
                fprintf(stderr, "fiskta: -u/--until-idle requires a value\n");
                *exit_code_out = FISKTA_EXIT_USAGE;
                return false;
            }
            if (parse_until_idle_option(value, &cfg.idle_timeout_ms) != 0) {
                *exit_code_out = FISKTA_EXIT_USAGE;
                return false;
            }
            argi++;
            continue;
        }
        if (strcmp(arg, "--for") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --for requires a value\n");
                *exit_code_out = FISKTA_EXIT_USAGE;
                return false;
            }
            if (parse_time_option(argv[argi + 1], "--for", &cfg.exec_timeout_ms) != 0) {
                *exit_code_out = FISKTA_EXIT_USAGE;
                return false;
            }
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--for=", 6) == 0) {
            if (parse_time_option(arg + 6, "--for", &cfg.exec_timeout_ms) != 0) {
                *exit_code_out = FISKTA_EXIT_USAGE;
                return false;
            }
            argi++;
            continue;
        }
        if (strcmp(arg, "-c") == 0 || strcmp(arg, "--continue") == 0
            || strcmp(arg, "-C") == 0 || strcmp(arg, "--continue-on-fail") == 0) {
            cfg.loop_enabled = true;
            if (arg[1] == 'C' || strstr(arg, "-on-fail")) {
                cfg.ignore_loop_failures = true;
            }
            // Optional time value follows. Only consume if it looks like a time token.
            if (argi + 1 < argc) {
                const char* val = argv[argi + 1];
                // Quick lookahead without emitting parse errors
                bool ok = false;
                if (val && *val) {
                    const unsigned char* p = (const unsigned char*)val;
                    int base = 0;
                    while (*p >= '0' && *p <= '9') {
                        base = base * 10 + (int)(*p - '0');
                        p++;
                    }
                    if (p != (const unsigned char*)val) {
                        const char* suf = (const char*)p;
                        if (*suf == '\0') {
                            ok = (base == 0);
                        } else if (strcmp(suf, "ms") == 0 || strcmp(suf, "s") == 0 || strcmp(suf, "m") == 0 || strcmp(suf, "h") == 0) {
                            ok = true;
                        }
                    }
                }
                if (ok) {
                    if (parse_time_option(val, arg, &cfg.loop_ms) != 0) {
                        *exit_code_out = FISKTA_EXIT_USAGE;
                        return false;
                    }
                    argi += 2;
                    continue;
                }
            }
            // No interval provided: default tight loop
            cfg.loop_ms = 0;
            argi++;
            continue;
        }
        if (strcmp(arg, "--ops") == 0) {
            if (ops_arg || ops_file) {
                fprintf(stderr, "fiskta: --ops specified multiple times\n");
                *exit_code_out = FISKTA_EXIT_USAGE;
                return false;
            }
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --ops requires a string\n");
                *exit_code_out = FISKTA_EXIT_USAGE;
                return false;
            }
            ops_arg = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (strcmp(arg, "--ops-file") == 0) {
            if (ops_arg || ops_file) {
                fprintf(stderr, "fiskta: --ops-file conflicts with previous --ops/--ops-file\n");
                *exit_code_out = FISKTA_EXIT_USAGE;
                return false;
            }
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --ops-file requires a path\n");
                *exit_code_out = FISKTA_EXIT_USAGE;
                return false;
            }
            ops_file = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (arg[0] == '-') {
            if (arg[1] == '\0' || isdigit((unsigned char)arg[1])) {
                break;
            }
            fprintf(stderr, "fiskta: unknown option %s\n", arg);
            *exit_code_out = FISKTA_EXIT_USAGE;
            return false;
        }
        break;
    }

    *config_out = cfg;
    *input_path_out = input_path;
    *ops_arg_out = ops_arg;
    *ops_file_out = ops_file;
    *ops_index = argi;
    return true;
}

enum {
    MAX_TOKENS = 1024,
    MAX_NEEDLE_BYTES = 16384
};

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
        if (base > INT32_MAX / 10 || (base == INT32_MAX / 10 && digit > (INT32_MAX % 10))) {
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
    if (*suffix != '\0' && strcmp(suffix, "ms") != 0 && strcmp(suffix, "s") != 0 && strcmp(suffix, "m") != 0 && strcmp(suffix, "h") != 0) {
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

    if (base > 0 && base > INT32_MAX / multiplier) {
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
static int load_ops_from_cli_options(const char* ops_arg, const char* ops_file, int ops_index, int argc, char** argv, Operations* out)
{
    if (!out) {
        return FISKTA_EXIT_PARSE;
    }

    // Static buffers for operations loading
    static char file_content_buf[MAX_NEEDLE_BYTES];
    static FisktaString tokens_view[MAX_TOKENS];
    static char tokenize_scratch[MAX_NEEDLE_BYTES];

    if (ops_file) {
        // Load operations from file
        if (ops_index < argc) {
            fprintf(stderr, "fiskta: --ops cannot be combined with positional operations\n");
            return FISKTA_EXIT_USAGE;
        }

        FILE* cf = fopen(ops_file, "rb");
        if (!cf) {
            fprintf(stderr, "fiskta: unable to open ops file %s\n", ops_file);
            return FISKTA_EXIT_IO;
        }

        size_t total = fread(file_content_buf, 1, sizeof(file_content_buf) - 1, cf);
        if (ferror(cf)) {
            fclose(cf);
            fprintf(stderr, "fiskta: error reading ops file %s\n", ops_file);
            return FISKTA_EXIT_IO;
        }
        if (!feof(cf)) {
            fclose(cf);
            fprintf(stderr, "fiskta: operations file too long (max %d bytes)\n", MAX_NEEDLE_BYTES);
            return FISKTA_EXIT_CAPACITY;
        }
        fclose(cf);

        file_content_buf[total] = '\0';
        if (total == 0) {
            fprintf(stderr, "fiskta: empty ops file\n");
            return FISKTA_EXIT_USAGE;
        }

        i32 n = tokenize_ops_string(file_content_buf, tokens_view, MAX_TOKENS, tokenize_scratch, sizeof(tokenize_scratch));
        if (n == -1) {
            fprintf(stderr, "fiskta: operations string too long (max %d bytes)\n", MAX_NEEDLE_BYTES);
            return FISKTA_EXIT_CAPACITY;
        }
        if (n <= 0) {
            fprintf(stderr, "fiskta: empty ops string\n");
            return FISKTA_EXIT_USAGE;
        }

        out->tokens = tokens_view;
        out->token_count = n;
        /* no-op */

    } else if (ops_arg) {
        // Load operations from --ops string
        if (ops_index < argc) {
            fprintf(stderr, "fiskta: --ops cannot be combined with positional operations\n");
            return FISKTA_EXIT_USAGE;
        }

        i32 n = tokenize_ops_string(ops_arg, tokens_view, MAX_TOKENS, tokenize_scratch, sizeof(tokenize_scratch));
        if (n == -1) {
            fprintf(stderr, "fiskta: operations string too long (max %d bytes)\n", MAX_NEEDLE_BYTES);
            return FISKTA_EXIT_CAPACITY;
        }
        if (n <= 0) {
            fprintf(stderr, "fiskta: empty ops string\n");
            return FISKTA_EXIT_USAGE;
        }

        out->tokens = tokens_view;
        out->token_count = n;
        /* no-op */

    } else {
        // Load operations from positional arguments
        i32 token_count = (i32)(argc - ops_index);
        if (token_count <= 0) {
            fprintf(stderr, "fiskta: missing operations\n");
            fprintf(stderr, "Try 'fiskta --help' for more information.\n");
            return FISKTA_EXIT_USAGE;
        }

        char** tokens = argv + ops_index;
        if (token_count == 1 && strchr(tokens[0], ' ')) {
            // Single token with spaces - use optimized tokenizer
            i32 n = tokenize_ops_string(tokens[0], tokens_view, MAX_TOKENS, tokenize_scratch, sizeof(tokenize_scratch));
            if (n == -1) {
                fprintf(stderr, "fiskta: operations string too long (max %d bytes)\n", MAX_NEEDLE_BYTES);
                return FISKTA_EXIT_CAPACITY;
            }
            if (n <= 0) {
                fprintf(stderr, "fiskta: empty operations string\n");
                return FISKTA_EXIT_USAGE;
            }
            out->tokens = tokens_view;
            out->token_count = n;
            /* no-op */
        } else {
            // Multiple tokens or single token without spaces
            if (token_count > MAX_TOKENS) {
                fprintf(stderr, "fiskta: too many operation tokens (max %d)\n", MAX_TOKENS);
                return FISKTA_EXIT_CAPACITY;
            }
            out->tokens = tokens_view;
            out->token_count = token_count;
            convert_tokens_to_strings(tokens, token_count, tokens_view);
        }
    }

    return FISKTA_EXIT_OK; // Success
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    /************************
     * CLI ARGUMENT PARSING *
     ************************/
    FisktaRuntimeConfig config;
    const char* input_path = NULL;
    const char* ops_arg = NULL;
    const char* ops_file = NULL;
    int ops_index = 0;
    int parse_exit = -1;
    if (!parse_cli_args(argc, argv, &config, &input_path, &ops_arg, &ops_file, &ops_index, &parse_exit)) {
        return (parse_exit >= 0) ? parse_exit : FISKTA_EXIT_OK;
    }

    /***************************
     * OPERATION TOKEN PARSING *
     ***************************/
    Operations ops;
    int ops_result = load_ops_from_cli_options(ops_arg, ops_file, ops_index, argc, argv, &ops);
    if (ops_result != FISKTA_EXIT_OK) {
        return ops_result;
    }

    /*******************************
     * PROGRAM MEMORY REQUIREMENTS *
     *******************************/
    FisktaBuildOptions build_opts = { 0 }; // Use defaults
    FisktaRuntimeRequirements req;
    int ret = fiskta_program_requirements(ops.token_count, ops.tokens, &build_opts, &req);
    if (ret != FISKTA_EXIT_OK) {
        if (fiskta_error_code() != FISKTA_E_OK) {
            print_err(fiskta_error_code());
        }
        return ret;
    }

    /********************
     * ARENA ALLOCATION *
     ********************/
    void* arena = malloc(req.arena_bytes);
    if (!arena) {
        fprintf(stderr, "fiskta: failed to allocate %zu bytes\n", req.arena_bytes);
        return FISKTA_EXIT_RESOURCE;
    }

    /***************************
     * BUILD PROGRAM (COMPILE) *
     ***************************/
    FisktaProgram prog;
    FisktaRuntimeBuffers buffers;
    ret = fiskta_build_program(ops.token_count, ops.tokens, &build_opts, &prog, arena, req.arena_bytes, &buffers);
    if (ret != FISKTA_EXIT_OK) {
        if (fiskta_error_code() != FISKTA_E_OK) {
            print_err(fiskta_error_code());
        }
        free(arena);
        return ret;
    }

    /*******************
     * EXECUTE PROGRAM *
     *******************/
    ret = fiskta_runtime_execute(&prog, input_path, &buffers, &config);
    if (ret != FISKTA_EXIT_OK && ret != FISKTA_EXIT_PROGRAM_FAIL) {
        if (fiskta_error_code() != FISKTA_E_OK) {
            print_err(fiskta_error_code());
        }
    }

    /**************
     * CLEANUP    *
     **************/
    free(arena);

    return ret;
}

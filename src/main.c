#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cli_help.h"
#include "error.h"
#include "fiskta.h"
#include "runtime.h"
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

typedef struct {
    String* tokens;
    i32 token_count;
    bool tokens_need_conversion;
} Operations;

// Forward declarations
static int parse_time_option(const char* value, const char* opt_name, i32* out);
static int load_ops_from_cli_options(const char* ops_arg, const char* ops_file, int ops_index, int argc, char** argv, Operations* out);
static int parse_until_idle_option(const char* value, i32* out);

static bool parse_cli_args(int argc, char** argv,
    RuntimeConfig* config_out,
    const char** ops_arg_out,
    const char** ops_file_out,
    int* ops_index,
    int* exit_code_out)
{
    if (!config_out || !ops_arg_out || !ops_file_out || !ops_index || !exit_code_out) {
        return false;
    }

    RuntimeConfig cfg = {
        .input_path = "-",
        .loop_ms = 0,
        .loop_enabled = false,
        .ignore_loop_failures = false,
        .idle_timeout_ms = -1,
        .exec_timeout_ms = -1,
        .loop_mode = LOOP_MODE_CONTINUE
    };
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
                *exit_code_out = FISKTA_EXIT_PARSE;
                return false;
            }
            cfg.input_path = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--input=", 8) == 0) {
            if (arg[8] == '\0') {
                fprintf(stderr, "fiskta: --input requires a path\n");
                *exit_code_out = FISKTA_EXIT_PARSE;
                return false;
            }
            cfg.input_path = arg + 8;
            argi++;
            continue;
        }
        if (strcmp(arg, "--every") == 0) {
            cfg.loop_enabled = true;
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --every requires a time value\n");
                *exit_code_out = FISKTA_EXIT_PARSE;
                return false;
            }
            if (parse_time_option(argv[argi + 1], "--every", &cfg.loop_ms) != 0) {
                *exit_code_out = FISKTA_EXIT_PARSE;
                return false;
            }
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--every=", 8) == 0) {
            cfg.loop_enabled = true;
            if (parse_time_option(arg + 8, "--every", &cfg.loop_ms) != 0) {
                *exit_code_out = FISKTA_EXIT_PARSE;
                return false;
            }
            argi++;
            continue;
        }
        if (strcmp(arg, "-u") == 0 || strcmp(arg, "--until-idle") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: -u/--until-idle requires a value\n");
                *exit_code_out = FISKTA_EXIT_PARSE;
                return false;
            }
            if (parse_until_idle_option(argv[argi + 1], &cfg.idle_timeout_ms) != 0) {
                *exit_code_out = FISKTA_EXIT_PARSE;
                return false;
            }
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--until-idle=", 13) == 0) {
            if (parse_until_idle_option(arg + 13, &cfg.idle_timeout_ms) != 0) {
                *exit_code_out = FISKTA_EXIT_PARSE;
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
                *exit_code_out = FISKTA_EXIT_PARSE;
                return false;
            }
            if (parse_until_idle_option(value, &cfg.idle_timeout_ms) != 0) {
                *exit_code_out = FISKTA_EXIT_PARSE;
                return false;
            }
            argi++;
            continue;
        }
        if (strcmp(arg, "--for") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --for requires a value\n");
                *exit_code_out = FISKTA_EXIT_PARSE;
                return false;
            }
            if (parse_time_option(argv[argi + 1], "--for", &cfg.exec_timeout_ms) != 0) {
                *exit_code_out = FISKTA_EXIT_PARSE;
                return false;
            }
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--for=", 6) == 0) {
            if (parse_time_option(arg + 6, "--for", &cfg.exec_timeout_ms) != 0) {
                *exit_code_out = FISKTA_EXIT_PARSE;
                return false;
            }
            argi++;
            continue;
        }
        if (strcmp(arg, "-m") == 0 || strcmp(arg, "--monitor") == 0) {
            cfg.loop_mode = LOOP_MODE_MONITOR;
            cfg.loop_enabled = true;
            argi++;
            continue;
        }
        if (strcmp(arg, "-c") == 0 || strcmp(arg, "--continue") == 0) {
            cfg.loop_mode = LOOP_MODE_CONTINUE;
            cfg.loop_enabled = true;
            argi++;
            continue;
        }
        if (strcmp(arg, "-f") == 0 || strcmp(arg, "--follow") == 0) {
            cfg.loop_mode = LOOP_MODE_FOLLOW;
            cfg.loop_enabled = true;
            argi++;
            continue;
        }
        if (strcmp(arg, "-k") == 0 || strcmp(arg, "--ignore-failures") == 0) {
            cfg.ignore_loop_failures = true;
            argi++;
            continue;
        }
        if (strcmp(arg, "--ops") == 0) {
            if (ops_arg || ops_file) {
                fprintf(stderr, "fiskta: --ops specified multiple times\n");
                *exit_code_out = FISKTA_EXIT_PARSE;
                return false;
            }
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta: --ops requires a string\n");
                *exit_code_out = FISKTA_EXIT_PARSE;
                return false;
            }
            const char* value = argv[argi + 1];
            FILE* test = fopen(value, "rb");
            if (test) {
                fclose(test);
                ops_file = value;
            } else {
                ops_arg = value;
            }
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--ops=", 6) == 0) {
            if (ops_arg || ops_file) {
                fprintf(stderr, "fiskta: --ops specified multiple times\n");
                *exit_code_out = FISKTA_EXIT_PARSE;
                return false;
            }
            if (arg[6] == '\0') {
                fprintf(stderr, "fiskta: --ops requires a string\n");
                *exit_code_out = FISKTA_EXIT_PARSE;
                return false;
            }
            const char* value = arg + 6;
            FILE* test = fopen(value, "rb");
            if (test) {
                fclose(test);
                ops_file = value;
            } else {
                ops_arg = value;
            }
            argi++;
            continue;
        }
        if (arg[0] == '-') {
            if (arg[1] == '\0' || isdigit((unsigned char)arg[1])) {
                break;
            }
            fprintf(stderr, "fiskta: unknown option %s\n", arg);
            *exit_code_out = FISKTA_EXIT_PARSE;
            return false;
        }
        break;
    }

    *config_out = cfg;
    *ops_arg_out = ops_arg;
    *ops_file_out = ops_file;
    *ops_index = argi;
    return true;
}

enum {
    MAX_TOKENS = 1024,
    MAX_NEEDLE_BYTES = 4096
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
static int load_ops_from_cli_options(const char* ops_arg, const char* ops_file, int ops_index, int argc, char** argv, Operations* out)
{
    if (!out) {
        return FISKTA_EXIT_PARSE;
    }

    // Static buffers for operations loading
    static char file_content_buf[MAX_NEEDLE_BYTES];
    static String tokens_view[MAX_TOKENS];

    if (ops_file) {
        // Load operations from file
        if (ops_index < argc) {
            fprintf(stderr, "fiskta: --ops cannot be combined with positional operations\n");
            return FISKTA_EXIT_PARSE;
        }

        FILE* cf = fopen(ops_file, "rb");
        if (!cf) {
            fprintf(stderr, "fiskta: unable to open ops file %s\n", ops_file);
            return FISKTA_EXIT_PARSE;
        }

        size_t total = fread(file_content_buf, 1, sizeof(file_content_buf) - 1, cf);
        if (ferror(cf)) {
            fclose(cf);
            fprintf(stderr, "fiskta: error reading ops file %s\n", ops_file);
            return FISKTA_EXIT_PARSE;
        }
        if (!feof(cf)) {
            fclose(cf);
            fprintf(stderr, "fiskta: operations file too long (max %d bytes)\n", MAX_NEEDLE_BYTES);
            return FISKTA_EXIT_PARSE;
        }
        fclose(cf);

        file_content_buf[total] = '\0';
        if (total == 0) {
            fprintf(stderr, "fiskta: empty ops file\n");
            return FISKTA_EXIT_PARSE;
        }

        i32 n = tokenize_ops_string(file_content_buf, tokens_view, MAX_TOKENS);
        if (n == -1) {
            fprintf(stderr, "fiskta: operations string too long (max %d bytes)\n", MAX_NEEDLE_BYTES);
            return FISKTA_EXIT_PARSE;
        }
        if (n <= 0) {
            fprintf(stderr, "fiskta: empty ops string\n");
            return FISKTA_EXIT_PARSE;
        }

        out->tokens = tokens_view;
        out->token_count = n;
        out->tokens_need_conversion = false;

    } else if (ops_arg) {
        // Load operations from --ops string
        if (ops_index < argc) {
            fprintf(stderr, "fiskta: --ops cannot be combined with positional operations\n");
            return FISKTA_EXIT_PARSE;
        }

        i32 n = tokenize_ops_string(ops_arg, tokens_view, MAX_TOKENS);
        if (n == -1) {
            fprintf(stderr, "fiskta: operations string too long (max %d bytes)\n", MAX_NEEDLE_BYTES);
            return FISKTA_EXIT_PARSE;
        }
        if (n <= 0) {
            fprintf(stderr, "fiskta: empty ops string\n");
            return FISKTA_EXIT_PARSE;
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
            return FISKTA_EXIT_PARSE;
        }

        char** tokens = argv + ops_index;
        if (token_count == 1 && strchr(tokens[0], ' ')) {
            // Single token with spaces - use optimized tokenizer
            i32 n = tokenize_ops_string(tokens[0], tokens_view, MAX_TOKENS);
            if (n == -1) {
                fprintf(stderr, "fiskta: operations string too long (max %d bytes)\n", MAX_NEEDLE_BYTES);
                return FISKTA_EXIT_PARSE;
            }
            if (n <= 0) {
                fprintf(stderr, "fiskta: empty operations string\n");
                return FISKTA_EXIT_PARSE;
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

    return FISKTA_EXIT_OK; // Success
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    /***********************
     * CLI ARGUMENT PARSING
     ***********************/
    RuntimeConfig config;
    const char* ops_arg = NULL;
    const char* ops_file = NULL;
    int ops_index = 0;
    int parse_exit = -1;
    if (!parse_cli_args(argc, argv, &config, &ops_arg, &ops_file, &ops_index, &parse_exit)) {
        return (parse_exit >= 0) ? parse_exit : FISKTA_EXIT_OK;
    }

    /**************************
     * OPERATION TOKEN PARSING
     **************************/
    Operations ops;
    int ops_result = load_ops_from_cli_options(ops_arg, ops_file, ops_index, argc, argv, &ops);
    if (ops_result != FISKTA_EXIT_OK) {
        return ops_result;
    }

    return run_program(ops.token_count, ops.tokens, &config);
}

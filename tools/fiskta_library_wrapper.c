// fiskta_library_wrapper.c
//
// CLI wrapper that uses the library API internally.
// This allows the test suite to validate the library interface
// by running all 633 tests through fiskta_runtime_execute_buffer() and callbacks.

#include "fiskta.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

// Output callback: write to stdout
static void output_to_stdout(const void* data, size_t len, void* userdata)
{
    (void)userdata;
    fwrite(data, 1, len, stdout);
}

// Error callback: write to stderr in fiskta format (match fiskta.c:print_err)
static void error_to_stderr(enum Err err, const char* context,
    i32 position, const char* message, void* userdata)
{
    (void)userdata;

    fprintf(stderr, "fiskta: %s", fiskta_err_str(err));

    if (message && message[0]) {
        if (position >= 0) {
            fprintf(stderr, ": %s (token %d)", message, position + 1);
        } else {
            fprintf(stderr, ": %s", message);
        }
    }

    fprintf(stderr, "\n");
}

// Print last error in the same format as CLI (used when a call returns non-OK)
static void print_last_error_cli_like(void)
{
    enum Err e = fiskta_error_code();
    if (e == E_OK) return;
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

// Read entire file into memory
static unsigned char* read_file_to_memory(const char* path, size_t* len_out)
{
    FILE* f;

    if (strcmp(path, "-") == 0) {
        f = stdin;
    } else {
        f = fopen(path, "rb");
        if (!f) {
            return NULL;
        }
    }

    // Get file size
    if (f != stdin) {
        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            return NULL;
        }
        long size = ftell(f);
        if (size < 0) {
            fclose(f);
            return NULL;
        }
        if (fseek(f, 0, SEEK_SET) != 0) {
            fclose(f);
            return NULL;
        }

        unsigned char* buffer = malloc((size_t)size);
        if (!buffer) {
            fclose(f);
            return NULL;
        }

        size_t nread = fread(buffer, 1, (size_t)size, f);
        fclose(f);

        *len_out = nread;
        return buffer;
    } else {
        // Read stdin in chunks
        size_t capacity = 4096;
        size_t length = 0;
        unsigned char* buffer = malloc(capacity);
        if (!buffer) {
            return NULL;
        }

        while (1) {
            size_t space = capacity - length;
            if (space < 1024) {
                capacity *= 2;
                unsigned char* new_buffer = realloc(buffer, capacity);
                if (!new_buffer) {
                    free(buffer);
                    return NULL;
                }
                buffer = new_buffer;
                space = capacity - length;
            }

            size_t nread = fread(buffer + length, 1, space, stdin);
            length += nread;

            if (nread == 0) {
                break;
            }
        }

        *len_out = length;
        return buffer;
    }
}

static void usage(const char* prog)
{
    fprintf(stderr, "Usage: %s [options] <operations...>\n", prog);
    fprintf(stderr, "\nLibrary wrapper for testing fiskta library API\n");
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --input FILE         Read from FILE (or '-' for stdin)\n");
    fprintf(stderr, "  --continue [delay]   Loop mode with optional delay (ms|s|m|h)\n");
    fprintf(stderr, "  --until-idle TIME    Exit after TIME with no new data (requires --continue)\n");
    fprintf(stderr, "  --for TIME           Exit after TIME elapsed (execution timeout)\n");
    fprintf(stderr, "  --ignore-failures    Continue on clause failure in loop mode\n");
    fprintf(stderr, "  --help               Show this help\n");
}

// Parse time string like "100ms", "5s", "1m", "2h" or plain number (ms)
static i32 parse_time_ms(const char* str)
{
    if (!str) {
        return 0;
    }

    char* end;
    long val = strtol(str, &end, 10);
    if (val < 0) {
        return -1;
    }

    if (*end == '\0') {
        // Plain number, assume milliseconds
        return (i32)val;
    }

    // Check suffix
    if (strcmp(end, "ms") == 0) {
        return (i32)val;
    } else if (strcmp(end, "s") == 0) {
        return (i32)(val * 1000);
    } else if (strcmp(end, "m") == 0) {
        return (i32)(val * 60 * 1000);
    } else if (strcmp(end, "h") == 0) {
        return (i32)(val * 60 * 60 * 1000);
    }

    return -1; // Invalid suffix
}

int main(int argc, char** argv)
{
    const char* input_path = NULL;
    const char* ops_file = NULL;
    const char* ops_string = NULL;
    bool loop_enabled = false;
    bool ignore_loop_failures = false;
    i32 loop_ms = 0;
    i32 idle_timeout_ms = -1;
    i32 exec_timeout_ms = -1;
    int argi = 1;

    // Manual argument parsing (like main.c) to handle optional arguments properly
    while (argi < argc) {
        const char* arg = argv[argi];

        // Stop at first non-option
        if (arg[0] != '-') {
            break;
        }

        // --help
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            return 0;
        }

        // --input PATH or --input=PATH
        if (strcmp(arg, "--input") == 0 || strcmp(arg, "-i") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta_library_wrapper: --input requires a value\n");
                return 7;
            }
            input_path = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--input=", 8) == 0) {
            input_path = arg + 8;
            argi++;
            continue;
        }

        // --continue [delay] or --continue=delay
        if (strcmp(arg, "--continue") == 0) {
            loop_enabled = true;
            // Check if next arg is a time value (not an operation token)
            if (argi + 1 < argc && argv[argi + 1][0] != '-') {
                const char* next = argv[argi + 1];
                // Try to parse as time; if it fails, assume it's an operation
                i32 ms = parse_time_ms(next);
                if (ms >= 0) {
                    loop_ms = ms;
                    argi += 2;
                    continue;
                }
            }
            // No delay specified, use 0 (tight loop)
            loop_ms = 0;
            argi++;
            continue;
        }
        if (strncmp(arg, "--continue=", 11) == 0) {
            loop_enabled = true;
            loop_ms = parse_time_ms(arg + 11);
            if (loop_ms < 0) {
                fprintf(stderr, "fiskta_library_wrapper: invalid time format: %s\n", arg + 11);
                return 7;
            }
            argi++;
            continue;
        }

        // --until-idle N or --until-idle=N
        if (strcmp(arg, "--until-idle") == 0 || strcmp(arg, "-u") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta_library_wrapper: --until-idle requires a value\n");
                return 7;
            }
            idle_timeout_ms = parse_time_ms(argv[argi + 1]);
            if (idle_timeout_ms < 0) {
                fprintf(stderr, "fiskta_library_wrapper: invalid time format: %s\n", argv[argi + 1]);
                return 7;
            }
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--until-idle=", 13) == 0) {
            idle_timeout_ms = parse_time_ms(arg + 13);
            if (idle_timeout_ms < 0) {
                fprintf(stderr, "fiskta_library_wrapper: invalid time format: %s\n", arg + 13);
                return 7;
            }
            argi++;
            continue;
        }
        if (strncmp(arg, "-u=", 3) == 0) {
            idle_timeout_ms = parse_time_ms(arg + 3);
            if (idle_timeout_ms < 0) {
                fprintf(stderr, "fiskta_library_wrapper: invalid time format: %s\n", arg + 3);
                return 7;
            }
            argi++;
            continue;
        }

        // --for N or --for=N
        if (strcmp(arg, "--for") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta_library_wrapper: --for requires a value\n");
                return 7;
            }
            exec_timeout_ms = parse_time_ms(argv[argi + 1]);
            if (exec_timeout_ms < 0) {
                fprintf(stderr, "fiskta_library_wrapper: invalid time format: %s\n", argv[argi + 1]);
                return 7;
            }
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--for=", 6) == 0) {
            exec_timeout_ms = parse_time_ms(arg + 6);
            if (exec_timeout_ms < 0) {
                fprintf(stderr, "fiskta_library_wrapper: invalid time format: %s\n", arg + 6);
                return 7;
            }
            argi++;
            continue;
        }

        // --ignore-failures or -k
        if (strcmp(arg, "--ignore-failures") == 0 || strcmp(arg, "-k") == 0) {
            ignore_loop_failures = true;
            argi++;
            continue;
        }

        // --ops STRING (inline operations)
        if (strcmp(arg, "--ops") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta_library_wrapper: --ops requires a value\n");
                return 7;
            }
            ops_string = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--ops=", 6) == 0) {
            ops_string = arg + 6;
            argi++;
            continue;
        }

        // --ops-file FILE (operations from file)
        if (strcmp(arg, "--ops-file") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "fiskta_library_wrapper: --ops-file requires a value\n");
                return 7;
            }
            ops_file = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (strncmp(arg, "--ops-file=", 11) == 0) {
            ops_file = arg + 11;
            argi++;
            continue;
        }

        // Unknown option, stop parsing
        break;
    }

    int token_start_idx = argi;

    // If no --input specified, read from stdin
    if (!input_path) {
        input_path = "-";
    }

    // Get operation tokens
    String tokens[1024];
    int token_count;
    char file_content_buf[16384];
    char tokenize_scratch[16384];

    if (ops_string) {
        // Parse inline operations string
        if (token_start_idx < argc) {
            fprintf(stderr, "fiskta_library_wrapper: --ops cannot be combined with positional operations\n");
            return 7;
        }

        i32 n = tokenize_ops_string(ops_string, tokens, 1024, tokenize_scratch, sizeof(tokenize_scratch));
        if (n == -1) {
            fprintf(stderr, "fiskta_library_wrapper: operations string too long (max %d bytes)\n", 16384);
            return FISKTA_EXIT_CAPACITY;
        }
        if (n <= 0) {
            fprintf(stderr, "fiskta_library_wrapper: empty ops string\n");
            return 7;
        }

        token_count = n;
    } else if (ops_file) {
        // Load operations from file
        if (token_start_idx < argc) {
            fprintf(stderr, "fiskta_library_wrapper: --ops-file cannot be combined with positional operations\n");
            return 7;
        }

        FILE* cf = fopen(ops_file, "rb");
        if (!cf) {
            fprintf(stderr, "fiskta_library_wrapper: cannot open ops file %s\n", ops_file);
            return FISKTA_EXIT_IO;
        }

        size_t total = fread(file_content_buf, 1, sizeof(file_content_buf) - 1, cf);
        if (ferror(cf)) {
            fclose(cf);
            fprintf(stderr, "fiskta_library_wrapper: error reading ops file %s\n", ops_file);
            return FISKTA_EXIT_IO;
        }
        if (!feof(cf)) {
            fclose(cf);
            fprintf(stderr, "fiskta_library_wrapper: operations file too long (max %d bytes)\n", 16384);
            return FISKTA_EXIT_CAPACITY;
        }
        fclose(cf);

        file_content_buf[total] = '\0';
        if (total == 0) {
            fprintf(stderr, "fiskta_library_wrapper: empty ops file\n");
            return 7;
        }

        i32 n = tokenize_ops_string(file_content_buf, tokens, 1024, tokenize_scratch, sizeof(tokenize_scratch));
        if (n == -1) {
            fprintf(stderr, "fiskta_library_wrapper: operations string too long (max %d bytes)\n", 16384);
            return FISKTA_EXIT_CAPACITY;
        }
        if (n <= 0) {
            fprintf(stderr, "fiskta_library_wrapper: empty ops string\n");
            return 7;
        }

        token_count = n;
    } else {
        // Get operations from command line
        token_count = argc - token_start_idx;

        // Handle --version as special case (like main.c)
        if (token_count == 1 && strcmp(argv[token_start_idx], "--version") == 0) {
            printf("fiskta - (fi)nd (sk)ip (ta)ke v" FISKTA_VERSION "\n");
            return 0;
        }

        if (token_count == 0) {
            fprintf(stderr, "fiskta_library_wrapper: no operations specified\n");
            usage(argv[0]);
            return 7;
        }

        if (token_count > 1024) {
            fprintf(stderr, "fiskta_library_wrapper: too many operations\n");
            return FISKTA_EXIT_CAPACITY;
        }

        for (int i = 0; i < token_count; i++) {
            tokens[i].bytes = argv[token_start_idx + i];
            tokens[i].len = (i32)strlen(argv[token_start_idx + i]);
        }
    }

    // Set up error callback (used by runtime path when engine reports errors)
    fiskta_set_error_handler(error_to_stderr, NULL);

    // Get program requirements
    BuildOptions opts = {0};  // Use defaults
    RuntimeRequirements reqs;
    int result = fiskta_program_requirements(token_count, tokens, &opts, &reqs);
    if (result != FISKTA_EXIT_OK) {
        print_last_error_cli_like();
        return result;
    }

    // Allocate arena
    void* arena = malloc(reqs.arena_bytes);
    if (!arena) {
        fprintf(stderr, "fiskta_library_wrapper: malloc failed\n");
        return FISKTA_EXIT_RESOURCE;
    }

    // Build program
    Program prog = {0};
    RuntimeBuffers buffers = {0};
    result = fiskta_build_program(token_count, tokens, &opts, &prog, arena, reqs.arena_bytes, &buffers);
    if (result != FISKTA_EXIT_OK) {
        print_last_error_cli_like();
        free(arena);
        return result;
    }

    // Read input file into memory
    size_t input_len;
    unsigned char* input_data = read_file_to_memory(input_path, &input_len);
    if (!input_data) {
        fprintf(stderr, "fiskta_library_wrapper: failed to read input\n");
        free(arena);
        return FISKTA_EXIT_IO;
    }

    // Configure runtime with output callback
    RuntimeConfig config = {
        .loop_ms = loop_ms,
        .loop_enabled = loop_enabled,
        .ignore_loop_failures = ignore_loop_failures,
        .idle_timeout_ms = idle_timeout_ms,
        .exec_timeout_ms = exec_timeout_ms,
        .error_callback = error_to_stderr,
        .error_userdata = NULL,
        .output_callback = output_to_stdout,
        .output_userdata = NULL
    };

    // Execute on in-memory buffer
    result = fiskta_runtime_execute_buffer(&prog, input_data, input_len, &buffers, &config);
    if (result != FISKTA_EXIT_OK && result != FISKTA_EXIT_PROGRAM_FAIL) {
        print_last_error_cli_like();
    }

    // Cleanup
    free(input_data);
    free(arena);

    return result;
}

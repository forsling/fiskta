// main.c
#include "fiskta.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

enum Err parse_program(int, char**, Program*, const char**);
void parse_free(Program*);
enum Err engine_run(const Program*, const char*, FILE*);

static void die(enum Err e, const char* msg)
{
    if (msg)
        fprintf(stderr, "fiskta: %s\n", msg);
    else
        fprintf(stderr, "fiskta: error %d\n", e);
}

static void print_usage(void)
{
    printf("Usage:\n");
    printf("  fiskta <tokens...> <inputfile|->\n");
    printf("\n");
    printf("Clauses:\n");
    printf("  Separate clauses with '::'. Each clause is all-or-nothing (commit or discard).\n");
    printf("\n");
    printf("Units:\n");
    printf("  b = bytes, l = lines (lines split only on LF, 0x0A)\n");
    printf("\n");
    printf("Labels:\n");
    printf("  NAME is UPPERCASE (A-Z _ -), length <= 16\n");
    printf("\n");
    printf("Search:\n");
    printf("  find [to <loc-expr>] <needle>\n");
    printf("    # Searches within [min(cursor,L), max(cursor,L)), default L=EOF.\n");
    printf("    # Picks the match closest to the cursor:\n");
    printf("    #   - forward window: first match\n");
    printf("    #   - backward window: rightmost match\n");
    printf("    # On success: cursor -> match-start; last_match set.\n");
    printf("\n");
    printf("Movement:\n");
    printf("  skip  <N><b|l>\n");
    printf("  label <NAME>\n");
    printf("  goto  <loc-expr>\n");
    printf("\n");
    printf("Take (cursor-anchored):\n");
    printf("  take <±N><b|l>                 # +N forward bytes/lines; -N backward; cursor -> far end\n");
    printf("  take to <loc-expr>             # [min(cursor,L), max(cursor,L)); cursor -> far end\n");
    printf("  take until <needle> [at match-start|match-end|line-start|line-end[±K<b|l>]]\n");
    printf("\n");
    printf("Locations:\n");
    printf("  loc-expr := loc [±K<b|l>]\n");
    printf("  loc      := cursor | BOF | EOF | NAME | match-start | match-end | line-start | line-end\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help                  Show this help message\n");
    printf("  -v, --version               Show version information\n");
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
            printf("fiskta v2.2\n");
            printf("Streaming text extraction tool\n");
            return 0;
        }
    }

    if (argc < 3) {
        fprintf(stderr, "Usage: fiskta [options] <operations> [file|-]\n");
        fprintf(stderr, "Try 'fiskta --help' for more information.\n");
        return 2;
    }

    Program prg = { 0 };
    const char* path = NULL;
    enum Err e = parse_program(argc, argv, &prg, &path);
    if (e != E_OK) {
        die(e, "parse error");
        parse_free(&prg);
        return 2;
    }

    e = engine_run(&prg, path, stdout);
    parse_free(&prg);
    if (e != E_OK) {
        die(e, "execution error");
        return 2;
    }
    return 0;
}

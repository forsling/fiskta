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

static const char* err_str(enum Err e) {
    switch (e) {
    case E_OK: return "ok";
    case E_PARSE: return "parse error";
    case E_BAD_NEEDLE: return "empty needle";
    case E_LOC_RESOLVE: return "location not resolvable";
    case E_NO_MATCH: return "no match in window";
    case E_LABEL_FMT: return "bad label (A-Z, _ or -, ≤16)";
    case E_IO: return "I/O error";
    case E_OOM: return "out of memory";
    default: return "unknown error";
    }
}

static void die(enum Err e, const char* msg) {
    if (msg) fprintf(stderr, "fiskta: %s (%s)\n", msg, err_str(e));
    else     fprintf(stderr, "fiskta: %s\n", err_str(e));
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
    printf("  take to <location>          Order-normalized: emits [min(cursor,L), max(cursor,L));\n");
    printf("                              cursor moves to the high end\n");
    printf("  take until <string> [at <location>]\n");
    printf("                              Forward-only: emits [cursor, B) where B is derived\n");
    printf("                              from the match; cursor moves only if B > cursor\n");
    printf("  label <name>                Mark current position with label\n");
    printf("  goto <location>             Jump to labeled position\n");
    printf("\n");
    printf("UNITS:\n");
    printf("  b                           Bytes\n");
    printf("  l                           Lines (LF only, CR treated as bytes)\n");
    printf("  c                           UTF-8 code points (never splits sequences)\n");
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

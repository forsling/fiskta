# (fi)nd (sk)ip (ta)ke

**fiskta** is a cursor-oriented data extraction tool. Unlike traditional tools that rely primarily on pattern matching, fiskta is built around cursor position and movement: find a pattern, skip around, capture some bytes, characters or lines.

It may be a good fit when grep is insufficient but you don't want to deal with something like awk: Extract text between delimiters, navigate multi-line structures, and build conditional extractions step by step. No cryptic syntax, just relatively straightforward imperative operations.

**Features:**
- Works with bytes, lines, and UTF-8 characters
- Flexible search that works with plain strings, regex or binary data (hex search strings)
- Regular expressions with character classes, quantifiers, grouping, and anchors
- Atomic clauses with rollback on failure
- Views to restrict operations to file regions
- Loop mode for continuous operation like monitoring streams or changing files
- Small footprint: binaries ~60-100 KB, memory use <8 MB (for standard builds)
- Written with plain C with zero dependencies beyond libc

## What does it do?

**Output 7 characters starting from the second line:**
```bash
$ printf "Starting text\nMiddle line\nEnding line" | ./fiskta skip 1l take 7c
Middle
```

**Output all the data except for the last 10 bytes:**
```bash
$ fiskta --input data.file take to EOF-10b
```

**Find a pattern and take the rest of the line:**
```bash
$ echo 'Connecting... ERROR: connection failed' | fiskta find "ERROR:" take to line-end
ERROR: connection failed
```

**Try multiple patterns—first match wins:**
```bash
$ echo 'WARNING: disk full' | fiskta find "ERROR:" take to line-end OR find "WARNING:" take to line-end
WARNING: disk full
```

**Extract text between delimiters:**
```bash
$ echo 'start: [content here] end' | fiskta find "[" skip 1b take until "]"
content here
```

**Output five lines even if find fails:**
```bash
$ fiskta --input file.txt find "Optional Section" THEN take 5l
```

**Try to find "user=" and extract username, fallback to "id=" if not found:**
```bash
$ echo 'id=12345 user=john role=admin' | fiskta find "user=" skip 5b take until " " OR find "id=" skip 3b take until " "
john
```

**Detect PNG file header:**
```bash
$ fiskta -i image.bin find:bin "89 50 4E 47 0D 0A 1A 0A" print "PNG" OR fail "Not a PNG file"
PNG
```

**Extract email addresses using regex:**
```bash
$ echo 'Contact: john@example.com or jane@test.org' | fiskta find:re "[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+" take to match-end
john@example.com
```

**Process data in chunks:**
```bash
$ fiskta --input source --continue 200ms find:re "^BEGIN" take until:re "\s{4}:"
```

**Tail log file, stop when no new data appears for 1 minute:**
```bash
$ fiskta -C 1s --until-idle 1m --input service.log find "ERROR" take to line-end THEN skip to EOF
```

**Monitor changing file content:**
```bash
$ fiskta --input status.txt -C 2s --for 8h clear view THEN skip to BOF THEN find "DISCONNECTED" take -10l THEN skip to EOF
```

## Overview

**Extracting and Moving:**
- `take <n><unit>` - Extract n units from current position
- `skip <n><unit>` - Move cursor n units forward (no output)
- `take to <location>` - Order-normalized: emits `[min(cursor,L), max(cursor,L))`; cursor moves to the high end
- `take until <string> [at match-start|match-end|line-start|line-end]` - Forward-only: emits `[cursor, B)` where B is derived from the match; cursor moves only if B > cursor. Default: `at match-start` (exclude pattern). `line-start`/`line-end` are relative to the match.
- `take until:re <regex> [at match-start|match-end|line-start|line-end]` - Same as `take until` but with regex pattern support
- `take until:bin <hex-string> [at match-start|match-end|line-start|line-end]` - Same as `take until` but with binary pattern support (hex format like `find:bin`)

**Searching:**
- `find [to <location>] <string>` - Search within `[min(cursor,L), max(cursor,L))`, default L=EOF; picks match closest to cursor
- `find:re [to <location>] <regex>` - Search using regular expressions within `[min(cursor,L), max(cursor,L))`; supports character classes, quantifiers, anchors
- `find:bin [to <location>] <hex-string>` - Search for binary patterns specified as hexadecimal (e.g., `DEADBEEF` or `DE AD BE EF`); case-insensitive, whitespace-ignored

**Navigation:**
- `label <name>` - Mark current position with label (fails if already set)
- `clear <name>` - Unset a label (allows relabeling)
- `skip to <location>` - Move cursor to labeled position
- `view <L1> <L2>` - Limit all ops to `[min(L1,L2), max(L1,L2))`
- `clear view` - Clear view; return to full file

**Utilities:**
- `print <string>` - Emit literal bytes (alias: echo). Supports escape sequences: `\n \t \r \0 \\ \xHH \c`. Participates in clause atomicity
- `fail <message>` - Write message to stderr and fail clause. Message written immediately (not staged). Useful with OR for error messages

### Units

- `b` - Bytes
- `l` - Lines
- `c` - UTF-8 code points

### Labels

- Names must be UPPERCASE, <16 chars, `[A-Z0-9_-]` (first must be A-Z)

### Locations

- `cursor` - Current cursor position
- `BOF` - Beginning of file
- `EOF` - End of file
- `match-start` - Start of last match
- `match-end` - End of last match
- `line-start` - Start of current line
- `line-end` - End of current line
- `<label>` - Named label position

**Note**: `line-start`/`line-end` are relative to the cursor here; in "at" (for "take until") they're relative to the last match.

### Offsets

- `<location> +<n><unit>` - n units after location
- `<location> -<n><unit>` - n units before location
- (inline offsets like `BOF+100b` are allowed)

### Regex Syntax

- **Character Classes**: `\d` (digits), `\D` (non-digits), `\w` (word), `\W` (non-word), `\s` (space), `\S` (non-space), `[a-z]`, `[^0-9]`
- **Quantifiers**: `*` (0+), `+` (1+), `?` (0-1), `{n}` (exactly n), `{n,m}` (n to m), `{n,}` (n or more)
  - Greedy by default (match as much as possible)
  - Lazy/non-greedy: `*?`, `+?`, `??`, `{n,m}?`, `{n,}?` (match as little as possible)
  - Max 16 distinct `{n,m}` counters per pattern; max 256 alternations
- **Grouping**: `( ... )` (group subpatterns), `(a|b)+` (quantified groups)
- **Anchors**: `^` (line start), `$` (line end)
- **Alternation**: `|` (OR)
- **Escape**: `\n`, `\t`, `\r`, `\f`, `\v`, `\0`
- **Special**: `.` (any char except newline)

### Clauses and Atomicity

Operations are grouped into **clauses** separated by logical operators. Each clause is atomic:
- **All operations succeed** → clause commits (output emitted, labels saved, cursor moved)
- **Any operation fails** → clause rolls back (no output, no state changes)

Example:
```bash
find "user=" skip 5b take until " "  # One clause with 3 operations
                                     # All succeed or all fail together
```

### Logical Operators

Connect clauses with:

**`THEN`** - always run next clause
```bash
find "user" THEN take to line-end       # always take, even if find fails
```

**`OR`** - run next clause only if this one fails
```bash
find "cache" OR find "buffer"           # try cache first, fallback to buffer
```

Use OR when you want to try multiple alternatives - only the first successful clause runs.

Evaluation is strictly left-to-right (no operator precedence).

### Command-Line Options

**Input/Output:**
- `-i, --input <path>` - Read from file instead of stdin

**Command Modes:**
- `--ops <string>` - Provide operations as a single inline string
- `--ops-file <path>` - Load operations from a file
- `--` - Treat remaining args as operations

**Looping:**
- `-c, --continue [delay]` - Enable looping; optional delay between iterations (`ms`, `s`, `m`, `h`; default `0` for tight loop)
- `-C, --continue-on-fail [delay]` - Like `-c`, but keeps looping even if clauses fail
- `--for <time>` - Stop after total wall-clock time elapses
- `-u, --until-idle <time>` - Stop once the input window is empty for the given duration (`0` exits immediately on idle)

Loop mode resumes from the last cursor position. For follow/monitor emulation, see the Looping section below.

**Examples:**
```bash
# Operations as arguments (default)
fiskta find "ERROR" take to line-end < log.txt

# Operations as string
fiskta --ops 'find "ERROR" take to line-end' --input log.txt

# Operations from file
fiskta --ops-file program.fisk --input log.txt

# Tail/follow behavior with 1s cadence (append THEN skip to EOF)
fiskta --continue 1s --until-idle 0 --input service.log find "ERROR" take to line-end THEN skip to EOF
```

## Installation

### Pre-built binaries

Pre-built binaries for Linux x86_64 (glibc and musl), macOS aarch64, and Windows x86_64 are available from the [GitHub releases page](https://github.com/forsling/fiskta/releases).

### Using Make

```bash
make                                # Build optimized binary (./fiskta)
make debug                          # Build with debug symbols
make release                        # Build stripped binary + static library in dist/
make test                           # Run test suite
```

### Using Zig (cross-compilation)

```bash
zig build                           # Build for host platform (zig-out/bin/fiskta)
zig build test                      # Build and run test suite
zig build release                   # Cross-compile for all platforms
```

## Command Reference

### Finding

#### `find [to <location>] <pattern>`

Search for a literal string pattern. Moves cursor to the match position.

- Searches in range `[min(cursor, location), max(cursor, location))`
- Default location is `EOF` (search forward from cursor)
- For backward search, use `find to BOF "pattern"`
- Picks the match closest to cursor if multiple exist

```bash
find "ERROR"              # search forward from cursor
find to BOF "START"       # search backward from cursor
find to EOF+1000b "END"   # search forward but only 1000 bytes
```

#### `find:re [to <location>] <regex>`

Search using regular expressions. Same search behavior as `find`.

Regex syntax:
- Classes: `\d` (digit), `\D` (non-digit), `\w` (word), `\W` (non-word), `\s` (space), `\S` (non-space), `[a-z]`, `[^0-9]`
- Quantifiers: `*` (0+), `+` (1+), `?` (0-1), `{n}`, `{n,m}`, `{n,}`
  - Greedy (default): `*`, `+`, `?`, `{n,m}`
  - Lazy: `*?`, `+?`, `??`, `{n,m}?`, `{n,}?`
- Anchors: `^` (line start), `$` (line end)
- Alternation: `|`, Grouping: `(...)`, Any char: `.`
- Escapes: `\n`, `\t`, `\r`, `\f`, `\v`, `\0`

```bash
find:re "ERROR|WARN"                    # alternation
find:re "^\[.*\]"                       # line start anchor
find:re "[0-9]{1,3}\.[0-9]{1,3}"        # IP address pattern
find:re "[A-Za-z]+@[A-Za-z.]+"          # simple email pattern
find:re "<.*?>"                         # lazy: match minimal content between < and >
find:re "a.*?b"                         # lazy: match minimal chars between 'a' and 'b'
```

#### `find:bin [to <location>] <hex-string>`

Search for binary patterns specified as hexadecimal strings. Same search behavior as `find` and `find:re`.

Hex string format:
- Pairs of hex digits (00-FF)
- Case-insensitive (both `DEADBEEF` and `deadbeef` work)
- Whitespace ignored (can use spaces/tabs for readability: `DE AD BE EF`)
- Must have even number of hex digits
- Invalid hex characters or odd digit count cause parse errors

```bash
find:bin "89504E470D0A1A0A"              # PNG file header
find:bin "50 4B 03 04"                   # ZIP file signature (with spaces)
find:bin "CAFEBABE"                      # Java class file magic number
find:bin "de ad be ef"                   # lowercase works too
find:bin to BOF "FFFE"                   # backward search for UTF-16 BOM
```

Common use cases:
- File format detection (magic numbers)
- Binary protocol parsing
- Firmware analysis
- Data carving from disk images

### Extracting

#### `take <n><unit>`

Extract n units from cursor position. Positive goes forward, negative goes backward.

```bash
take 10b      # take 10 bytes forward from cursor
take -5b      # take 5 bytes backward from cursor
take 3l       # take 3 lines forward
take -2l      # take 2 lines backward
take 20c      # take 20 UTF-8 characters forward
```

#### `take to <location>`

Extract from cursor to location (order-normalized). Always emits `[min(cursor, loc), max(cursor, loc))`.

```bash
take to EOF              # everything from cursor to end
take to BOF              # everything from cursor to beginning
take to match-end        # from cursor to end of last match
take to line-end         # from cursor to end of current line
take to MYLABEL          # from cursor to labeled position
take to cursor+100b      # 100 bytes from cursor
```

#### `take until <pattern> [at match-start|match-end|line-start|line-end]`

Forward-only search. Extracts from cursor until pattern is found. Cursor moves only if match is past current position.

The `at` clause controls where extraction ends relative to the match:
- Default: `at match-start` (exclude the pattern)
- `at match-end` (include the pattern)
- `at line-start` (extract until start of line containing match)
- `at line-end` (extract until end of line containing match)

**Note**: `line-start` and `line-end` are relative to the match, not the cursor.

```bash
take until ";"                    # extract until semicolon (excluded)
take until "END" at match-end     # extract until END (included)
take until "\n\n"                 # extract until blank line
take until "---" at line-start    # extract until line with ---
```

#### `take until:re <regex> [at match-start|match-end|line-start|line-end]`

Forward-only search using regular expressions. Extracts from cursor until regex pattern is found. Same behavior and `at` clause options as `take until`.

```bash
take until:re "\\d+"                     # extract until first number (excluded)
take until:re "[A-Z]" at match-end       # extract until first capital letter (included)
take until:re "\\s+" at line-start       # extract until whitespace, up to line start
take until:re "\\n\\n"                   # extract until blank line
```

#### `take until:bin <hex-string> [at match-start|match-end|line-start|line-end]`

Forward-only search for binary patterns specified as hexadecimal strings. Extracts from cursor until binary pattern is found. Same behavior and `at` clause options as `take until` and `take until:re`.

Hex string format follows the same rules as `find:bin` (case-insensitive, whitespace-ignored, must have even number of hex digits).

```bash
take until:bin "0D0A"                    # extract until CRLF (excluded)
take until:bin "00 00" at match-end      # extract until double null (included)
take until:bin "DEADBEEF"                # extract until marker bytes
take until:bin "FF D8 FF" at match-end   # extract until JPEG SOI marker (included)
```

Common use cases:
- Extract data until binary delimiter
- Parse binary protocols with markers
- Extract sections of binary files
- Process structured binary data

### Movement

#### `skip <n><unit>`

Move cursor forward without emitting output. Negative values move backward.

```bash
skip 100b      # skip 100 bytes forward
skip -10b      # skip 10 bytes backward
skip 5l        # skip 5 lines forward
skip -2l       # skip 2 lines backward
skip 50c       # skip 50 UTF-8 characters forward
```

#### `skip to <location>`

Move cursor to a specific location without emitting output.

```bash
skip to BOF                 # move to beginning
skip to EOF                 # move to end
skip to MYLABEL             # move to labeled position
skip to match-start         # move to start of last match
skip to line-start          # move to start of current line
skip to cursor+100b         # move 100 bytes from current position
```

### Labels

#### `label <name>`

Mark the current cursor position with a label. Labels can be used with `skip to` and `take to`.

- Names must be UPPERCASE
- Start with A-Z, contain A-Z0-9_-
- Maximum 15 characters
- Maximum 128 labels
- Labels are write-once: setting a label that is already set fails the clause
- Use `clear <NAME>` to unset a label before reassigning it

```bash
label START
label SECTION_A
label END-OF-HEADER
clear START                 # unset START so it can be reassigned
label START                 # now succeeds
```

### Views

#### `view <location1> <location2>`

Restrict all subsequent operations to the range `[min(loc1, loc2), max(loc1, loc2))`. View boundaries are order-normalized.

Once a view is set:
- `find`, `find:re`, and `find:bin` only search within the view
- `take` and `skip` can't move outside the view
- `skip to` fails if the target is outside the view
- View changes are staged and committed atomically with the clause
- The view remains active until `clear view` or program ends
- Views can't be nested, but can be replaced by setting a new view

```bash
view BOF+100b EOF-100b        # exclude first/last 100 bytes
view cursor cursor+1000b      # limit to next 1000 bytes
view SECTION_START SECTION_END # limit to labeled section
```

#### `clear view`

Remove view restrictions, returning to full file access.

```bash
clear view
```

### Utilities

#### `print <string>` (alias: `echo`)

Emit a literal string. Participates in clause atomicity (output only emitted if clause succeeds).

Supports escape sequences:

- `\n` — newline (LF)
- `\t` — tab
- `\r` — carriage return (CR)
- `\0` — NUL byte
- `\\` — literal backslash
- `\xHH` — literal byte from two hex digits
- `\c` — decimal cursor offset at the point of staging

```bash
print "---\n"           # print separator with newline
print "\x1b[31m"        # print ANSI color code
echo "Hello world"      # alias for print
print "cursor=\c\n"    # embed cursor position and newline
```

#### `fail <message>`

Write message to stderr and always fail the clause. Unlike other operations, the message is written immediately (not staged), so it appears even though the clause fails.

Most useful with OR to provide helpful error messages:

```bash
find "config" OR fail "Config section not found\n"
find "user=" skip 5b take until " " OR fail "Could not extract username\n"
```

Supports the same escape sequences as `print`.

## Looping (Continue Mode)

fiskta can loop over your operations as files grow or change. Loop mode resumes from the previous cursor location, preserving labels and view state between runs.

### Loop Options

- `-c` / `--continue [delay]` — Enable looping; optional delay between iterations (`ms`, `s`, `m`, `h`; default `0` for tight loop)
- `-C` / `--continue-on-fail [delay]` — Like `-c`, but keeps looping even if clauses fail
- `--for <time>` — Stop after total run time hits limit (works for single execution too)
- `-u` / `--until-idle <time>` — Stop once the input window is empty for the given period (`0` exits immediately on idle)

```bash
fiskta --continue 200ms --input metrics.log \
    find "latency=" take until " " print "\n"
```

### Emulating Follow Mode

To process only new data appended since the previous iteration (like tailing logs), append `THEN skip to EOF` to your program:

```bash
fiskta --continue 1s --until-idle 0 --input service.log \
    find "WARNING:" take to line-end THEN skip to EOF
```

### Emulating Monitor Mode

To re-scan the entire file each iteration (for files that mutate instead of only growing), prefix `clear view THEN skip to BOF` and append `THEN skip to EOF`:

```bash
fiskta --continue 5m --input status.txt \
    clear view THEN skip to BOF THEN find "STATE=READY" take to line-end THEN skip to EOF
```

## Exit Codes

fiskta uses exit codes to indicate success, failure, and the type of error encountered.

- **0**: Success (includes normal -u/--until-idle stop)
  - At least one clause succeeded in the final iteration
- **1**: Program failure (no clause succeeded in final iteration)
  - Returned when every clause in the last iteration failed
  - Suppressed by `-C` / `--continue-on-fail` while looping
- **2**: Execution timeout (`--for` elapsed)
- **7**: Usage error (CLI misuse)
  - Unknown or unsupported flags, invalid flag combinations
  - Missing required option values
- **8**: Parse error (program grammar or regex syntax)
  - Invalid operation syntax, unknown operation, missing arguments
  - Invalid regex pattern syntax or semantics
  - Caught during program parsing, before execution
- **9**: Capacity exceeded (policy limits)
  - Regex pattern exceeded execution budget (thread limit, seen table, work budget)
  - Too many operations, labels, or alternations in regex
  - Operations string or file too long
- **10**: I/O error (open/read/write failure)
  - File not found, permission denied, read/write errors
- **11**: Resource exhaustion (system cannot fulfill request)
  - Memory allocation request exceeds addressable limits (SIZE_MAX overflow)
  - malloc() failed (insufficient available memory)
  - Arena allocation exhausted

## Limits

| Resource | Limit |
|----------|-------|
| Labels | 128 total, 15 character names |
| Operation tokens | 1024 per program |
| Search pattern size | 16 KB |
| Regex `{n,m}` counters | 16 per pattern |
| Regex alternations | 256 per group |
| Regex VM memory | 2 MiB (configurable via `FisktaBuildOptions`) |
| Regex work budget | 50M thread enqueues per search |

## Examples

### Extract fixed-length fields

Problem: Get the first 10 characters from each line in a log file

```bash
fiskta --input app.log take 10b skip to line-end skip 1b
```

### Skip header, extract body

Problem: Skip first 5 lines of a report, then extract the next 20 lines

```bash
fiskta --input report.txt skip 5l take 20l
```

### Conditional extraction

Problem: Extract the username only if the line contains "login success"

```bash
fiskta --input auth.log \
    find "login success" skip to line-end skip -1l find "user=" skip 5b take until " "
```

### Extract between delimiters

Problem: Extract content between `<tag>` and `</tag>`

```bash
fiskta --input data.xml find "<tag>" skip 5b take until "<"
```

### Extract from specific section

Problem: Extract all config values from the `[database]` section only

```bash
fiskta --continue --input config.ini \
    find "[database]" skip to line-end \
    view cursor cursor+1000b \
    find "=" skip -1l take to line-end
```

### Find binary patterns

Problem: Detect file type by magic number and extract relevant data

```bash
# Find PNG header and extract image dimensions
fiskta --input image.bin \
    find:bin "89 50 4E 47 0D 0A 1A 0A" \
    skip 8b \
    find:bin "49484452" \
    skip 4b \
    take 8b | xxd

# Detect ZIP files
fiskta --input archive.dat find:bin "504B0304" take to match-end

# Find all JPEG markers in a file
fiskta --continue --input photo.jpg \
    find:bin "FFD8" OR find:bin "FFE0" OR find:bin "FFE1" \
    take to match-end \
    print "\n"
```

---

## Grammar

```
Program        = Clause { ( "THEN" | "OR" ) Clause } .
Clause         = { Op } .
Op             = Find | FindRegex | FindBinary | Skip | Take | Label | ClearLabel
               | View | ClearView | Print | Fail .
Find           = "find" [ "to" LocationExpr ] String .
FindRegex      = "find" ":" "re" [ "to" LocationExpr ] String .
FindBinary     = "find" ":" "bin" [ "to" LocationExpr ] String .
Skip           = "skip" ( Number Unit | "to" LocationExpr ) .
Take           = "take" ( SignedNumber Unit
                          | "to" LocationExpr
                          | "until" String [ "at" AtExpr ]
                          | "until" ":" "re" String [ "at" AtExpr ]
                          | "until" ":" "bin" String [ "at" AtExpr ] ) .
Label          = "label" Name .
ClearLabel     = "clear" Name .
View           = "view" LocationExpr LocationExpr .
ClearView      = "clear" "view" .
Print          = ( "print" | "echo" ) String .
Fail           = "fail" String .
LocationExpr   = Location [ Offset ] .
Location       = "cursor" | "BOF" | "EOF" | Name
               | "match-start" | "match-end"
               | "line-start" | "line-end" .
AtExpr         = ( "match-start" | "match-end"
                 | "line-start" | "line-end" ) [ Offset ] .
Offset         = ( "+" | "-" ) Number Unit .
SignedNumber   = [ "+" | "-" ] Number .
Unit           = "b" | "l" | "c" .
Number         = Digit { Digit } .
Name           = UpperLetter { UpperLetter | Digit | "_" | "-" } .
String         = ShellString .
UpperLetter    = "A" | "B" | "C" | "D" | "E" | "F" | "G"
               | "H" | "I" | "J" | "K" | "L" | "M" | "N"
               | "O" | "P" | "Q" | "R" | "S" | "T" | "U"
               | "V" | "W" | "X" | "Y" | "Z" .
Digit          = "0" | "1" | "2" | "3" | "4" | "5"
               | "6" | "7" | "8" | "9" .
ShellString    = shell-quoted byte string (may be empty for print/echo) .
```

## Library API

fiskta can be embedded as a C library. Include `fiskta.h` (the single public header) and link against `libfiskta.a`.

### Three-phase execution

```c
#include "fiskta.h"

// 1. Measure memory requirements (no allocation, no side effects)
FisktaBuildOptions opts = {0};  // use defaults
FisktaRuntimeRequirements req;
fiskta_program_requirements(token_count, tokens, &opts, &req);

// 2. Allocate arena and build program
void* arena = malloc(req.arena_bytes);
FisktaProgram prog;
FisktaRuntimeBuffers buffers;
fiskta_build_program(token_count, tokens, &opts, &prog, arena, req.arena_bytes, &buffers);

// 3. Execute against a file (or in-memory buffer)
FisktaRuntimeConfig config = {0};
int rc = fiskta_runtime_execute(&prog, "input.txt", &buffers, &config);
// or: fiskta_runtime_execute_buffer(&prog, data, len, &buffers, &config);

free(arena);
```

All functions return `int` `FISKTA_EXIT_*` codes. Detailed error information is available via `fiskta_error_code()`, `fiskta_error_message()`, and `fiskta_error_position()`.

### Configuration

`FisktaBuildOptions` controls regex engine resource limits:
- `regex_budget_bytes` — total memory for regex VM (default: 2 MiB)
- `regex_work_budget` — max thread enqueues per search (default: 50M)

`FisktaRuntimeConfig` controls execution behavior:
- `loop_enabled` / `loop_ms` — continue mode with delay
- `ignore_loop_failures` — suppress program-failure exits while looping
- `idle_timeout_ms` / `exec_timeout_ms` — timeouts
- `error_callback` / `output_callback` — intercept errors and output instead of stderr/stdout

### Memory model

- All memory is allocated once at startup via a single arena (`req.arena_bytes`)
- Zero allocations during execution; memory usage is independent of input size
- The caller owns the arena and frees it when done
- `FisktaRuntimeBuffers` may be reused across calls but is not thread-safe

### Building the library

```bash
make release    # produces dist/lib/libfiskta.a and dist/include/fiskta.h
```

Or with zig:

```bash
zig build release    # produces shared and static libraries per platform
```

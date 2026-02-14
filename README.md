# (fi)nd (sk)ip (ta)ke

**fiskta** is a cursor-oriented data extraction tool that operates on bytes, lines or UTF-u characters. With straightforward imperative operations you can find a pattern, skip around, and extract what you need. 

It is small (~100KB), dependency free (libc only), and allocation-averse (memory use independent of input size).

## Examples

Extract the first 5 bytes:

```
$ echo "hello world" \
| fiskta take 5b
hello
```

Find a pattern and take the rest of the line:

```
$ echo 'Connecting... ERROR: connection failed' \
| fiskta find "ERROR:" take to line-end
ERROR: connection failed
```

Skip lines, take lines:

```
$ fiskta --input data.txt skip 2l take 5l
```

Extract text between delimiters:

```
$ echo 'start: [content here] end' | fiskta find "[" skip 1b take until "]"
content here
```

Try multiple strategies — first success wins:

```
$ echo 'id=12345 role=admin' | fiskta find "user=" skip 5b take until " " OR find "id=" skip 3b take until " "
12345
```

## How it works

fiskta maintains a **cursor** — a byte position in the input. Every operation reads or moves this cursor. A program is a sequence of operations evaluated left to right.

There are three units for movement and extraction:

| Unit | Meaning |
|------|---------|
| `b` | Bytes |
| `l` | Lines |
| `c` | UTF-8 code points |

### Operations

| Operation | Description |
|-----------|-------------|
| `find [to <loc>] <string>` | Search for literal string; move cursor to match. Default direction: toward EOF |
| `find:re [to <loc>] <regex>` | Search using regular expression |
| `find:bin [to <loc>] <hex>` | Search for binary pattern (e.g., `"89 50 4E 47"`) |
| `take <n><unit>` | Extract n units from cursor. Negative goes backward |
| `take to <loc>` | Extract from cursor to location (order-normalized) |
| `take until <string>` | Extract forward until pattern is found (excluded by default) |
| `take until:re <regex>` | Same, with regex |
| `take until:bin <hex>` | Same, with binary pattern |
| `skip <n><unit>` | Move cursor without output. Negative goes backward |
| `skip to <loc>` | Move cursor to location without output |
| `label <NAME>` | Mark current cursor position |
| `clear <NAME>` | Unset a label (allows relabeling) |
| `view <loc> <loc>` | Restrict all operations to a region |
| `clear view` | Remove view restriction |
| `print <string>` | Emit literal string (alias: `echo`). Supports `\n \t \r \0 \\ \xHH \c` |
| `fail <message>` | Write message to stderr and fail the current clause |

### Locations

Locations are used with `to`, `skip to`, `take to`, `view`, and as label targets.

| Location | Meaning |
|----------|---------|
| `cursor` | Current position |
| `BOF` | Beginning of file |
| `EOF` | End of file |
| `match-start` | Start of last match |
| `match-end` | End of last match |
| `line-start` | Start of current line (relative to cursor, or to match in `at` clauses) |
| `line-end` | End of current line (relative to cursor, or to match in `at` clauses) |
| `<LABEL>` | A named label position |

Locations accept offsets: `EOF-10b`, `match-end+1b`, `BOF+100b`.

### Clauses and atomicity

Operations are grouped into **clauses** connected by `THEN` or `OR`. Each clause is atomic: either all operations in it succeed (output emitted, cursor moved, labels committed) or the entire clause rolls back as if it never ran. If a program has no `THEN` or `OR`, the whole program is a single clause.

`THEN` always runs the next clause. `OR` runs the next clause only if the current one failed.

```
# One atomic clause: if find fails, the skip and take never happened
find "user=" skip 5b take until " "

# OR: try first strategy, fall back to second
find "user=" skip 5b take until " " OR find "id=" skip 3b take until " "

# THEN: take line regardless of whether find succeeded
find "Section:" THEN take to line-end
```

Rollback means staged output is discarded on failure, not just cursor position:

```
$ echo 'START hello' | fiskta find "START" skip 6b take 5b find "END" OR print "END missing\n"
END missing
```

Here `take 5b` stages "hello", but when `find "END"` fails, the entire clause rolls back — "hello" is never emitted. The `OR` clause runs instead.

## Searching

### Literal search (`find`)

```
find "ERROR"              # search forward from cursor
find to BOF "START"       # search backward
```

`find` searches within `[min(cursor, location), max(cursor, location))` and picks the match closest to the cursor.

### Regex search (`find:re`)

```
find:re "ERROR|WARN"
find:re "[0-9]{1,3}\.[0-9]{1,3}"
find:re "<.*?>"                         # lazy matching
```

Supported syntax: character classes (`\d`, `\w`, `\s`, `[a-z]`, `[^0-9]`), quantifiers (`*`, `+`, `?`, `{n}`, `{n,m}`, `{n,}`) with greedy (default) and lazy (`*?`, `+?`, `??`, `{n,m}?`) variants, grouping `(...)`, alternation `|`, anchors `^` `$`, and `.` (any char except newline).

### Binary search (`find:bin`)

```
find:bin "89504E470D0A1A0A"       # PNG header
find:bin "50 4B 03 04"            # ZIP signature (spaces ignored)
```

Hex digits are case-insensitive and whitespace is ignored. Must have an even number of digits.

## Extracting

### `take until` and the `at` clause

By default, `take until` excludes the matched pattern. The `at` clause controls where extraction stops:

| `at` value | Behavior |
|------------|----------|
| `match-start` | Stop before the match (default) |
| `match-end` | Stop after the match (include it) |
| `line-start` | Stop at start of the line containing the match |
| `line-end` | Stop at end of the line containing the match |

Note: `line-start` and `line-end` in `at` clauses are relative to the **match**, not the cursor.

```
take until ";"                        # up to semicolon (excluded)
take until "END" at match-end         # up to and including END
take until "---" at line-start        # up to start of line containing ---
```

`take until:re` and `take until:bin` support the same `at` clause.

## Navigation and state

### Labels

Mark positions for later reference:

```
label START
find "end of header"
take to START               # extract from START to here
```

Labels must be UPPERCASE (`[A-Z][A-Z0-9_-]`, max 15 chars, max 128 labels). Labels are write-once — setting one that already exists fails the clause. Use `clear <NAME>` to unset it first.

### Views

Restrict all operations to a region of the file:

```
view BOF+100b EOF-100b        # ignore first and last 100 bytes
find "secret"                 # only searches within the view
clear view                    # back to full file
```

## Looping

The `--continue` flag re-runs the program in a loop with an optional delay between iterations. In continue mode, cursor and labels persist across iterations. Each iteration starts with an implicit view `[cursor, EOF)`.

```
fiskta --continue 200ms --input metrics.log find "latency=" take until " " print "\n"
```

Since the cursor is saved between iterations, appending `THEN skip to EOF` gives tail-like semantics — each iteration only processes newly appended data:

```
fiskta --continue 1s --until-idle 0 --input service.log find "ERROR" take to line-end THEN skip to EOF
```

| Flag | Description |
|------|-------------|
| `-c, --continue [delay]` | Loop with optional delay (`ms`, `s`, `m`, `h`). Default: tight loop |
| `-C, --continue-on-fail [delay]` | Like `-c`, but keeps looping even when all clauses fail |
| `--for <time>` | Stop after total wall-clock time elapses |
| `-u, --until-idle <time>` | Stop when input has been empty for the given duration (`0` = exit immediately on idle) |

## CLI reference

```
fiskta [options] <operations>
```

| Option | Description |
|--------|-------------|
| `-i, --input <path>` | Read from file (default: stdin) |
| `--ops <string>` | Operations as inline string |
| `--ops-file <path>` | Load operations from a file |
| `--` | Treat remaining args as operations |
| `-c`, `-C`, `--for`, `-u` | Looping options (see [Looping](#looping)) |
| `-h, --help` | Show help |
| `-v, --version` | Show version |

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Program failure (no clause succeeded) |
| 2 | Execution timeout (`--for` elapsed) |
| 7 | Usage error (bad flags, missing values) |
| 8 | Parse error (grammar or regex syntax) |
| 9 | Capacity exceeded (pattern too complex, too many labels/ops) |
| 10 | I/O error (file not found, permission denied) |
| 11 | Resource exhaustion (out of memory) |

## Installation

### Pre-built binaries

Available for Linux x86_64 (glibc and musl), macOS aarch64, and Windows x86_64 from the [releases page](https://github.com/forsling/fiskta/releases).

### Build from source

```
make                    # optimized binary (./fiskta)
make debug              # with debug symbols
make test               # run test suite
```

Or with Zig for cross-compilation:

```
zig build               # host platform
zig build release       # all platforms
zig build test          # run tests
```

## Library API

fiskta can be embedded as a C library. Include `fiskta.h` and link against `libfiskta.a`.


```c
#include "fiskta.h"

// 1. Measure memory requirements (no allocation, no side effects)
FisktaBuildOptions opts = {0};
FisktaRuntimeRequirements req;
fiskta_program_requirements(token_count, tokens, &opts, &req);

// 2. Allocate arena and build program
void* arena = malloc(req.arena_bytes);
FisktaProgram prog;
FisktaRuntimeBuffers buffers;
fiskta_build_program(token_count, tokens, &opts, &prog, arena, req.arena_bytes, &buffers);

// 3. Execute
FisktaRuntimeConfig config = {0};
int rc = fiskta_runtime_execute(&prog, "input.txt", &buffers, &config);
// or: fiskta_runtime_execute_buffer(&prog, data, len, &buffers, &config);

free(arena);
```

All memory is allocated once at startup via a single arena. Zero allocations during execution — memory usage is independent of input size. The caller owns the arena and frees it when done.

`FisktaBuildOptions` controls regex engine limits (`regex_budget_bytes`, `regex_work_budget`). `FisktaRuntimeConfig` controls loop behavior, timeouts, and error/output callbacks. All functions return `FISKTA_EXIT_*` codes; detailed errors via `fiskta_error_code()`, `fiskta_error_message()`, and `fiskta_error_position()`.

## Limits

| Resource | Limit |
|----------|-------|
| Labels | 128 total, 15 character names |
| Operation tokens | 1024 per program |
| Search pattern size | 16 KB |
| Regex `{n,m}` counters | 16 per pattern |
| Regex alternations | 256 per group |
| Regex VM memory | 2 MiB (configurable) |
| Regex work budget | 50M thread enqueues per search |

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
UpperLetter    = "A" .. "Z" .
Digit          = "0" .. "9" .
ShellString    = shell-quoted byte string (may be empty for print/echo) .
```


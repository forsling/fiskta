# (fi)nd (sk)ip (ta)ke

**fiskta** is a cursor-oriented data extraction tool that operates on bytes, lines, and UTF-8 characters.

Instead of writing complex pattern pipelines, you move a cursor through data with simple imperative operations: find something, skip around, take what you need. It is small (~100KB), zero-dependency (libc only), and allocation-averse (memory independent of input size).

## Quick Start

### Install

Pre-built binaries for Linux x86_64 (glibc and musl), macOS aarch64, and Windows x86_64 are available on the [GitHub releases page](https://github.com/forsling/fiskta/releases).

### Build from source

```bash
make                # Build optimized binary (./fiskta)
make debug          # Build with -O0 -g -DDEBUG
make release        # Build stripped binary + static library in dist/
make test           # Run test suite

zig build           # Build for host platform (zig-out/bin/fiskta)
zig build test      # Build and run test suite
zig build release   # Cross-compile for all platforms
```

### First commands

```bash
# Skip one line, then emit 7 UTF-8 characters
printf "Starting text\nMiddle line\nEnding line" | fiskta skip 1l take 7c

# Find a marker and emit to end of line
echo 'Connecting... ERROR: timeout' | fiskta find "ERROR:" take to line-end

# Extract text between delimiters
echo 'start: [content here] end' | fiskta find "[" skip 1b take until "]"
```

## CLI Usage

```bash
fiskta [options] <operations>
```

### Input and program source

- `-i, --input <path>`: read from file instead of stdin
- `--ops <string>`: provide operations as a single string
- `--ops-file <path>`: load operations from a file
- `--`: treat remaining arguments as operations

### Looping and timing

- `-c, --continue [delay]`: loop execution (`ms`, `s`, `m`, `h`; default `0`)
- `-C, --continue-on-fail [delay]`: continue looping even when no clause succeeds
- `--for <time>`: stop after total elapsed wall-clock time
- `-u, --until-idle <time>`: stop once the input window is empty for a duration (`0` exits immediately on idle)

### Other

- `-h, --help`: show help
- `-v, --version`: show version

## Core Concepts

### Units

- `b`: bytes
- `l`: lines
- `c`: UTF-8 code points

### Locations

- `cursor`: current cursor position
- `BOF`: beginning of file
- `EOF`: end of file
- `match-start`: start of last match
- `match-end`: end of last match
- `line-start`: start of current line
- `line-end`: end of current line
- `<LABEL>`: named label position

Note: `line-start` / `line-end` in location expressions are relative to the cursor. In `take until ... at ...`, they are relative to the match.

### Offsets

- `<location> +<n><unit>`
- `<location> -<n><unit>`

Inline forms like `BOF+100b` are valid.

### Labels

- Must be uppercase names: first char `A-Z`, then `[A-Z0-9_-]`
- Max length: 15 chars
- Max labels: 128
- Labels are write-once until cleared (`clear <NAME>`)

## Command Reference

### Finding

#### `find [to <location>] <string>`

Searches in `[min(cursor, L), max(cursor, L))` where `L` is `to <location>` (default `EOF`).

- Forward search: `find "x"`
- Backward search: `find to BOF "x"`
- If multiple matches exist, picks the match closest to cursor

#### `find:re [to <location>] <regex>`

Same search semantics as `find`, but with regex pattern matching.

#### `find:bin [to <location>] <hex-string>`

Same search semantics as `find`, but with binary hex patterns.

Hex rules:

- Pairs of hex digits (`00`-`FF`)
- Case-insensitive
- Whitespace ignored (`DEADBEEF` and `DE AD BE EF` are equivalent)
- Must contain an even number of hex digits

### Extracting

#### `take <n><unit>`

Emit `n` units from cursor and move cursor. Negative values emit backward.

```bash
take 10b
take -2l
take 20c
```

#### `take to <location>`

Order-normalized extraction: emits `[min(cursor, L), max(cursor, L))` and moves cursor to the high end.

```bash
take to EOF
take to line-end
take to cursor+100b
```

#### `take until <string> [at match-start|match-end|line-start|line-end]`

Forward-only search from cursor. Emits `[cursor, B)` where `B` is derived from the match. Cursor moves only if `B > cursor`.

- Default `at match-start`: exclude delimiter
- `at match-end`: include delimiter
- `at line-start` / `at line-end`: relative to the matched line

#### `take until:re <regex> [at ...]`

Same behavior as `take until`, using regex matching.

#### `take until:bin <hex-string> [at ...]`

Same behavior as `take until`, using binary hex matching (same hex rules as `find:bin`).

### Movement and state

#### `skip <n><unit>`

Move cursor without output. Negative values move backward.

#### `skip to <location>`

Move cursor directly to a location without output.

#### `label <NAME>`

Set label at current cursor (fails if already set).

#### `clear <NAME>`

Unset a label so it can be assigned again.

#### `view <L1> <L2>`

Restrict subsequent operations to `[min(L1, L2), max(L1, L2))`.

Inside a view:

- Finds only search within view
- Cursor movement/extraction cannot leave view
- `skip to` fails if target is outside view
- View changes are clause-atomic
- View remains active until `clear view` or program end

#### `clear view`

Remove view restrictions.

### Output and control

#### `print <string>` (alias: `echo`)

Emit literal bytes (staged with clause atomicity).

Supported escapes: `\n`, `\t`, `\r`, `\0`, `\\`, `\xHH`, `\c` (cursor offset when staged).

#### `fail <message>`

Write message to stderr and fail the current clause.

Unlike staged output, this message is written immediately.
Supports the same escapes as `print`.

## Program Flow

Operations execute left-to-right against one cursor.

If a program has no `THEN` or `OR`, the entire program is a single clause.

### Clause atomicity

Each clause commits as a unit:

- If all ops succeed: staged output/state changes commit
- If any op fails: clause rolls back (no staged output, no cursor/label/view changes)

Example:

```bash
echo 'START hello' | fiskta find "START" skip 6b take 5b find "END" OR print "END missing\n"
```

`take 5b` stages `hello`, but `find "END"` fails, so `hello` is rolled back and only the fallback clause runs.

### Clause operators

- `THEN`: always run the next clause
- `OR`: run the next clause only if the current clause fails

Evaluation is strictly left-to-right (no operator precedence).

## Regex Syntax

Supported in `find:re` and `take until:re`:

- Character classes: `\d`, `\D`, `\w`, `\W`, `\s`, `\S`, `[a-z]`, `[^0-9]`
- Quantifiers: `*`, `+`, `?`, `{n}`, `{n,m}`, `{n,}`
- Lazy quantifiers: `*?`, `+?`, `??`, `{n,m}?`, `{n,}?`
- Grouping: `( ... )`
- Alternation: `|`
- Anchors: `^`, `$`
- Escapes: `\n`, `\t`, `\r`, `\f`, `\v`, `\0`
- Dot: `.` (any char except newline)

Limits:

- Max 16 distinct `{n,m}` counters per pattern
- Max 256 alternations

## Looping (Continue Mode)

Loop mode resumes from the last cursor position and preserves label/view state across iterations.

Append `THEN skip to EOF`:

```bash
fiskta --continue 1s --until-idle 0 --input service.log \
  find "WARNING:" take to line-end THEN skip to EOF
```

For files rewritten in place (not only appended), reset before scanning:

- Prefix with `clear view THEN skip to BOF`
- Keep `THEN skip to EOF` at the end

## More Examples

### Skip header, extract body

```bash
fiskta --input report.txt skip 5l take 20l
```

### Conditional extraction

```bash
fiskta --input auth.log \
  find "login success" skip to line-end skip -1l find "user=" skip 5b take until " "
```

### Binary file detection

```bash
fiskta --input image.bin \
  find:bin "89 50 4E 47 0D 0A 1A 0A" print "PNG" OR fail "Not a PNG file"
```

## Exit Codes

- `0`: success (includes normal `--until-idle` stop)
- `1`: program failure (no clause succeeded in final iteration)
- `2`: timeout (`--for` elapsed)
- `7`: CLI usage error
- `8`: parse error (program grammar or regex syntax)
- `9`: capacity exceeded (limits hit)
- `10`: I/O error
- `11`: resource exhaustion (allocation failure/OOM)

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

## Grammar

```ebnf
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

## Library Embedding

If you need to embed fiskta as a C library, see `docs/library-api.md`.

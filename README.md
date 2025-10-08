# (fi)nd (sk)ip (ta)ke

**fiskta** is a cursor-oriented data extraction tool. Unlike traditional text tools that match patterns on lines, fiskta lets you navigate through files with explicit position and movement commands—find a pattern, skip ahead, take some bytes. You think in terms of "where am I?" and "what do I do from here?" rather than "what pattern matches this line?"

It may be a good fit when grep is insufficient but you don't want to deal with something like awk: Extract text between delimiters, navigate multi-line structures, and build complex conditional extractions step by step. No cryptic syntax, just relatively straightforward imperative operations.

fiskta has no external dependencies. Linux binaries are < 100KB when dynamically linked or when statically linked with musl. Memory use does not depend on input, only on fiskta operations (regex requires more memory), but should not be more than a few megabytes at worst.

## What does it do?

Extract first 10 bytes:
```bash
$ echo 'GET /api/users HTTP/1.1' | fiskta take 10b
GET /api
```

Extract lines from a file:
```bash
$ fiskta --input report.txt skip 5l take 3l
# (skips first 5 lines, takes next 3)
```

Find a pattern and take the rest of the line:
```bash
$ echo 'ERROR: connection failed' | fiskta find "ERROR:" take to line-end
ERROR: connection failed
```
Try multiple patterns—first match wins:
```bash
$ echo 'WARNING: disk full' | fiskta find "ERROR:" OR find "WARNING:" AND take to line-end
WARNING: disk full
```

Extract text between delimiters:
```bash
$ echo 'start: [content here] end' | fiskta find "[" skip 1b take until "]"
content here
```


## Overview

**Extracting and Moving:**
- `take <n><unit>` - Extract n units from current position
- `skip <n><unit>` - Move cursor n units forward (no output)
- `take to <location>` - Order-normalized: emits `[min(cursor,L), max(cursor,L))`; cursor moves to the high end
- `take until <string> [at match-start|match-end|line-start|line-end]` - Forward-only: emits `[cursor, B)` where B is derived from the match; cursor moves only if B > cursor. Default: `at match-start` (exclude pattern). `line-start`/`line-end` are relative to the match.
- `take until:re <regex> [at match-start|match-end|line-start|line-end]` - Same as `take until` but with regex pattern support

**Searching:**
- `find [to <location>] <string>` - Search within `[min(cursor,L), max(cursor,L))`, default L=EOF; picks match closest to cursor
- `find:re [to <location>] <regex>` - Search using regular expressions within `[min(cursor,L), max(cursor,L))`; supports character classes, quantifiers, anchors

**Navigation:**
- `label <name>` - Mark current position with label
- `goto <location>` - Jump to labeled position
- `view <L1> <L2>` - Limit all ops to `[min(L1,L2), max(L1,L2))`
- `clear view` - Clear view; return to full file

**Utilities:**
- `print <string>` - Emit literal bytes (alias: echo). Supports escape sequences: `\n \t \r \0 \\ \xHH`. Participates in clause atomicity

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

- **Character Classes**: `\d` (digits), `\w` (word), `\s` (space), `[a-z]`, `[^0-9]`
- **Quantifiers**: `*` (0+), `+` (1+), `?` (0-1), `{n}` (exactly n), `{n,m}` (n to m)
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

#### **`AND`** - run next clause only if this one succeeds

This provides **conditional execution with short-circuiting**:
```bash
# AND: conditional execution - skip remaining clauses if any fails
find "ERROR" AND take to line-end AND print "\n"
# If ERROR found: outputs line + newline
# If ERROR not found: outputs nothing (short-circuits)

# Without AND (single clause): atomic rollback
find "ERROR" take to line-end print "\n"
# If ERROR not found: outputs nothing (atomic rollback)
# Same output as AND, but different mechanism

# With THEN: always runs all clauses
find "ERROR" THEN take to line-end THEN print "\n"
# If ERROR not found: outputs newline anyway (no short-circuit)
# Different output than AND!
```

AND combines the commit after clause success (regardless of outcome of later clauses)
behavior of THEN with the cascading failure mode of single-clause constructions.

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
- `-c, --commands <string|file>` - Provide operations as a string or file path
- `--` - Treat remaining args as operations

**Looping & Streaming:**
- `-l, --loop <time>` - Re-run program every time (requires suffix: `ms`, `s`, `m`, `h`; or `0` to disable)
- `-t, --loop-timeout <time>` - Stop after time with no input growth (requires suffix: `ms`, `s`, `m`, `h`)
- `--loop-view <policy>` - Which view of the file to process on each loop:
  - `cursor` - continue from last cursor position (default)
  - `delta` - only new data since last run
  - `rescan` - re-scan entire file each time

**Examples:**
```bash
# Operations as arguments (default)
fiskta find "ERROR" take to line-end < log.txt

# Operations as string
fiskta -c 'find "ERROR" take to line-end' --input log.txt

# Tail-like behavior
fiskta --loop 1s --loop-timeout 0 --input service.log find "ERROR" take to line-end
```

## Installation

### Using Make

```bash
make              # Build optimized binary (./fiskta)
make debug        # Build with debug symbols
make test         # Run test suite (requires Python 3)
```

### Using Zig (easy cross-compilation)

```bash
zig build                    # Build for host platform (zig-out/bin/fiskta)
zig build test               # Build and run test suite
zig build all                # Build for all platforms (Linux, macOS, Windows)
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
- Classes: `\d` (digit), `\w` (word), `\s` (space), `[a-z]`, `[^0-9]`
- Quantifiers: `*` (0+), `+` (1+), `?` (0-1), `{n}`, `{n,m}`
- Anchors: `^` (line start), `$` (line end)
- Alternation: `|`, Grouping: `(...)`, Any char: `.`
- Escapes: `\n`, `\t`, `\r`, `\f`, `\v`, `\0`

```bash
find:re "ERROR|WARN"                    # alternation
find:re "^\[.*\]"                       # line start anchor
find:re "[0-9]{1,3}\.[0-9]{1,3}"        # IP address pattern
find:re "[A-Za-z]+@[A-Za-z.]+"          # simple email pattern
```

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

#### `box <right><b|c> <down>l`

Extract a rectangular section from the cursor position. Creates a box of specified width and height, adding newlines after each line segment.

- `right`: horizontal offset in bytes (`b`) or characters (`c`); negative = left
- `down`: vertical offset in lines (`l`); negative = up
- Auto-detects line endings from first line (CRLF or LF)
- Strips trailing `\r` and adds consistent line endings
- Does not fail when going out of bounds

```bash
box 0b 0l                  # extract single byte at cursor + newline
box 2b 1l                  # extract 3x2 box (3 bytes wide, 2 lines tall)
box 5c 2l                  # extract 6 characters wide, 3 lines tall (respects UTF-8)
box -1b -1l                # extract box going left and up
box 10b 5l                 # extract large rectangular section
```

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

#### `goto <location>`

Jump cursor to a specific location.

```bash
goto BOF                 # jump to beginning
goto EOF                 # jump to end
goto MYLABEL             # jump to labeled position
goto match-start         # jump to start of last match
goto line-start          # jump to start of current line
goto cursor+100b         # jump 100 bytes from current position
```

### Labels

#### `label <name>`

Mark the current cursor position with a label. Labels can be used with `goto` and `take to`.

- Names must be UPPERCASE
- Start with A-Z, contain A-Z0-9_-
- Maximum 15 characters
- Maximum 32 labels (LRU eviction after that)

```bash
label START
label SECTION_A
label END-OF-HEADER
```

### Views

#### `view <location1> <location2>`

Restrict all subsequent operations to the range `[min(loc1, loc2), max(loc1, loc2))`. View boundaries are order-normalized.

Operations will fail if they try to move outside the view. View changes are staged and committed atomically with the clause.

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

Supports escape sequences: `\n` (newline), `\t` (tab), `\r` (CR), `\0` (null), `\\` (backslash), `\xHH` (hex byte)

```bash
print "---\n"           # print separator with newline
print "\x1b[31m"        # print ANSI color code
echo "Hello world"      # alias for print
```

## Streaming and Looping

fiskta can monitor a file and repeatedly execute your program as new data arrives, useful for tailing log files or processing streams.

### Loop Mode

Enable with `--loop <time>` to re-run your program at regular intervals. Time values require a suffix: `ms` (milliseconds), `s` (seconds), `m` (minutes), `h` (hours). The special value `0` (without suffix) disables looping:

```bash
fiskta --loop 1s --input service.log find "ERROR" take to line-end
```

This will check the file every second and output new ERROR lines.

### Loop View

Control which view of the file is processed on each iteration with `--loop-view`:

**`cursor`** (default) - Continue from last cursor position:
- Maintains cursor state across iterations
- Can use labels and goto across iterations
- Most flexible, preserves program state

**`delta`** - Only process new data since last run:
- Efficient for monitoring growing files
- Each iteration only sees newly appended data
- Good for log tailing

**`rescan`** - Re-scan entire file each iteration:
- Useful when file content changes (not just grows)
- Processes entire file every time

### Loop Timeout

Stop looping after a period of no file growth:

```bash
fiskta --loop 1s --loop-timeout 5s --input log.txt find "ERROR"
```

This will loop every second, but stop after 5 seconds of no input growth (file size unchanged). Use `--loop-timeout 0` to wait forever.

## Exit Codes

fiskta uses exit codes to indicate success, failure, and the type of error encountered.

- **0**: Success (at least one clause succeeded)
  - Even if some clauses fail, if any clause succeeds, the exit code is 0
- **1**: I/O error
  - File not found, permission denied, read/write errors
- **2**: Parse error
  - Invalid syntax, unknown operation, missing arguments
  - Caught during program parsing, before execution
- **3**: Regex error
  - Invalid regex pattern
- **4**: Resource limit
  - Program too large, out of memory during startup
- **10+**: All clauses failed (exit code = 10 + index of last failed clause)
  - Only returned when no clauses succeeded
  - The number indicates which clause failed last
  - Program execution continues through failures (with THEN/OR logic)

## Views and Scoping

Views let you restrict operations to a specific region of the file. This is useful for extracting from sections or preventing operations from wandering too far.

### Basic Usage

```bash
# Limit operations to bytes 100-200
fiskta --input data.txt view BOF+100b BOF+200b take to EOF

# Extract only from the [server] section
fiskta --input config.ini \
    find "[server]" skip to line-end label START \
    find "[" label END \
    view START END \
    find "port" take to line-end
```

### View Atomicity

Views are part of clause atomicity. If a clause fails, view changes are rolled back:

```bash
# View is only set if find succeeds
find "[section]" AND view cursor cursor+1000b
```

### View Scope

Once a view is set:
- `find` and `find:re` only search within the view
- `take` and `skip` can't move outside the view
- `goto` fails if the target is outside the view
- The view remains active until `clear view` or program ends

### Nested Views

You can't nest views, but you can replace them:

```bash
view BOF EOF-1000b    # exclude last 1000 bytes
# ... do stuff ...
view BOF EOF          # back to full file
```

## Common Patterns

**Extract from specific section:**
```bash
find "[database]" skip to line-end label S \
find "[" label E \
view S E \
label LOOP \
find "=" take to line-end \
goto LOOP
```

**Limit search scope:**
```bash
# Only look in next 1000 bytes for pattern
view cursor cursor+1000b find "marker"
```

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

### Extract after a marker

Problem: Find "ERROR:" and extract the rest of the line

```bash
fiskta --input app.log find "ERROR:" take to line-end
```

### Conditional extraction

Problem: Extract the username only if the line contains "login success"

```bash
fiskta --input auth.log \
    find "login success" AND skip to line-end skip -1l find "user=" skip 5b take until " "
```

### Try multiple patterns

Problem: Extract error messages that might start with "ERROR:" or "FATAL:"

```bash
fiskta --input app.log \
    find "ERROR:" OR find "FATAL:" AND take to line-end
```

### Extract between delimiters

Problem: Extract content between `<tag>` and `</tag>`

```bash
fiskta --input data.xml find "<tag>" skip 5b take until "<"
```

### Extract all occurrences

Problem: Extract all email addresses (using labels and goto for loops)

```bash
fiskta --input contacts.txt \
    label START \
    find:re "[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+" take to match-end print "\n" \
    goto START
```

### Extract from specific section

Problem: Extract all config values from the `[database]` section only

```bash
fiskta --input config.ini \
    find "[database]" skip to line-end \
    view cursor cursor+1000b \
    label LOOP \
    find "=" skip -1l take to line-end \
    goto LOOP
```

---

## Grammar

```
Program        = Clause { ( "THEN" | "AND" | "OR" ) Clause } .
Clause         = { Op } .
Op             = Find | FindRegex | Skip | Take | Label | Goto
               | View | ClearView | Print .
Find           = "find" [ "to" LocationExpr ] String .
FindRegex      = "find" ":" "re" [ "to" LocationExpr ] String .
Skip           = "skip" Number Unit .
Take           = "take" ( SignedNumber Unit
                          | "to" LocationExpr
                          | "until" String [ "at" AtExpr ]
                          | "until" ":" "re" String [ "at" AtExpr ] ) .
Label          = "label" Name .
Goto           = "goto" LocationExpr .
View           = "view" LocationExpr LocationExpr .
ClearView      = "clear" "view" .
Print          = ( "print" | "echo" ) String .
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
ShellString    = shell-quoted non-empty byte string .
```



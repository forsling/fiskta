# fiskta

**fiskta** (FInd SKip TAke) is a cursor-oriented data extraction tool. You write imperative programs that navigate through files and extract what you need—find a pattern, skip ahead, take some bytes. No cryptic syntax, just straightforward step-by-step (hopefully) intuitive operations.

* Zero dependencies beyond a C11 compiler and libc.
* Binary size: Stripped binary < 100 KiB.
* Memory Usage: ~2-10 MiB.

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

Extract fields conditionally—take the path only if the method is POST:
```bash
$ echo 'POST /api/login HTTP/1.1' | fiskta find POST AND skip 1b take until " "
/api/login
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

## How is this different?

Most text tools are pattern-based: you write a pattern that matches a line, then extract fields from it. fiskta is cursor-based: you write a sequence of operations that move a cursor and extract regions.

**Traditional approach** (awk):
```bash
# Match lines starting with ERROR, extract field 2
awk '/^ERROR/ {print $2}' log.txt
```

**fiskta approach**:
```bash
# Find "ERROR", move past it, take until space
fiskta --input log.txt find "ERROR" skip 6b take until " "
```

The fiskta approach is more explicit about position and movement, which makes it easier to:
- Extract text between delimiters without regex capture groups
- Navigate multi-line structures (skip 5 lines, take 3 lines)
- Build up complex extractions step by step
- Conditionally extract based on what you find

You think in terms of "where am I?" and "what do I do from here?" rather than "what pattern matches this line?"

## Core concepts

### Cursor and Position

You have a **cursor** that points to a position in the file (starting at byte 0). Operations move the cursor or extract text relative to it.

### Units

Operations measure distance in:
- **`b`** - bytes
- **`l`** - lines (LF-terminated; `\r` is treated as a regular byte)
- **`c`** - UTF-8 code points (never splits multi-byte sequences)

Examples: `10b`, `5l`, `20c`, `-3b` (negative = backward)

### Locations

You can refer to specific positions:
- **`cursor`** - current position
- **`BOF`** / **`EOF`** - beginning/end of file
- **`match-start`** / **`match-end`** - from last `find`/`findr`
- **`line-start`** / **`line-end`** - current line boundaries
- **`LABEL_NAME`** - saved positions (uppercase names)

Locations can have offsets: `BOF+100b`, `EOF-50b`, `cursor+10l`

### Basic Operations

**Finding**: Search for patterns and move cursor to them
- `find "text"` - search forward for literal text
- `find to BOF "text"` - search backward
- `find to cursor+1000b "text"` - search forward but only 1000 bytes
- `findr "regex"` - search using regular expressions

**Extracting**: Output text and move cursor
- `take 10b` - extract 10 bytes forward
- `take -5b` - extract 5 bytes backward
- `take to EOF` - extract from cursor to end of file
- `take until ";"` - extract until pattern found (forward only)

**Moving**: Change cursor position without output
- `skip 100b` - move forward 100 bytes
- `goto EOF` - jump to end of file
- `goto MYLABEL` - jump to saved position

**Labels**: Save and reuse positions
- `label NAME` - save current cursor position (uppercase names: `A-Z`, `0-9`, `_`, `-`)
- `goto NAME` - jump to saved position

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

**`AND`** - run next clause only if this one succeeds
```bash
find "ERROR" AND take to line-end       # take only if ERROR found
```

**`OR`** - run next clause only if this one fails
```bash
find "cache" OR find "buffer"           # try cache first, fallback to buffer
```

Evaluation is strictly left-to-right (no operator precedence).

## When to use fiskta

**Good fit:**
- Extracting fields from structured logs or data files
- Navigating multi-line text structures (skip headers, extract body)
- Conditional extraction (take X only if Y is present)
- Extracting regions between delimiters
- Building extraction logic step-by-step

**Not a good fit:**
- Simple line filtering (use `grep`)
- Field splitting on single-character delimiters (use `cut` or `awk`)
- Complex transformations of content (use `sed` or `awk`)
- Full document parsing (use a proper parser)

fiskta sits between grep (simple pattern matching) and awk (field processing/transformation). It's for when grep is too simple but you don't need awk's transformation capabilities—you just need to navigate and extract.

## Installation

```bash
make          # builds ./fiskta
make test     # runs test suite
```

Zero dependencies (only libc). Works on Linux, macOS, BSDs, Windows.

## Common Patterns

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
    findr "[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+" take to match-end print "\n" \
    goto START
```

### Extract from specific section

Problem: Extract all config values from the `[database]` section only

```bash
fiskta --input config.ini \
    find "[database]" skip to line-end \
    viewset cursor cursor+1000b \
    label LOOP \
    find "=" skip -1l take to line-end \
    goto LOOP
```

## Operations Reference

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

#### `findr [to <location>] <regex>`

Search using regular expressions. Same search behavior as `find`.

Regex syntax:
- Classes: `\d` (digit), `\w` (word), `\s` (space), `[a-z]`, `[^0-9]`
- Quantifiers: `*` (0+), `+` (1+), `?` (0-1), `{n}`, `{n,m}`
- Anchors: `^` (line start), `$` (line end)
- Alternation: `|`, Grouping: `(...)`, Any char: `.`
- Escapes: `\n`, `\t`, `\r`, `\f`, `\v`, `\0`

```bash
findr "ERROR|WARN"                    # alternation
findr "^\[.*\]"                       # line start anchor
findr "[0-9]{1,3}\.[0-9]{1,3}"        # IP address pattern
findr "[A-Za-z]+@[A-Za-z.]+"          # simple email pattern
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

#### `take until <pattern> [at <location>]`

Forward-only search. Extracts from cursor until pattern is found. Cursor moves only if match is past current position.

The `at` clause controls where extraction ends relative to the match:
- Default: `at match-start` (exclude the pattern)
- `at match-end` (include the pattern)
- `at line-start` (up to start of line containing match)
- `at line-end` (up to end of line containing match)

```bash
take until ";"                    # extract until semicolon (excluded)
take until "END" at match-end     # extract until END (included)
take until "\n\n"                 # extract until blank line
take until "---" at line-start    # extract until line with ---
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

#### `viewset <location1> <location2>`

Restrict all subsequent operations to the range `[min(loc1, loc2), max(loc1, loc2))`. View boundaries are order-normalized.

Operations will fail if they try to move outside the view. View changes are staged and committed atomically with the clause.

```bash
viewset BOF+100b EOF-100b        # exclude first/last 100 bytes
viewset cursor cursor+1000b      # limit to next 1000 bytes
viewset SECTION_START SECTION_END # limit to labeled section
```

#### `viewclear`

Remove view restrictions, returning to full file access.

```bash
viewclear
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

#### `sleep <duration>`

Pause execution. Duration is specified with `ms` (milliseconds) or `s` (seconds) suffix.

```bash
sleep 100ms      # sleep 100 milliseconds
sleep 1s         # sleep 1 second
sleep 500ms      # sleep half a second
```

## Logical Operators

Operations are grouped into **clauses** separated by logical operators. Each clause is atomic—all operations succeed and commit together, or all fail and roll back.

### The Three Operators

**`THEN`** - Sequential execution (always run next clause)
```bash
find "user" THEN take to line-end       # always take, even if find fails
```

**`AND`** - Conditional execution (run next only if this succeeds)
```bash
find "ERROR" AND take to line-end       # take only if ERROR is found
```

**`OR`** - Alternative execution (run next only if this fails)
```bash
find "cache" OR find "buffer"           # try cache, fallback to buffer
```

### The Four Rules

Evaluation is **strictly left-to-right** with these rules:

1. **THEN always executes** - the next clause runs regardless of previous success/failure
2. **AND creates chains** - if any clause in an AND chain fails, skip the rest of the chain
3. **OR short-circuits** - if a clause succeeds, skip remaining consecutive OR clauses
4. **THEN breaks chains** - clauses after THEN always run, even if previous AND/OR chains failed

### Common Patterns

```bash
# Sequential: always do both
find "start" THEN take 10b

# Conditional: only take if find succeeds
find "ERROR" AND take to line-end

# Fallback: try first, then second
find "primary" OR find "secondary"

# Cleanup: always run final clause
find "data" AND take until "end" THEN skip 1l
```

### Important: Left-to-Right Evaluation

There's **no operator precedence**. Evaluation is strictly left-to-right, which creates some non-obvious behaviors:

```bash
# A OR B AND C evaluates as (A OR B) AND C
# If A succeeds, B is skipped, then C runs
find abc OR find def AND take +3b

# A AND B OR C evaluates as (A AND B) OR C
# If A AND B succeeds, C is skipped
# If A AND B fails, C runs as fallback
find abc AND take +3b OR skip 1b

# A AND B OR C AND D evaluates as ((A AND B) OR C) AND D
# NOT as (A AND B) OR (C AND D)
# If (A AND B) succeeds, skip C but run D
find abc AND skip 3b OR find def AND take +3b
```

**Rule of thumb**: Chains only contain one operator type. An AND chain ends when you hit OR or THEN. An OR chain ends when you hit AND or THEN.

If you need different grouping, use THEN to break chains or use multiple invocations.

### Exit Status

`fiskta` uses a diagnostic exit code system to help you understand what happened:

**Success:**
- **0** - Success: at least one clause succeeded

**System/Parse Errors (1-9):**
- **1** - I/O error (file not found, permission denied, disk full, etc.)
- **2** - Parse error (invalid syntax, unknown operation, bad arguments)
- **3** - Regex error (invalid regex pattern)
- **4** - Resource limit (program too large, out of memory)

**Execution Failures (10+):**
- **10+** - Clause N failed (exit code = 10 + clause index)
  - Exit 10: First clause (index 0) failed
  - Exit 11: Second clause (index 1) failed
  - Exit 12: Third clause (index 2) failed
  - And so on...

## Locations, Units & Syntax

### Units

- `b` - Bytes
- `l` - Lines (LF-terminated; CR is treated as a regular byte)
- `c` - UTF-8 code points (never splits multi-byte sequences)

### Locations

- `cursor` - Current cursor position
- `BOF` - Beginning of file (position 0)
- `EOF` - End of file
- `match-start` - Start of last successful find/findr match
- `match-end` - End of last successful find/findr match
- `line-start` - Start of current line (line containing cursor)
- `line-end` - End of current line
- `<NAME>` - Position of a labeled location

**Note**: In most contexts, `line-start`/`line-end` are relative to the cursor. In the `at` clause of `take until`, they're relative to the match position.

### Offsets

Locations can be modified with offsets:

```bash
BOF+100b         # 100 bytes after beginning
EOF-50b          # 50 bytes before end
cursor+10l       # 10 lines after cursor
MYLABEL-5c       # 5 characters before label
match-end+1b     # 1 byte after match end
```

### Label Names

- Must start with uppercase letter (A-Z)
- Can contain: A-Z, 0-9, `_`, `-`
- Maximum 15 characters
- Examples: `START`, `SECTION_A`, `END-OF-BLOCK`


## Command-Line Options

### Input/Output

```bash
-i, --input <path>              # Read from file instead of stdin
```

### Command Modes

```bash
-c, --commands <string>         # Provide operations as a string
--commands-stdin                # Read operations from stdin (one program per line)
--                              # Treat remaining args as operations
```

Examples:
```bash
# Operations as arguments (default)
fiskta find "ERROR" take to line-end < log.txt

# Operations as string
fiskta -c 'find "ERROR" take to line-end' --input log.txt

# Operations from stdin, data from file
echo 'find "ERROR" take to line-end' | fiskta --commands-stdin --input log.txt
```

### Looping & Streaming

```bash
--loop <ms>                     # Re-run program every N milliseconds
--idle-timeout <ms>             # Stop after N ms with no input growth
--window-policy <policy>        # How to process data on each loop:
                                #   cursor - continue from last cursor position (default)
                                #   delta  - only new data since last run
                                #   rescan - re-scan entire file each time
```

Example - tail-like behavior:
```bash
fiskta --loop 1000 --idle-timeout 0 --input service.log \
    find "ERROR" take to line-end
```

## Advanced: Streaming and Looping

fiskta can monitor a file and repeatedly execute your program as new data arrives, useful for tailing log files or processing streams.

### Loop Mode

Enable with `--loop <ms>` to re-run your program every N milliseconds:

```bash
fiskta --loop 1000 --input service.log find "ERROR" take to line-end
```

This will check the file every second and output new ERROR lines.

### Window Policies

Control what data is processed on each iteration with `--window-policy`:

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
- Higher CPU usage

### Idle Timeout

Stop looping after N milliseconds of no file growth:

```bash
fiskta --loop 1000 --idle-timeout 5000 --input log.txt find "ERROR"
```

This will loop every second, but stop after 5 seconds of no new data. Use `--idle-timeout 0` to wait forever.

### Command Stream Mode

Read operations from stdin, data from a file. Each line of stdin is a separate program:

```bash
echo -e 'find "ERROR"\nfind "WARN"' | fiskta --commands-stdin --input log.txt
```

This runs two programs sequentially: first extracts ERROR lines, then WARN lines.

Useful for:
- Processing a file multiple ways without re-reading
- Dynamic program generation
- Scripting complex extraction logic

---

## Advanced: Views and Scoping

Views let you restrict operations to a specific region of the file. This is useful for extracting from sections or preventing operations from wandering too far.

### Basic Usage

```bash
# Limit operations to bytes 100-200
fiskta --input data.txt viewset BOF+100b BOF+200b take to EOF

# Extract only from the [server] section
fiskta --input config.ini \
    find "[server]" skip to line-end label START \
    find "[" label END \
    viewset START END \
    find "port" take to line-end
```

### View Atomicity

Views are part of clause atomicity. If a clause fails, view changes are rolled back:

```bash
# View is only set if find succeeds
find "[section]" AND viewset cursor cursor+1000b
```

### View Scope

Once a view is set:
- `find` and `findr` only search within the view
- `take` and `skip` can't move outside the view
- `goto` fails if the target is outside the view
- The view remains active until `viewclear` or program ends

### Nested Views

You can't nest views, but you can replace them:

```bash
viewset BOF EOF-1000b    # exclude last 1000 bytes
# ... do stuff ...
viewset BOF EOF          # back to full file
```

### Common Patterns

**Extract from specific section:**
```bash
find "[database]" skip to line-end label S \
find "[" label E \
viewset S E \
label LOOP \
find "=" take to line-end \
goto LOOP
```

**Limit search scope:**
```bash
# Only look in next 1000 bytes for pattern
viewset cursor cursor+1000b find "marker"
```

---

## Grammar

```
Program        = Clause { ( "THEN" | "AND" | "OR" ) Clause } .
Clause         = { Op } .
Op             = Find | FindRegex | Skip | Take | Label | Goto
               | Viewset | Viewclear | Sleep | Print .
Find           = "find" [ "to" LocationExpr ] String .
FindRegex      = "findr" [ "to" LocationExpr ] String .
Skip           = "skip" Number Unit .
Take           = "take" ( SignedNumber Unit
                        | "to" LocationExpr
                        | "until" String [ "at" AtExpr ] ) .
Label          = "label" Name .
Goto           = "goto" LocationExpr .
Viewset        = "viewset" LocationExpr LocationExpr .
Viewclear      = "viewclear" .
Sleep          = "sleep" Duration .
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
Duration       = Number ( "ms" | "s" ) .
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

## Build & Test

```bash
make              # Build optimized binary (./fiskta)
make debug        # Build with debug symbols
make test         # Run test suite (requires Python 3)
```


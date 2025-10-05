# fiskta - (fi)nd (sk)ip (ta)ke

fiskta is a minimal streaming text extractor built around a cursor oriented execution model, independent atomic clauses. Built with plain C and with zero dependencies.

## Highlights
- **Streaming by default:** bounded buffers work on stdin or huge files without spills.
- **Cursor-driven:** each operation is relative to the cursor, keeping programs easy to reason about.
- **Atomic clauses:** clauses commit all staged output and labels together or roll back entirely.
- **No surprises:** single startup allocation, no threads, no locale quirks, no regex backtracking traps.

## Basic Usage Examples

```bash
fiskta [options] <operations> [file|-]
```

Extract the HTTP request path after the method token:
```bash
./fiskta --input access.log find " " skip 1b take until " "
```
Capture ten lines from the body after skipping the header block:
```bash
./fiskta --input reports.txt goto BOF skip 1000l take 10l
```
Emit 20 UTF-8 characters around a marker without splitting code points:
```bash
./fiskta --input journal.txt find "[OK]" take -10c take 20c
```
Regex capture of an email address, then emit the remainder of the line:
```bash
./fiskta --input mailboxes.txt findr "[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+" take to line-end
```
Fall back to a secondary match when the primary clause fails:
```bash
./fiskta --input service.log findr "^WARN" take to line-end THEN findr "^INFO" take to line-end
```
Re-run the same program every second until the input grows:
```bash
./fiskta --loop 1000 --idle-timeout 0 --input service.log find "STATUS" take to EOF
```

## Usage
```
  fiskta [options] <operations>
  (use --input <path> to select input; defaults to stdin)

OPERATIONS:
  take <n><unit>              Extract n units from current position
  skip <n><unit>              Move cursor n units forward (no output)
  find [to <location>] <string>
                              Search within [min(cursor,L), max(cursor,L)),
                              default L=EOF; picks match closest to cursor
  findr [to <location>] <regex>
                              Search using regular expressions within
                              [min(cursor,L), max(cursor,L)); supports
                              character classes, quantifiers, anchors
  take to <location>          Order-normalized: emits [min(cursor,L), max(cursor,L));
                              cursor moves to the high end
  take until <string> [at <location>]
                              Forward-only: emits [cursor, B) where B is derived
                              from the match; cursor moves only if B > cursor
  label <name>                Mark current position with label
  goto <location>             Jump to labeled position
  viewset <L1> <L2>           Limit all ops to [min(L1,L2), max(L1,L2))
  viewclear                   Clear view; return to full file
  sleep <duration>            Pause execution; duration suffix ms or s (e.g., 500ms, 1s)
  print <string>              Emit literal bytes (alias: echo)
                              Supports escape sequences: \n \t \r \0 \\ \xHH
                              Participates in clause atomicity

UNITS:
  b                           Bytes
  l                           Lines (LF only, CR treated as bytes)
  c                           UTF-8 code points (never splits sequences)

LABELS:
  NAME                        UPPERCASE, <16 chars, [A-Z0-9_-] (first must be A-Z)

LOCATIONS:
  cursor                      Current cursor position
  BOF                         Beginning of file
  EOF                         End of file
  match-start                 Start of last match
  match-end                   End of last match
  line-start                  Start of current line
  line-end                    End of current line
  <label>                     Named label position
  Note: line-start/line-end are relative to the cursor here; in "at" (for
  "take until") they're relative to the last match.

OFFSETS:
  <location> +<n><unit>       n units after location
  <location> -<n><unit>       n units before location
                              (inline offsets like BOF+100b are allowed)

REGEX SYNTAX:
  Character Classes: \d (digits), \w (word), \s (space), [a-z], [^0-9]
  Quantifiers: * (0+), + (1+), ? (0-1), {n} (exactly n), {n,m} (n to m)
  Grouping: ( ... ) (group subpatterns), (a|b)+ (quantified groups)
  Anchors: ^ (line start), $ (line end)
  Alternation: | (OR)
  Escape: \n, \t, \r, \f, \v, \0
  Special: . (any char except newline)

CLAUSES AND LOGICAL OPERATORS:
  Operations are grouped into clauses connected by logical operators:
    THEN    Sequential execution (always runs next clause)
    AND     Both clauses must succeed (short-circuits on failure)
    OR      First success wins (short-circuits on success)

  Within a clause: all ops must succeed or the clause fails atomically.
  On Failure: clause rolls back (no output or label changes).
  On Success: emits staged output, commits labels, updates cursor and last-match.

  Evaluation is strictly left-to-right with these rules:
    1. THEN always executes the next clause (regardless of previous result)
    2. AND creates a chain - if any clause in an AND chain fails, skip
       remaining clauses in that chain and continue after it
    3. OR short-circuits - if a clause succeeds, skip all remaining OR
       alternatives in that chain
    4. THEN acts as a "chain breaker" - clauses after THEN always run,
       even if previous AND chains failed

  Examples:
    find abc THEN take +3b          # Sequential: always do both
    find abc AND take +3b           # Conditional: take only if find succeeds
    find abc OR find xyz            # Alternative: try abc, or try xyz
    find abc AND take +3b THEN skip 1b   # Mixed: skip runs even if AND fails

  Exit status: succeeds (0) if any clause commits OR if AND chains fail but
  later THEN clauses succeed; fails (2) if AND chains fail with no successful
  clauses after.

OPTIONS:
  -i, --input <path>          Read input from path (default: stdin)
  -c, --commands <string>     Provide operations as a single string
      --                      Treat subsequent arguments as operations
      --loop <ms>             Re-run the program every ms (0 disables looping)
      --idle-timeout <ms>     Stop looping after ms with no input growth
      --window-policy <p>     Loop window policy: delta | rescan | cursor (default: cursor)
  -h, --help                  Show this help message
  -v, --version               Show version information
```

## Grammar
```
Program        = Clause { "THEN" Clause } .
Clause         = { Op } .
Op             = Find | FindRegex | Skip | Take | Label | Goto | Viewset | Viewclear | Sleep | Print .
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
               | "match-start" | "match-end" | "line-start" | "line-end" .
AtExpr         = ( "match-start" | "match-end" | "line-start" | "line-end" ) [ Offset ] .
Offset         = ( "+" | "-" ) Number Unit .
SignedNumber   = [ "+" | "-" ] Number .
Unit           = "b" | "l" | "c" .
Duration       = Number "ms" | Number "s" .
Number         = Digit { Digit } .
Name           = Upper { Upper | Digit | "_" | "-" } .
String         = ShellString .
Upper          = "A" | "B" | "C" | "D" | "E" | "F" | "G"
               | "H" | "I" | "J" | "K" | "L" | "M" | "N"
               | "O" | "P" | "Q" | "R" | "S" | "T" | "U"
               | "V" | "W" | "X" | "Y" | "Z" .
Digit          = "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9" .
Path           = filesystem path string .
ShellString    = shell-quoted non-empty byte string .
```

## Build & Test
- `make` builds the optimized binary (`fiskta`).
- `make debug` builds with symbols and `DEBUG` instrumentation.
- `make test` runs the Python acceptance suite (`tests/run_tests.py`).

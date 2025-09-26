# fiskta — FInd SKip TAke

Minimal streaming text extractor with a cursor-first model and atomic clauses. Zero dependencies.

* **Streaming:** bounded memory; works on files and stdin.
* **Cursor-anchored:** every `take` starts or ends at the cursor.
* **Clauses:** groups of ops that commit all-or-nothing, separated by `::`.

---

## Operations (exhaustive, quick to learn)

All commands end with `<file|->`. Units: `b` = bytes, `l` = lines (LF `0x0A`; CR is just a byte).

### `find [to <loc-expr>] <needle>`

Search within a window from the **cursor** to `L` (default `L=EOF`).

* **Window:** `[min(cursor,L), max(cursor,L))`
* **Selection:** forward window → **first** match; backward window → **rightmost** match (closest to the cursor)
* **Effect:** `cursor = match-start`; `last_match = {ms,me}`
* Empty needles are invalid.

**Examples**

```bash
find "ERROR"
find to BOF "HEADER"       # search backward window, pick rightmost
find to LABEL+1l "X"
```

---

### `skip <N><b|l>`

Move the cursor **forward** by bytes or whole lines (clamped to file bounds).

**Examples**

```bash
skip 100b
skip 2l
```

---

### `take <±N><b|l>`

Signed length capture anchored at the cursor.

* `+N`: emit `[cursor, cursor+N)`
* `-N`: emit `[cursor-N, cursor)`  (i.e., “last N …”)
* **Cursor law:** cursor moves to the **far end** (the first byte **after** what was emitted)
* Empty spans succeed and don’t move.

**Examples**

```bash
take 200b
take -3l                     # the 3 lines before the cursor’s line
```

---

### `take to <loc-expr>`

Capture the span between the cursor and a location. **Order-normalized** and **half-open**.

* Emit `[min(cursor,L), max(cursor,L))`
* Cursor moves to `max(cursor,L)`

**Examples**

```bash
take to EOF
take to BOF+100b
take to LABEL-1l
```

---

### `take until <needle> [at <at-expr>]`

Search **forward** from the cursor, then capture up to a derived point.

* On match: compute base `B` from `<at-expr>` relative to the match (default `match-start`)
* Emit `[cursor, B)`, set `cursor = B`
* Empty needles invalid.
* Cursor **does** move to the derived point (even if no bytes were emitted).

**Examples**

```bash
take until "ERROR"                    # up to match-start
take until "ERROR" at match-end       # up to 1 past match
take until "ERROR" at line-start+1l   # up to start of the line after ERROR’s line
```

---

### `label <NAME>` / `goto <loc-expr>`

* `label` stages `NAME := cursor` (commits on clause success). Up to 32 labels; LRU eviction on overflow.
* `goto` resolves a location and sets the cursor.

**Examples**

```bash
label START
goto START+2l
```

---

## Usage

```bash
fiskta <tokens...> <file|->
```

**Examples**

```bash
fiskta take 100b file.txt
fiskta find "HEADER" take to match-start file.txt
echo "hello world" | fiskta take 5b -
```

---

## Clauses (atomic execution)

Separate clauses with `::`. Each clause stages captures, label writes, and cursor/match updates:

* If **any op fails** → the **whole clause** is rolled back (no output, no state change).
* On **success** → staged captures stream to stdout **in the order staged**, labels commit, then cursor/match update.
* Program exit status is **success if any clause succeeds**.

**Examples**

```bash
# Two independent attempts
fiskta find "ERROR" take 80b :: find "WARNING" take 80b file.txt

# Mark then use (two clauses)
fiskta label HERE :: goto HERE take 20b file.txt
```

---

## Range semantics (half-open) & “inverted” spans

All captures use half-open intervals `[start, end)`.

* `take to <L>` always emits `[min(cursor,L), max(cursor,L))`
* Cursor moves to the **high end** (`max(cursor,L)`)
* All `take*` ops move the cursor to the **byte just after the emitted range**. If nothing was emitted (empty range), the cursor **does not move**.

**Inverted example**

```bash
# cursor=5
fiskta skip 5b take to BOF file.txt
# Emits [0,5): bytes 0..4; cursor stays 5
```

**Equivalences**

```bash
skip 5b; take to BOF     # [0,5)
take -5b                 # also [0,5)
```

Empty captures (e.g., `take 0b`, `take to cursor`) **succeed** and do not move the cursor.

---

## Locations & offsets

`loc-expr := loc [ ±N<b|l> ]`

* Bases: `cursor | BOF | EOF | <LABEL> | match-start | match-end | line-start | line-end`
* Offsets can be in bytes or **lines** (line offsets step whole line boundaries)
* `line-start` / `line-end` (in `loc-expr`) are computed from the **cursor**. In `at-expr`, they're computed from the **last match**.
* `at-expr` (for `take until`) uses the same bases but **requires a valid last match**

**Examples**

```bash
BOF+100b     EOF-2l
match-end+1b line-start
LABEL-3l
```

---

## Lines (unit `l`)

* Lines end at LF (`0x0A`) or EOF; CR (`0x0D`) is just a byte.
* Line captures anchor at the **line start** of the cursor’s line.
* `take +Nl` → from that line start forward by N complete lines.
  `take -Nl` → the previous N complete lines ending at that line start.

---

## Direction cheat sheet

* **`find`**: direction is implied by the window (`cursor → L`); forward picks **first**, backward picks **rightmost**.
* **`take ±N`**: sign sets direction relative to the cursor.
* **`take to`**: order-normalized; you don’t need to know which side is earlier.
* **`take until`**: always searches forward; the `at` target can land before/inside/after the match depending on offsets.

---

## Practical recipes

```bash
# Nearest ERROR in the last MiB, then capture back to BOF
fiskta find to cursor-1048576b "ERROR" take to BOF file.txt

# Previous 5 lines, then next 20 bytes
fiskta take -5l take 20b file.txt

# Everything between here and a label, order unknown
fiskta take to HERE file.txt

# Emit up to (but not including) the line that starts with 'END'
fiskta take until "END" at line-start file.txt

# Last 50 bytes of a file
fiskta goto EOF take -50b file.txt
```

---

## Exit codes

* `0` — at least one clause succeeded
* `2` — parse or execution error (all clauses failed)

---

## Appendix — Grammar (BNF-ish)

```
program   := clause { "::" clause } input
clause    := { op }

op        := find | skip | take | label | goto

find      := "find" [ "to" loc-expr ] STRING
skip      := "skip" N ("b"|"l")
take      := "take" ( signedN ("b"|"l") | "to" loc-expr | "until" STRING [ "at" at-expr ] )
label     := "label" NAME
goto      := "goto"  loc-expr

loc-expr  := loc [ offset ]
loc       := "cursor" | "BOF" | "EOF" | NAME
          |  "match-start" | "match-end" | "line-start" | "line-end"

at-expr   := ("match-start"|"match-end"|"line-start"|"line-end") [ offset ]

offset    := ("+"|"-") N ("b"|"l")
signedN   := ["+"|"-"] N

# Lexical
N         := DIGIT { DIGIT }
NAME      := UPPER { UPPER | "_" | "-" } ; length ≤ 16
STRING    := shell-quoted non-empty byte string
UPPER     := "A"…"Z"
DIGIT     := "0"…"9"
```

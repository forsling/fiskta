# Fiskta (FInd SKip TAke)

## Overview

Fiskta is a command-line tool for extracting text from files or stdin using an intuitive set of operations.

## Key Features

- **Streaming Processing**: Handles files of any size with bounded memory usage
- **Cursor-Anchored Operations**: All operations are relative to a current cursor position
- **Clause Atomicity**: Operations within a clause execute all-or-nothing

## Installation

```bash
# Build from source
make
sudo make install

# Or run directly
./fiskta [options] <operations> [file]
```

## Quick Start

```bash
# Extract first 10 bytes
fiskta take 10b file.txt

# Extract first 3 lines
fiskta take 3l file.txt

# Find text and extract to that point
fiskta find "pattern" take to match-end file.txt

# Skip 5 bytes then take 10 bytes
fiskta skip 5b take 10b file.txt

# Process from stdin
echo "Hello World" | fiskta take 5b -
```

## Operations

### Basic Operations

- **`take <n><unit>`** - Extract n units from current position
- **`skip <n><unit>`** - Move cursor n units forward (no output)
- **`find <string>`** - Search for string and update cursor to match position

### Advanced Operations

- **`take to <location>`** - Extract from cursor to specified location (half-open range, excludes far endpoint)
- **`take until <string>`** - Extract from cursor until string is found
- **`label <name>`** - Mark current position with a label
- **`goto <location>`** - Jump to a labeled position

### Units

- **`b`** - Bytes
- **`l`** - Lines (split only on LF `0x0A`, CR `0x0D` treated as regular bytes)

### Locations

- **`cursor`** - Current cursor position
- **`BOF`** - Beginning of file
- **`EOF`** - End of file
- **`match-start`** - Start of last match
- **`match-end`** - End of last match
- **`line-start`** - Start of current line
- **`line-end`** - End of current line
- **`<label>`** - Named label position (UPPERCASE, ≤16 chars, `[A-Z_-]`)

### Offsets

Locations can be modified with offsets:
- **`BOF +5b`** - 5 bytes after beginning of file
- **`EOF -10b`** - 10 bytes before end of file
- **`cursor +1l`** - 1 line after current cursor
- **`match-end -3b`** - 3 bytes before end of last match

### Range Semantics

**`take to <location>`** operations use **half-open intervals** that exclude the far endpoint:

- **Range**: `[min(cursor, location), max(cursor, location))`
- **Cursor movement**: After `take to`, cursor moves to `max(cursor, location)`
- **Inverted ranges**: Order doesn't matter - `skip 5b take to BOF` gives same result as `take to BOF +5b` (both give range [0, 5))

#### Examples

```bash
# Skip 5 bytes, then take everything before cursor
fiskta skip 5b take to BOF file.txt
# Emits: [0, 5) → bytes at indices 0,1,2,3,4 (5 bytes before cursor)
# Cursor stays at 5

# Take everything after cursor
fiskta skip 5b take to EOF file.txt  
# Emits: [5, EOF) → everything after cursor, excluding cursor byte
# Cursor moves to EOF

# Empty capture
fiskta skip 5b take to cursor file.txt
# Emits: [5, 5) → empty range
# Cursor stays at 5
```

#### Equivalent Operations

```bash
# These are equivalent (both give range [0, 5)):
fiskta skip 5b take to BOF file.txt    # Inverted range [0, 5)
fiskta take to BOF +5b file.txt         # Forward range [0, 5)
```
### Clauses and Error Handling

**Clauses** are groups of operations separated by double colons (`::`). They are fundamental to how fiskta handles errors and operations:

#### Clause Atomicity

- **All-or-nothing execution**: If any operation in a clause fails, the entire clause is rolled back
- **State preservation**: Failed clauses don't affect the cursor position or file state
- **Independent execution**: Each clause executes independently; failure of one doesn't affect others
- **Staging**: Each clause maintains staged state (cursor, captures, labels) until commit

#### Single Clause

```bash
# Single clause - all operations must succeed
fiskta find "ERROR" take to match-end file.txt
```

If `find "ERROR"` fails (no match found), the entire command fails and nothing is extracted.

#### Multiple Clauses

```bash
# Multiple clauses - each clause is independent
fiskta find "ERROR" take to match-end "::" find "WARNING" take to match-end file.txt
```

- First clause: Find "ERROR" and extract to it
- If "ERROR" not found, first clause fails but second clause still executes
- Second clause: Find "WARNING" and extract to it
- Independent execution: failure of one clause doesn't affect others

#### Error Handling Examples

```bash
# Clause 1 fails, Clause 2 succeeds
fiskta find "MISSING" take 10b "::" find "FOUND" take 10b file.txt
# Result: Extracts 10 bytes from "FOUND" (first clause ignored)

# All clauses fail
fiskta find "A" take 5b "::" find "B" take 5b file.txt
# Result: No output, command fails

# All clauses succeed
fiskta find "FIRST" take 5b "::" find "SECOND" take 5b file.txt
# Result: Concatenated output from both matches
```

#### Why Clauses Matter

1. **Robust extraction**: If one pattern fails, try another
2. **Conditional processing**: Different extraction strategies
3. **Error isolation**: One failure doesn't break everything
4. **Complex workflows**: Chain multiple independent operations

#### Important Behaviors

- **Half-open ranges**: `take to <location>` excludes the far endpoint (cursor or target location)
- **Empty captures**: `take 0b` or `take to cursor` succeed but emit nothing
- **Cursor movement**: After `take`, cursor moves to the end of captured range
- **Inverted ranges**: `skip 5b take to BOF` ≡ `label HERE take to HERE` (both give [0, 5))
- **Line semantics**: Lines split only on LF (`0x0A`), CR (`0x0D`) is just a byte
- **Search direction**: Forward search finds first match, backward finds rightmost match
- **Label limits**: Maximum 32 labels with LRU eviction policy

## Examples

### Basic Text Extraction

```bash
# Extract first 20 characters
fiskta take 20b file.txt

# Extract first 5 lines
fiskta take 5l file.txt

# Extract from stdin
cat file.txt | fiskta take 10b -
```

### Search and Extract

```bash
# Find "ERROR" and extract everything up to it (excluding ERROR)
fiskta find "ERROR" take to match-start file.txt

# Find "END" and extract everything after it (excluding END)
fiskta find "END" take to match-end file.txt

# Extract until you find a string
fiskta take until "---" file.txt
```

### Location-Based Extraction

```bash
# Extract first 100 bytes (from BOF to BOF+100b)
fiskta take to BOF +100b file.txt

# Extract last 50 bytes (from EOF-50b to EOF)
fiskta take to EOF -50b file.txt

# Skip 10 bytes, then take next 20 bytes (from cursor to cursor+20b)
fiskta skip 10b take to cursor +20b file.txt
```

### Multi-Clause Operations

```bash
# Skip then extract (separate clauses)
fiskta skip 5b "::" take 10b file.txt

# Find then extract to end of line
fiskta find "pattern" take to line-end file.txt

# Complex extraction with labels
fiskta label start take 5b "::" goto start take 5b file.txt
```

### Error Handling with Clauses

```bash
# Fallback extraction - try multiple patterns
fiskta find "ERROR" take 10b "::" find "WARNING" take 10b "::" find "INFO" take 10b file.txt

# Conditional extraction - different strategies
fiskta find "header" take to match-end "::" find "body" take 100b file.txt

# Robust processing - continue on failure
fiskta find "section1" take 50b "::" find "section2" take 50b "::" find "section3" take 50b file.txt
```

### Advanced Patterns

```bash
# Extract with line-based offsets
fiskta take to BOF +2l file.txt

# Extract with match-based locations
fiskta find "header" take to match-end +1l file.txt

# Extract until string at specific location
fiskta take until "footer" at line-start file.txt
```

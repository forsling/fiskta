# fiskta

**fiskta** - (fi)nd (sk)ip (ta)ke: A fast, composable stream processing tool for extracting and manipulating data from files and streams.

[![License: zlib](https://img.shields.io/badge/License-zlib-blue.svg)](LICENSE)

## Overview

fiskta is a high-performance command-line tool and embeddable C library for stream-based data extraction. It provides a declarative language for navigating, searching, and extracting data from files using literal strings, regular expressions, or binary patterns.

Unlike traditional text processing tools, fiskta provides:
- **Zero-copy streaming architecture** with predictable memory usage
- **Atomic clause execution** with rollback on failure
- **Byte, line, and UTF-8 character navigation**
- **Custom Thompson NFA regex engine** with lazy quantifiers and bounded repetition
- **Binary pattern matching** via hex strings
- **Continue mode** for monitoring and following files
- **Embeddable library** with no dynamic allocation during execution

## Key Features

- **Multi-modal search**: literal strings, regex patterns, binary (hex) patterns
- **Flexible navigation**: skip/take by bytes, lines, or UTF-8 characters
- **Atomic operations**: clauses succeed or fail as units (all-or-nothing)
- **Logical operators**: `THEN` (sequential), `OR` (first-success)
- **Labels**: mark and reference positions in the file
- **Views**: restrict operations to specific file regions
- **Continue mode**: loop with configurable intervals and idle timeouts
- **Predictable performance**: no heap allocation during execution, fixed memory budget
- **Cross-platform**: Linux, macOS, Windows (static binaries available)

## Quick Start

```bash
# Extract lines 5-12 from a file
fiskta --input file.txt skip 5l take 8l

# Find and extract email addresses
fiskta -i contacts.txt find:re "[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+" take to match-end

# Extract database section from config file
fiskta -i config.ini find "[database]" skip 1l take until "["

# Verify PNG file signature
fiskta -i image.bin find:bin "89 50 4E 47 0D 0A 1A 0A" print "PNG" OR fail "Not a PNG file"

# Monitor log file for errors
fiskta -i app.log --continue 1s find "ERROR:" take to line-end
```

## Installation

### Pre-built Binaries

Download from the [releases page](https://github.com/forsling/fiskta/releases):
- Linux x86_64 (glibc and musl static)
- macOS ARM64
- Windows x86_64

### Build from Source

**Requirements**: Zig compiler (for cross-compilation) or any C11 compiler

#### Using Zig (recommended)

```bash
zig build
# Binary output: zig-out/bin/fiskta
```

#### Using shell script

```bash
./build.sh
# Binary output: ./fiskta
```

#### Custom compiler

```bash
cc -std=c11 -O3 -DFISKTA_VERSION=\"dev\" \
   src/main.c src/parse.c src/fiskta.c src/engine.c \
   src/fileio.c src/search_literal.c src/regex_vm.c \
   src/regex_prog.c src/util.c -o fiskta
```

## Usage

```
fiskta [options] <operations>
```

### Options

- `-i, --input <path>` - Read from file instead of stdin
- `--ops <string>` - Provide operations as a single string
- `--ops-file <path>` - Load operations from file
- `-c, --continue [delay]` - Enable loop mode with optional delay (ms|s|m|h)
- `-u, --until-idle <time>` - Stop when input is idle for specified duration
- `--for <time>` - Halt execution after specified duration
- `-k, --ignore-failures` - Continue on clause failures (useful with loops)
- `-h, --help` - Show help
- `-v, --version` - Show version

### Core Operations

#### Navigation

```bash
# Skip operations (move cursor without output)
skip <n><unit>              # Skip n bytes/lines/chars
skip to <location>          # Jump to location

# Units: b (bytes), l (lines), c (UTF-8 chars)
```

#### Extraction

```bash
# Take operations (extract and output)
take <n><unit>              # Extract n units
take to <location>          # Extract to location (order-normalized)
take until <pattern> [at <position>]
                            # Extract until pattern found
                            # Positions: match-start (default), match-end,
                            #           line-start, line-end
```

#### Search

```bash
# Find operations (move cursor to match)
find [to <location>] <string>      # Literal string search
find:re [to <location>] <regex>    # Regular expression search
find:bin [to <location>] <hex>     # Binary pattern search

# Take until variants
take until <string> [at <pos>]     # Extract until literal match
take until:re <regex> [at <pos>]   # Extract until regex match
take until:bin <hex> [at <pos>]    # Extract until binary match
```

#### Control Flow

```bash
# Logical operators (connect clauses)
THEN                        # Sequential: always run next clause
OR                          # Alternation: run if previous failed

# Utility operations
label <NAME>                # Mark current position
view <L1> <L2>              # Restrict operations to range
clear view                  # Remove restriction
print <string>              # Emit literal (supports \n, \t, \xHH, \c)
fail <message>              # Fail clause with message to stderr
```

### Locations

- `cursor` - Current position
- `BOF` - Beginning of file
- `EOF` - End of file
- `match-start` - Start of last match
- `match-end` - End of last match
- `line-start` - Start of current line
- `line-end` - End of current line
- `<LABEL>` - Named label position

Locations support offsets: `EOF-10b`, `BOF+5l`, `MARKER+100b`

### Labels

Labels must be:
- UPPERCASE alphanumeric with `_` or `-`
- Start with A-Z
- Maximum 15 characters
- Maximum 128 labels per program

### Regular Expressions

fiskta includes a custom Thompson NFA regex engine:

**Character classes**: `\d`, `\D`, `\w`, `\W`, `\s`, `\S`, `[a-z]`, `[^0-9]`

**Quantifiers**:
- `*` (0+), `+` (1+), `?` (0-1)
- `{n}` (exactly n), `{n,m}` (n to m), `{n,}` (n or more)
- Lazy variants: `*?`, `+?`, `??`, `{n,m}?`, `{n,}?`

**Grouping**: `(...)` for subpatterns, `(a|b)+` for quantified groups

**Anchors**: `^` (line start), `$` (line end)

**Alternation**: `|` (OR)

**Escape sequences**: `\n`, `\t`, `\r`, `\f`, `\v`, `\0`

**Special**: `.` (any character except newline)

## Examples

### Extract specific lines

```bash
# Lines 10-20
fiskta -i file.txt skip 10l take 10l

# Last 5 lines
fiskta -i file.txt skip to EOF-5l take 5l
```

### Parse structured data

```bash
# Extract JSON value
echo '{"name":"Alice","age":30}' | \
  fiskta find '"age":' skip 1b take until ',' at line-end

# Extract between markers
fiskta -i doc.txt find "BEGIN" skip 1l take until "END"
```

### Work with binary files

```bash
# Check magic number
fiskta -i file.zip find:bin "50 4B 03 04" print "Valid ZIP" \
  OR fail "Invalid ZIP"

# Extract PNG chunks
fiskta -i image.png find:bin "89504E47" skip 8b \
  label START take 4b print "Chunk: " take 4c
```

### Complex extraction

```bash
# Extract email domains
fiskta -i emails.txt \
  find:re "[A-Za-z0-9._%+-]+@" take to match-end \
  take until:re "[^A-Za-z0-9.-]" OR take to EOF

# Extract function definitions (C-style)
fiskta -i source.c \
  find:re "^[a-zA-Z_][a-zA-Z0-9_]*\\s+[a-zA-Z_][a-zA-Z0-9_]*\\s*\\(" \
  take to line-end
```

### Monitoring and streaming

```bash
# Follow log file for errors (like tail -f | grep)
fiskta -i /var/log/app.log --continue 1s --until-idle 5m \
  find "ERROR" take to line-end

# Monitor configuration changes
fiskta -i config.conf --continue 2s \
  find "[updated]" skip 1l take 5l
```

### Clause atomicity

```bash
# All operations in a clause must succeed
fiskta -i data.txt \
  find "START" skip 1b take 10b find "END" \
  OR \
  fail "Invalid format"

# Chain with THEN
fiskta -i log.txt \
  find "Session:" take 8c label SID \
  THEN \
  find "User:" skip 6b take until " "
```

## Architecture

### Components

- **Parser** (`parse.c`): Two-phase parser (preflight + build) with memory planning
- **Engine** (`engine.c`): VM executor with atomic clause semantics and staging buffers
- **File I/O** (`fileio.c`): Buffered streaming with LRU-cached line indexing
- **Literal Search** (`search_literal.c`): Boyer-Moore-Horspool for exact matches
- **Regex Engine** (`regex_vm.c`, `regex_prog.c`): Thompson NFA with streaming execution
- **Runtime** (`fiskta.c`): Two-phase execution API (build + execute)

### Memory Model

fiskta uses a **zero-malloc execution model**:

1. **Preflight**: `program_requirements()` analyzes input and computes memory needs
2. **Allocation**: Caller allocates single arena block
3. **Build**: `build_program()` compiles program into arena (no mallocs)
4. **Execute**: `runtime_execute()` uses pre-allocated buffers (no mallocs)

All memory requirements are known before execution begins. Typical usage:
- Small program: ~500 KiB
- Complex regex: ~2 MiB (configurable)
- File buffer: 6 MiB (forward), 3 MiB (backward)

### Performance

- **Streaming**: Processes files in chunks, not limited by file size
- **Line indexing**: LRU-cached 512 KiB blocks with 2 KiB subchunks
- **Regex**: Thompson NFA with fixed thread budget (10K threads default)
- **Search**: Boyer-Moore-Horspool for literals, streaming NFA for regex
- **Zero-copy**: Direct file-to-output where possible

## Library Usage

fiskta can be embedded as a C library:

```c
#include "fiskta.h"

// Define operations
String tokens[] = {
    {.bytes = "find", .len = 4},
    {.bytes = "hello", .len = 5},
    {.bytes = "take", .len = 4},
    {.bytes = "5b", .len = 2}
};

// Query requirements
RuntimeRequirements req;
program_requirements(4, tokens, &req);

// Allocate arena
void* arena = malloc(req.arena_bytes);

// Build program
Program prog;
RuntimeBuffers buffers;
build_program(4, tokens, &prog, arena, req.arena_bytes, &buffers);

// Execute
RuntimeConfig config = {
    .loop_enabled = false,
    .loop_ms = 0,
    .idle_timeout_ms = -1,
    .exec_timeout_ms = -1,
    .ignore_loop_failures = false,
    .error_callback = NULL,
    .output_callback = NULL
};
int result = runtime_execute(&prog, "input.txt", &buffers, &config);

// Cleanup
free(arena);
```

### In-Memory Execution

Process buffers without file I/O:

```c
const unsigned char data[] = "Hello, world!";
runtime_execute_buffer(&prog, data, sizeof(data)-1, &buffers, &config);
```

### Custom Callbacks

```c
void error_handler(enum Err err, const char* ctx, i32 pos,
                   const char* msg, void* userdata) {
    fprintf(stderr, "Error at %d: %s\n", pos, msg);
}

void output_handler(const void* data, size_t len, void* userdata) {
    fwrite(data, 1, len, stdout);
}

config.error_callback = error_handler;
config.output_callback = output_handler;
```

### Build Options

Control regex engine resource limits via `BuildOptions`:

```c
BuildOptions opts = {
    .regex_budget_bytes = 4 * 1024 * 1024,  // 4 MiB total (0 = 2 MiB default)
    .regex_work_budget = 100000000          // 100M enqueues (0 = 50M default)
};

RuntimeRequirements req;
fiskta_program_requirements(4, tokens, &opts, &req);

Program prog;
RuntimeBuffers buffers;
fiskta_build_program(4, tokens, &opts, &prog, arena, req.arena_bytes, &buffers);
```

**Resource limits:**

- `regex_budget_bytes` (default: 2 MiB) - Total memory budget for regex VM
  - Split between seen tables (2 tables, sized per-pattern) and thread lists
  - Typical usage: ~10K concurrent NFA threads for normal patterns
  - Increase for very large patterns (>16K instructions)

- `regex_work_budget` (default: 50M enqueues) - Max thread enqueues per search
  - Prevents step-count explosion from pathological patterns like `((a?){50}){50}`
  - 50M allows legitimate searches while catching adversarial patterns in ~0.5-1s
  - Increase for legitimate complex patterns over very large files

Pass `NULL` or use zero values to accept defaults.

## Exit Codes

- `0` - Success
- `1` - Program failure (no clause succeeded)
- `2` - Execution timeout (`--for` elapsed)
- `7` - Usage error (invalid arguments)
- `8` - Parse error (invalid syntax)
- `9` - Capacity exceeded (pattern too complex)
- `10` - I/O error
- `11` - Resource exhaustion (OOM)

## Building and Testing

```bash
# Build
zig build                    # Default build
zig build -Doptimize=ReleaseFast  # Optimized
zig build asan              # With AddressSanitizer

# Test
zig build test              # CLI tests
zig build test-lib          # Library wrapper tests

# Release builds (all platforms)
zig build release           # Creates binaries <150 KiB
```

## Design Principles

1. **Predictable memory**: All allocations known upfront, no runtime malloc
2. **Atomic semantics**: Clauses succeed or fail as units with rollback
3. **Streaming**: Process files larger than RAM
4. **Composability**: Chain operations with clear semantics
5. **Embeddability**: Library-first design, CLI is thin wrapper
6. **Performance**: Zero-copy where possible, efficient algorithms

## Limitations

- Maximum 1024 operation tokens
- Maximum 128 labels
- Maximum 16 KiB per pattern (literal or regex)
- Maximum 16K regex instructions per pattern
- Regex thread budget: 10K threads (configurable)
- File buffer: 6 MiB forward, 3 MiB backward (configurable)

## Contributing

Contributions are welcome! Please ensure:
- Code follows existing style (C11, strict warnings)
- All tests pass: `zig build test`
- No regressions: `zig build test-lib`
- Memory safe: test with `zig build asan`

## License

fiskta is licensed under the [zlib License](LICENSE).

Copyright (c) 2025 Simon Forsling

## Acknowledgments

- Regex engine inspired by Russ Cox's [Regular Expression Matching Can Be Simple And Fast](https://swtch.com/~rsc/regexp/)
- Thompson NFA implementation with streaming execution and lazy quantifiers
- Boyer-Moore-Horspool for literal string search

## See Also

- [Advanced Examples](tests/) - Test suite with complex use cases
- [Library API](src/fiskta.h) - Complete API documentation
- [Benchmarks](bench/) - Performance comparisons

# AGENTS.md

This file provides guidance to AI coding assistants when working with code in this repository.

## Project Overview

**fiskta** is a cursor-oriented data extraction tool written in C11. It operates on streams/files by maintaining a cursor position and executing imperative operations (find, skip, take) rather than pattern-matching on lines. The tool is designed to be lightweight (< 100KB binary), zero-dependency, and allocation-averse with memory usage independent of input size.

## Project Structure

- **`src/`**: Core C sources
  - `main.c` - CLI entry point, argument parsing, program lifecycle management, loop/streaming modes
  - `parse.c` - Converts command-line operations into internal `Program` representation (clauses, ops, names)
  - `engine.c` - VM execution engine with staged commit/rollback semantics for atomic clause execution
  - `iosearch.c` - Streaming search implementation for literal and regex pattern matching with windowing
  - `reprog.c` - Regex compiler and matcher (custom implementation, no external dependencies)
  - `fiskta.h` - Core types (`Program`, `Clause`, `Op`, `VM`, `LocExpr`, `Range`, etc.)
  - `parse_plan.h` - Pre-allocation planning for parse phase
  - `util.h` - Shared utility macros and helpers
- **`build/`**: Generated object files and intermediates created by Makefile; always rebuildable, not version-controlled
- **`tests/`**: Python acceptance suite; `run_tests.py` with fixtures under `tests/fixtures/`
- **`bench/`**: Canned data for manual performance checks; keep large artifacts out of version control
- **`VERSION`**: Single source of truth for version number (read by Makefile, build.zig, and tests)

## Build Commands

### Using Make

- `make` - Build optimized binary (`./fiskta`)
- `make debug` - Build with `-O0 -g -DDEBUG` for debugging
- `make test` - Run full test suite (requires Python 3, see note on filtering below)
- `make clean` - Remove build artifacts
- `./fiskta -h` - Quick regression check for CLI argument parsing

### Using Zig (for cross-compilation)

- `zig build` - Build for host platform (`zig-out/bin/fiskta`)
- `zig build test` - Build and run test suite (see note on filtering below)
- `zig build all` - Build for all platforms (Linux, macOS, Windows)
- `zig build -Doptimize=ReleaseSmall` - Optimize for size

## Architecture

### Core Execution Flow

```
CLI args → split_ops_string → parse (parse.c) → Program (clauses + ops)
                                                      ↓
                                               io_open (engine.c)
                                                      ↓
Program + VM → execute_clause_stage_only → StagedResult (ranges, labels, vm state)
                                                      ↓
                                            commit or rollback based on Err
                                                      ↓
                                            emit ranges to stdout
```

### Key Design Principles

1. **Allocation-averse**: All allocations happen at startup. Never add mid-run `malloc`/`free` paths or hidden container growth.

2. **Atomic clause execution**: Each clause stages its operations (cursor movement, output ranges, label writes). On any failure, the entire clause rolls back. On success, all staged changes commit atomically.

3. **Staged execution**: Operations accumulate `Range` structs (file regions or literal strings) which are emitted only on clause success. This enables atomic rollback.

4. **VM state**: The `VM` struct maintains cursor position, last match location, view boundaries, and label positions. Clauses operate on staged copies.

5. **View restrictions**: `view` operations limit all subsequent operations to a file region, providing scoped extraction boundaries.

6. **Streaming search**: `iosearch.c` handles pattern matching with windowed buffering (8MB forward windows, 4MB backward blocks) and overlap regions to handle patterns spanning buffer boundaries.

### Clause Linking

Operations are grouped into clauses connected by:
- `THEN` - Always run next clause (sequential)
- `AND` - Run next only if current succeeds (short-circuit on failure)
- `OR` - Run next only if current fails (first success wins)

## Coding Style

- **C11** with 4-space indentation, brace-on-same-line (see `src/engine.c`)
- `snake_case` for functions/locals, `UPPERCASE` for macros, prefixed structs/enums
- Project headers before system headers; avoid mixing declarations and logic in headers
- Never add mid-run allocations; only guarded startup allocations
- Prefer lightweight helper functions over macros when useful
- Document tricky branches with brief comments
- Check `.clang-format` for formatting rules

## Testing

- Extend `tests/run_tests.py` using existing fixture helpers to keep inputs reproducible
- New scenarios use explicit command arrays and expected byte ranges
- Run `make test` locally before committing changes
- **IMPORTANT**: Always filter test output to avoid excessive tokens. Use `make test 2>&1 | grep -v '\[PASS\]'` or `make test 2>&1 | grep -E "^(Summary:|FAIL)"` to show only failures and summary

## Performance Considerations

- Memory usage should strive to be in the low megabytes range regardless of input size
- Regex operations use more memory than literal search
- Benchmark scripts available in `bench/` for manual performance checks

## Commit & Pull Request Guidelines

- Check formaatting: Run clang-format -i on source files before committing
- **Commit subjects**: Use present-tense imperative (`Align capture bounds`, `Fix regex anchor handling`)
- **Commit bodies**: Wrap details in the body when nuance is needed

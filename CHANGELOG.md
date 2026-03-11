# Changelog

## 2.1 (2026-03-12

### Compatibility notes

**Library API:**
- Removed dead `FisktaRuntimeConfig.error_callback` / `.error_userdata` fields. Error routing now goes through `fiskta_set_error_handler()`; `FisktaRuntimeConfig` only controls runtime behavior and output callbacks.
- Removed misleading `FisktaRuntimeRequirements.any_lazy_quantifiers` / `.any_counters` fields.

### Bug fixes

- Fixed the thread-local library error callback path so handlers registered with `fiskta_set_error_handler()` are actually invoked.
- Fixed stale thread-local error state: successful public API calls now clear prior `fiskta_error_*()` values.

## 2.0 (2026-02-11)

### Breaking changes

**CLI:**
- Removed `--monitor`, `--follow`, and `--every` flags; consolidated on `--continue` as the single looping mechanism.
- Removed deprecated CLI aliases (`-m`, `-f`, `-e`, `--ignore-failures`).
- `--ops` is now always an inline string. Use `--ops-file <path>` to read operations from a file.
- Added `-C`/`--continue-on-fail` (replaces `--ignore-failures`). `-C` implies `-c`.
- Exit codes revised: 0 success, 1 program failure, 2 timeout, 7 usage error, 8 parse error, 9 capacity exceeded, 10 I/O error, 11 resource exhaustion.

**Library API:**
- All public types prefixed with `Fiskta`/`FISKTA_` (e.g., `Program` -> `FisktaProgram`, `E_OK` -> `FISKTA_E_OK`).
- All public functions prefixed with `fiskta_` (e.g., `build_program` -> `fiskta_build_program`).
- Library functions return `int` `FISKTA_EXIT_*` codes; detailed internal errors available via `fiskta_error_code()`.
- `FisktaString.len` changed from `i32` to `size_t`.
- `error_set()` moved to internal header; no longer part of the public API.
- `ReThread` and `LabelWrite` typedefs removed from public header.
- Added `FisktaBuildOptions` struct for configurable regex budget and work limits.
- Execution split into three-phase API: `fiskta_program_requirements` (measure), `fiskta_build_program` (compile), `fiskta_runtime_execute` (run).

### New features

- **Regex engine rewrite**: counter-based quantifier implementation with lazy (non-greedy) matching (`*?`, `+?`, `??`, `{n,m}?`). Work budget (50M steps) prevents runaway patterns.
- **Label management**: labels cannot be silently overwritten. Use `clear <NAME>` to unset a label before reassigning it.
- **`print` operation**: emit literal bytes with escape sequence support (`\n`, `\t`, `\xHH`, `\c` for cursor offset). Alias: `echo`. Participates in clause atomicity.
- **`fail` operation**: write a message to stderr and fail the current clause.
- **`--ops-file <path>`**: read operations from a file instead of inline arguments.
- **`--continue` / `-C` delay**: optional delay between loop iterations (e.g., `-c 100ms`, `-C 1s`).
- **In-memory buffer execution**: library API supports operating on byte buffers without file I/O.
- **Error/output callbacks**: library users can intercept error messages and output instead of relying on stderr/stdout.
- **Cross-compilation**: `zig build release` cross-compiles for Linux x86_64 (glibc and musl), macOS aarch64, and Windows x86_64.

### Bug fixes

- Fixed regex quantifier parsing overflow (e.g., `{2147483648}` no longer silently wraps).
- Fixed parser crashes on malformed input (bare quantifiers at regex start, inverted quantifier bounds, missing closing braces).
- Fixed `\x01` sentinel collision in print strings; cursor offsets now use a separate array.
- Fixed INT64_MIN edge case in signed integer parsing.
- Fixed buffer overflow in positional argument token parsing.
- Fixed unterminated quotes and token overflow in ops string tokenizer.
- Fixed ops string length limit mismatch (was 4096, now matches 16384 MAX_NEEDLE_BYTES).
- Fixed label validation and cleared-label behavior.
- Fixed incorrect exit code mappings for capacity and I/O errors.
- Fixed `i64`-to-`i32` truncation in line/char step functions.
- Fixed `i16` cursor offset overflow in print operation.
- Fixed overlap handling in backward literal search.
- Fixed Windows stdin CRLF translation (now set to binary mode).
- Replaced hard exit in alignment helper with proper error propagation.

## 1.3 (2025-10-22)

### Features

- Split exit code 11 into two: exit 11 for OOM (malloc failure), exit 14 for capacity exceeded (buffer limits, pattern too long).
- Added 16KB maximum search pattern limit across all search operations. Exceeding returns exit 14.
- Improved error messages: `skip to <location>` now reports a clear error when the target is out of bounds.
- Automatic version strings from `git describe`.

### Bug fixes

- Fixed regex quantifier parsing to require closing brace (e.g., `{99` no longer accepted).
- Fixed regex quantifier bounds validation (e.g., `{50,2}` now rejected at parse time).
- Rejected regex patterns with empty alternatives in quantified groups (e.g., `(|a)*`, `(a|)*`).
- Fixed exit code reporting for resource errors during execution; previously returned exit 1 instead of exit 11/14.

## 1.2 (2025-10-20)

### Features

- Newlines treated as whitespace in operations files for easier multi-line formatting.
- Added fuzzer (`fuzz.py`) for crash and hang testing.

### Bug fixes

- Fixed inconsistent operation counting when clause keywords (`THEN`/`OR`) appear as operation arguments, causing memory corruption.
- Fixed stack overflow with excessive regex alternations; now capped at 256.
- Fixed OOM with quantifiers on character classes (e.g., `[a-z]{50,100}`).
- Fixed OOM when expanding regex quantifiers; expansion now capped at 100 repetitions.
- Fixed crash with out-of-range label index references.

## 1.1 (2025-10-17)

### Features

- Added `\c` escape in `print` to output the current cursor position.
- Improved parse error messages: syntax errors now point to the offending token.
- Added `-u` short option for `--until-idle`.

### Bug fixes

- Fixed exit codes to return documented values for parse, regex, I/O, and program failures.
- Fixed UTF-8 cursor alignment: char-based take/skip no longer duplicate or skip bytes at multi-byte boundaries.
- Fixed double-counting of staged print/fail operations causing range overallocation.
- Fixed inline offset parsing for location expressions (e.g., `BOF+2l`).
- Prevented programs from ending with dangling `OR`/`THEN` clauses.
- Fixed `--until-idle` behavior: now correctly tracks idle state per mode instead of exiting after one iteration.
- Fixed `--follow` mode to process existing data on first iteration instead of starting at EOF.
- Removed hidden mallocs in regex compiler; all allocations now go through the arena.

## 1.0 (2025-10-14)

Initial release. Cursor-oriented data extraction tool with:

- 15 operations: `find`, `find:re`, `find:bin`, `skip`, `take`, `take to`, `take until`, `take until:re`, `take until:bin`, `label`, `clear`, `view`, `clear view`, `print`, `fail`.
- Units: bytes (`b`), lines (`l`), UTF-8 code points (`c`).
- Location expressions with offsets (`BOF`, `EOF`, `cursor`, `match-start`, `match-end`, `line-start`, `line-end`, labels).
- Clause operators: `THEN` (sequential), `OR` (first success wins).
- Atomic clause semantics with staged commit/rollback.
- Looping modes: `--continue`, `--follow`, `--monitor`, `--until-idle`.
- Regex engine with quantifiers, character classes, groups, alternation, anchors.
- LRU-cached line indexing for fast line navigation.
- Single arena allocation at startup; zero mid-run allocations.
- Cross-compilation via zig build.

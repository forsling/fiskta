# Repository Guidelines

## Project Structure & Module Organization
- `src/`: core C sources; `main.c` wires CLI entry, `parse*.{c,h}` handle program parsing, `engine.c` drives execution, `iosearch.c` hosts streaming search.
- `build/`: generated object files and intermediates created by the Makefile; always rebuildable.
- `tests/`: Python acceptance suite; `run_tests.py` seeds fixtures under `tests/fixtures/` before asserting behaviors.
- `bench/`: canned data for manual performance checks; keep large artifacts out of version control.

## Build, Test, and Development Commands
- `make`: produce optimized `./fiskta` with the default warning set; cleans stale objects on demand.
- `make debug`: rebuild with `-O0 -g -DDEBUG` for reversible debugging sessions.
- `make test`: run the end-to-end suite (`python3 tests/run_tests.py`), regenerating fixtures and verifying CLI outputs.
- `./fiskta -h`: quick regression check for argument parsing whenever operations or help text change.

## Coding Style, Performance & Naming Conventions
- Stick to C11, 4-space indentation, and brace-on-same-line style as seen in `src/engine.c`.
- We are extremely allocation-averse: only perform the guarded startup allocations already present; never add mid-run `malloc`/`free` paths or hidden container growth.
- Use `snake_case` for functions/locals, uppercase identifiers for macros, and keep public structs/enums prefixed with concise nouns.
- Group project headers before system headers; avoid mixing declarations and logic in headers.
- Prefer lightweight helper functions over macros when useful; document tricky branches with brief comments.

## Testing Guidelines
- Acceptance coverage lives in `tests/run_tests.py`; extend using the existing fixture helpers to keep inputs reproducible.
- Describe new scenarios with explicit command arrays and expected byte ranges; mirror current naming style.
- Run `make test` locally before pushing; capture any noteworthy stderr/stdout deltas in the PR discussion.

## Commit & Pull Request Guidelines
- Keep commit subjects in present-tense imperative (`Align capture bounds`); wrap details in the body when nuance is needed.
- Limit commits to a single intent—parser changes, test fixtures, and doc updates should land separately.
- PRs should flag ABI or CLI changes, link related issues, and attach demo commands or benchmarks when they influenced the fix.

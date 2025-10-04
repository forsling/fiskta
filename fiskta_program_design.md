# fiskta Program Design (Iterative/Streaming Execution)

This document proposes an extension to fiskta that enables running fiskta programs iteratively, potentially forever, while preserving the project’s core principles: simplicity, streaming, clause atomicity, and allocation aversion.

The key idea is to drive execution in ticks. On each tick, the runtime establishes a temporary view window, runs the user‑provided program (one or more clauses), commits if successful, then advances the window and repeats. Input does not have to grow; this works for fixed inputs as well.

## Goals
- Keep fiskta programs intuitive and stable: the same ops and clauses work unchanged.
- Add an iterative driver: the runtime loops, applying the program over a temporary view.
- Remain allocation‑averse: one bounded startup allocation reused across ticks.
- Support both models:
  - Iterative execution against a file (growing or fixed) or spooled stdin.
  - Command‑stream execution where ops arrive incrementally while the input is static.

## Concepts

- Program: the existing list of clauses and ops.
- Tick: one iteration where the runtime constrains the working set via `viewset`, executes the program, and updates driver state.
- Window: the half‑open range the driver chooses for a tick. The runtime implicitly sets a temporary `viewset window_lo window_hi` for the whole program execution on that tick. This driver view intersects with any user `viewset` expressed in the program.
- Checkpoint: driver‑maintained high‑water marks (e.g., `processed_hi`) that advance over time.

## Execution Modes

1) Iterative Window Mode (default extension)
- Driver maintains a checkpoint `processed_hi` per input.
- On each tick:
  - Refresh file size (or spooled stdin size) as `sz`.
  - Compute `window = [processed_hi, sz)`; if empty, optionally sleep/backoff.
  - Apply a temporary `viewset` to `window ∩ user_view` and execute the program once.
  - On clause success, commit VM and advance `processed_hi := sz` (or a configurable policy, see Window Policies).
- Works for both growing inputs and fixed inputs (ticks after first will be no‑ops if unchanged).

2) Command‑Stream Mode
- Input is static (file/spooled stdin snapshot).
- Read newline‑delimited ops strings from stdin or a control FD. Each line is a standalone fiskta program.
- For each incoming program:
  - Preflight → build → compile into bounded pools → execute once.
  - VM persistence optional (see State Persistence).
- Errors for a given line are reported; subsequent lines continue.

## Window Policies

Different workloads may benefit from different policies. Initial support can focus on a simple default with room to extend.

- Delta (Append‑Only) [default]: `window = [processed_hi, sz)`. Emits only new data; never re‑emits old bytes.
- Rescan‑All (optional): `window = [0, sz)`. Re‑run full program every tick (useful for monitoring stateful patterns).
- Sliding (optional future): `window = [sz - W, sz)` with W fixed or derived; bounded re‑scan for tail‑window analyses.

The driver view always intersects with any `viewset` expressed by the program so user constraints still apply.

## State Persistence

- VM (cursor, labels, last_match snapshot, and active view) persists across ticks within Iterative Window Mode.
- In Command‑Stream Mode:
  - Default: reset VM between ops lines (isolation).
  - Option: `--persist-state` to keep VM across lines if desired.

Clause atomicity is unchanged: within a tick, a clause either commits its staged outputs/labels and updates VM, or it is rolled back. A tick itself does not force commit; it simply drives program execution once per window.

## Minimal New Operation

- `sleep <n>ms`
  - Always succeeds, emits nothing, does not change cursor/match.
  - Unit is milliseconds; explicit `ms` suffix avoids collision with existing units.
  - Primary use: backoff in command‑stream programs or controlled pacing within a clause.

(Optionally later: `nop` for testing; `flush` to emit staged literal ranges early—default remains staged and emitted at clause commit.)

## CLI Additions (Proposed)

- Iteration driver flags:
  - `--iterate` Enable the iterative driver (runs ticks until exit condition).
  - `--tick-ms=N` Sleep N ms between ticks (default e.g. 200).
  - `--idle-exit-ms=N` Exit if no new work has occurred for N ms (useful for CI/tests).
  - `--window-policy=delta|rescan|sliding[:W]` Select window policy; default `delta`.

- Command‑stream flags:
  - `--ops-stdin` Read newline‑delimited ops strings from stdin; each line is a full program.
  - `--ops-fd=N` Same as above but from an explicit file descriptor.
  - `--persist-state` Keep VM across ops lines (default off).

- Caps and sizing (override defaults):
  - `--max-clauses`, `--max-ops`, `--str-pool-bytes`, `--max-re-ins`, `--max-re-classes`.

All flags are optional; running without them preserves existing one‑shot behavior.

## Memory Model & Sizing

- No mid‑run allocations; single startup allocation sized from caps.
- Pools reused across ticks and across command lines:
  - Ops/Clauses arrays sized by `max-*` caps.
  - String pool for literals/patterns.
  - Regex program pools (instructions/classes) compiled per program, capped.
  - Regex thread lists and seen bitsets sized by `max-re-ins` (threads ~4× instructions, min 32) as today.
- If an incoming program exceeds caps, fail fast with E_OOM/E_PARSE; driver reports and continues (command‑stream) or idles (iteration).

## I/O & Input Handling

- File inputs: each tick re‑stats file size; `io_emit` and searches remain bounded by the driver’s temporary view.
- Stdin: continue spooling into a temporary file; its size grows as more stdin is read. Iteration works identically on the spool.
- No extra buffers are introduced; continue using the single reusable search buffer.

## Semantics & Compatibility

- Program semantics are unchanged. The driver only adds an implicit temporary `viewset` per tick.
- User `viewset` ops remain valid and combine via intersection with the driver view.
- Backward operations (e.g., `find to BOF`, negative `take`) work because the runtime has random access to the spooled or file input.
- Exit status in iterative modes:
  - Iterative Window Mode: process ticks until exit condition (signal, idle timeout). Non‑fatal clause failures do not exit.
  - Command‑Stream Mode: exit 0 unless a fatal error occurs in the driver; per‑line errors are reported on stderr.

## Error Handling

- Parse/compile errors: reported with message and offending program/line; iteration continues where applicable.
- I/O errors: abort the run unless the error is explicitly transient (policy TBD).
- Resource limit errors (caps): clear, actionable messages suggesting `--max-*` adjustments.

## Testing Plan (High‑Level)

- Iteration (delta policy): append to a temp file while fiskta runs with `--iterate`; assert only new bytes are emitted.
- Fixed input: run with `--iterate --window-policy=delta`; first tick emits expected bytes, later ticks produce no output.
- Command‑stream: feed multiple programs via `--ops-stdin`; verify outputs and that VM persistence toggles via `--persist-state`.
- `sleep`: within a clause, ensure it does not change output or state besides time.

## Open Questions

- Naming: prefer `--iterate` over `--follow` to avoid implying only growing inputs. Acceptable?
- Defaults: should `--tick-ms` be 0 (busy) or a sensible backoff (e.g., 200ms)?
- Should we include a built‑in idle backoff even without `sleep` ops?
- Command‑stream framing: accept only one ops string per line, or support length‑prefixed/stanza formats later?
- Sliding policy: worth prioritizing now, or add later after delta/rescan prove out?

## Minimal Implementation Slice

1) Parse `sleep` op (`sleep <n>ms`) and add `OP_SLEEP` handling in the engine.
2) Add iterative driver in `main.c` gated by `--iterate`, with delta window policy and `--tick-ms`/`--idle-exit-ms`.
3) Add basic command‑stream driver with `--ops-stdin` (VM reset per line by default), plus `--persist-state`.
4) Keep all allocations bounded and reused; expose `--max-*` flags to size pools at startup.
5) Extend tests with a small set of iterative/command‑stream cases.

This plan keeps core fiskta semantics unchanged while enabling powerful long‑running workflows, with clear boundaries on memory and complexity.


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

### Delta vs. Sliding (clarification)

- Delta (append-only):
  - Window per tick: `[processed_hi, sz)` where `processed_hi` advances to `sz` on success.
  - Never re-processes old bytes; ideal for tailing append-only logs and streaming inputs.
  - Outputs are naturally de-duplicated since prior regions are not revisited.

- Sliding:
  - Window per tick: `[max(0, sz - W), sz)` for a fixed width `W`.
  - Re-processes an overlapping suffix every tick; useful when matches depend on cross-boundary context or you need a rolling analysis window.
  - May re-emit the same bytes unless the program is written to be idempotent within the sliding window (e.g., constrained by labels or content markers).

Notes:
- Rescan-All is the extreme case of Sliding with `W = sz`.
- For log rotation/truncation: Sliding and Rescan-All are robust; Delta should be paired with detection of shrink and reset the checkpoint when a truncation is observed.
- For stdin spooling, all policies apply identically since the spool is a growing random-access file.

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

## Incremental Implementation Plan (WIP)

Small, testable steps to de-risk complexity and allow course corrections:

1) Switch clause separator to `THEN`
   - Parser/tokenizer updates (including ops-string splitter)
   - Update help/README/examples; migrate tests

2) Add `sleep <n>ms` op
   - Parse/build support; `OP_SLEEP` in engine as no-op side-effect
   - Unit tests to ensure no state/output changes

3) Add iterative loop (Rescan-All policy)
   - Flags: `--iterate`, `--tick-ms`, `--idle-exit-ms`
   - Re-run program each tick without windowing yet

4) Add delta window via temporary `viewset`
   - Maintain `processed_hi`; intersect driver view with user `view`
   - Tests appending to a file; assert only new data is emitted

5) Add `--ops-stdin` command-stream mode
   - One ops string per line; default VM reset; `--persist-state` option
   - Error reporting per-line, continue processing

6) Refactor clause execution to stage-only
   - Extract a function that stages ranges/labels and returns a staged VM without emitting
   - Make current `execute_clause_with_scratch` a thin stage+commit wrapper

7) Introduce ClauseExpr AST scaffolding
   - Build AST with `LEAF` nodes only (sequencing via `THEN` remains a list of exprs)
   - Preflight hooks ready for boolean nodes

8) Implement `OR` with short-circuit
   - Require parentheses for mixed expressions initially
   - Stage-only executor returns the winning branch; commit on success

9) Implement `AND` with group atomicity
   - Stage left, then right using left’s staged VM; commit combined outputs on success
   - Tests for atomicity, view/label behavior

10) Add expression-level caps and flags
   - `--max-ranges-per-expr`, `--max-labels-per-expr`, `--max-depth`; clear failure messages

11) Add window policy options
   - `--window-policy=delta|rescan|sliding[:W]`; ensure default remains delta for iterative mode

12) Polish docs and examples
   - Update README/help with THEN, sleep, iterate/ops-stdin, window policies, and logical expressions

Each step is independently useful, minimizes rewrites, and provides observable behavior for evaluation.

## Logical Clause Expressions (WIP)

Goal: enable boolean composition of clauses with short‑circuiting while preserving atomicity and readability.

### Syntax (proposed)
- Sequencing operator: `THEN` (replaces the old `::`). Example:
  - `CLAUSE1 THEN CLAUSE2 THEN CLAUSE3`
- Boolean operators (keywords to avoid shell quoting):
  - `AND`, `OR`
  - Parentheses `(` `)` for grouping
- Example: `( CLAUSE1 OR CLAUSE2 ) AND CLAUSE3 THEN CLAUSE4`

Notes:
- Keep precedence simple initially: require parentheses for any mixed `AND`/`OR` expression (no implicit precedence). We can add `AND > OR` later if desired.

### Semantics
- Group‑level atomicity: a grouped expression commits as a unit; no partial side‑effects.
- Short‑circuiting:
  - `A AND B`: evaluate A; on failure → fail; on success → evaluate B; commit both only if both succeed.
  - `A OR B`: evaluate A; on success → commit A; on failure → evaluate B; commit B if it succeeds.
- Staging rules:
  - Each evaluation uses a staged VM (cursor, last_match, view, labels) and staged output ranges.
  - `AND`: the right side starts from the left’s staged VM; on success combine outputs (left then right); commit combined.
  - `OR`: the right side starts from the original VM; if left succeeds, right is not evaluated.
- View operations (viewset/viewclear) modify the staged view; commit on group success only.

### Parsing & IR
- Extend parser to build a ClauseExpr AST:
  - `LEAF` → existing Clause
  - `AND(left,right)`
  - `OR(left,right)`
- Sequencing: top‑level is a list of ClauseExpr nodes separated by `THEN`.

### Preflight & Sizing (allocation‑averse)
- For each Clause (LEAF):
  - `ranges_cap = count(TAKE_LEN, TAKE_TO, TAKE_UNTIL, PRINT)`
  - `labels_cap = count(LABEL)`
  - Regex pools sized from pattern estimates as today.
- For `AND` node: `ranges_cap = left.ranges_cap + right.ranges_cap`; `labels_cap = left.labels_cap + right.labels_cap`; regex ins/classes capacities = sum of children estimates.
- For `OR` node: `ranges_cap = max(left.ranges_cap, right.ranges_cap)`; labels analogously; regex ins/classes capacities = max of children estimates.
- For nested groups, apply rules recursively. Preallocate staging arrays at the root sufficient for the worst case; reuse within evaluation.

### Engine Execution Sketch
- Add a stage‑only executor that evaluates a Clause into staging (no emission) and returns success + staged VM/output.
- Implement `execute_expr_stage` for AND/OR using the rules above.
- Top‑level driver: for each ClauseExpr between `THEN`, call stage, and if success → emit staged ranges, commit labels and staged VM; else → continue to next `THEN` expression (overall success if any expression commits, unchanged).

## Sequencing Operator
- `THEN` is the only sequential separator (the legacy `::` is removed).
- Help/README/examples/tests will use `THEN` exclusively.

## Caps & Memory Implications (WIP)

We will keep fixed, bounded pools driven by startup caps. Two commonly discussed caps are per‑clause op counts and per‑group (logical expression) op counts.

Definitions:
- Let `R = sizeof(Range)`, `L = sizeof(LabelWrite)`, `O = sizeof(Op)`, `C = sizeof(Clause)`.
- Let `take_like(op)` be any of: TAKE_LEN, TAKE_TO, TAKE_UNTIL, PRINT (each can stage a `Range`).

Per‑Clause (LEAF) worst‑case with cap `MAX_OPS_PER_CLAUSE = 255`:
- Ops storage: `255 * O` (fits in the existing ops buffer).
- Staged ranges: `ranges_cap ≤ min(255, count of take_like))` → worst case `255 * R`.
- Staged labels: `labels_cap ≤ min(255, count of LABEL)` → worst case `255 * L`.
- Regex: worst case all ops are `findr` → `sum_findr_ops = 255`, bounded by `--max-re-ins` and `--max-re-classes` pools.

Per‑Group (logical) worst‑case with cap `MAX_OPS_PER_GROUP = 255`:
- For `AND`, capacities add: `ranges_cap_group = Σ child.ranges_cap`; labels analogous; regex ins/classes add. This can exceed 255 easily if both children are large.
- For `OR`, capacities take the max across children.
- Implication: a single numeric cap on “ops per group” can be too limiting for AND trees. Prefer caps that reflect real resources:
  - `--max-ranges-per-expr` (derived by preflight from op kinds)
  - `--max-labels-per-expr`
  - `--max-re-ins`, `--max-re-classes`
  - `--max-depth` (AST nesting depth, e.g., 32)
- Keep `--max-ops-per-clause` (LEAF) for parser sanity, but size staging by the computed `ranges_cap`/`labels_cap` using the sum/max rules above.

Arena sizing formulas (high‑level):
- Ranges buffer per staged evaluation: `ranges_bytes = align(R) * ranges_cap_expr`
- Labels buffer: `labels_bytes = align(L) * labels_cap_expr`
- Ops/clauses arrays: `O * total_ops + C * total_clauses` (bounded by `--max-ops`/`--max-clauses`)
- Regex pools: `sizeof(ReProg) * sum_findr_ops + sizeof(ReInst) * --max-re-ins + sizeof(ReClass) * --max-re-classes`
- Plus existing search buffer, thread buffers, seen bitsets

Recommendation:
- Keep `MAX_OPS_PER_CLAUSE = 255` as a pragmatic parser cap.
- Do not introduce a hard `MAX_OPS_PER_GROUP = 255`; instead, compute group staging caps via sum/max and bound them against `--max-ranges-per-expr` / `--max-labels-per-expr` (configurable). Fail fast if exceeded with a clear message.
- Provide sensible defaults (e.g., `--max-ranges-per-expr=512`, `--max-labels-per-expr=256`) that keep memory bounded while accommodating common AND compositions.

# Task System v2 — Setup

This file is the bootstrap for the agent task-flow system: a setup guide plus the
`TASKS.md` template. **Do not copy this Setup section into `TASKS.md`** — copy
only the template below the `---` divider (from the `# Tasks` heading down).

## Changes from v1 (remove this section after review)

1. **New `[H]` marker** — *implemented, awaiting human sign-off* — with an
   `Evidence:` line and a `signoff` workflow. Human-gated tasks (art review,
   audible checks) previously scattered across Design entries, held-`[x]`, and
   `[*]` now form one batchable queue.
2. **Ledger tool contract** — a small per-project `tools/tasks_cli` script that
   answers "next task / what unblocks / is the ledger well-formed" so loop agents
   stop re-parsing the whole file each pass.
3. **Suite Health section** — one place recording gates that are red for
   task-independent/environment reasons. Tasks and reviews cite it instead of
   embedding their own (soon-stale) suite claims.
4. **History-condensation rule** — on every marker flip, superseded
   `Status:`/`Review:`/`Progress:` prose is compressed to one line each; full
   history lives in git.
5. **Backported v1-project learnings** as generic policy: the bug repro gate, the
   ledger-commit convention, the no-free-standing-notes rule, the
   capability-check (facility-exists) rule, and an explicit
   `Blocked: external —` form.
6. **Project verification policy slot** — a designated section for
   project-specific evidence rules (e.g. rendered-frame proofs) so they have a
   home without forking the template.
7. **BACKLOG hygiene** — promotions leave a one-line record in `ARCHIVE.md`
   instead of a resolved stub in `BACKLOG.md`; every entry carries a
   `Promote when:` trigger; `cleanup` prunes both files.
8. **No provenance annotations** — an explicit rule against the emergent habit
   of stamping tasks with the process/date that created them
   ("(designrev 2026-07-02)", "promoted from BACKLOG 2026-07-02"). Git records
   provenance; conventions in the ledger self-propagate, so the ban is written
   down. Dates appear only where a rule consumes them (Suite Health staleness,
   Backlog re-park records).

## Setup steps

To set up the system in a project:

1. **Create `TASKS.md`** — copy everything below the divider. Fill in (or
   delete) the *Project verification policy* placeholder.
2. **Create `ARCHIVE.md`** from the starter below. It starts empty; the review
   gate (`reviewtasks` / `superdevloop`) and `signoff` append to it and `cleanup`
   prunes it.
3. **Create `BACKLOG.md`** from the starter below. It starts empty; `bughunt` /
   `rethink` / `bigthink` append ideas to it and `backlogsweep` triages them.
4. **Create the ledger CLI** (recommended) — `tools/tasks_cli` implementing the
   contract in the *Ledger tool* section of the template. Loop skills use it when
   present and fall back to manual scanning when not.
5. **Skills are global** — the workflow skills live at `~/.claude/skills/`
   (Claude) and `~/.codex/skills/` (Codex), available in every project. Nothing
   to copy per project. See the Workflows table in the template for the index.

`ARCHIVE.md` starter:

```
# Archive

Tasks that passed review and are **done done**, plus one-line records of Backlog
items that were promoted or dropped. A task lands here only via the review gate —
`reviewtasks` (interactive) or the reviewer step in `superdevloop` (autonomous) —
or via `signoff` for `[H]` tasks, once checked against its Acceptance criteria.
Implementing agents never read this file.

Entries are ONE line each: title + outcome + commit; full detail lives in git
history — no review essays.
Older entries are periodically cleared via the `cleanup` process.

_None currently._
```

`BACKLOG.md` starter:

```
# Backlog

Long-term ideas, **not yet actionable**. Each entry states the idea and a
**`Promote when:`** trigger — the concrete condition under which it graduates to
a [Design](TASKS.md#design) question (needs a human decision) or a ready
[Auto](TASKS.md#auto) task (well-defined) via the `backlogsweep` process, or is
dropped.

This is the *not-yet* end of the task lifecycle: `TASKS.md` holds active work,
`ARCHIVE.md` holds the *done* end. Implementing agents never read this file.

Hygiene: when an entry is promoted or dropped, it **moves out wholesale** — a
one-line record (name + outcome) goes to `ARCHIVE.md`; do not keep
resolved stubs here. If an idea was considered and re-parked, update the entry
in place with the re-park reasoning and date (that history is load-bearing: it
stops the next sweep from re-litigating). `cleanup` prunes this file too.

Commit convention: see *Committing the ledger files* in `TASKS.md` — this file
may be committed wholesale alongside another agent's in-flight edits.

_None currently._
```

---

# Tasks

**Purpose:** This document tracks *active* work — items needing user input
([Design](#design)) and ready-to-implement work ([Auto](#auto)). Long-term,
not-yet-actionable ideas live in a sibling `BACKLOG.md`; completed-and-reviewed
work is moved to a sibling `ARCHIVE.md`. Implementing agents read only this file.

---

## Task Lifecycle

Auto tasks move through these states, tracked by the checkbox marker:

| Marker | Meaning | Next step |
|--------|---------|-----------|
| `[ ]`  | Not implemented | An implementer (`devloop` / `superdevloop` / `guidedtask`) picks it up |
| `[x]`  | Implemented, **pending review** | The review gate checks it against Acceptance |
| `[H]`  | Implemented, review-verified, **awaiting human sign-off** — an `Evidence:` line points at the artifacts | The `signoff` process walks it with the user; approve → archive, reject → `[*]` |
| `[*]`  | **Failed review** — reasons listed in a `Review:` block directly underneath | An implementer reworks it, addresses the reasons, and flips it back to `[x]` |
| `[-]`  | **Blocked** — a `Blocked:` line lists the task ID(s) or external condition it waits on | Implementers skip it; it auto-unblocks to `[ ]` once every listed task is `[x]`/`[H]`/archived |

The **review gate** — `reviewtasks` (interactive) or `superdevloop`'s reviewer
(autonomous) — is the primary path out of this file: on pass it moves the task
to `ARCHIVE.md`; on fail it marks the task `[*]` with reasons. When every
Acceptance bullet is verified **except** one requiring human judgment (visual
sign-off, audible check, feel), the reviewer sets `[H]` with an `Evidence:` line
instead of holding `[x]`, failing it, or inventing a Design question — `[H]` is
the queue for exactly that situation. A ticked (`[x]`) task still here has
**not** been reviewed yet.

**Blocked vs Design vs `[H]`.** A task that needs *other work done first* is
`[-]` blocked — not a Design item. A finished task that needs a human to *look
at the result* is `[H]` — also not a Design item. Reserve [Design](#design) for
genuine open questions that need a human *decision about what to build*. An
implementer that hits an unforeseen block marks the task `[-]` and moves on
rather than dumping it in Design. Each pass, an implementer re-checks `[-]`
tasks and unblocks any whose blockers are now done. If only `[-]`/`[H]` tasks
remain, that is reported, not forced.

**History condensation.** On every marker flip, compress superseded `Status:` /
`Review:` / `Progress:` prose under the task to **at most one line each** (what
happened). A task block carries only its current spec plus the *latest* review
reasons in long form; full history lives in git. The implementer of a `[*]`
task needs the current rejection, not the archaeology.

---

## Ledger tool

If the project has `tools/tasks_cli` (any language; the contract is the
interface), agents use it instead of re-deriving ledger state by reading prose:

| Command | Output |
|---------|--------|
| `next` | The first actionable Auto task (`[ ]`/`[*]` in file order, `[*]` preferred), as `id<TAB>title`; exit 1 if none |
| `unblock` | `[-]` tasks whose `Blocked:` IDs are all now `[x]`/`[H]`/archived/absent — one per line; `--apply` flips them to `[ ]` |
| `check` | Validates the ledger: unique IDs, every `Blocked:` reference resolves to a task ID or is an explicit `external —` condition, markers well-formed, `[*]` has a `Review:` block, `[H]` has an `Evidence:` line, `[-]` has a `Blocked:` line; exit nonzero on violations |
| `status` | Counts per state, plus the pending-review (`[x]`) and pending-signoff (`[H]`) queues |

Loop skills run `check` + `unblock --apply` at the start of each pass and `next`
to pick work. Without the tool, fall back to a manual scan with the same rules.

---

## Suite Health

Gates that are currently red for **task-independent** reasons (environment,
unrelated regression) are recorded here — one entry per gate: gate name,
first-seen date, exact error evidence, and what would clear it. Rules:

- Tasks and reviews **cite entries here** instead of embedding their own suite
  claims — per-task copies go stale silently; this table has one owner.
- The review gate may pass a task with its **focused gates green** plus
  unrelated reds cited from this table. It must NOT pass a task whose *own*
  verification gate is the red one.
- An entry is **re-verified before being cited** if older than ~2 weeks —
  environments change; stale entries block work that could proceed.
- When an entry clears, delete it and run the unblock sweep: any `[-]` task
  whose `Blocked:` line cites it becomes actionable.

_None currently._

---

## Workflows

These workflows are **skills** — invoke them through your agent (e.g. `/devloop`).
Each skill defines its own behavior; the table below is just an index. Reading
this file is not an instruction to start any of them, and a long-running process
must never be auto-started or restarted without an explicit user instruction.

| Skill | What it does |
|-------|--------------|
| `devloop` | Autonomously implement Auto tasks (`[ ]` / `[*]`) until none remain. |
| `superdevloop` | Supervised devloop: a fresh worker per task, then an independent reviewer; archives on pass, marks `[H]` when only human sign-off remains, `[*]` on fail. |
| `guidedtask <task>` | Interactively implement one named task with approval gates (plan → approve → implement → sign-off → commit). |
| `reviewtasks` | The review gate: verify `[x]` tasks against Acceptance; archive passers, set `[H]` when only human sign-off remains, send failures back as `[*]`. |
| `signoff` | Walk the `[H]` queue with the user: present each task's `Evidence:` artifacts, approve → archive, reject → `[*]` with reasons. |
| `designrev` | Walk Design tasks with the user, resolve open questions, promote to Auto. |
| `backlogsweep` | Triage `BACKLOG.md` items — promote to Design/Auto in `TASKS.md` (recording a one-liner in `ARCHIVE.md`) or drop. |
| `bughunt` / `rethink` / `bigthink` | Find defects / structural flaws / high-value features; add to `BACKLOG.md` after confirmation. |
| `cleanup` | Prune old entries from `ARCHIVE.md` and resolved/stale entries from `BACKLOG.md`. |

---

## How to Add Tasks

Add bugs, refactors, and well-defined work directly to [Auto](#auto).
Add features and architecture decisions that need discussion to [Design](#design).

Inside [Auto](#auto) and [Design](#design), keep only `###` grouping headings and
task blocks. Do not add free-standing paragraphs, notes, or decision records
there: put durable rationale in a task `Context:`, in `BACKLOG.md`/`ARCHIVE.md`,
or in a dedicated `docs/` file.

**No provenance annotations.** Do not stamp tasks with the process or date that
created them — no "(designrev …)", "promoted from BACKLOG …", "rethink …
sweep", or dated `Status:` trails. Git history records who/what/when; a
`Context:` line states facts about the code and the goal, not about the
workflow that produced the task. Dates belong only where a rule consumes them:
Suite Health first-seen (staleness re-verification) and Backlog re-park records
(stops re-litigating). Ledger conventions self-propagate — one stray annotation
becomes the house style — so this rule is explicit.

**Check existing facilities before building (capability-check rule).** Before
adding a feature-level system, consult the project's capability/placement map
(if one exists — e.g. a `docs/API.md` map): if an existing facility already
covers it, adopt that facility or explicitly justify the divergence in the task.
Non-use of a facility is inconclusive (it may be wrong-shaped, or just not
reached yet) — so the justification names which.

Every task carries a short stable **ID** — a `(kebab-id)` token right after the
checkbox (or in the `###` heading, for Design entries). IDs are how `Blocked:`
and other cross-references point at a task, so an auto-unblock check is exact
rather than a fuzzy title match. Keep them short, unique, and stable once set.

**Auto task format:**
```markdown
- [ ] (task-id) Brief title describing the work
  Context: why this matters, current state, relevant files/locations
  Repro gate: for bug tasks, the implementer must first demonstrate the bug on the
  current tree, or point to an already-recorded failing test/artifact named here
  Files: `path/to/file.go` (what to change), `path/to/other.go` (why)
  Acceptance: how to verify completion, including the test/assertion/artifact
  that proves the work is done (and, for bugs, that the fix holds)
```

**Bug-task proof requirement.** An implementer must not proceed from diagnosis to
code changes on a bug task until it has demonstrated the bug in the current tree
with a concrete failing command, screenshot, trace, or assertion. If the bug
cannot be reproduced, the implementer marks the task `[-]` with the attempted
repro steps and stops rather than inventing a cause. The task must also define a
fix-verification path: a repeatable check that fails before the fix and passes
after it.

**Design task format:**
```markdown
- (task-id) Brief title describing the problem
  Context: current state, constraints, relevant files/locations
  Questions:
  - Decision point or tradeoff to resolve
```

**Awaiting-sign-off format** (set by the review gate when every Acceptance bullet
is verified except a human-judgment one):
```markdown
- [H] (task-id) Brief title describing the work
  Context: ...
  Files: ...
  Acceptance: ...
  Evidence: everything the human needs to sign off with MINIMAL effort, plus
  one line on what to judge ("does the trap read as dangerous?"). For visual
  checks: artifact paths, already produced — never a command to go render
  them. For live checks (audio, feel): the exact command that lands the human
  directly at the thing being judged — a preview profile, start-state flag, or
  debug camera ("play until you reach it" is not Evidence). If no such
  shortcut exists, wiring one is part of the task, not the human's problem.
```

**Failed-review rework format** (set by the review gate or `signoff`; keep the
original task block, change the marker to `[*]`, and add a `Review:` block —
condensing any superseded history per the condensation rule):
```markdown
- [*] (task-id) Brief title describing the work
  Context: ...
  Files: ...
  Acceptance: ...
  Review: what the implementation fails to satisfy — be specific (which
  Acceptance bullet, what's missing or wrong, where). An implementer addresses
  these, then flips the marker back to `[x]` for re-review.
```

**Blocked format** (set when a task must wait on other unfinished work; keep the
original task block, change the marker to `[-]`, and add a `Blocked:` line):
```markdown
- [-] (task-id) Brief title describing the work
  Context: ...
  Files: ...
  Acceptance: ...
  Blocked: <task-id>[, <task-id>…] — auto-unblocks once every listed task is
  `[x]`/`[H]`/archived/gone. For a non-task condition, write
  `Blocked: external — <condition>` naming the concrete evidence and what would
  clear it (or cite a Suite Health entry); external blocks never auto-unblock
  and are surfaced, not silently skipped, when only they remain.
```

---

## Committing the ledger files

`TASKS.md`, `ARCHIVE.md`, and `BACKLOG.md` are shared **ledgers**, not source
code — so the usual "never commit changes that aren't mine" caution does not
apply. It is always fine to `git add` and commit these three files wholesale,
even if that sweeps up another agent's in-flight edit: a partial ledger update
can't break a build, and the other agent just commits their remainder on top.
Each agent is responsible only for confirming *its own* edit landed.

**Still forbidden:** *destructive* git ops on these files — `checkout`/`restore`,
`stash`, `reset --hard`, `clean` — which discard others' *uncommitted* work
irrecoverably. Additive commits are blessed; destructive ones are not.

**Verify, then alleviate:** after committing, check your change is in `HEAD`
(`git log -1 -p -- TASKS.md`). If it's missing, add it in a **new** commit —
never `--amend` or rebase, which rewrites shared history.

---

## Project verification policy

<!-- Project-specific evidence rules live here so they have a designated home.
Examples of what belongs: which task classes need rendered-frame/screenshot
proof rather than state checks alone; artifact/location conventions (e.g.
proofs under `out/`); asset-review rules ("new art requires human sign-off via
[H]"); domain-specific evidence standards (e.g. geometry dumps for collision
work). Delete this section if the project needs none. -->

_None yet._

---

## Auto

Group related tasks under `###` subheadings for readability. Ordering
top-to-bottom reflects intended implementation order — the loops pick the first
actionable task, so position is priority.

### Regex correctness

- [H] (regex-zero-min-bounds) Honor zero occurrences in bounded regex quantifiers
  Context: atom and grouped `{0,n}` quantifiers are marked nullable, but their counter-based programs emit the quantified atom once before reaching the loop/exit split. Consequently `a{0,2}` and `(a){0,2}` cannot take the zero-occurrence path, and lazy variants consume when they should prefer an empty match.
  Repro gate: `zig-out/bin/fiskta --input fixtures/empty.txt find:re 'a{0,2}' print found` exits 1 on the current tree after `zig build test` creates fixtures; it should print `found` and exit 0.
  Files: `src/regex_prog.c` (counter-program construction for atoms and groups), `tools/test.py` (zero-minimum greedy and lazy regressions)
  Acceptance: atom and grouped `{0,n}` patterns match empty input and positions where the atom is absent; greedy variants still prefer the longest permitted match while lazy variants prefer the shortest; exact and positive-minimum bounds remain unchanged; `zig build test` passes.
  Evidence: The compiler now gives atom and grouped `{0,n}` programs an ordered entry split that can bypass the bounded loop; `python3 tools/test.py --filter regex-zero-min` exercises eight empty, absent, greedy, lazy, atom, and group cases, while `zig build test` covers the full suite. Judge whether ordinary greedy-longest/lazy-shortest behavior is intended when a following atom forces backtracking. Independent review did not run a standalone memory benchmark, but verified the change adds one fixed NFA instruction per affected quantifier with no runtime allocation or input-dependent storage.

### View boundaries

- [H] (view-utf8-char-boundary) Keep character extraction inside byte-bounded views
  Context: positive character takes call `io_prev_char_start()` and use the returned character start without clamping it to the active view. If the view begins inside a multi-byte UTF-8 character, `take +Nc` can stage and emit bytes before the view's lower bound, violating the view's `[lo, hi)` restriction.
  Repro gate: after `zig build test`, `zig-out/bin/fiskta --input fixtures/unicode-test.txt view BOF+7b EOF take +1c | od -An -tx1` emits `e4 b8 96`; the first byte is at offset 6, before the view lower bound at offset 7.
  Files: `src/engine.c` (character take range construction), `src/fileio.c` (character-boundary behavior if needed), `tools/test.py` (views bisecting UTF-8 characters)
  Acceptance: no character-based take or skip stages output outside an active view, including when either view boundary bisects a valid or malformed UTF-8 sequence; regression tests cover positive and negative movement at both bounds; `zig build test` passes.
  Evidence: Active-view character decoding is now window-local for takes, skip offsets, and location offsets, with boundary fragments treated as one-byte characters and view-edge crossings preserved as failures; `python3 tools/test.py --exe zig-out/bin/fiskta --filter view-utf8` runs 15 focused cases and `zig build test` covers the full suite. Judge whether treating UTF-8 fragments bisected by byte views as independent one-byte characters is the intended user-facing model. Independent review did not profile allocation or exercise near-`INT64_MAX` files, but source inspection found only stack locals and existing fixed buffers.

### Library error reporting

- [H] (or-error-state-leak) Suppress errors from recovered clause alternatives
  Context: operations call `error_set()` while clauses are still being evaluated. A failing `OR` branch can therefore invoke the public error callback and leave thread-local error details set even when a later alternative succeeds and the runtime call returns 0. This contradicts the API contract that successful calls clear error state and makes ordinary fallback control flow appear erroneous to embedders.
  Repro gate: `zig build wrapper` followed by `zig-out/bin/fiskta_library_wrapper --input fixtures/overlap.txt view BOF+2b EOF-2b skip to BOF-1b OR take +1b` exits 0 and emits `a`, but also invokes the callback and prints `fiskta: location not resolvable...` to stderr.
  Files: `src/fiskta.c` (final outcome and error-state lifecycle), `src/engine.c` (recoverable diagnostic staging if needed), `src/fiskta.h` (contract only if clarification is necessary), `tools/test.py` and `tools/fiskta_library_wrapper.c` (library regressions)
  Acceptance: errors from failed alternatives are reported only if they determine the final call outcome; a successful recovered call invokes no error callback and leaves `fiskta_error_code()` as `FISKTA_E_OK`, `fiskta_error_message()` as `NULL`, and position as `-1`; terminal failures retain their diagnostics; `zig build test` and `zig build test-lib` pass.
  Evidence: Clause diagnostics are now captured with callbacks deferred, cleared on successful or ignored outcomes, and published once with message and position on terminal failures through the shared file/buffer finalization path; run `zig build wrapper && python3 tools/test.py --exe zig-out/bin/fiskta_library_wrapper --filter error-006`, with `zig build test` and `zig build test-lib` as full gates. Judge whether recovered failures are completely invisible while terminal diagnostics remain useful and occur exactly once. Independent review did not fault-inject post-open disk I/O or resource exhaustion; source inspection confirmed fatal runtime statuses use the same finalization mapping.

### CLI robustness

- [ ] (continue-time-lookahead-overflow) Parse optional continue intervals without signed overflow
  Context: the `--continue` optional-value lookahead accumulates its numeric prefix in a signed `int` without bounds checks before passing accepted values to the checked time parser. A long numeric token therefore invokes undefined behavior during CLI parsing instead of producing a usage error.
  Repro gate: `zig build asan` followed by `zig-out/bin/fiskta-asan --continue 999999999999999999999h --input fixtures/overlap.txt take +1b` aborts with signed integer overflow at `src/main.c:191` on the current tree.
  Files: `src/main.c` (overflow-free optional-value recognition), `tools/test.py` (oversized interval regression)
  Acceptance: optional `--continue`/`--continue-on-fail` interval recognition performs no unchecked arithmetic; oversized numeric values return exit 7 with a clear diagnostic under optimized and sanitizer builds; valid zero and suffixed intervals keep their current behavior; `zig build test` passes under the normal and ASan binaries.

---

## Design

Items requiring user input before implementation. Design tasks are never
completed directly — once decisions are made, they are promoted to one or more
[Auto](#auto) tasks with implementation notes, then removed from this section.

### Retry regex matches at overlapping candidate starts (regex-overlap-starts)

Context: Concurrent candidate starts fixed simple overlap misses, but two independent review rounds found that the current scheduling still cannot satisfy earliest-forward/rightmost-backward semantics within the fixed work budget for long counted repeats. The first rework stopped retaining input-proportional counted-repeat candidates, but backward `a{2,}` over `aaa` still returns start 0 instead of the rightmost start 1, while forward `a{20000}b` after a 10,000-byte failed prefix still exhausts the 50M work budget before reaching a valid later match.
Questions:
- Which bounded scheduling strategy should replace serial retry/concurrent-state heuristics so every viable start is considered without input-proportional memory or quadratic work?
- Should backward search use a distinct reverse/rightmost algorithm, or can one forward scan provide rightmost results for live unbounded repeats without delaying later candidate starts?

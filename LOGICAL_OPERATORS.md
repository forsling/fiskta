# Logical Operators in fiskta

This document explains how the `AND`, `OR`, and `THEN` operators work in fiskta.

## Basic Principles

**Evaluation Order**: Strictly left-to-right, no operator precedence.

**Atomicity**: Each clause is atomic - either all operations succeed and commit, or none do.

**Three Operators**:

- `THEN` - Sequential execution (always runs next clause)
- `AND` - Conditional execution (next clause runs only if current succeeds)
- `OR` - Alternative execution (try next clause only if current fails)

## The Four Rules

### Rule 1: THEN Always Executes

The next clause after `THEN` always runs, regardless of whether the previous clause succeeded or failed.

```bash
# Both clauses always run
find abc THEN take +3b

# Even if find fails, take still runs (from wherever cursor is)
find NOTFOUND THEN take +3b
```

### Rule 2: AND Creates Failure Chains

When a clause linked with `AND` fails, fiskta skips all remaining consecutive `AND` clauses and continues after the chain.

```bash
# If find succeeds, take runs. If find fails, take is skipped.
find abc AND take +3b

# Chain: if any fails, skip rest of chain
find abc AND skip 3b AND take +3b
# If find fails → skip both skip and take
# If find succeeds but skip fails → skip take
```

### Rule 3: OR Creates Success Chains

When a clause linked with `OR` succeeds, fiskta skips all remaining consecutive `OR` clauses and continues after the chain.

```bash
# Try abc first; if it succeeds, skip xyz
find abc OR find xyz

# Chain: first success wins
find abc OR find def OR find xyz
# If abc succeeds → skip def and xyz
# If abc fails, try def → if def succeeds, skip xyz
```

### Rule 4: THEN Breaks Chains

`THEN` acts as a "chain breaker" - clauses after `THEN` always execute, even if previous `AND` or `OR` chains failed or succeeded.

```bash
# skip always runs, even if the AND chain fails
find NOTFOUND AND take +3b THEN skip 1b

# take always runs, even if OR chain succeeded on first clause
find abc OR find xyz THEN take +3b
```

## Common Patterns

### Sequential Processing (Do Both)

```bash
# Always do both operations
find abc THEN take +3b
```

### Conditional Processing (Do Second Only If First Succeeds)

```bash
# Only take if find succeeds
find abc AND take +3b
```

### Fallback (Try First, Then Second)

```bash
# Try abc, if not found try xyz
find abc OR find xyz
```

### Try With Cleanup (Always Clean Up)

```bash
# Try find+take, but always skip at the end
find abc AND take +3b THEN skip 1b
```

### Error Recovery

```bash
# If find fails, take anyway from current position
find abc AND take +3b OR take +3b
```

## Left-to-Right Evaluation Examples

Because there's no operator precedence, evaluation is strictly left-to-right. This creates some non-obvious behaviors:

### Example 1: `A OR B AND C`

```bash
find abc OR find def AND take +3b
```

**Evaluation**: `(A OR B) AND C`

- If `find abc` succeeds → skip `find def` (OR short-circuit) → continue to `take +3b`
- If `find abc` fails → try `find def` AND `take +3b`

**Mental model**: The OR chain is `(A OR B)`, then that result is used for the AND with C.

### Example 2: `A AND B OR C`

```bash
find abc AND take +3b OR skip 1b
```

**Evaluation**: `(A AND B) OR C`

- If `find abc` succeeds → `take +3b` runs
  - If `take` succeeds → skip `skip 1b` (OR short-circuit)
  - If `take` fails → AND chain fails, try `skip 1b`
- If `find abc` fails → AND chain fails, try `skip 1b`

**Mental model**: The AND chain is `(A AND B)`, then if it fails, try C.

### Example 3: `A AND B THEN C`

```bash
find xyz AND take +3b THEN skip 1b
```

**Evaluation**: AND chain, then THEN breaks the chain

- If `find xyz` succeeds → `take +3b` runs
  - If `take` succeeds → `skip 1b` runs (THEN always runs)
  - If `take` fails → skip to after AND chain → `skip 1b` runs (THEN always runs)
- If `find xyz` fails → skip `take +3b` → `skip 1b` runs (THEN always runs)

**Mental model**: THEN is a barrier - clauses after it always run.

### Example 4: Complex Chain

```bash
find abc AND skip 3b OR find def AND take +3b THEN label A
```

**Evaluation**: `((A AND B) OR (C AND D)) THEN E`

- Try `find abc` AND `skip 3b`
  - If both succeed → skip the OR alternatives → `label A` runs
  - If either fails → try `find def` AND `take +3b`
    - If both succeed → `label A` runs
    - If either fails → `label A` still runs (THEN always runs)

## Exit Codes

- **Exit 0 (success)**: If any clause succeeds, OR if AND chains fail but later THEN clauses succeed
- **Exit 2 (failure)**: If AND chains fail AND no clauses succeed after them

Examples:

```bash
# Exit 0: first clause succeeds
find abc THEN find NOTFOUND

# Exit 2: AND chain fails, no clauses after
find abc AND find NOTFOUND

# Exit 0: AND chain fails, but THEN clause succeeds
find abc AND find NOTFOUND THEN skip 1b

# Exit 2: all clauses fail
find NOTFOUND OR find ALSONOTFOUND
```

## Tips for Avoiding Confusion

1. **Use THEN for sequential operations**: If you always want something to run, use THEN
2. **AND is for requirements**: Use AND when the second operation only makes sense if the first succeeds
3. **OR is for alternatives**: Use OR when you want to try multiple approaches
4. **THEN breaks chains**: Use THEN to ensure cleanup or next steps always happen
5. **Think left-to-right**: Don't assume traditional boolean precedence (AND before OR)

## When You Need Precedence

If you need complex precedence, consider breaking your program into multiple invocations or using the command stream mode:

```bash
# Instead of: A OR B AND C (which evaluates as (A OR B) AND C)
# Run separately:
fiskta 'A OR B' && fiskta 'C'

# Or use command stream:
echo -e "A OR B\nC" | fiskta --commands-stdin --input file.txt
```

## Summary

The key insight is that fiskta doesn't have operator precedence - it's a simple left-to-right evaluator with three operators:

- **THEN** = "do this next, no matter what"
- **AND** = "only do this next if I succeeded"
- **OR** = "only do this next if I failed"

And one special rule: **THEN breaks AND/OR chains**, ensuring clauses after THEN always execute.

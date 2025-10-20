#!/usr/bin/env python3
"""fiskta fuzzer - Find crashes, hangs, and memory errors

Single-file fuzzer that generates random fiskta programs and inputs to find bugs.
Uses multiprocessing for parallel execution and proper signal handling.
"""

import argparse
import multiprocessing
import os
import random
import signal
import struct
import subprocess
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from multiprocessing import Pool, Queue, TimeoutError as MPTimeoutError
from pathlib import Path
from typing import Optional

# ========= Tunable Constants =========

# Strategy: command generation distribution
RANDOM_GENERATION_PCT = 0.40      # 40% pure random programs
MUTATED_PROGRAMS_PCT = 0.40       # 40% mutated programs
TARGETED_MISMATCH_PCT = 0.20      # 20% targeted mismatches (remaining)

# Strategy: input generation
CORPUS_MUTATION_PCT = 0.80        # 80% corpus mutations when corpus enabled

# ========= Types =========

@dataclass
class Config:
    """Fuzzer configuration"""
    fiskta_path: Path
    artifacts: Path
    run_dir: Path
    cases: Optional[int]  # None = continuous mode
    seed: int
    timeout_ms: int
    minimize: bool
    use_corpus: bool
    corpus_dir: Path
    workers: int
    repro_case: Optional[int]
    repro_input: Optional[Path]
    repro_ops: Optional[Path]
    is_asan: bool
    min_ops: int
    max_ops: int
    save_all: bool

@dataclass
class TestResult:
    """Result of executing a single test case"""
    crashed: bool
    timed_out: bool
    exit_code: int
    signal_num: Optional[int] = None

@dataclass
class Stats:
    """Aggregate statistics across all workers"""
    total: int = 0
    saved: int = 0
    timeouts: int = 0
    crashed: int = 0
    exits: dict[int, int] = field(default_factory=dict)
    start_time: float = field(default_factory=time.time)

# ========= System utils =========

def get_cpu_count() -> int:
    """Get number of CPU cores"""
    return os.cpu_count() or 1

def auto_detect_binary() -> tuple[Path, bool]:
    """Auto-detect fiskta binary (prefer ASAN version)

    Returns:
        (absolute_path, is_asan)
    """
    if Path("./fiskta-asan").exists():
        return Path("./fiskta-asan").absolute(), True
    if Path("zig-out/bin/fiskta-asan").exists():
        return Path("zig-out/bin/fiskta-asan").absolute(), True
    if Path("./fiskta").exists():
        return Path("./fiskta").absolute(), False
    if Path("zig-out/bin/fiskta").exists():
        return Path("zig-out/bin/fiskta").absolute(), False

    print("Error: No fiskta binary found. Run ./build.sh or zig build first", file=sys.stderr)
    sys.exit(2)

def setup_asan_env():
    """Configure ASAN environment variables"""
    if "ASAN_OPTIONS" not in os.environ:
        os.environ["ASAN_OPTIONS"] = "abort_on_error=1:detect_leaks=1:symbolize=1"
    if "UBSAN_OPTIONS" not in os.environ:
        os.environ["UBSAN_OPTIONS"] = "print_stacktrace=1"

# ========= Argument parsing =========

def parse_args() -> Config:
    """Parse command-line arguments"""
    binary, is_asan = auto_detect_binary()
    if is_asan:
        setup_asan_env()

    cpu_count = get_cpu_count()
    default_workers = max(1, cpu_count // 2)

    parser = argparse.ArgumentParser(
        description="fiskta fuzzer - Find crashes, hangs, and memory errors",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Strategy:
  • Commands: 50%% pure random generation, 50%% mutated (13 mutation types)
  • Input data: 80%% corpus mutations from seed files, 20%% pure random
  • Parallel workers scale to CPU count
  • Auto crash minimization

Examples:
  %(prog)s                                    # Recommended: continuous fuzzing
  %(prog)s --cases 100000                     # Fixed 100k test cases
  %(prog)s --quick                            # Quick sanity check
  %(prog)s --no-corpus --min-ops 30           # Deep random programs
  %(prog)s --repro-case 0                     # Reproduce crash from newest run
  %(prog)s --run-dir artifacts/run_2025-10-19_123456 --repro-case 5
        """,
    )

    parser.add_argument("--fiskta-path", type=Path, default=binary,
                        help=f"Binary to test (default: {binary})")
    parser.add_argument("--artifacts", type=Path, default=Path("artifacts"),
                        help="Output directory (default: artifacts/)")
    parser.add_argument("--run-dir", type=Path, dest="run_dir_override",
                        help="Specific run directory for --repro-case")
    parser.add_argument("--cases", type=int, metavar="N",
                        help="Fixed case count (default: continuous until Ctrl+C)")
    parser.add_argument("--minimize", action="store_true", default=True,
                        help="Minimize crashes (default: enabled)")
    parser.add_argument("--no-minimize", action="store_false", dest="minimize",
                        help="Disable minimization")
    parser.add_argument("--corpus", action="store_true", default=True,
                        help="Use corpus mutations (default: enabled)")
    parser.add_argument("--no-corpus", action="store_false", dest="corpus",
                        help="Pure random generation only")
    parser.add_argument("--corpus-dir", type=Path, default=Path("tests/fixtures"),
                        help="Corpus directory (default: tests/fixtures/)")
    parser.add_argument("--timeout-ms", type=int, default=1500, metavar="N",
                        help="Per-case timeout in milliseconds (default: 1500)")
    parser.add_argument("--seed", type=int, default=int(time.time()),
                        help="RNG seed (default: current time)")
    parser.add_argument("--workers", type=int, default=default_workers, metavar="N",
                        help=f"Parallel workers (default: {default_workers})")
    parser.add_argument("--min-ops", type=int, default=3, metavar="N",
                        help="Min operations per command (default: 3)")
    parser.add_argument("--max-ops", type=int, default=15, metavar="N",
                        help="Max operations per command (default: 15)")
    parser.add_argument("--save-all", action="store_true",
                        help="Save all cases, not just crashes")
    parser.add_argument("--quick", action="store_true",
                        help="Quick test (10k cases, 2 workers)")
    parser.add_argument("--repro-case", type=int, metavar="N",
                        help="Reproduce crash case N")
    parser.add_argument("--repro", nargs=2, metavar=("INPUT", "OPS"),
                        help="Reproduce with specific files")

    args = parser.parse_args()

    # Handle --quick mode
    if args.quick:
        args.cases = 10000
        args.workers = 2

    # Determine run directory
    if hasattr(args, 'run_dir_override') and args.run_dir_override:
        run_dir = args.run_dir_override
    elif not args.repro_case and not args.repro:
        timestamp = datetime.now().strftime("%Y-%m-%d_%H%M%S")
        run_dir = args.artifacts / f"run_{timestamp}"
    else:
        run_dir = args.artifacts

    return Config(
        fiskta_path=args.fiskta_path,
        artifacts=args.artifacts,
        run_dir=run_dir,
        cases=args.cases,
        seed=args.seed,
        timeout_ms=args.timeout_ms,
        minimize=args.minimize,
        use_corpus=args.corpus,
        corpus_dir=args.corpus_dir,
        workers=args.workers,
        repro_case=args.repro_case,
        repro_input=Path(args.repro[0]) if args.repro else None,
        repro_ops=Path(args.repro[1]) if args.repro else None,
        is_asan=is_asan,
        min_ops=args.min_ops,
        max_ops=args.max_ops,
        save_all=args.save_all,
    )

# ========= Random helpers =========

def rand_number() -> str:
    """Generate random number string"""
    length = random.randint(1, 6)
    return ''.join(random.choices('0123456789', k=length))

def rand_unit() -> str:
    """Generate random unit (b, l, c)"""
    return random.choice(['b', 'l', 'c'])

def rand_signed_number() -> str:
    """Generate signed number"""
    sign = random.choice(['', '-'])
    return sign + rand_number()

def rand_offset() -> str:
    """Generate random offset like +10b or -5c"""
    sign = random.choice(['+', '-'])
    return sign + rand_number() + rand_unit()

def rand_upper_letter() -> str:
    """Generate random uppercase letter"""
    return random.choice('ABCDEFGHIJKLMNOPQRSTUVWXYZ')

def rand_name() -> str:
    """Generate random label name"""
    length = random.randint(1, 10)
    chars = [rand_upper_letter()]
    chars.extend(random.choices('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-', k=length-1))
    return ''.join(chars)

def rand_string_token() -> str:
    """Generate random string token for patterns"""
    length = random.randint(1, 26)
    pool = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 \t.:;,_-+/\\[]{}()|?*^$@!~\n\r'
    s = ''.join(random.choices(pool, k=length))
    s = s.strip(' \n\r\t')
    return s if s else 'x'

def gen_random_regex() -> str:
    """Generate random regex pattern using all supported features"""
    complexity = random.choice(['simple', 'medium', 'complex', 'pathological'])

    def atom():
        """Generate regex atom (character, class, or group)"""
        choice = random.randint(1, 20)
        if choice == 1:
            return random.choice(['a', 'b', 'x', '1', '0', ' ', '\n'])
        elif choice == 2:
            return '.'
        elif choice == 3:
            return '\\d'
        elif choice == 4:
            return '\\D'
        elif choice == 5:
            return '\\w'
        elif choice == 6:
            return '\\W'
        elif choice == 7:
            return '\\s'
        elif choice == 8:
            return '\\S'
        elif choice == 9:
            # Escape sequences: \n \t \r \f \v \0
            return random.choice(['\\n', '\\t', '\\r', '\\f', '\\v', '\\0'])
        elif choice == 10:
            # Escaped meta-characters
            return random.choice(['\\.', '\\*', '\\+', '\\?', '\\[', '\\]',
                                  '\\(', '\\)', '\\{', '\\}', '\\|', '\\^', '\\$'])
        elif choice <= 14:
            # Character class [...]
            class_type = random.randint(1, 4)
            if class_type == 1:
                return f"[{random.choice(['a-z', '0-9', 'A-Z', 'a-zA-Z'])}]"
            elif class_type == 2:
                negated_choices = ['0-9', 'a-z', ' \t']
                return f"[^{random.choice(negated_choices)}]"
            elif class_type == 3:
                chars = ''.join(random.sample('abcxyz0123', random.randint(2, 5)))
                return f"[{chars}]"
            else:
                return "[\\d\\w]"
        elif choice <= 16:
            # Anchors (sometimes as atoms in sequences)
            return random.choice(['^', '$'])
        else:
            # Empty or single char
            return random.choice(['', 'a', 'x'])

    def quantifier():
        """Generate quantifier"""
        q_type = random.randint(1, 11)
        if q_type <= 3:
            return random.choice(['*', '+', '?'])
        elif q_type <= 5:
            # Exact count {n}
            n = random.choice([0, 1, 2, 3, 5, 10, 20, 50, 99])
            return f"{{{n}}}"
        elif q_type <= 7:
            # Safe quantifiers within limits {n,m}
            min_val = random.randint(0, 20)
            max_val = min_val + random.randint(1, 30)
            return f"{{{min_val},{max_val}}}"
        elif q_type <= 9:
            # Edge case quantifiers near limits
            return random.choice(['{0,100}', '{99,100}', '{50,100}', '{0,1}', '{1,2}', '{100}'])
        else:
            # Extreme quantifiers (may cause resource exhaustion)
            return random.choice(['{0,999999}', '{999,}', '{40,80}', '{70,99}'])

    def term(depth=0):
        """Generate regex term (atom + optional quantifier)"""
        if depth > 3:  # Limit nesting depth
            return atom() + (quantifier() if random.random() < 0.3 else '')

        term_type = random.randint(1, 10)
        if term_type <= 6:
            # Simple atom with optional quantifier
            return atom() + (quantifier() if random.random() < 0.5 else '')
        elif term_type <= 8 and depth < 2:
            # Grouped term with quantifier
            inner = term(depth + 1)
            return f"({inner})" + (quantifier() if random.random() < 0.7 else '')
        else:
            # Nested group (potential for catastrophic backtracking)
            inner = atom() + quantifier()
            return f"({inner})" + (quantifier() if random.random() < 0.5 else '')

    def alternation(depth=0):
        """Generate alternation (a|b|c)"""
        num_alts = random.randint(2, random.choice([3, 5, 10, 50, 200]))
        if num_alts > 256:  # Respect MAX_ALTS limit
            num_alts = random.randint(200, 256)

        parts = [term(depth) for _ in range(num_alts)]
        result = '|'.join(parts)

        # Sometimes wrap in group with quantifier
        if random.random() < 0.3:
            result = f"({result})" + quantifier()

        return result

    # Build regex based on complexity
    if complexity == 'simple':
        # Single term
        return term()

    elif complexity == 'medium':
        # Few terms or small alternation
        if random.random() < 0.5:
            return ''.join(term() for _ in range(random.randint(1, 3)))
        else:
            return alternation()

    elif complexity == 'complex':
        # Multiple terms, groups, alternations
        parts = []
        for _ in range(random.randint(2, 4)):
            if random.random() < 0.3:
                parts.append(alternation(1))
            else:
                parts.append(term(1))
        result = ''.join(parts)

        # Sometimes add anchors
        if random.random() < 0.2:
            result = '^' + result
        if random.random() < 0.2:
            result = result + '$'

        return result

    else:  # pathological
        # Patterns designed to trigger edge cases
        pattern_type = random.randint(1, 10)
        if pattern_type <= 2:
            # Nested quantifiers: ((a+)+)+
            base = atom()
            for _ in range(random.randint(2, 4)):
                base = f"({base}{quantifier()})"
            return base
        elif pattern_type <= 4:
            # Many alternations near limit
            num = random.randint(200, 256)
            return '|'.join(atom() for _ in range(num))
        elif pattern_type <= 6:
            # Empty patterns
            return random.choice(['()*', '(|a)*', '(a|)*', '()*?', '()+', '()'])
        elif pattern_type <= 8:
            # Extreme quantifiers
            return atom() + random.choice(['{0,999999}', '{999,}', '{100,}'])
        else:
            # Mixed: group with alternation and quantifier
            alts = '|'.join(atom() for _ in range(random.randint(5, 20)))
            return f"({alts}){quantifier()}"

def rand_hex_string() -> str:
    """Generate random hex string like '0A 1F 2C'"""
    pairs = random.randint(1, 13)
    result = []
    for i in range(pairs):
        if i > 0 and random.random() < 0.5:
            result.append(' ')
        result.append(random.choice('0123456789ABCDEF'))
        result.append(random.choice('0123456789ABCDEF'))
    return ''.join(result)

# ========= Random operation generator =========

def gen_random_op() -> list[str]:
    """Generate a single random operation"""
    ops = [
        ('find', lambda: [rand_string_token()]),
        ('find:re', lambda: [gen_random_regex()]),
        ('find:bin', lambda: [rand_hex_string()]),
        ('skip', lambda: [rand_number() + rand_unit()]),
        ('take', lambda: [rand_signed_number() + rand_unit()] if random.random() < 0.5 else
                        (['until:re', gen_random_regex()] if random.random() < 0.4 else
                         ['until', rand_string_token()] if random.random() < 0.5 else
                         ['until:bin', rand_hex_string()] if random.random() < 0.5 else
                         ['to', gen_location_expr()])),
        ('label', lambda: [rand_name()]),
        ('view', lambda: [gen_location_expr(), gen_location_expr()]),
        ('clear', lambda: ['view']),
        ('print', lambda: [rand_string_token()]),
        ('fail', lambda: [rand_string_token()]),
    ]

    op, gen_args = random.choice(ops)
    tokens = [op] + gen_args()
    return tokens

def gen_location() -> str:
    """Generate a location base"""
    bases = ['cursor', 'BOF', 'EOF', 'match-start', 'match-end', 'line-start', 'line-end']
    if random.random() < 0.25:
        return rand_name()  # Random label
    return random.choice(bases)

def gen_location_expr() -> str:
    """Generate location expression (base with optional offset)"""
    loc = gen_location()
    if random.random() < 0.3:
        loc += rand_offset()
    return loc

def gen_random_program(min_ops: int, max_ops: int) -> list[str]:
    """Generate random program with multiple operations"""
    num_ops = random.randint(min_ops, max_ops)
    tokens = []

    for i in range(num_ops):
        tokens.extend(gen_random_op())

        # Add clause link between operations (or not - 25% chance of link)
        if i < num_ops - 1:
            if random.random() < 0.25:
                # Add clause link (split into new clause)
                roll = random.randint(1, 10)
                if roll <= 6:
                    tokens.append('THEN')
                elif roll <= 9:
                    tokens.append('OR')
                else:
                    tokens.append('AND')
            # else: no link - operations stay in same clause (75% of the time)

    return tokens

# ========= Program mutations (13 types) =========

def mutate_program(ops: list[str]) -> list[str]:
    """Apply 0-2 mutations to operation program"""
    mutated = ops.copy()
    num_mutations = random.randint(0, 2)

    for _ in range(num_mutations):
        mutation_type = random.randint(1, 13)

        if mutation_type == 1 and mutated:
            # Replace size with extreme value
            extreme_sizes = ['0b', '0c', '0l', '999999999b', '999999999c', '999999999l', '-1b', '-1c', '-1l']
            for i, token in enumerate(mutated):
                if token and (token[-1] in 'blc') and token[:-1].lstrip('-').isdigit():
                    if random.random() < 0.5:
                        mutated[i] = random.choice(extreme_sizes)
                        break

        elif mutation_type == 2 and mutated:
            # Replace location with extreme offset
            extreme_offsets = [
                'BOF+999999999b', 'EOF-999999999b', 'BOF+999999999c', 'EOF-999999999c',
                'BOF+999999999l', 'EOF-999999999l', 'cursor+999999999b', 'cursor-999999999b',
                'match-start+999999999b', 'match-end-999999999b'
            ]
            for i, token in enumerate(mutated):
                if token in ['BOF', 'EOF', 'cursor'] or token.startswith(('match-', 'line-')):
                    if random.random() < 0.5:
                        mutated[i] = random.choice(extreme_offsets)
                        break

        elif mutation_type == 3 and len(mutated) > 2:
            # Inject generated regex (all supported features)
            for i in range(1, len(mutated)):
                if mutated[i-1] == 'find:re':
                    if random.random() < 0.5:
                        mutated[i] = gen_random_regex()
                        break

        elif mutation_type == 4 and len(mutated) > 1:
            # Delete random token
            mutated.pop(random.randrange(len(mutated)))

        elif mutation_type == 5 and len(mutated) > 1:
            # Duplicate random token
            pos = random.randrange(len(mutated))
            mutated.insert(pos, mutated[pos])

        elif mutation_type == 6 and len(mutated) > 2:
            # Swap two adjacent tokens
            pos = random.randrange(len(mutated) - 1)
            mutated[pos], mutated[pos+1] = mutated[pos+1], mutated[pos]

        elif mutation_type == 7 and mutated:
            # Insert random operation keyword
            keywords = ['find', 'find:re', 'find:bin', 'skip', 'take', 'label', 'view', 'clear',
                        'print', 'fail', 'to', 'until', 'until:re', 'until:bin', 'at', 'THEN', 'OR', 'AND']
            pos = random.randint(0, len(mutated))
            mutated.insert(pos, random.choice(keywords))

        elif mutation_type == 8 and mutated:
            # Replace token with garbage
            garbage = ['', ' ', '  ', '\t', '\n', '999999999999999999999', '-999999999999999999999',
                       '0x', ':::', '...', '---', '+++', '////', '[[[[', ']]]]', '{{{{', '}}}}', '____', 'INVALID']
            mutated[random.randrange(len(mutated))] = random.choice(garbage)

        elif mutation_type == 9 and mutated:
            # Negate/flip numbers in sizes
            for i, token in enumerate(mutated):
                if token and token[-1] in 'blc' and token[:-1].lstrip('-').isdigit():
                    if random.random() < 0.5:
                        num_part = token[:-1]
                        unit = token[-1]
                        if num_part.startswith('-'):
                            mutated[i] = num_part[1:] + unit
                        else:
                            mutated[i] = '-' + num_part + unit
                        break

        elif mutation_type == 10 and len(mutated) > 1:
            # Inject invalid hex patterns
            invalid_hex = ['G', 'ZZ', '0G', '1H', 'XY', '  ', '0', '000', 'FFFFF']
            for i in range(1, len(mutated)):
                if mutated[i-1] in ['find:bin', 'until:bin']:
                    if random.random() < 0.5:
                        mutated[i] = random.choice(invalid_hex)
                        break

        elif mutation_type == 11 and mutated:
            # Off-by-one: increment/decrement numbers
            for i, token in enumerate(mutated):
                if token and token.lstrip('-').isdigit():
                    if random.random() < 0.5:
                        n = int(token)
                        mutated[i] = str(n + random.choice([-1, 1]))
                        break

        elif mutation_type == 12 and mutated:
            # Remove clause links
            for i in range(len(mutated) - 1, -1, -1):
                if mutated[i] in ['THEN', 'OR', 'AND']:
                    if random.random() < 0.5:
                        mutated.pop(i)
                        break

        elif mutation_type == 13 and len(mutated) > 2:
            # Duplicate sequence of 2-3 tokens
            start = random.randrange(len(mutated) - 1)
            count = min(random.randint(2, 3), len(mutated) - start)
            sequence = mutated[start:start+count]
            insert_pos = random.randint(0, len(mutated))
            mutated[insert_pos:insert_pos] = sequence

    return mutated


def mutate_for_input_mismatch(ops: list[str], input_data: bytes) -> tuple[list[str], bytes]:
    """Apply mutations that target pattern/input size mismatches (triggers BUG-1 type issues)"""
    mutated_ops = ops.copy()
    mutated_input = input_data

    mismatch_type = random.randint(1, 5)

    if mismatch_type == 1:
        # LONG PATTERN + SHORT INPUT (BUG-1: buffer underflow)
        for i in range(1, len(mutated_ops)):
            if mutated_ops[i-1] in ['find', 'find:re', 'until', 'until:re']:
                # Make pattern very long
                mutated_ops[i] = 'A' * random.randint(5000, 50000)
                # Make input very short
                mutated_input = b'x' * random.randint(1, 100)
                break

    elif mismatch_type == 2:
        # EXTREME QUANTIFIERS + SMALL INPUT
        for i in range(1, len(mutated_ops)):
            if mutated_ops[i-1] == 'find:re':
                # Borderline quantifiers (not caught by validation, but expensive)
                patterns = [
                    '.{50,100}',   # Expects 50+ chars
                    'a{40,80}',    # Expects 40+ 'a's
                    '(..){30,50}', # Expects 60+ chars
                    '\\w{70,99}',  # At limit
                    '(.|\n){80,100}', # Multiline
                ]
                mutated_ops[i] = random.choice(patterns)
                # Make input smaller than min quantifier
                mutated_input = b'a' * random.randint(1, 30)
                break

    elif mismatch_type == 3:
        # MANY ALTERNATIONS (BUG-3 type: stack overflow)
        for i in range(1, len(mutated_ops)):
            if mutated_ops[i-1] == 'find:re':
                # Generate many alternations (at the limit)
                num_alts = random.randint(200, 256)  # Near MAX_ALTS
                mutated_ops[i] = '|'.join('a' for _ in range(num_alts))
                break

    elif mismatch_type == 4:
        # UNDEFINED LABEL REFERENCES
        # Insert reference to label that doesn't exist
        fake_label = f"label_{random.randint(1000, 9999)}"
        mutated_ops.insert(0, 'view')
        mutated_ops.insert(1, fake_label)
        mutated_ops.insert(2, fake_label)

    elif mismatch_type == 5:
        # KEYWORDS AS ARGUMENTS (op counting mismatch bug - ccf0759)
        # Replace operation arguments with clause keywords
        for i in range(1, len(mutated_ops)):
            if mutated_ops[i-1] in ['find', 'find:re', 'find:bin', 'skip', 'take', 'print', 'fail']:
                # Replace the argument with a keyword
                mutated_ops[i] = random.choice(['THEN', 'OR', 'AND'])
                break

    return mutated_ops, mutated_input

# ========= Corpus loading =========

_corpus: list[bytes] = []

def load_corpus(corpus_dir: Path) -> int:
    """Load corpus files from directory"""
    global _corpus
    _corpus = []
    max_size = 512 * 1024  # Skip files > 512KB

    if not corpus_dir.exists():
        return 0

    for path in corpus_dir.rglob('*'):
        if path.is_file():
            try:
                size = path.stat().st_size
                if 0 < size <= max_size:
                    _corpus.append(path.read_bytes())
            except Exception:
                pass

    return len(_corpus)

# ========= Input data mutations (8 strategies) =========

def mutate_bit_flip(data: bytes) -> bytes:
    """Flip a single bit"""
    if not data:
        return data
    pos = random.randrange(len(data))
    bit_pos = random.randrange(8)
    byte_arr = bytearray(data)
    byte_arr[pos] ^= (1 << bit_pos)
    return bytes(byte_arr)

def mutate_byte_flip(data: bytes) -> bytes:
    """Replace a random byte"""
    if not data:
        return data
    pos = random.randrange(len(data))
    byte_arr = bytearray(data)
    byte_arr[pos] = random.randrange(256)
    return bytes(byte_arr)

def mutate_byte_insert(data: bytes) -> bytes:
    """Insert a random byte"""
    pos = random.randint(0, len(data))
    byte_val = bytes([random.randrange(256)])
    return data[:pos] + byte_val + data[pos:]

def mutate_byte_delete(data: bytes) -> bytes:
    """Delete a random byte"""
    if len(data) <= 1:
        return data
    pos = random.randrange(len(data))
    return data[:pos] + data[pos+1:]

def mutate_chunk_insert(data: bytes) -> bytes:
    """Insert a random chunk"""
    chunk_size = random.randint(1, 17)
    chunk = bytes(random.randrange(256) for _ in range(chunk_size))
    pos = random.randint(0, len(data))
    return data[:pos] + chunk + data[pos:]

def mutate_chunk_delete(data: bytes) -> bytes:
    """Delete a random chunk"""
    if len(data) <= 1:
        return data
    chunk_size = min(random.randint(1, 33), len(data))
    pos = random.randint(0, len(data) - chunk_size)
    return data[:pos] + data[pos+chunk_size:]

def mutate_splice(data: bytes) -> bytes:
    """Splice with another corpus file"""
    if not _corpus or len(_corpus) < 2:
        return data
    other = random.choice(_corpus)
    split1 = random.randint(0, len(data))
    split2 = random.randint(0, len(other))
    return data[:split1] + other[split2:]

def mutate_repeat(data: bytes) -> bytes:
    """Repeat a chunk multiple times"""
    if not data:
        return data
    chunk_size = min(random.randint(1, 17), len(data))
    pos = random.randint(0, len(data) - chunk_size)
    chunk = data[pos:pos+chunk_size]
    count = random.randint(1, 5)
    insert_pos = random.randint(0, len(data))
    return data[:insert_pos] + chunk * count + data[insert_pos:]

_mutations = [
    mutate_bit_flip,
    mutate_byte_flip,
    mutate_byte_insert,
    mutate_byte_delete,
    mutate_chunk_insert,
    mutate_chunk_delete,
    mutate_splice,
    mutate_repeat,
]

def apply_mutations(data: bytes, count: int) -> bytes:
    """Apply multiple mutations to input data"""
    for _ in range(count):
        mutator = random.choice(_mutations)
        data = mutator(data)
    return data

# ========= Input generator =========

def gen_input_from_corpus() -> bytes:
    """Generate input by mutating corpus file"""
    if not _corpus:
        return b''
    base = random.choice(_corpus)
    mutation_count = random.randint(1, 6)
    return apply_mutations(base, mutation_count)

def gen_input_random() -> bytes:
    """Generate random input data"""
    total = random.randint(64, 4160)
    result = []

    ascii_lines = [
        b'Starting text\n', b'Middle line\n', b'Ending line\n',
        b'ERROR: something failed\n', b'WARNING: disk almost full\n',
        b'user=john id=12345\n', b'[database]\nhost=localhost\nport=5432\n',
        b'BEGIN data ----\n', b'END data ----\n',
        b'READY STATE\n', b'STATE=READY\n',
    ]
    magics = [b'\x89PNG\r\n\x1a\n', b'\xFF\xD8\xFF', b'PK\x03\x04']

    length = 0
    while length < total:
        pick = random.randrange(11)
        if pick <= 5:
            s = random.choice(ascii_lines)
            result.append(s)
            length += len(s)
        elif pick == 6 and length + 8 < total:
            m = random.choice(magics)
            result.append(m)
            length += len(m)
        else:
            result.append(bytes([random.randrange(256)]))
            length += 1

    return b''.join(result)

def gen_input(use_corpus: bool) -> bytes:
    """Generate input data (corpus or random)"""
    if use_corpus and _corpus and random.random() < CORPUS_MUTATION_PCT:
        return gen_input_from_corpus()
    return gen_input_random()

# ========= Executor =========

def run_fiskta(ops_tokens: list[str], input_path: Path, timeout_ms: int,
               fiskta_path: Path) -> TestResult:
    """Execute fiskta with given operations and input"""
    cmd = [str(fiskta_path), '--input', str(input_path), '--'] + ops_tokens

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            timeout=timeout_ms / 1000.0,
        )
        exit_code = result.returncode
        # Negative exit codes indicate signal termination
        if exit_code < 0:
            return TestResult(crashed=True, timed_out=False,
                            exit_code=128 + abs(exit_code), signal_num=abs(exit_code))
        return TestResult(crashed=False, timed_out=False, exit_code=exit_code)

    except subprocess.TimeoutExpired:
        return TestResult(crashed=False, timed_out=True, exit_code=-2)

# ========= Minimizer =========

def same_failure(a: TestResult, b: TestResult) -> bool:
    """Check if two results represent the same failure"""
    if a.timed_out and b.timed_out:
        return True
    if a.crashed and b.crashed:
        return True
    return a.exit_code == b.exit_code

def minimize_tokens(orig_tokens: list[str], input_path: Path,
                   want_res: TestResult, timeout_ms: int,
                   fiskta_path: Path) -> list[str]:
    """Minimize crash case by removing tokens"""
    best = orig_tokens.copy()

    improved = True
    while improved:
        improved = False
        i = 0
        while i < len(best):
            if len(best) <= 1:
                break

            # Try removing token i
            trial = best[:i] + best[i+1:]
            res = run_fiskta(trial, input_path, timeout_ms, fiskta_path)

            if same_failure(want_res, res):
                best = trial
                improved = True
                i = 0  # Restart from beginning
            else:
                i += 1

    return best

# ========= File operations =========

def save_case(case_id: int, ops_tokens: list[str], input_data: bytes,
              res: TestResult, run_dir: Path):
    """Save test case to disk"""
    run_dir.mkdir(parents=True, exist_ok=True)

    base = run_dir / f"case_{case_id}"
    base.with_suffix('.ops.txt').write_text('\n'.join(ops_tokens))
    base.with_suffix('.input.bin').write_bytes(input_data)

    meta = f"exit={res.exit_code}\nsignal={res.signal_num}\ntimed_out={res.timed_out}\n"
    base.with_suffix('.meta.txt').write_text(meta)

# ========= Worker function =========

def worker_fn(args: tuple) -> dict:
    """Worker function that runs in subprocess via multiprocessing"""
    worker_id, num_workers, cfg_dict = args

    # Reconstruct config from dict
    cfg = Config(**cfg_dict)

    # Worker-specific seed
    random.seed(cfg.seed + worker_id)

    # Load corpus in worker
    if cfg.use_corpus:
        load_corpus(cfg.corpus_dir)

    stats = {'total': 0, 'saved': 0, 'timeouts': 0, 'crashed': 0, 'exits': {}}

    def write_worker_stats():
        """Write current stats to file for coordinator to read (atomic)"""
        stats_path = cfg.run_dir / f"worker_{worker_id}_stats.txt"
        temp_path = cfg.run_dir / f"worker_{worker_id}_stats.tmp"
        lines = [
            f"total={stats['total']}",
            f"saved={stats['saved']}",
            f"timeouts={stats['timeouts']}",
            f"crashed={stats['crashed']}",
        ]
        for code, count in stats['exits'].items():
            lines.append(f"exit[{code}]={count}")
        # Atomic write: write to temp, then rename
        temp_path.write_text('\n'.join(lines) + '\n')
        temp_path.rename(stats_path)

    case_iter = 0
    while True:
        case_id = worker_id + case_iter * num_workers
        if cfg.cases and case_id >= cfg.cases:
            break

        # Generate operations using configured distribution
        roll = random.random()
        if roll < RANDOM_GENERATION_PCT:
            ops = gen_random_program(cfg.min_ops, cfg.max_ops)
            input_data = gen_input(cfg.use_corpus)
        elif roll < RANDOM_GENERATION_PCT + MUTATED_PROGRAMS_PCT:
            ops = mutate_program(gen_random_program(cfg.min_ops, cfg.max_ops))
            input_data = gen_input(cfg.use_corpus)
        else:
            # Targeted mismatch mutations (pattern/input size mismatches)
            ops = gen_random_program(cfg.min_ops, cfg.max_ops)
            input_data = gen_input(cfg.use_corpus)
            ops, input_data = mutate_for_input_mismatch(ops, input_data)

        # Write temp input file
        tmp_path = cfg.run_dir / f"tmp_{worker_id}_{case_id}.bin"
        tmp_path.write_bytes(input_data)

        # Run fiskta
        res = run_fiskta(ops, tmp_path, cfg.timeout_ms, cfg.fiskta_path)

        # Update stats
        stats['total'] += 1
        stats['exits'][res.exit_code] = stats['exits'].get(res.exit_code, 0) + 1
        if res.timed_out:
            stats['timeouts'] += 1
        if res.crashed or res.exit_code in [10, 11, 14]:
            stats['crashed'] += 1

        # Check if interesting
        interesting = (res.timed_out or res.crashed or
                      res.exit_code in [2, 10, 11, 14] or cfg.save_all)

        if interesting:
            final_ops = ops
            # Minimize crashes (but not parse errors - exit code 12)
            if cfg.minimize and res.exit_code != 12:
                final_ops = minimize_tokens(ops, tmp_path, res, cfg.timeout_ms, cfg.fiskta_path)

            save_case(case_id, final_ops, input_data, res, cfg.run_dir)
            stats['saved'] += 1

        # Cleanup temp file
        tmp_path.unlink(missing_ok=True)

        # Write stats after each case
        write_worker_stats()

        case_iter += 1

    return stats

# ========= Coordinator =========

def format_time(seconds: int) -> str:
    """Format seconds as human-readable time"""
    if seconds < 60:
        return f"{seconds}s"
    if seconds < 3600:
        return f"{seconds//60}m{seconds%60}s"
    return f"{seconds//3600}h{(seconds%3600)//60}m"

def run_fuzzer(cfg: Config):
    """Main fuzzer coordinator with multiprocessing"""
    cfg.run_dir.mkdir(parents=True, exist_ok=True)

    print(f"Run directory: {cfg.run_dir}\n")
    print(f"fiskta fuzzer [{'ASAN' if cfg.is_asan else 'release'} | "
          f"seed={cfg.seed} | minimize={'on' if cfg.minimize else 'off'}]")
    print(f"Binary: {cfg.fiskta_path}")

    if cfg.cases:
        print(f"Mode: {cfg.cases} cases ({cfg.workers} workers)")
    else:
        print(f"Mode: continuous (Ctrl+C to stop, {cfg.workers} workers)")
    print()

    # Load corpus
    corpus_count = load_corpus(cfg.corpus_dir) if cfg.use_corpus else 0

    start_time = time.time()

    # Convert config to dict for pickling (Path -> str)
    cfg_dict = {
        'fiskta_path': Path(cfg.fiskta_path),
        'artifacts': Path(cfg.artifacts),
        'run_dir': Path(cfg.run_dir),
        'cases': cfg.cases,
        'seed': cfg.seed,
        'timeout_ms': cfg.timeout_ms,
        'minimize': cfg.minimize,
        'use_corpus': cfg.use_corpus,
        'corpus_dir': Path(cfg.corpus_dir),
        'workers': cfg.workers,
        'repro_case': cfg.repro_case,
        'repro_input': Path(cfg.repro_input) if cfg.repro_input else None,
        'repro_ops': Path(cfg.repro_ops) if cfg.repro_ops else None,
        'is_asan': cfg.is_asan,
        'min_ops': cfg.min_ops,
        'max_ops': cfg.max_ops,
        'save_all': cfg.save_all,
    }

    # Prepare worker arguments
    worker_args = [(i, cfg.workers, cfg_dict) for i in range(cfg.workers)]

    # Run workers in parallel
    pool = None
    worker_stats = None
    interrupted = False

    try:
        print(f"Spawned {cfg.workers} workers (Press Ctrl+C once to stop gracefully)...\n")

        pool = Pool(cfg.workers)
        # Use map_async to allow KeyboardInterrupt
        result = pool.map_async(worker_fn, worker_args)

        # Poll for completion with live stats display
        last_agg = {'total': 0, 'saved': 0, 'timeouts': 0, 'crashed': 0}
        while True:
            try:
                worker_stats = result.get(timeout=1)
                break
            except MPTimeoutError:
                # Still running, aggregate and show current stats
                agg = {'total': 0, 'saved': 0, 'timeouts': 0, 'crashed': 0}
                for i in range(cfg.workers):
                    stats_path = cfg.run_dir / f"worker_{i}_stats.txt"
                    if stats_path.exists():
                        try:
                            for line in stats_path.read_text().strip().split('\n'):
                                if '=' in line:
                                    key, val = line.split('=', 1)
                                    if key in agg:
                                        agg[key] = agg.get(key, 0) + int(val)
                        except:
                            pass

                # Only update display if stats increased (prevent backwards jumps)
                if agg['total'] >= last_agg['total']:
                    last_agg = agg
                else:
                    agg = last_agg

                elapsed = int(time.time() - start_time)
                rate = agg['total'] // elapsed if elapsed > 0 else 0
                if cfg.cases:
                    mode_str = f"{agg['total']}/{cfg.cases}"
                else:
                    mode_str = str(agg['total'])
                print(f"\r[{mode_str} cases | {rate} exec/s | "
                      f"{agg['crashed']} crashes | {agg['timeouts']} timeouts | "
                      f"{format_time(elapsed)}]", end='', flush=True)

        print()  # Clear progress line
        pool.close()
        pool.join()

    except KeyboardInterrupt:
        print("\n\nReceived Ctrl+C, shutting down workers gracefully...")
        interrupted = True
        if pool:
            pool.terminate()
            pool.join()
    finally:
        if pool:
            pool.close()

    # Aggregate stats from worker results or worker stats files
    agg_stats = Stats()

    if worker_stats:
        # Normal completion - use returned stats
        for stats in worker_stats:
            agg_stats.total += stats['total']
            agg_stats.saved += stats['saved']
            agg_stats.timeouts += stats['timeouts']
            agg_stats.crashed += stats['crashed']
            for code, count in stats['exits'].items():
                agg_stats.exits[code] = agg_stats.exits.get(code, 0) + count
    else:
        # Interrupted - read from worker stats files
        for i in range(cfg.workers):
            stats_path = cfg.run_dir / f"worker_{i}_stats.txt"
            if stats_path.exists():
                try:
                    for line in stats_path.read_text().strip().split('\n'):
                        if '=' in line:
                            key, val = line.split('=', 1)
                            if key == 'total':
                                agg_stats.total += int(val)
                            elif key == 'saved':
                                agg_stats.saved += int(val)
                            elif key == 'timeouts':
                                agg_stats.timeouts += int(val)
                            elif key == 'crashed':
                                agg_stats.crashed += int(val)
                            elif key.startswith('exit['):
                                # Parse exit[N]=count
                                code = int(key[5:-1])
                                agg_stats.exits[code] = agg_stats.exits.get(code, 0) + int(val)
                except:
                    pass

    # Clean up worker stats files
    for i in range(cfg.workers):
        stats_path = cfg.run_dir / f"worker_{i}_stats.txt"
        stats_path.unlink(missing_ok=True)

    # Write run summary
    summary_lines = [
        f"total={agg_stats.total}",
        f"saved={agg_stats.saved}",
        f"timeouts={agg_stats.timeouts}",
        f"crashed={agg_stats.crashed}",
    ]
    for code, count in sorted(agg_stats.exits.items()):
        summary_lines.append(f"exit[{code}]={count}")
    (cfg.run_dir / "run_summary.txt").write_text('\n'.join(summary_lines) + '\n')

    elapsed = int(time.time() - start_time)
    rate = agg_stats.total // elapsed if elapsed > 0 else 0

    # Print summary
    if interrupted:
        print("\n=== Run Summary (interrupted) ===")
    else:
        print("\n=== Run Summary ===")

    print(f"Total cases:    {agg_stats.total}")
    print(f"Execution rate: {rate} exec/s")
    print(f"Time elapsed:   {format_time(elapsed)}")
    print(f"Crashes:        {agg_stats.crashed}")
    print(f"Timeouts:       {agg_stats.timeouts}")
    print(f"Saved cases:    {agg_stats.saved}")

    if agg_stats.exits:
        print("\nExit code distribution:")
        for code, count in sorted(agg_stats.exits.items()):
            print(f"  exit {code}: {count}")

    print(f"\nRun directory: {cfg.run_dir}")
    if agg_stats.saved > 0:
        print("Reproduce: ./fuzz.py --repro-case N")
    print()

# ========= Repro modes =========

def repro_case(cfg: Config, case_num: int):
    """Reproduce a saved crash case"""
    # Search for case in run directories (newest first)
    case_path = None

    if cfg.run_dir and cfg.run_dir != cfg.artifacts:
        base = cfg.run_dir / f"case_{case_num}"
        if base.with_suffix('.ops.txt').exists():
            case_path = base

    if not case_path:
        # Search all run directories
        run_dirs = sorted(cfg.artifacts.glob('run_*'), reverse=True)
        for run_dir in run_dirs:
            base = run_dir / f"case_{case_num}"
            if base.with_suffix('.ops.txt').exists():
                case_path = base
                break

    if not case_path:
        print(f"Error: Case {case_num} not found in any run directory", file=sys.stderr)
        sys.exit(1)

    ops = case_path.with_suffix('.ops.txt').read_text().strip().split('\n')
    input_path = case_path.with_suffix('.input.bin')

    print(f"Reproducing case {case_num} from {case_path.parent}")
    res = run_fiskta(ops, input_path, cfg.timeout_ms, cfg.fiskta_path)
    print(f"exit={res.exit_code} signal={res.signal_num} timed_out={res.timed_out}")

def repro_paths(cfg: Config, input_path: Path, ops_path: Path):
    """Reproduce with specific input and ops files"""
    ops = ops_path.read_text().strip().split('\n')
    print(f"Reproducing with {input_path} and {ops_path}")
    res = run_fiskta(ops, input_path, cfg.timeout_ms, cfg.fiskta_path)
    print(f"exit={res.exit_code} signal={res.signal_num} timed_out={res.timed_out}")

# ========= Main entry point =========

def main():
    """Main entry point"""
    cfg = parse_args()

    if cfg.repro_case is not None:
        repro_case(cfg, cfg.repro_case)
    elif cfg.repro_input and cfg.repro_ops:
        repro_paths(cfg, cfg.repro_input, cfg.repro_ops)
    else:
        run_fuzzer(cfg)

if __name__ == "__main__":
    main()

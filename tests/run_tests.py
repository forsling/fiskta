#!/usr/bin/env python3
# Standard library only: subprocess, hashlib, json, argparse, pathlib, sys, os, textwrap
import subprocess, sys, os, hashlib, argparse, json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIX = ROOT / "tests" / "fixtures"

def write(path: Path, data: bytes):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)

def make_fixtures():
    # 1) small.txt
    small = b"Header\nbody 1\nbody 2\nERROR hit\ntail\nERROR 2\nfoo\nHEADER\nbar\n"
    write(FIX / "small.txt", small)

    # 2) overlap.txt (no trailing LF)
    write(FIX / "overlap.txt", b"abcdefghij")

    # 3) lines.txt
    lines = b"".join([
        b"L01 a\n", b"L02 bb\n", b"L03 ccc\n", b"L04 dddd\n", b"L05 eeeee\n",
        b"L06 ffffff\n", b"L07 ggggggg\n", b"L08 hhhhhhhh\n",
        b"L09 iiiiiiiii\n", b"L10 jjjjjjjjjj\n",
    ])
    write(FIX / "lines.txt", lines)

    # 4) crlf.txt (no LF at end)
    write(FIX / "crlf.txt", b"A\r\nB\r\nC")

    # 5) big-forward.bin (20 MiB of 'A' with 'NEEDLE' at a deep offset)
    big = bytearray(b"A" * (20 * 1024 * 1024))
    ins_off = 12 * 1024 * 1024 + 123 + 890
    big[ins_off:ins_off+6] = b"NEEDLE"
    write(FIX / "big-forward.bin", big)

    # 6) longline-left.bin (single long line, no LF)
    lleft = bytearray(b"A" * (12 * 1024 * 1024))
    lleft[10_000_000:10_000_006] = b"NEEDLE"
    write(FIX / "longline-left.bin", lleft)

    # 7) longline-right.bin: 32 'B', then 12 MiB of 'C', then LF, then 'TAIL\n'
    lright = bytearray(b"B" * 32 + b"C" * (12 * 1024 * 1024) + b"\nTAIL\n")
    write(FIX / "longline-right.bin", lright)

    # 8) labels-evict.txt
    write(FIX / "labels-evict.txt", b"0123456789abcdefghijklmnopqrstuvwxyz")

def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def run(exe: Path, tokens, in_path, stdin_data: bytes | None):
    # Build argv: fiskta <tokens...> <input>
    argv = [str(exe), *tokens, in_path]
    try:
        proc = subprocess.run(argv, input=stdin_data, capture_output=True)
        return proc.returncode, proc.stdout, proc.stderr
    except FileNotFoundError:
        print(f"ERROR: executable not found: {exe}", file=sys.stderr)
        sys.exit(2)

def expect_stdout(actual: bytes, expect: dict) -> tuple[bool, str]:
    if "stdout" in expect:
        want = expect["stdout"].encode("utf-8")
        ok = actual == want
        return ok, "" if ok else f"stdout mismatch\n---want({len(want)}B)\n{want!r}\n---got({len(actual)}B)\n{actual!r}"
    if "stdout_len" in expect:
        want_len = int(expect["stdout_len"])
        ok = len(actual) == want_len
        return ok, "" if ok else f"stdout_len mismatch want {want_len}, got {len(actual)}"
    if "stdout_sha256" in expect:
        want = expect["stdout_sha256"].lower()
        got = sha256(actual)
        ok = got == want
        return ok, "" if ok else f"sha256 mismatch want {want}, got {got}"
    # default: expect empty
    ok = len(actual) == 0
    return ok, "" if ok else f"expected empty stdout, got {len(actual)}B"

def tests():
    # NOTE: Using '::' as the clause separator per your decision.
    # Each test: id, tokens (without input path), in, stdin (optional), expect {stdout|stdout_len|stdout_sha256, exit}
    return [
        # ---------- Grammar & parsing ----------
        dict(id="gram-001-clause-sep",
             tokens=["take","+3b","::","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="abcde", exit=0)),

        dict(id="gram-002-no-signed-skip",
             tokens=["skip","-5b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),

        dict(id="gram-003-empty-needle-invalid",
             tokens=["find",""], input_file="small.txt",
             expect=dict(stdout="", exit=2)),

        dict(id="gram-004-label-name-validation",
             tokens=["label","Bad"], input_file="small.txt",
             expect=dict(stdout="", exit=2)),

        # ---------- Clause atomicity & staging ----------
        dict(id="atom-001-discard-within-clause",
             tokens=["take","+3b","find","NOPE"], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),  # whole clause fails, nothing emits

        dict(id="atom-002-independent-clauses",
             tokens=["take","+3b","::","find","NOPE","::","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="abcde", exit=0)),

        dict(id="atom-003-staging-order-overlap",
             tokens=["take","+5b","take","-2b"], input_file="overlap.txt",
             expect=dict(stdout="abcde", exit=0)),  # emits [0,5) then [3,5)

        # ---------- Cursor & Locality ----------
        dict(id="cur-001-take-moves-to-far-end",
             tokens=["take","+3b","::","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="abcde", exit=0)),

        dict(id="cur-002-empty-capture-no-move",
             tokens=["take","+0b","::","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="cur-003-take-to-cursor-empty",
             tokens=["take","to","cursor","::","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="cur-004-loc-expr-uses-staged-cursor",
             tokens=["skip","2b","take","to","cursor+3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),

        # ---------- Lines semantics ----------
        dict(id="line-001-forward-lines",
             tokens=["find","bb","take","+2l"], input_file="lines.txt",
             expect=dict(stdout="L02 bb\nL03 ccc\n", exit=0)),

        dict(id="line-002-backward-lines",
             tokens=["find","dddd","take","-2l"], input_file="lines.txt",
             expect=dict(stdout="L02 bb\nL03 ccc\n", exit=0)),

        dict(id="line-003-line-offsets-in-loc-expr",
             tokens=["find","ccc","goto","line-start","take","to","line-start+2l"], input_file="lines.txt",
             expect=dict(stdout="L03 ccc\nL04 dddd\n", exit=0)),

        dict(id="line-004-cr-is-just-a-byte",
             tokens=["take","+1l"], input_file="crlf.txt",
             expect=dict(stdout="A\r\n", exit=0)),

        dict(id="line-005-line-start-unbounded-backscan",
             tokens=["find","NEEDLE","goto","line-start","take","+3b"], input_file="longline-left.bin",
             expect=dict(stdout="AAA", exit=0)),

        dict(id="line-006-line-end-unbounded-forwardscan",
             tokens=["find","B","take","until","C","at","line-end"], input_file="longline-right.bin",
             # Expect many bytes; just assert the last 5 bytes are 'TAIL\n' by taking until line-end then ensure the total ends with LF before TAIL won't be included (we captured up to LF).
             # To keep it strict but small, assert only the length is > 12MB - 1 and < 13MB.
             expect=dict(stdout_len=32 + 12*1024*1024 + 1, exit=0)),  # from 'B'*32 + 'C'*12MiB up to and including LF

        # ---------- find semantics ----------
        dict(id="find-001-forward-first-match",
             tokens=["find","ERROR","take","+10b"], input_file="small.txt",
             expect=dict(stdout="ERROR hit\n", exit=0)),

        dict(id="find-002-backward-rightmost",
             tokens=["skip","100b","find","to","BOF","ERROR","take","+7b"], input_file="small.txt",
             expect=dict(stdout="ERROR 2", exit=0)),

        dict(id="find-003-forward-window-spans-buffers",
             tokens=["find","NEEDLE","take","+6b"], input_file="big-forward.bin",
             expect=dict(stdout="NEEDLE", exit=0)),

        dict(id="find-004-miss-fails-clause",
             tokens=["find","NOPE","take","+3b"], input_file="small.txt",
             expect=dict(stdout="", exit=2)),

        # ---------- take to ----------
        dict(id="take-001-order-normalized",
             tokens=["label","HERE","skip","+5b","take","to","HERE","take","+5b"], input_file="overlap.txt",
             expect=dict(stdout="abcde", exit=0)),

        dict(id="take-002-to-invalid-fails",
             tokens=["take","to","MISSING"], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),

        # ---------- take until ----------
        dict(id="until-001-default-at-match-start",
             tokens=["take","until","ERROR","::","take","+5b"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\n", exit=0)),

        dict(id="until-002-at-line-start",
             tokens=["take","until","ERROR","at","line-start"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\n", exit=0)),

        dict(id="until-003-at-match-end",
             tokens=["take","until","ERROR","at","match-end"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\nERROR", exit=0)),

        dict(id="until-004-miss-fails",
             tokens=["take","until","ZZZ"], input_file="small.txt",
             expect=dict(stdout="", exit=2)),

        # ---------- Labels & LRU ----------
        dict(id="lab-001-basic-label-goto",
             tokens=["label","A","skip","+3b","take","to","A"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="lab-002-unknown-label-fails",
             tokens=["goto","NOPE"], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),

        dict(id="lab-003-evict-lru-on-33rd",
             tokens=[
                # 32 labels in 32 clauses at pos 0
                "label","A01","::","label","A02","::","label","A03","::","label","A04","::",
                "label","A05","::","label","A06","::","label","A07","::","label","A08","::",
                "label","A09","::","label","A10","::","label","A11","::","label","A12","::",
                "label","A13","::","label","A14","::","label","A15","::","label","A16","::",
                "label","A17","::","label","A18","::","label","A19","::","label","A20","::",
                "label","A21","::","label","A22","::","label","A23","::","label","A24","::",
                "label","A25","::","label","A26","::","label","A27","::","label","A28","::",
                "label","A29","::","label","A30","::","label","A31","::","label","A32","::",
                # add A33 (evict LRU A01), then try goto A01
                "label","A33","goto","A01","take","+1b"
             ], input_file="labels-evict.txt",
             expect=dict(stdout="", exit=2)),

        # ---------- Bounds & clamps ----------
        dict(id="clamp-001-skip-clamps",
             tokens=["goto","EOF","skip","100b","take","to","BOF"], input_file="overlap.txt",
             expect=dict(stdout="abcdefghij", exit=0)),

        dict(id="clamp-002-loc-expr-offset-clamps",
             tokens=["take","to","EOF+100b"], input_file="overlap.txt",
             expect=dict(stdout="abcdefghij", exit=0)),

        # ---------- Stdin & spooling ----------
        dict(id="io-001-stdin-forward",
             tokens=["take","+5b"], input_file="-", stdin=b"Hello\nWorld\n",
             expect=dict(stdout="Hello", exit=0)),

        dict(id="io-002-stdin-backward-search",
             tokens=["find","to","BOF","World","take","+5b"], input_file="-", stdin=b"Hello\nWorld\n",
             expect=dict(stdout="World", exit=0)),

        # ---------- last_match requirements ----------
        dict(id="match-001-atloc-requires-valid",
             tokens=["goto","match-start"], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),
    ]

def main():
    ap = argparse.ArgumentParser(description="Run fiskta v2 test suite")
    ap.add_argument("--exe", default=str(ROOT / "fiskta"), help="Path to fiskta executable")
    ap.add_argument("--filter", default="", help="Substring to filter test IDs")
    ap.add_argument("--list", action="store_true", help="List tests and exit")
    ap.add_argument("--no-fixtures", action="store_true", help="Do not regenerate fixtures")
    args = ap.parse_args()

    exe = Path(args.exe)
    if not args.no_fixtures:
        make_fixtures()

    all_tests = tests()
    if args.filter:
        all_tests = [t for t in all_tests if args.filter in t["id"]]

    if args.list:
        for t in all_tests:
            print(t["id"])
        return 0

    failures = 0
    passed = 0

    for t in all_tests:
        tid = t["id"]
        tokens = t["tokens"]
        in_name = t["input_file"]
        stdin_data = t.get("stdin", None)

        in_path = "-" if in_name == "-" else str(FIX / in_name)
        code, out, err = run(exe, tokens, in_path, stdin_data)
        ok_stdout, why = expect_stdout(out, t["expect"])
        ok_exit = (code == t["expect"]["exit"])

        if ok_stdout and ok_exit:
            print(f"[PASS] {tid}")
            passed += 1
        else:
            print(f"[FAIL] {tid}")
            if not ok_exit:
                print(f"  exit: want {t['expect']['exit']}, got {code}")
            if not ok_stdout:
                print(f"  {why}")
            if err:
                print(f"  stderr: {err.decode('utf-8', 'ignore').strip()}")
            failures += 1

    total = passed + failures
    print(f"\nSummary: {passed}/{total} passed, {failures} failed")
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main())


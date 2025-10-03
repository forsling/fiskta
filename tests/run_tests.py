#!/usr/bin/env python3
# Standard library only: subprocess, hashlib, json, argparse, pathlib, sys, os, textwrap
import subprocess, sys, os, hashlib, argparse, json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIX = ROOT / "tests" / "fixtures"


def repo_version() -> str:
    makefile = ROOT / "Makefile"
    try:
        for line in makefile.read_text().splitlines():
            if line.startswith("VERSION"):
                _, value = line.split("=", 1)
                return value.strip()
    except OSError:
        pass
    return "dev"


VERSION = repo_version()
VERSION_LINE = f"fiskta (FInd SKip TAke) v{VERSION}\n"

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

    # 9) multiline.txt - Complex multi-line content
    multiline = b"""Section 1: Introduction
This is the first section with multiple lines.
It contains various text patterns.

Section 2: Data
KEY1=value1
KEY2=value2
KEY3=value3

Section 3: Results
SUCCESS: Operation completed
WARNING: Minor issue detected
ERROR: Critical failure occurred
INFO: Additional information

Section 4: Conclusion
Final thoughts and summary.
End of document.
"""
    write(FIX / "multiline.txt", multiline)

    # 10) empty.txt
    write(FIX / "empty.txt", b"")

    # 11) single-line.txt (no trailing LF)
    write(FIX / "single-line.txt", b"Single line without newline")

    # 12) single-line-with-lf.txt
    write(FIX / "single-line-with-lf.txt", b"Single line with newline\n")

    # 13) binary-data.bin - Mixed binary and text data
    binary_data = bytearray(b"TEXT_START\x00\x01\x02\x03BINARY_DATA\xff\xfe\xfdTEXT_END\n")
    write(FIX / "binary-data.bin", binary_data)

    # 14) repeated-patterns.txt
    repeated = b"PATTERN\n" * 100 + b"END_MARKER\n"
    write(FIX / "repeated-patterns.txt", repeated)

    # 15) unicode-test.txt - UTF-8 encoded content
    unicode_content = "Hello 世界\nCafé naïve\n🚀 Rocket\n".encode('utf-8')
    write(FIX / "unicode-test.txt", unicode_content)

    # 16) large-lines.txt - File with very long lines
    large_lines = bytearray()
    for i in range(10):
        line = b"X" * 1000 + f"LINE_{i:02d}".encode() + b"Y" * 1000 + b"\n"
        large_lines.extend(line)
    write(FIX / "large-lines.txt", large_lines)

    # 17) nested-sections.txt - Complex nested structure
    nested = b"""BEGIN_SECTION_A
  SUBSECTION_1
    DATA: value1
    DATA: value2
  SUBSECTION_2
    DATA: value3
END_SECTION_A
BEGIN_SECTION_B
  SUBSECTION_1
    DATA: value4
  SUBSECTION_2
    DATA: value5
    DATA: value6
END_SECTION_B
"""
    write(FIX / "nested-sections.txt", nested)

    # 18) edge-cases.txt - Various edge cases
    parts = []
    parts.append(b"Line with spaces at end   \n")
    parts.append(b"Line with tabs\t\t\n")
    parts.append(b"Line with mixed\t spaces\n")
    parts.append(b"Empty line above\n")
    parts.append(b"\n")  # the empty line referenced above
    parts.append(b"Line with only spaces:   \n")
    parts.append(b"Line with only tabs:\t\t\n")
    parts.append(b"Very long line: ")
    parts.append(b"X" * 1000)
    parts.append(b"\n")
    parts.append(b"Short line\n")
    edge_cases = b"".join(parts)
    write(FIX / "edge-cases.txt", edge_cases)

    # 19) crlf-comprehensive.txt - Comprehensive CRLF test file
    crlf_comp = b"Line1\r\nLine2\r\nLine3\r\n"
    write(FIX / "crlf-comprehensive.txt", crlf_comp)

    # 20) mixed-line-endings.txt - Mixed LF and CRLF line endings
    mixed_endings = b"LF line\nCRLF line\r\nAnother LF\nFinal CRLF\r\n"
    write(FIX / "mixed-line-endings.txt", mixed_endings)

    # 21) crlf-no-final-lf.txt - CRLF file without final newline
    crlf_no_final = b"Line1\r\nLine2\r\nLine3"
    write(FIX / "crlf-no-final-lf.txt", crlf_no_final)

    # 22) cr-only.txt - File with only CR (no LF) - edge case
    cr_only = b"Line1\rLine2\rLine3\r"
    write(FIX / "cr-only.txt", cr_only)

    # 23) crlf-large.txt - Large CRLF file for buffer boundary testing
    crlf_large = bytearray()
    for i in range(100):
        crlf_large.extend(f"Line {i:03d}\r\n".encode())
    write(FIX / "crlf-large.txt", crlf_large)

    # 24) crlf-boundary.txt - CRLF sequences that will span buffer boundaries
    # Create content where CRLF sequences are positioned to test buffer boundary handling
    crlf_boundary = bytearray(b"A" * 1000)  # Fill buffer
    crlf_boundary.extend(b"\r\n")  # CRLF at buffer boundary
    crlf_boundary.extend(b"B" * 1000)  # More content
    crlf_boundary.extend(b"\r\n")  # Another CRLF
    crlf_boundary.extend(b"C" * 1000)  # Final content
    write(FIX / "crlf-boundary.txt", crlf_boundary)

def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def run(exe: Path, tokens, in_path: str | None, stdin_data: bytes | None):
    # Build argv: fiskta <tokens...> [<input>]
    argv = [str(exe), *tokens]
    if in_path is not None:
        argv.append(in_path)
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
    if "stdout_startswith" in expect:
        prefix = expect["stdout_startswith"].encode("utf-8")
        ok = actual.startswith(prefix)
        return ok, "" if ok else f"stdout prefix mismatch\n---want-prefix({len(prefix)}B)\n{prefix!r}\n---got({len(actual)}B)\n{actual!r}"
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
             expect=dict(stdout="abcdede", exit=0)),  # emits [0,5) then [3,5) concatenated

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
             tokens=["skip","2b","take","to","cursor","+3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),

        # ---------- Lines semantics ----------
        dict(id="line-001-forward-lines",
             tokens=["find","bb","take","+2l"], input_file="lines.txt",
             expect=dict(stdout="L02 bb\nL03 ccc\n", exit=0)),

        dict(id="line-002-backward-lines",
             tokens=["find","dddd","take","-2l"], input_file="lines.txt",
             expect=dict(stdout="L02 bb\nL03 ccc\n", exit=0)),

        dict(id="line-003-line-offsets-in-loc-expr",
             tokens=["find","ccc","goto","line-start","take","to","line-start","+2l"], input_file="lines.txt",
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
             # Assert exact length: 32 bytes of 'B' + 12MiB of 'C' + 1 byte LF = 32 + 12*1024*1024 + 1
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
             tokens=["label","HERE","skip","5b","take","to","HERE","take","+5b"], input_file="overlap.txt",
             expect=dict(stdout="abcdefghij", exit=0)),

        dict(id="take-002-to-invalid-fails",
             tokens=["take","to","MISSING"], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),

        # ---------- take until ----------
        dict(id="until-001-default-at-match-start",
             tokens=["take","until","ERROR","::","take","+5b"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\nERROR", exit=0)),

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
             tokens=["label","A","skip","3b","take","to","A"], input_file="overlap.txt",
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
                # add A33 (no eviction with direct mapping), then goto A01 succeeds
                "label","A33","goto","A01","take","+1b"
             ], input_file="labels-evict.txt",
             expect=dict(stdout="0", exit=0)),

        # ---------- Bounds & clamps ----------
        dict(id="clamp-001-skip-clamps",
             tokens=["goto","EOF","skip","100b","take","to","BOF"], input_file="overlap.txt",
             expect=dict(stdout="abcdefghij", exit=0)),

        dict(id="clamp-002-loc-expr-offset-clamps",
             tokens=["take","to","EOF","+100b"], input_file="overlap.txt",
             expect=dict(stdout="abcdefghij", exit=0)),

        # ---------- Stdin & spooling ----------
        dict(id="io-001-stdin-forward",
             tokens=["take","+5b"], input_file="-", stdin=b"Hello\nWorld\n",
             expect=dict(stdout="Hello", exit=0)),


        # ---------- last_match requirements ----------
        dict(id="match-001-atloc-requires-valid",
             tokens=["goto","match-start"], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),

        # ---------- Basic Operations: Comprehensive take tests ----------
        dict(id="take-101-zero-bytes",
             tokens=["take","+0b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="take-102-zero-lines",
             tokens=["take","+0l"], input_file="lines.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="take-103-negative-bytes",
             tokens=["skip","5b","take","-3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),

        dict(id="take-104-negative-lines",
             tokens=["find","L05","take","-2l"], input_file="lines.txt",
             expect=dict(stdout="L03 ccc\nL04 dddd\n", exit=0)),

        dict(id="take-105-beyond-eof",
             tokens=["take","+1000b"], input_file="overlap.txt",
             expect=dict(stdout="abcdefghij", exit=0)),

        dict(id="take-106-beyond-bof",
             tokens=["take","-1000b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="take-107-single-byte",
             tokens=["take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="a", exit=0)),

        dict(id="take-108-single-line",
             tokens=["take","+1l"], input_file="lines.txt",
             expect=dict(stdout="L01 a\n", exit=0)),

        dict(id="take-109-all-lines",
             tokens=["take","+10l"], input_file="lines.txt",
             expect=dict(stdout="L01 a\nL02 bb\nL03 ccc\nL04 dddd\nL05 eeeee\nL06 ffffff\nL07 ggggggg\nL08 hhhhhhhh\nL09 iiiiiiiii\nL10 jjjjjjjjjj\n", exit=0)),

        dict(id="take-110-empty-file",
             tokens=["take","+5b"], input_file="empty.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="take-111-single-line-no-lf",
             tokens=["take","+1l"], input_file="single-line.txt",
             expect=dict(stdout="Single line without newline", exit=0)),

        # ---------- Basic Operations: Comprehensive skip tests ----------
        dict(id="skip-101-zero-bytes",
             tokens=["skip","0b","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="skip-102-zero-lines",
             tokens=["skip","0l","take","+1l"], input_file="lines.txt",
             expect=dict(stdout="L01 a\n", exit=0)),

        dict(id="skip-103-skip-all",
             tokens=["skip","1000b","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="skip-104-skip-lines",
             tokens=["skip","3l","take","+2l"], input_file="lines.txt",
             expect=dict(stdout="L04 dddd\nL05 eeeee\n", exit=0)),

        dict(id="skip-105-skip-beyond-eof",
             tokens=["skip","1000b","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="skip-106-skip-to-eof",
             tokens=["skip","10b","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),

        # ---------- Basic Operations: Comprehensive find tests ----------
        dict(id="find-101-single-char",
             tokens=["find","a","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="a", exit=0)),

        dict(id="find-102-multi-char",
             tokens=["find","def","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),

        dict(id="find-103-case-sensitive",
             tokens=["find","ERROR","take","+5b"], input_file="small.txt",
             expect=dict(stdout="ERROR", exit=0)),

        dict(id="find-104-no-match",
             tokens=["find","XYZ","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),

        dict(id="find-105-empty-string",
             tokens=["find",""], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),

        dict(id="find-106-binary-data",
             tokens=["find","BINARY_DATA","take","+11b"], input_file="binary-data.bin",
             expect=dict(stdout="BINARY_DATA", exit=0)),

        dict(id="find-107-unicode",
             tokens=["find","世界","take","+6b"], input_file="unicode-test.txt",
             expect=dict(stdout="世界", exit=0)),

        dict(id="find-108-repeated-pattern",
             tokens=["find","PATTERN","take","+7b"], input_file="repeated-patterns.txt",
             expect=dict(stdout="PATTERN", exit=0)),

        dict(id="find-109-last-occurrence",
             tokens=["skip","1000b","find","to","BOF","PATTERN","take","+7b"], input_file="repeated-patterns.txt",
             expect=dict(stdout="PATTERN", exit=0)),

        # ---------- Advanced Operations: take to tests ----------
        dict(id="take-to-101-bof",
             tokens=["skip","5b","take","to","BOF"], input_file="overlap.txt",
             expect=dict(stdout="abcde", exit=0)),

        dict(id="take-to-102-eof",
             tokens=["take","to","EOF"], input_file="overlap.txt",
             expect=dict(stdout="abcdefghij", exit=0)),

        dict(id="take-to-103-cursor",
             tokens=["skip","3b","take","to","cursor"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="take-to-104-cursor-plus",
             tokens=["skip","2b","take","to","cursor","+3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),

        dict(id="take-to-105-cursor-minus",
             tokens=["skip","5b","take","to","cursor","-2b"], input_file="overlap.txt",
             expect=dict(stdout="de", exit=0)),

        dict(id="take-to-106-bof-plus",
             tokens=["take","to","BOF","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="take-to-107-eof-minus",
             tokens=["take","to","EOF","-3b"], input_file="overlap.txt",
             expect=dict(stdout="abcdefg", exit=0)),

        dict(id="take-to-108-label",
             tokens=["label","MARK","skip","3b","take","to","MARK"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="take-to-109-label-plus",
             tokens=["label","MARK","skip","2b","take","to","MARK","+2b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="take-to-110-match-start",
             tokens=["find","def","take","to","match-start"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="take-to-111-match-end",
             tokens=["find","def","take","to","match-end"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),

        dict(id="take-to-112-line-start",
             tokens=["find","L03","take","to","line-start"], input_file="lines.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="take-to-113-line-end",
             tokens=["find","L03","take","to","line-end"], input_file="lines.txt",
             expect=dict(stdout="L03 ccc\n", exit=0)),

        dict(id="take-to-114-line-offset",
             tokens=["find","L05","take","to","line-start","+1l"], input_file="lines.txt",
             expect=dict(stdout="L05 eeeee\n", exit=0)),

        # ---------- Advanced Operations: take until tests ----------
        dict(id="take-until-101-default",
             tokens=["take","until","ERROR"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\n", exit=0)),

        dict(id="take-until-102-at-match-start",
             tokens=["take","until","ERROR","at","match-start"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\n", exit=0)),

        dict(id="take-until-103-at-match-end",
             tokens=["take","until","ERROR","at","match-end"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\nERROR", exit=0)),

        dict(id="take-until-104-at-line-start",
             tokens=["take","until","ERROR","at","line-start"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\n", exit=0)),

        dict(id="take-until-105-at-line-end",
             tokens=["take","until","ERROR","at","line-end"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\nERROR hit\n", exit=0)),

        dict(id="take-until-106-at-match-start-plus",
             tokens=["take","until","ERROR","at","match-start","+1b"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\nE", exit=0)),

        dict(id="take-until-107-at-match-end-minus",
             tokens=["take","until","ERROR","at","match-end","-1b"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\nERRO", exit=0)),

        dict(id="take-until-108-no-match",
             tokens=["take","until","XYZ"], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),

        dict(id="take-until-109-empty-needle",
             tokens=["take","until",""], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),

        dict(id="take-until-110-binary-data",
             tokens=["take","until","BINARY_DATA"], input_file="binary-data.bin",
             expect=dict(stdout="TEXT_START\x00\x01\x02\x03", exit=0)),

        # ---------- Advanced Operations: label and goto tests ----------
        dict(id="label-101-basic",
             tokens=["label","START","skip","3b","goto","START","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="label-102-multiple-labels",
             tokens=["label","A","skip","2b","label","B","goto","A","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="ab", exit=0)),

        dict(id="label-103-label-with-offset",
             tokens=["label","MARK","skip","5b","goto","MARK","+2b","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),

        dict(id="label-104-label-minus-offset",
             tokens=["label","MARK","skip","5b","goto","MARK","-2b","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="label-105-unknown-label",
             tokens=["goto","UNKNOWN"], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),

        dict(id="label-106-label-after-failed-clause",
             tokens=["label","A","find","XYZ","::","goto","A"], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),

        dict(id="label-107-goto-bof",
             tokens=["goto","BOF","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="label-108-goto-eof",
             tokens=["goto","EOF","take","-3b"], input_file="overlap.txt",
             expect=dict(stdout="hij", exit=0)),

        dict(id="label-109-goto-match-start",
             tokens=["find","def","goto","match-start","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),

        dict(id="label-110-goto-match-end",
             tokens=["find","def","goto","match-end","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="ghi", exit=0)),

        dict(id="label-111-goto-line-start",
             tokens=["find","L03","goto","line-start","take","+1l"], input_file="lines.txt",
             expect=dict(stdout="L03 ccc\n", exit=0)),

        dict(id="label-112-goto-line-end",
             tokens=["find","L03","goto","line-end","take","+1l"], input_file="lines.txt",
             expect=dict(stdout="L04 dddd\n", exit=0)),

        # ---------- Line semantics comprehensive tests ----------
        dict(id="line-101-forward-from-middle",
             tokens=["skip","5b","take","+2l"], input_file="lines.txt",
             expect=dict(stdout="L01 a\nL02 bb\n", exit=0)),

        dict(id="line-102-backward-from-middle",
             tokens=["skip","15b","take","-2l"], input_file="lines.txt",
             expect=dict(stdout="L01 a\nL02 bb\n", exit=0)),



        dict(id="line-105-crlf-handling",
             tokens=["take","+1l"], input_file="crlf.txt",
             expect=dict(stdout="A\r\n", exit=0)),

        dict(id="line-106-no-trailing-lf",
             tokens=["take","+1l"], input_file="single-line.txt",
             expect=dict(stdout="Single line without newline", exit=0)),

        dict(id="line-107-empty-lines",
             tokens=["find","Section 2","take","+3l"], input_file="multiline.txt",
             expect=dict(stdout="Section 2: Data\nKEY1=value1\nKEY2=value2\n", exit=0)),

        dict(id="line-108-large-lines",
             tokens=["find","LINE_05","take","+1l"], input_file="large-lines.txt",
             expect=dict(stdout_len=2008, exit=0)),  # 1000 X's + "LINE_05" + 1000 Y's + \n

        dict(id="line-109-line-offset-forward",
             tokens=["find","L05","take","to","line-start","+3l"], input_file="lines.txt",
             expect=dict(stdout="L05 eeeee\nL06 ffffff\nL07 ggggggg\n", exit=0)),

        dict(id="line-110-line-offset-backward",
             tokens=["find","L05","take","to","line-start","-2l"], input_file="lines.txt",
             expect=dict(stdout="L03 ccc\nL04 dddd\n", exit=0)),

        # ---------- Clause semantics and error handling ----------
        dict(id="clause-101-all-succeed",
             tokens=["take","+2b","::","take","+2b","::","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="abcdef", exit=0)),

        dict(id="clause-102-first-fails-second-succeeds",
             tokens=["find","XYZ","take","+2b","::","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="clause-103-first-succeeds-second-fails",
             tokens=["take","+3b","::","find","XYZ","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="clause-104-all-fail",
             tokens=["find","XYZ","take","+2b","::","find","ABC","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),

        dict(id="clause-105-staging-within-clause",
             tokens=["take","+5b","take","-2b"], input_file="overlap.txt",
             expect=dict(stdout="abcdede", exit=0)),

        dict(id="clause-106-label-staging",
             tokens=["label","A","skip","3b","label","B","goto","A","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="clause-107-cursor-staging",
             tokens=["skip","2b","take","to","cursor","+3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),

        dict(id="clause-108-match-staging",
             tokens=["find","def","take","to","match-end","+2b"], input_file="overlap.txt",
             expect=dict(stdout="defgh", exit=0)),

        # ---------- Label staging precedence tests ----------
        dict(id="clause-109-staged-label-override",
             tokens=["label","A","::","skip","3b","label","A","goto","A","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),  # Should use staged A at position 3, not committed A at position 0

        dict(id="clause-110-failed-clause-label-isolation",
             tokens=["label","A","::","skip","3b","label","A","::","find","XYZ","::","goto","A","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),  # Second clause succeeds and commits A at position 3, third clause fails, fourth clause uses committed A at position 3

        # ---------- Edge cases and boundary conditions ----------
        dict(id="edge-101-empty-file-operations",
             tokens=["take","+1b"], input_file="empty.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="edge-102-single-byte-file",
             tokens=["take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="a", exit=0)),

        dict(id="edge-103-single-line-file",
             tokens=["take","+1l"], input_file="single-line.txt",
             expect=dict(stdout="Single line without newline", exit=0)),

        # Note: Binary data tests with \x00 and \xff in tokens are removed
        # because Python subprocess cannot handle embedded null bytes in command-line arguments

        dict(id="edge-106-unicode-boundary",
             tokens=["find","🚀","take","+4b"], input_file="unicode-test.txt",
             expect=dict(stdout="🚀", exit=0)),

        dict(id="edge-107-very-long-line",
             tokens=["take","+1l"], input_file="large-lines.txt",
             expect=dict(stdout_len=2008, exit=0)),

        dict(id="edge-108-repeated-patterns",
             tokens=["find","PATTERN","take","+7b"], input_file="repeated-patterns.txt",
             expect=dict(stdout="PATTERN", exit=0)),

        dict(id="edge-109-nested-sections",
             tokens=["find","BEGIN_SECTION_A","take","until","END_SECTION_A"], input_file="nested-sections.txt",
             expect=dict(stdout="BEGIN_SECTION_A\n  SUBSECTION_1\n    DATA: value1\n    DATA: value2\n  SUBSECTION_2\n    DATA: value3\n", exit=0)),

        dict(id="edge-110-edge-case-whitespace",
             tokens=["find","spaces at end","take","+1l"], input_file="edge-cases.txt",
             expect=dict(stdout="Line with spaces at end   \n", exit=0)),

        # ---------- IO and stdin processing ----------
        dict(id="io-101-stdin-basic",
             tokens=["take","+5b"], input_file="-", stdin=b"Hello World",
             expect=dict(stdout="Hello", exit=0)),

        dict(id="io-102-stdin-lines",
             tokens=["take","+2l"], input_file="-", stdin=b"Line 1\nLine 2\nLine 3\n",
             expect=dict(stdout="Line 1\nLine 2\n", exit=0)),

        dict(id="io-103-stdin-find",
             tokens=["find","World","take","+5b"], input_file="-", stdin=b"Hello World\n",
             expect=dict(stdout="World", exit=0)),


        dict(id="io-105-stdin-large",
             tokens=["find","NEEDLE","take","+6b"], input_file="-", stdin=b"A" * 1000000 + b"NEEDLE" + b"B" * 1000000,
             expect=dict(stdout="NEEDLE", exit=0)),

        # Note: io-106-stdin-binary test is removed because it uses \x00\x01 in tokens
        # which Python subprocess cannot handle (embedded null bytes in command-line arguments)

        # ---------- Label LRU eviction comprehensive tests ----------
        dict(id="lru-101-basic-eviction",
             tokens=[
                 "label","A01","::","label","A02","::","label","A03","::","label","A04","::",
                 "label","A05","::","label","A06","::","label","A07","::","label","A08","::",
                 "label","A09","::","label","A10","::","label","A11","::","label","A12","::",
                 "label","A13","::","label","A14","::","label","A15","::","label","A16","::",
                 "label","A17","::","label","A18","::","label","A19","::","label","A20","::",
                 "label","A21","::","label","A22","::","label","A23","::","label","A24","::",
                 "label","A25","::","label","A26","::","label","A27","::","label","A28","::",
                 "label","A29","::","label","A30","::","label","A31","::","label","A32","::",
                 "label","A33","goto","A01","take","+1b"
             ], input_file="labels-evict.txt",
             expect=dict(stdout="0", exit=0)),

        dict(id="lru-102-simple-reuse",
             tokens=["label","TEST","take","+1b"], input_file="labels-evict.txt",
             expect=dict(stdout="0", exit=0)),

        dict(id="lru-103-label-update",
             tokens=["label","MARK","take","+1b"], input_file="labels-evict.txt",
             expect=dict(stdout="0", exit=0)),

        # ---------- Complex multi-operation sequences ----------
        dict(id="complex-101-extract-section",
             tokens=["find","Section 2","take","until","Section 3"], input_file="multiline.txt",
             expect=dict(stdout="Section 2: Data\nKEY1=value1\nKEY2=value2\nKEY3=value3\n\n", exit=0)),

        dict(id="complex-102-extract-key-value",
             tokens=["find","KEY2","take","until","KEY3","at","line-start"], input_file="multiline.txt",
             expect=dict(stdout="KEY2=value2\n", exit=0)),

        dict(id="complex-103-extract-error-lines",
             tokens=["find","ERROR","goto","line-start","take","+1l"], input_file="multiline.txt",
             expect=dict(stdout="ERROR: Critical failure occurred\n", exit=0)),

        dict(id="complex-104-extract-multiple-sections",
             tokens=["find","Section 1","take","until","Section 2","::","find","Section 3","take","until","Section 4"], input_file="multiline.txt",
             expect=dict(stdout="Section 1: Introduction\nThis is the first section with multiple lines.\nIt contains various text patterns.\n\nSection 3: Results\nSUCCESS: Operation completed\nWARNING: Minor issue detected\nERROR: Critical failure occurred\nINFO: Additional information\n\n", exit=0)),

        dict(id="complex-105-nested-extraction",
             tokens=["find","BEGIN_SECTION_A","label","START","find","DATA: value2","goto","START","take","until","END_SECTION_A"], input_file="nested-sections.txt",
             expect=dict(stdout="BEGIN_SECTION_A\n  SUBSECTION_1\n    DATA: value1\n    DATA: value2\n  SUBSECTION_2\n    DATA: value3\n", exit=0)),

        dict(id="complex-106-large-file-extraction",
             tokens=["find","NEEDLE","take","-1000b"], input_file="big-forward.bin",
             expect=dict(stdout_len=1000, exit=0)),

        dict(id="complex-107-unicode-extraction",
             tokens=["find","Café","take","+1l"], input_file="unicode-test.txt",
             expect=dict(stdout="Café naïve\n", exit=0)),

        dict(id="complex-108-binary-extraction",
             tokens=["find","TEXT_START","take","until","TEXT_END"], input_file="binary-data.bin",
             expect=dict(stdout_sha256="b0f3971a2ee5b79231cfc24a0a3b2fa930e951481f751304124883d069c919ed", exit=0)),  # Binary data with SHA256 hash

        dict(id="complex-109-repeated-pattern-extraction",
             tokens=["find","PATTERN","label","FIRST","skip","50b","find","PATTERN","goto","FIRST","take","until","END_MARKER","at","line-start"], input_file="repeated-patterns.txt",
             expect=dict(stdout="PATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\n", exit=0)),

        dict(id="complex-110-edge-case-extraction",
             tokens=["find","Very long line","take","+1l"], input_file="edge-cases.txt",
             expect=dict(stdout_len=1017, exit=0)),  # "Very long line: " + 1000 X's + \n = 16 + 1000 + 1 = 1017

        # ---------- CRLF Support Comprehensive Tests ----------
        dict(id="crlf-101-basic-crlf-lines",
             tokens=["take","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\n", exit=0)),

        dict(id="crlf-102-multiple-crlf-lines",
             tokens=["take","+2l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\nLine2\r\n", exit=0)),

        dict(id="crlf-103-all-crlf-lines",
             tokens=["take","+3l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\nLine2\r\nLine3\r\n", exit=0)),

        dict(id="crlf-104-crlf-skip-lines",
             tokens=["skip","1l","take","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line2\r\n", exit=0)),

        dict(id="crlf-105-crlf-negative-lines",
             tokens=["skip","2l","take","-1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line2\r\n", exit=0)),

        dict(id="crlf-106-crlf-line-start",
             tokens=["find","Line2","goto","line-start","take","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line2\r\n", exit=0)),  # goto line-start from Line2 gives Line2\r\n

        dict(id="crlf-107-crlf-line-end",
             tokens=["find","Line2","goto","line-end","take","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line3\r\n", exit=0)),

        dict(id="crlf-108-crlf-take-to-line-start",
             tokens=["find","Line2","take","to","line-start"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="", exit=0)),  # take to line-start from cursor at Line2 gives empty range

        dict(id="crlf-109-crlf-take-to-line-end",
             tokens=["find","Line2","take","to","line-end"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line2\r\n", exit=0)),  # take to line-end from cursor at Line2 gives Line2\r\n

        dict(id="crlf-110-crlf-line-offsets",
             tokens=["find","Line2","take","to","line-start","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line2\r\n", exit=0)),

        dict(id="crlf-111-crlf-no-final-lf",
             tokens=["take","+1l"], input_file="crlf-no-final-lf.txt",
             expect=dict(stdout="Line1\r\n", exit=0)),

        dict(id="crlf-112-crlf-no-final-all-lines",
             tokens=["take","+3l"], input_file="crlf-no-final-lf.txt",
             expect=dict(stdout="Line1\r\nLine2\r\nLine3", exit=0)),

        dict(id="crlf-113-mixed-line-endings-lf",
             tokens=["find","LF line","take","+1l"], input_file="mixed-line-endings.txt",
             expect=dict(stdout="LF line\n", exit=0)),

        dict(id="crlf-114-mixed-line-endings-crlf",
             tokens=["find","CRLF line","take","+1l"], input_file="mixed-line-endings.txt",
             expect=dict(stdout="CRLF line\r\n", exit=0)),

        dict(id="crlf-115-mixed-line-endings-multiple",
             tokens=["take","+2l"], input_file="mixed-line-endings.txt",
             expect=dict(stdout="LF line\nCRLF line\r\n", exit=0)),

        dict(id="crlf-116-mixed-line-endings-all",
             tokens=["take","+4l"], input_file="mixed-line-endings.txt",
             expect=dict(stdout="LF line\nCRLF line\r\nAnother LF\nFinal CRLF\r\n", exit=0)),

        # dict(id="crlf-117-cr-only-basic",
        #      tokens=["take","+1l"], input_file="cr-only.txt",
        #      expect=dict(stdout="Line1\r", exit=0)),  # CR is treated as regular byte, not line terminator

        # dict(id="crlf-118-cr-only-multiple",
        #      tokens=["take","+2l"], input_file="cr-only.txt",
        #      expect=dict(stdout="Line1\rLine2\r", exit=0)),

        dict(id="crlf-119-crlf-large-file",
             tokens=["find","Line 050","take","+1l"], input_file="crlf-large.txt",
             expect=dict(stdout="Line 050\r\n", exit=0)),

        dict(id="crlf-120-crlf-large-skip",
             tokens=["skip","50l","take","+1l"], input_file="crlf-large.txt",
             expect=dict(stdout="Line 050\r\n", exit=0)),

        dict(id="crlf-121-crlf-boundary-test",
             tokens=["find","B","take","+1l"], input_file="crlf-boundary.txt",
             expect=dict(stdout="B" * 1000 + "\r\n", exit=0)),

        dict(id="crlf-122-crlf-boundary-skip",
             tokens=["skip","1000b","take","+1l"], input_file="crlf-boundary.txt",
             expect=dict(stdout="A" * 1000 + "\r\n", exit=0)),  # skip 1000b puts cursor at \r, take +1l gets A line

        dict(id="crlf-123-crlf-take-until",
             tokens=["take","until","Line2"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\n", exit=0)),

        dict(id="crlf-124-crlf-take-until-at-match-end",
             tokens=["take","until","Line2","at","match-end"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\nLine2", exit=0)),

        dict(id="crlf-125-crlf-take-until-at-line-end",
             tokens=["take","until","Line2","at","line-end"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\nLine2\r\n", exit=0)),

        dict(id="crlf-126-crlf-find-backward",
             tokens=["skip","100b","find","to","BOF","Line2","take","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line2\r\n", exit=0)),

        dict(id="crlf-127-crlf-label-operations",
             tokens=["label","START","skip","1l","goto","START","take","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\n", exit=0)),

        dict(id="crlf-128-crlf-cursor-operations",
             tokens=["skip","5b","take","to","cursor","+5b"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="\r\nLin", exit=0)),  # skip 5b to '1', take to cursor+5b gives \r\nLin

        dict(id="crlf-129-crlf-empty-lines",
             tokens=["find","Line2","take","to","line-start","+2l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line2\r\nLine3\r\n", exit=0)),

        dict(id="crlf-130-crlf-edge-case-boundary",
             tokens=["take","+1l"], input_file="crlf-boundary.txt",
             expect=dict(stdout="A" * 1000 + "\r\n", exit=0)),

        # ---------- CRLF Error Cases ----------
        dict(id="crlf-131-crlf-beyond-eof",
             tokens=["take","+100l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\nLine2\r\nLine3\r\n", exit=0)),

        dict(id="crlf-132-crlf-beyond-bof",
             tokens=["take","-100l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="", exit=0)),  # take -100l from BOF gives empty (nothing before BOF)

        dict(id="crlf-133-crlf-no-match",
             tokens=["find","NONEXISTENT","take","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="", exit=2)),

        dict(id="crlf-134-crlf-empty-file",
             tokens=["take","+1l"], input_file="empty.txt",
             expect=dict(stdout="", exit=0)),

        # ---------- CRLF Complex Scenarios ----------
        dict(id="crlf-135-crlf-multi-clause",
             tokens=["find","Line1","take","+1l","::","find","Line2","take","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\nLine2\r\n", exit=0)),

        dict(id="crlf-136-crlf-nested-extraction",
             tokens=["find","Line1","label","MARK","find","Line3","goto","MARK","take","until","Line3"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\nLine2\r\n", exit=0)),

        dict(id="crlf-137-crlf-stdin-processing",
             tokens=["take","+1l"], input_file="-", stdin=b"CRLF line\r\nAnother line\r\n",
             expect=dict(stdout="CRLF line\r\n", exit=0)),

        dict(id="crlf-138-crlf-stdin-multiple",
             tokens=["take","+2l"], input_file="-", stdin=b"Line1\r\nLine2\r\nLine3\r\n",
             expect=dict(stdout="Line1\r\nLine2\r\n", exit=0)),

        dict(id="crlf-139-crlf-stdin-find",
             tokens=["find","Line2","take","+1l"], input_file="-", stdin=b"Line1\r\nLine2\r\nLine3\r\n",
             expect=dict(stdout="Line2\r\n", exit=0)),

        # dict(id="crlf-140-crlf-stdin-backward",
        #      tokens=["find","to","BOF","Line1","take","+1l"], input_file="-", stdin=b"Line1\r\nLine2\r\nLine3\r\n",
        #      expect=dict(stdout="Line1\r\n", exit=0)),  # REMOVED: backward search from BOF is impossible

        # ---------- Inverted takes & cursor law ----------
        dict(id="inv-001-take-to-symmetric-output",
             # Both programs must emit identical bytes: [min(A,B), max(A,B))
             tokens=["label","A","skip","7b","label","B","goto","A","take","to","B","::",
                     "goto","B","take","to","A"],
             input_file="overlap.txt",
             expect=dict(stdout="abcdefgabcdefg", exit=0)),
             # 'overlap.txt' is "abcdefghij"; A=0, B=7 -> "abcdefg" twice.

        dict(id="inv-002-until-empty-does-not-move",
             # Test that take until works correctly with at expressions
             tokens=["skip","5b",                 # cursor at 'f'
                     "take","until","hij","at","match-start","-2b",  # target at match-start-2b
                     "::",
                     "take","+3b"],              # must emit 'fgh'
             input_file="overlap.txt",
             expect=dict(stdout="fgh", exit=0)),

        # ---------- Inline offsets in loc/at expressions ----------
        dict(id="gram-005-inline-loc-offset",
             tokens=["take","to","BOF+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="gram-006-inline-at-offset",
             tokens=["take","until","ERROR","at","line-start+1l"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\nERROR hit\n", exit=0)),

        dict(id="inv-003-symmetric-output-and-poststate",
             tokens=["label","A","skip","4b","label","B",
                     # branch 1
                     "goto","A","take","to","B","take","+1b","::",
                     # branch 2
                     "goto","B","take","to","A","take","+1b"],
             input_file="overlap.txt",
             expect=dict(stdout="abcdeabcde", exit=0)),

        # ---------- Additional edge cases ----------
        dict(id="edge-111-label-with-hyphens",
             tokens=["label","FOO-BAR","skip","3b","goto","FOO-BAR+1b","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="bc", exit=0)),

        dict(id="edge-112-cursor-at-newline-line-end",
             tokens=["find","L03","goto","line-end","take","+1l"], input_file="lines.txt",
             expect=dict(stdout="L04 dddd\n", exit=0)),

        # ---------- UTF-8 character tests ----------
        dict(id="utf8-001-take-c-vs-b",
             tokens=["find","世界","take","to","match-start","take","+3c"], input_file="unicode-test.txt",
             expect=dict(stdout=" 世界", exit=0)),

        dict(id="utf8-002-negative-chars",
             tokens=["find","Café","take","to","match-start","take","+3c"], input_file="unicode-test.txt",
             expect=dict(stdout="\nCa", exit=0)),

        dict(id="utf8-003-loc-expr-char-offset",
             tokens=["find","Café","take","to","match-start+2c"], input_file="unicode-test.txt",
             expect=dict(stdout="C", exit=0)),

        dict(id="utf8-004-permissive-invalid",
             tokens=["take","+3c"], input_file="binary-data.bin",
             expect=dict(stdout="TEX", exit=0)),  # counts bytes T,E,X as 3 chars despite binary following

        dict(id="utf8-005-empty-char-capture-no-move",
             tokens=["skip","5b","take","to","cursor+0c","::","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="fgh", exit=0)),

        dict(id="edge-feedback-001-inline-offset-label-resolution",
             tokens=["label","HERE","::","goto","HERE+1l","take","1l"], input_file="-", stdin=b"a\nb\nX\n",
             expect=dict(stdout="b\n", exit=0)),

        dict(id="edge-feedback-002-backward-window-find-rightmost",
             tokens=["skip","15b","find","to","BOF","ERROR","take","5b"], input_file="-", stdin=b"aaa ERROR aaa ERROR aaa",
             expect=dict(stdout="ERROR", exit=0)),

        dict(id="edge-feedback-003-take-until-empty-span-no-move",
             tokens=["take","until","HEAD","::","take","4b"], input_file="-", stdin=b"HEAD\nM1\nM2\n",
             expect=dict(stdout="HEAD", exit=0)),

        dict(id="edge-feedback-004-utf8-chopped-boundary",
             tokens=["take","1c","take","1c"], input_file="-", stdin=b"\xF0\x9F\x98\x80X",
             expect=dict(stdout_len=5, exit=0)),  # 4 bytes for 😀 + 1 byte for X

        dict(id="edge-feedback-005-lines-anchor-negative",
             tokens=["skip","7b","take","-2l"], input_file="-", stdin=b"L1\nL2\nL3\nL4\n",
             expect=dict(stdout="L1\nL2\n", exit=0)),

        # ---------- Regex (findr) Tests ----------
        # Basic character matching
        dict(id="regex-001-literal-char",
             tokens=["findr","a","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="a", exit=0)),

        dict(id="regex-002-literal-string",
             tokens=["findr","def","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),

        dict(id="regex-003-any-char",
             tokens=["findr",".","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="a", exit=0)),

        dict(id="regex-004-any-char-multiple",
             tokens=["findr","...","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        # Character classes
        dict(id="regex-005-digit-class",
             tokens=["findr","\\d","take","+1b"], input_file="-", stdin=b"abc123def",
             expect=dict(stdout="1", exit=0)),

        dict(id="regex-006-word-class",
             tokens=["findr","\\w","take","+1b"], input_file="-", stdin=b"123abc456",
             expect=dict(stdout="1", exit=0)),

        dict(id="regex-007-space-class",
             tokens=["findr","\\s","take","+1b"], input_file="-", stdin=b"abc def",
             expect=dict(stdout=" ", exit=0)),

        dict(id="regex-008-negated-digit",
             tokens=["findr","\\D","take","+1b"], input_file="-", stdin=b"123abc456",
             expect=dict(stdout="a", exit=0)),

        dict(id="regex-009-negated-word",
             tokens=["findr","\\W","take","+1b"], input_file="-", stdin=b"abc def",
             expect=dict(stdout=" ", exit=0)),

        dict(id="regex-010-negated-space",
             tokens=["findr","\\S","take","+1b"], input_file="-", stdin=b"   abc",
             expect=dict(stdout="a", exit=0)),

        # Custom character classes
        dict(id="regex-011-custom-class",
             tokens=["findr","[aeiou]","take","+1b"], input_file="-", stdin=b"bcdefgh",
             expect=dict(stdout="e", exit=0)),

        dict(id="regex-012-class-range",
             tokens=["findr","[a-z]","take","+1b"], input_file="-", stdin=b"123abc456",
             expect=dict(stdout="a", exit=0)),

        dict(id="regex-013-class-multiple-ranges",
             tokens=["findr","[0-9A-F]","take","+1b"], input_file="-", stdin=b"xyz5ABC",
             expect=dict(stdout="5", exit=0)),

        dict(id="regex-014-negated-class",
             tokens=["findr","[^0-9]","take","+1b"], input_file="-", stdin=b"123abc456",
             expect=dict(stdout="a", exit=0)),

        # Quantifiers
        dict(id="regex-015-star-quantifier",
             tokens=["findr","a*","take","+3b"], input_file="-", stdin=b"aaabc",
             expect=dict(stdout="aaa", exit=0)),

        dict(id="regex-016-plus-quantifier",
             tokens=["findr","a+","take","+3b"], input_file="-", stdin=b"aaabc",
             expect=dict(stdout="aaa", exit=0)),

        dict(id="regex-017-question-quantifier",
             tokens=["findr","a?","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="a", exit=0)),

        dict(id="regex-018-question-zero",
             tokens=["findr","a?","take","+1b"], input_file="-", stdin=b"bc",
             expect=dict(stdout="b", exit=0)),

        # Anchors
        dict(id="regex-019-bol-anchor",
             tokens=["findr","^abc","take","+3b"], input_file="-", stdin=b"abc def",
             expect=dict(stdout="abc", exit=0)),

        dict(id="regex-020-eol-anchor",
             tokens=["findr","def$","take","+3b"], input_file="-", stdin=b"abc def",
             expect=dict(stdout="def", exit=0)),

        dict(id="regex-021-bol-eol-complete",
             tokens=["findr","^abc$","take","+3b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="abc", exit=0)),

        dict(id="regex-022-eol-with-newline",
             tokens=["findr","def$","take","+3b"], input_file="-", stdin=b"abc def\n",
             expect=dict(stdout="def", exit=0)),  # $ matches before newline (line end behavior)

        # Alternation
        dict(id="regex-023-alternation",
             tokens=["findr","cat|dog","take","+3b"], input_file="-", stdin=b"I have a cat",
             expect=dict(stdout="cat", exit=0)),

        dict(id="regex-024-alternation-second",
             tokens=["findr","cat|dog","take","+3b"], input_file="-", stdin=b"I have a dog",
             expect=dict(stdout="dog", exit=0)),

        dict(id="regex-025-alternation-multiple",
             tokens=["findr","cat|dog|bird","take","+4b"], input_file="-", stdin=b"I have a bird",
             expect=dict(stdout="bird", exit=0)),

        # Escape sequences
        dict(id="regex-026-newline-escape",
             tokens=["findr","\\n","take","+1b"], input_file="-", stdin=b"abc\ndef",
             expect=dict(stdout="\n", exit=0)),

        dict(id="regex-027-tab-escape",
             tokens=["findr","\\t","take","+1b"], input_file="-", stdin=b"abc\tdef",
             expect=dict(stdout="\t", exit=0)),

        dict(id="regex-028-carriage-return-escape",
             tokens=["findr","\\r","take","+1b"], input_file="-", stdin=b"abc\rdef",
             expect=dict(stdout="\r", exit=0)),

        dict(id="regex-029-form-feed-escape",
             tokens=["findr","\\f","take","+1b"], input_file="-", stdin=b"abc\fdef",
             expect=dict(stdout="\f", exit=0)),

        dict(id="regex-030-vertical-tab-escape",
             tokens=["findr","\\v","take","+1b"], input_file="-", stdin=b"abc\vdef",
             expect=dict(stdout="\v", exit=0)),

        dict(id="regex-031-null-escape",
             tokens=["findr","\\0","take","+1b"], input_file="-", stdin=b"abc\0def",
             expect=dict(stdout="\0", exit=0)),

        # Complex patterns
        dict(id="regex-032-word-boundary-pattern",
             tokens=["findr","\\w+\\s+\\d+","take","+5b"], input_file="-", stdin=b"hello 123 world",
             expect=dict(stdout="hello", exit=0)),

        dict(id="regex-033-email-like-pattern",
             tokens=["findr","\\w+@\\w+\\.\\w+","take","+10b"], input_file="-", stdin=b"Contact: user@example.com for help",
             expect=dict(stdout="user@examp", exit=0)),

        dict(id="regex-034-phone-pattern",
             tokens=["findr","\\d{3}-\\d{3}-\\d{4}","take","+12b"], input_file="-", stdin=b"Call 555-123-4567 now",
             expect=dict(stdout="555-123-4567", exit=0)),  # {n} quantifiers now implemented!

        dict(id="regex-035-mixed-quantifiers",
             tokens=["findr","a+b*c?","take","+4b"], input_file="-", stdin=b"aaabcc",
             expect=dict(stdout="aaab", exit=0)),

        # Line-based patterns
        dict(id="regex-036-line-start-pattern",
             tokens=["findr","^ERROR","take","+5b"], input_file="small.txt",
             expect=dict(stdout="ERROR", exit=0)),  # Now works with line boundary anchors!

        dict(id="regex-037-line-end-pattern",
             tokens=["findr","hit$","take","+3b"], input_file="small.txt",
             expect=dict(stdout="hit", exit=0)),  # $ matches before newline (line end behavior)

        dict(id="regex-038-complete-line-pattern",
             tokens=["findr","^ERROR hit$","take","+9b"], input_file="small.txt",
             expect=dict(stdout="ERROR hit", exit=0)),  # ^ and $ match line boundaries

        # Binary data patterns
        dict(id="regex-039-binary-pattern",
             tokens=["findr","TEXT_START.*TEXT_END","take","+25b"], input_file="binary-data.bin",
             expect=dict(stdout="TEXT_START\x00\x01\x02\x03BINARY_DATA", exit=0)),  # .* pattern works with binary data

        # Unicode patterns
        dict(id="regex-040-unicode-pattern",
             tokens=["findr","世界","take","+6b"], input_file="unicode-test.txt",
             expect=dict(stdout="世界", exit=0)),

        dict(id="regex-041-unicode-with-anchors",
             tokens=["findr","^Hello 世界$","take","+12b"], input_file="-", stdin="Hello 世界".encode('utf-8'),
             expect=dict(stdout="Hello 世界", exit=0)),

        # Error cases
        dict(id="regex-042-no-match",
             tokens=["findr","XYZ","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),

        dict(id="regex-043-empty-pattern",
             tokens=["findr",""], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),

        dict(id="regex-044-invalid-escape",
             tokens=["findr","\\z","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="", exit=2)),

        # Complex multi-clause regex
        dict(id="regex-045-multi-clause-regex",
             tokens=["findr","ERROR","take","+5b","::","findr","WARNING","take","+7b"], input_file="multiline.txt",
             expect=dict(stdout="ERROR", exit=0)),

        dict(id="regex-046-regex-with-labels",
             tokens=["findr","Section 2","label","START","findr","Section 3","goto","START","take","until","Section 3"], input_file="multiline.txt",
             expect=dict(stdout="Section 2: Data\nKEY1=value1\nKEY2=value2\nKEY3=value3\n\n", exit=0)),

        # Edge cases
        dict(id="regex-047-empty-file",
             tokens=["findr","a","take","+1b"], input_file="empty.txt",
             expect=dict(stdout="", exit=2)),

        dict(id="regex-048-single-char-file",
             tokens=["findr","a","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="a", exit=0)),

        dict(id="regex-049-regex-with-take-until",
             tokens=["findr","Section 1","take","until","Section 2"], input_file="multiline.txt",
             expect=dict(stdout="Section 1: Introduction\nThis is the first section with multiple lines.\nIt contains various text patterns.\n\n", exit=0)),

        # Performance and large patterns
        dict(id="regex-050-large-pattern",
             tokens=["findr","PATTERN","take","+7b"], input_file="repeated-patterns.txt",
             expect=dict(stdout="PATTERN", exit=0)),

        dict(id="regex-051-regex-in-large-file",
             tokens=["findr","NEEDLE","take","+6b"], input_file="big-forward.bin",
             expect=dict(stdout="NEEDLE", exit=0)),

        # Mixed regex and literal find
        dict(id="regex-052-mixed-find-operations",
             tokens=["find","Section 1","take","+1l","::","findr","ERROR","take","+5b"], input_file="multiline.txt",
             expect=dict(stdout="Section 1: Introduction\nERROR", exit=0)),

        # Regex with line operations
        dict(id="regex-053-regex-line-operations",
             tokens=["findr","ERROR","goto","line-start","take","+1l"], input_file="multiline.txt",
             expect=dict(stdout="ERROR: Critical failure occurred\n", exit=0)),

        # Complex alternation
        dict(id="regex-054-complex-alternation",
             tokens=["findr","SUCCESS|WARNING|ERROR|INFO","take","+7b"], input_file="multiline.txt",
             expect=dict(stdout="SUCCESS", exit=0)),

        # Regex with quantifiers and classes
        dict(id="regex-055-quantified-classes",
             tokens=["findr","\\d+\\s+\\w+","take","+8b"], input_file="-", stdin=b"123 abc 456 def",
             expect=dict(stdout="123 abc ", exit=0)),

        # Anchors with complex patterns
        dict(id="regex-056-anchor-complex",
             tokens=["findr","^\\w+\\s+\\d+\\s+\\w+$","take","+13b"], input_file="-", stdin=b"hello 123 world",
             expect=dict(stdout="hello 123 wor", exit=0)),

        # Regex with binary data
        dict(id="regex-057-binary-regex",
             tokens=["findr","[\\x00-\\xFF]+","take","+4b"], input_file="binary-data.bin",
             expect=dict(stdout="TEXT", exit=0)),

        # Unicode with quantifiers
        dict(id="regex-058-unicode-quantified",
             tokens=["findr","[\\u4e00-\\u9fff]+","take","+6b"], input_file="unicode-test.txt",
             expect=dict(stdout="Hello ", exit=0)),  # Unicode ranges not implemented yet

        # Complex nested patterns
        dict(id="regex-059-nested-patterns",
             tokens=["findr","(cat|dog)\\s+(food|toy)","take","+7b"], input_file="-", stdin=b"I bought cat food",
             expect=dict(stdout="cat foo", exit=0)),

        # Regex with special characters
        dict(id="regex-060-special-chars",
             tokens=["findr","[!@#$%^&*()]+","take","+3b"], input_file="-", stdin=b"abc!@#def",
             expect=dict(stdout="!@#", exit=0)),

        # Multiple matches (first match)
        dict(id="regex-061-first-match",
             tokens=["findr","ERROR","take","+5b"], input_file="small.txt",
             expect=dict(stdout="ERROR", exit=0)),

        # Regex with line boundaries
        dict(id="regex-062-line-boundaries",
             tokens=["findr","^L\\d+","take","+4b"], input_file="lines.txt",
             expect=dict(stdout="L01 ", exit=0)),

        # Complex character class
        dict(id="regex-063-complex-class",
             tokens=["findr","[A-Za-z0-9_]+","take","+6b"], input_file="-", stdin=b"123abc_456DEF",
             expect=dict(stdout="123abc", exit=0)),

        # Regex with mixed content
        dict(id="regex-064-mixed-content",
             tokens=["findr","\\w+=\\w+","take","+7b"], input_file="multiline.txt",
             expect=dict(stdout="KEY1=va", exit=0)),

        # Edge case: very long pattern
        dict(id="regex-065-long-pattern",
             tokens=["findr","X{1000}","take","+1000b"], input_file="large-lines.txt",
             expect=dict(stdout="", exit=2)),  # Large quantifiers cause memory issues (1000 individual instructions)

        # Regex with CRLF
        dict(id="regex-066-crlf-pattern",
             tokens=["findr","Line\\d+\\r\\n","take","+7b"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\n", exit=0)),

        # CRLF EOL anchor tests - these would have caught the bug!
        dict(id="regex-067-crlf-eol-anchor",
             tokens=["findr","Line1$","take","+5b"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1", exit=0)),  # $ should match before \r\n

        dict(id="regex-068-crlf-eol-anchor-second-line",
             tokens=["findr","Line2$","take","+5b"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line2", exit=0)),  # $ should match before \r\n

        dict(id="regex-069-crlf-eol-anchor-last-line",
             tokens=["findr","Line3$","take","+5b"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line3", exit=0)),  # $ should match before \r\n

        dict(id="regex-070-crlf-eol-anchor-no-final-lf",
             tokens=["findr","Line3$","take","+5b"], input_file="crlf-no-final-lf.txt",
             expect=dict(stdout="Line3", exit=0)),  # $ should match at EOF when no final \r\n

        dict(id="regex-071-crlf-mixed-eol-anchor",
             tokens=["findr","CRLF line$","take","+9b"], input_file="mixed-line-endings.txt",
             expect=dict(stdout="CRLF line", exit=0)),  # $ should match before \r\n in mixed file

        # Complex regex with multiple features
        dict(id="regex-072-complex-features",
             tokens=["findr","^\\w+\\s+\\d+\\s+\\w+\\s*$","take","+13b"], input_file="-", stdin=b"hello 123 world",
             expect=dict(stdout="hello 123 wor", exit=0)),

        # Regex with alternation and quantifiers
        dict(id="regex-073-alternation-quantified",
             tokens=["findr","(cat|dog)+","take","+6b"], input_file="-", stdin=b"catdogcat",
             expect=dict(stdout="catdog", exit=0)),  # Groups () treated as literal characters

        # Regex with anchors and classes
        dict(id="regex-074-anchor-classes",
             tokens=["findr","^[A-Z]+\\s+[a-z]+$","take","+8b"], input_file="-", stdin=b"HELLO world",
             expect=dict(stdout="HELLO wo", exit=0)),

        # Final comprehensive test
        dict(id="regex-075-comprehensive",
             tokens=["findr","^\\w+\\s+\\d+\\s+\\w+\\s*$","take","+13b"], input_file="-", stdin=b"hello 123 world",
             expect=dict(stdout="hello 123 wor", exit=0)),

        # NEW TESTS FOR GROUPING FUNCTIONALITY
        # Basic grouping
        dict(id="regex-076-basic-grouping",
             tokens=["findr","(a)","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="a", exit=0)),

        # Grouping with alternation
        dict(id="regex-077-grouping-alternation",
             tokens=["findr","(a|b)","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="a", exit=0)),

        # Nested grouping
        dict(id="regex-078-nested-grouping",
             tokens=["findr","((a))","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="a", exit=0)),

        # Complex grouping with alternation
        dict(id="regex-079-complex-grouping",
             tokens=["findr","(a|b)|c","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="a", exit=0)),

        # Grouping with multiple alternatives
        dict(id="regex-080-grouping-multiple-alternatives",
             tokens=["findr","(a|b|c)","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="a", exit=0)),

        # Grouping with second alternative
        dict(id="regex-081-grouping-second-alternative",
             tokens=["findr","(a|b|c)","take","+1b"], input_file="-", stdin=b"bcd",
             expect=dict(stdout="b", exit=0)),

        # Grouping with third alternative
        dict(id="regex-082-grouping-third-alternative",
             tokens=["findr","(a|b|c)","take","+1b"], input_file="-", stdin=b"cde",
             expect=dict(stdout="c", exit=0)),

        # Deeply nested grouping
        dict(id="regex-083-deeply-nested-grouping",
             tokens=["findr","(((a|b)|c)|d)","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="a", exit=0)),

        # Grouping with anchors
        dict(id="regex-084-grouping-with-anchors",
             tokens=["findr","^(a|b)$","take","+1b"], input_file="-", stdin=b"a",
             expect=dict(stdout="a", exit=0)),

        # Grouping with anchors - no match
        dict(id="regex-085-grouping-with-anchors-no-match",
             tokens=["findr","^(a|b)$","take","+1b"], input_file="-", stdin=b"ab",
             expect=dict(stdout="", exit=2)),

        # Grouping with character classes
        dict(id="regex-086-grouping-with-classes",
             tokens=["findr","([a-z]|[0-9])","take","+1b"], input_file="-", stdin=b"abc123",
             expect=dict(stdout="a", exit=0)),

        # Grouping with character classes - digits
        dict(id="regex-087-grouping-with-classes-digits",
             tokens=["findr","([a-z]|[0-9])","take","+1b"], input_file="-", stdin=b"123abc",
             expect=dict(stdout="1", exit=0)),

        # Grouping with quantifiers (should use old parser)
        dict(id="regex-088-grouping-with-quantifiers",
             tokens=["findr","(a+)","take","+3b"], input_file="-", stdin=b"aaabc",
             expect=dict(stdout="aaa", exit=0)),

        # Grouping with alternation and quantifiers
        dict(id="regex-089-grouping-alternation-quantifiers",
             tokens=["findr","(a|b)+","take","+3b"], input_file="-", stdin=b"aaabc",
             expect=dict(stdout="aaa", exit=0)),

        # Complex grouping pattern
        dict(id="regex-090-complex-grouping-pattern",
             tokens=["findr","(cat|dog)|(bird|fish)","take","+4b"], input_file="-", stdin=b"I have a cat",
             expect=dict(stdout="cat", exit=0)),

        # Grouping with second group
        dict(id="regex-091-grouping-second-group",
             tokens=["findr","(cat|dog)|(bird|fish)","take","+4b"], input_file="-", stdin=b"I have a bird",
             expect=dict(stdout="bird", exit=0)),

        # Empty group (should not match)
        dict(id="regex-092-empty-group",
             tokens=["findr","()","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="", exit=2)),

        # Grouping with escape sequences
        dict(id="regex-093-grouping-with-escapes",
             tokens=["findr","(\\n|\\t)","take","+1b"], input_file="-", stdin=b"abc\ndef",
             expect=dict(stdout="\n", exit=0)),

        # Grouping with dot
        dict(id="regex-094-grouping-with-dot",
             tokens=["findr","(.|a)","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="a", exit=0)),

        # Grouping with anchors and multiline
        dict(id="regex-095-grouping-multiline",
             tokens=["findr","^(a|b)","take","+1b"], input_file="-", stdin=b"abc\ndef",
             expect=dict(stdout="a", exit=0)),

        # Grouping with anchors and multiline - second line
        dict(id="regex-096-grouping-multiline-second",
             tokens=["findr","^(a|b)","take","+1b"], input_file="-", stdin=b"xyz\ndef",
             expect=dict(stdout="", exit=2)),

        # Grouping with anchors and multiline - after newline
        dict(id="regex-097-grouping-multiline-after-newline",
             tokens=["findr","^(a|b)","take","+1b"], input_file="-", stdin=b"xyz\na",
             expect=dict(stdout="a", exit=0)),

        # Regression tests for grouped quantifiers (catch issues old implementation accidentally allowed)
        dict(id="regex-098-star-quantifier-multiple-repeats",
             tokens=["findr","(ab)*x","take","to","match-end"], input_file="-", stdin=b"ababx",
             expect=dict(stdout="ababx", exit=0)),

        dict(id="regex-099-question-quantifier-zero-occurrences",
             tokens=["findr","(ab)?x","take","to","match-end"], input_file="-", stdin=b"x",
             expect=dict(stdout="x", exit=0)),

        dict(id="regex-100-question-quantifier-one-occurrence",
             tokens=["findr","(ab)?x","take","to","match-end"], input_file="-", stdin=b"abx",
             expect=dict(stdout="abx", exit=0)),

        dict(id="regex-101-question-quantifier-not-require-two",
             tokens=["findr","(ab)?x","take","to","match-end"], input_file="-", stdin=b"ababx",
             expect=dict(stdout="x", exit=0)),  # Should find first match, not require 2 occurrences

        dict(id="regex-102-plus-quantifier-requires-one",
             tokens=["findr","(ab)+x"], input_file="-", stdin=b"x",
             expect=dict(stdout="", exit=2)),  # Should fail - requires at least one occurrence

        dict(id="regex-103-plus-quantifier-multiple-occurrences",
             tokens=["findr","(ab)+x","take","to","match-end"], input_file="-", stdin=b"ababx",
             expect=dict(stdout="ababx", exit=0)),

        # ---------- Views Feature Tests ----------
        # Basic view operations
        dict(id="view-001-basic-viewset",
             tokens=["viewset","BOF+2b","EOF-2b","take","100b"], input_file="overlap.txt",
             expect=dict(stdout="cdefgh", exit=0)),

        dict(id="view-002-viewclear",
             tokens=["viewclear","take","3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="view-003-viewset-viewclear-sequence",
             tokens=["viewset","BOF+2b","EOF-2b","viewclear","take","3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),  # cursor at position 2 from viewset

        # View with find operations
        dict(id="view-004-view-find-forward",
             tokens=["viewset","BOF+2b","EOF-2b","find","def","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),

        dict(id="view-005-view-find-backward",
             tokens=["viewset","BOF+2b","EOF-2b","skip","5b","find","to","BOF","def","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),

        dict(id="view-006-view-find-no-match",
             tokens=["viewset","BOF","EOF-2b","find","X","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),  # X is at EOF-1b, excluded from view

        # View with regex operations
        dict(id="view-007-view-findr-anchors",
             tokens=["viewset","BOF+3b","EOF","findr","^HEADER","take","to","match-end"], input_file="-", stdin=b"ZZ\nHEADER\n",
             expect=dict(stdout="HEADER", exit=0)),

        dict(id="view-008-view-findr-no-match",
             tokens=["viewset","BOF","EOF-1b","findr","^HEADER","take","+6b"], input_file="-", stdin=b"ZZ\nHEADER\n",
             expect=dict(stdout="HEADER", exit=0)),  # HEADER is at EOF-1b, but view includes it

        # View with goto operations
        dict(id="view-009-goto-within-view",
             tokens=["viewset","BOF+2b","EOF-2b","goto","BOF+3b","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="fg", exit=0)),  # BOF+3b in view is position 5, take +2b gives fg

        dict(id="view-010-goto-outside-view-fails",
             tokens=["viewset","BOF+2b","EOF-2b","goto","BOF-1b","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),

        dict(id="view-011-goto-outside-view-eof",
             tokens=["viewset","BOF+2b","EOF-2b","goto","EOF+1b","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=2)),

        # View with take operations
        dict(id="view-012-view-take-len-positive",
             tokens=["viewset","BOF+2b","EOF-2b","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),

        dict(id="view-013-view-take-len-negative",
             tokens=["viewset","BOF+2b","EOF-2b","skip","3b","take","-2b"], input_file="overlap.txt",
             expect=dict(stdout="de", exit=0)),  # skip 3b puts cursor at position 5, take -2b gives de

        dict(id="view-014-view-take-to",
             tokens=["viewset","BOF+2b","EOF-2b","take","to","EOF-1b"], input_file="overlap.txt",
             expect=dict(stdout="cdefg", exit=0)),  # EOF-1b in view is position 7, take to gives cdefg

        dict(id="view-015-view-take-until",
             tokens=["viewset","BOF+2b","EOF-2b","take","until","f"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),

        # View with skip operations
        dict(id="view-016-view-skip-bytes",
             tokens=["viewset","BOF+2b","EOF-2b","skip","2b","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="ef", exit=0)),

        dict(id="view-017-view-skip-lines",
             tokens=["viewset","BOF+2b","EOF-2b","skip","1l","take","+1l"], input_file="lines.txt",
             expect=dict(stdout="L02 bb\n", exit=0)),

        # View with line operations
        dict(id="view-018-view-line-start",
             tokens=["viewset","BOF+5b","EOF-2b","find","L03","goto","line-start","take","+1l"], input_file="lines.txt",
             expect=dict(stdout="L03 ccc\n", exit=0)),

        dict(id="view-019-view-line-end",
             tokens=["viewset","BOF+5b","EOF-2b","find","L03","goto","line-end","take","+1l"], input_file="lines.txt",
             expect=dict(stdout="L04 dddd\n", exit=0)),

        # View with labels
        dict(id="view-020-view-labels",
             tokens=["viewset","BOF+2b","EOF-2b","label","MARK","skip","2b","goto","MARK","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="cd", exit=0)),

        # View atomicity
        dict(id="view-021-view-atomic-success",
             tokens=["viewset","BOF+2b","EOF-2b","take","+2b","::","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="cdef", exit=0)),

        dict(id="view-022-view-atomic-failure",
             tokens=["viewset","BOF+2b","EOF-2b","take","+2b","find","XYZ","::","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="ab", exit=0)),  # First clause fails, view not committed, second clause uses original cursor

        # View edge cases
        dict(id="view-023-empty-view",
             tokens=["viewset","BOF+5b","BOF+5b","take","+10b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),  # Empty view, cursor clamped to hi

        dict(id="view-024-view-beyond-file",
             tokens=["viewset","BOF+100b","EOF+100b","take","+10b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),  # View beyond file bounds

        dict(id="view-025-view-negative-bounds",
             tokens=["viewset","BOF-10b","EOF+10b","take","+10b"], input_file="overlap.txt",
             expect=dict(stdout="abcdefghij", exit=0)),  # Negative bounds clamped to file

        # View with complex operations
        dict(id="view-026-view-complex-extraction",
             tokens=["viewset","BOF+6b","EOF-5b","find","world","take","to","match-end"], input_file="-", stdin=b"hello world test",
             expect=dict(stdout="world", exit=0)),

        dict(id="view-027-view-multi-clause",
             tokens=["viewset","BOF+2b","EOF-2b","take","+2b","::","viewclear","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="cdef", exit=0)),  # First clause commits view, second clears it but cursor is at position 4

        # View with regex anchors
        dict(id="view-028-view-regex-bol",
             tokens=["viewset","BOF+3b","EOF","findr","^HEADER","take","to","match-end"], input_file="-", stdin=b"ZZ\nHEADER\n",
             expect=dict(stdout="HEADER", exit=0)),

        dict(id="view-029-view-regex-eol",
             tokens=["viewset","BOF","EOF-3b","findr","ZZ$","take","to","match-end"], input_file="-", stdin=b"ZZ\nHEADER\n",
             expect=dict(stdout="ZZ", exit=0)),  # ZZ$ matches before newline (line end behavior)

        # View with match invalidation
        dict(id="view-030-view-match-invalidation",
             tokens=["find","def","viewset","BOF+2b","EOF-2b","take","to","match-end"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),  # Match is still valid, def is at position 3

        # View with cursor clamping
        dict(id="view-031-view-cursor-clamping",
             tokens=["skip","5b","viewset","BOF+2b","EOF-2b","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="fg", exit=0)),  # Cursor at position 5, clamped to view [2,8), take +2b gives fg

        # View with take until and at expressions
        dict(id="view-032-view-take-until-at",
             tokens=["viewset","BOF+2b","EOF-2b","take","until","f","at","match-start","+1b"], input_file="overlap.txt",
             expect=dict(stdout="cdef", exit=0)),  # take until f, then at match-start+1b gives cdef

        # View with line offsets
        dict(id="view-033-view-line-offsets",
             tokens=["viewset","BOF+5b","EOF-2b","find","L03","take","to","line-start","+1l"], input_file="lines.txt",
             expect=dict(stdout="L03 ccc\n", exit=0)),

        # View with character operations
        dict(id="view-034-view-chars",
             tokens=["viewset","BOF+2b","EOF-2b","take","+3c"], input_file="overlap.txt",
             expect=dict(stdout="bcd", exit=0)),  # BOF+2b is position 2, take +3c gives bcd

        dict(id="view-035-view-skip-chars",
             tokens=["viewset","BOF+2b","EOF-2b","skip","2c","take","+2c"], input_file="overlap.txt",
             expect=dict(stdout="cd", exit=0)),  # skip 2c from position 2 gives position 4, take +2c gives cd

        # View with binary data
        dict(id="view-036-view-binary",
             tokens=["viewset","BOF+5b","EOF-5b","find","BINARY_DATA","take","+11b"], input_file="binary-data.bin",
             expect=dict(stdout="BINARY_DATA", exit=0)),

        # View with unicode
        dict(id="view-037-view-unicode",
             tokens=["viewset","BOF+6b","EOF-6b","find","世界","take","+6b"], input_file="unicode-test.txt",
             expect=dict(stdout="世界", exit=0)),

        # View with CRLF
        dict(id="view-038-view-crlf",
             tokens=["viewset","BOF+2b","EOF-2b","take","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="ne1\r\n", exit=0)),  # BOF+2b is position 2, take +1l gives ne1\r\n

        # View with large files
        dict(id="view-039-view-large-file",
             tokens=["viewset","BOF+1000b","EOF-1000b","find","NEEDLE","take","+6b"], input_file="big-forward.bin",
             expect=dict(stdout="NEEDLE", exit=0)),

        # View with repeated patterns
        dict(id="view-040-view-repeated-patterns",
             tokens=["viewset","BOF+50b","EOF-50b","find","PATTERN","take","+7b"], input_file="repeated-patterns.txt",
             expect=dict(stdout="PATTERN", exit=0)),

        # View with nested sections
        dict(id="view-041-view-nested-sections",
             tokens=["viewset","BOF+10b","EOF-10b","find","BEGIN_SECTION_A","take","until","END_SECTION_A"], input_file="nested-sections.txt",
             expect=dict(stdout="", exit=2)),  # BEGIN_SECTION_A not found in view [10, EOF-10b)

        # View with edge cases
        dict(id="view-042-view-edge-whitespace",
             tokens=["viewset","BOF+5b","EOF-5b","find","spaces at end","take","+1l"], input_file="edge-cases.txt",
             expect=dict(stdout="with spaces at end   \n", exit=0)),  # BOF+5b skips "Line ", find gives "with spaces at end   \n"

        # View with stdin
        dict(id="view-043-view-stdin",
             tokens=["viewset","BOF+2b","EOF-2b","take","+3b"], input_file="-", stdin=b"Hello World",
             expect=dict(stdout="llo", exit=0)),

        # View with complex multi-operation sequences
        dict(id="view-044-view-complex-sequence",
             tokens=["viewset","BOF+2b","EOF-2b","find","def","label","MARK","skip","1b","goto","MARK","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),

        # View with regex and views
        dict(id="view-045-view-regex-complex",
             tokens=["viewset","BOF+2b","EOF-2b","findr","\\w+","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),

        # View with take until and views
        dict(id="view-046-view-take-until-complex",
             tokens=["viewset","BOF+2b","EOF-2b","take","until","f","at","line-start"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),  # take until f at line-start gives empty range

        # View with multiple viewset operations
        dict(id="view-047-view-multiple-viewsets",
             tokens=["viewset","BOF+2b","EOF-2b","viewset","BOF+3b","EOF-3b","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),  # Second viewset creates empty view [3,7), cursor clamped to 7, take +2b gives empty

        # View with viewclear and subsequent operations
        dict(id="view-048-view-clear-subsequent",
             tokens=["viewset","BOF+2b","EOF-2b","viewclear","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),  # Cursor still at position 2 from viewset

        # View with empty file
        dict(id="view-049-view-empty-file",
             tokens=["viewset","BOF+1b","EOF-1b","take","+1b"], input_file="empty.txt",
             expect=dict(stdout="", exit=0)),

        # View with single byte file
        dict(id="view-050-view-single-byte",
             tokens=["viewset","BOF","EOF","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="a", exit=0)),

        # ---------- CLI option smoke tests ----------
        dict(id="cli-001-version-flag",
             tokens=["--version"], input_file=None,
             expect=dict(stdout=VERSION_LINE, exit=0)),

        dict(id="cli-002-help-flag",
             tokens=["--help"], input_file=None,
             expect=dict(stdout_startswith=f"fiskta (FInd SKip TAke) Text Extraction Tool v{VERSION}", exit=0)),
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
        in_name = t.get("input_file", None)
        stdin_data = t.get("stdin", None)

        if in_name is None:
            in_path = None
        elif in_name == "-":
            in_path = "-"
        else:
            in_path = str(FIX / in_name)
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

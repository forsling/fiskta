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
                # add A33 (evict LRU A01), then try goto A01
                "label","A33","goto","A01","take","+1b"
             ], input_file="labels-evict.txt",
             expect=dict(stdout="", exit=2)),

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
             expect=dict(stdout="", exit=2)),

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
             expect=dict(stdout="TEXT_START\x00\x01\x02\x03BINARY_DATA\xff\xfe\xfd", exit=0)),  # Fixed bytes

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

        dict(id="crlf-117-cr-only-basic",
             tokens=["take","+1l"], input_file="cr-only.txt",
             expect=dict(stdout="Line1\r", exit=0)),  # CR is treated as regular byte, not line terminator

        dict(id="crlf-118-cr-only-multiple",
             tokens=["take","+2l"], input_file="cr-only.txt",
             expect=dict(stdout="Line1\rLine2\r", exit=0)),

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

        # Cursor law retained with chars
        dict(id="utf8-005-empty-char-capture-no-move",
             tokens=["skip","5b","take","to","cursor+0c","::","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="efgh", exit=0)),
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


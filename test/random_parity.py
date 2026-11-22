#!/usr/bin/env python3
"""Random Parity messages, encoded and converted by `capnp` and capnp-eval.

Usage: random_parity.py BUILD_DIR [COUNT] [SEED]. Every output is
byte-compared, and the exit status is the number of failing cases.
"""
import os
import random
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 40
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 20
CLI = os.path.join(BUILD, "capnp-eval")
SCHEMA = os.path.join(ROOT, "schema/parity.capnp")
INC = os.path.join(os.path.dirname(os.path.dirname(shutil.which("capnp"))), "include")
R = random.Random(SEED)


def rtext():
    alphabet = ["a", "b", "z", " ", "\\n", "\\t", "\\\"", "\\\\", "é", "\\x01", "\\x7f", "日", "😀", "_"]
    return '"' + "".join(R.choice(alphabet) for _ in range(R.randint(0, 12))) + '"'


def rdata():
    return '0x"' + " ".join("%02x" % R.randint(0, 255) for _ in range(R.randint(0, 9))) + '"'


def rint(bits, signed):
    if signed:
        lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    else:
        lo, hi = 0, (1 << bits) - 1
    v = R.choice([lo, hi, 0, 1, -1 if signed else 2, R.randint(lo, hi)])
    sign, mag = ("-", -v) if v < 0 else ("", v)
    form = R.random()
    if form < 0.15:
        return sign + "0x%x" % mag
    if form < 0.3:
        return sign + "0%o" % mag
    if form < 0.35 and bits == 64 and not signed:
        return str(mag + (R.randint(1, 3) << 64))
    return str(v)


def rfloat(f32):
    c = R.random()
    if c < 0.1:
        return R.choice(["inf", "-inf", "nan"])
    if c < 0.2:
        return R.choice(["0", "-0.0", "1", "-1"])
    if f32:
        v = struct.unpack("<f", struct.pack("<f", R.uniform(-1e6, 1e6)))[0]
        return repr(v)
    return repr(R.choice([R.uniform(-1e300, 1e300), R.uniform(-1, 1), R.random() * 1e-300]))


def rpoint():
    parts = []
    if R.random() < 0.7:
        parts.append("x = " + rint(32, True))
    if R.random() < 0.7:
        parts.append("y = " + rint(32, True))
    return "(" + ", ".join(parts) + ")"


def rlist(gen, maxn=6):
    return "[" + ", ".join(gen() for _ in range(R.randint(0, maxn))) + "]"


def rjson(depth=0):
    c = R.randint(0, 6 if depth < 2 else 3)
    if c == 0:
        return "(null = void)"
    if c == 1:
        return "(boolean = %s)" % R.choice(["true", "false"])
    if c == 2:
        return "(number = %s)" % R.choice(["0", "1.5", "-3", "1e10", "123456789"])
    if c == 3:
        return "(string = %s)" % rtext()
    if c == 4:
        return "(array = %s)" % rlist(lambda: rjson(depth + 1), 3)
    return "(object = %s)" % rlist(lambda: "(name = %s, value = %s)" % (rtext(), rjson(depth + 1)), 3)


FIELDS = [
    ("vVoid", lambda: "void"),
    ("vBool", lambda: R.choice(["true", "false"])),
    ("vI8", lambda: rint(8, True)), ("vI16", lambda: rint(16, True)),
    ("vI32", lambda: rint(32, True)), ("vI64", lambda: rint(64, True)),
    ("vU8", lambda: rint(8, False)), ("vU16", lambda: rint(16, False)),
    ("vU32", lambda: rint(32, False)), ("vU64", lambda: rint(64, False)),
    ("vF32", lambda: rfloat(True)), ("vF64", lambda: rfloat(False)),
    ("vText", rtext), ("vData", rdata),
    ("vEnum", lambda: R.choice(["red", "green", "blue"])),
    ("vStruct", rpoint),
    ("lVoid", lambda: rlist(lambda: "void")),
    ("lBool", lambda: rlist(lambda: R.choice(["true", "false"]), 70)),
    ("lI8", lambda: rlist(lambda: rint(8, True))),
    ("lI16", lambda: rlist(lambda: rint(16, True))),
    ("lI32", lambda: rlist(lambda: rint(32, True))),
    ("lI64", lambda: rlist(lambda: rint(64, True))),
    ("lU8", lambda: rlist(lambda: rint(8, False))),
    ("lU16", lambda: rlist(lambda: rint(16, False))),
    ("lU32", lambda: rlist(lambda: rint(32, False))),
    ("lU64", lambda: rlist(lambda: rint(64, False))),
    ("lF32", lambda: rlist(lambda: rfloat(True))),
    ("lF64", lambda: rlist(lambda: rfloat(False))),
    ("lText", lambda: rlist(rtext)),
    ("lData", lambda: rlist(rdata)),
    ("lEnum", lambda: rlist(lambda: R.choice(["red", "green", "blue"]))),
    ("lStruct", lambda: rlist(rpoint)),
    ("lNested", lambda: rlist(lambda: rlist(rpoint, 3), 3)),
    ("renamed", rtext), ("b64Data", rdata), ("hexData", rdata),
    ("flatGroup", lambda: "(alpha = %s, beta = %s)" % (rint(32, False), rtext())),
    ("discUnion", lambda: R.choice(["(optA = (aVal = %s))" % rtext(), "(optB = (bNum = %s))" % rint(32, False)])),
    ("valUnion", lambda: R.choice(["(vTxt = %s)" % rtext(), "(vNum = %s)" % rint(32, False)])),
    ("voidUnion", lambda: R.choice(["(none = void)", "(some = %s)" % rint(32, False)])),
    ("extDisc", lambda: R.choice(["(num = %s)" % rint(32, False), "(flat = (subName = %s, subCode = %s))" % (rtext(), rint(16, False))])),
    ("rawJson", lambda: rjson()),
    ("boxedText", lambda: "(value = %s)" % rtext()),
    ("paired", lambda: "(left = %s, right = %s)" % (rdata(), rlist(lambda: rint(32, True)))),
    ("nestedBox", lambda: "(value = (value = %s))" % rpoint()),
]
UNION = [("uQuiet", lambda: "void"), ("uNote", rtext), ("uCount", lambda: rint(64, False))]


def gen():
    parts = []
    fs = list(FIELDS)
    R.shuffle(fs)
    for name, g in fs:
        if R.random() < 0.5:
            parts.append("%s = %s" % (name, g()))
    if R.random() < 0.6:
        name, g = R.choice(UNION)
        parts.append("%s = %s" % (name, g()))
    return "(" + ", ".join(parts) + ")"


def run(cmd, data):
    p = subprocess.run(cmd, input=data, capture_output=True)
    return p.returncode, p.stdout, p.stderr


def splits_early(text):
    """True when `capnp`'s readOneText would end the message before the end.

    It skips comments but never enters its string states, so parentheses
    inside string literals still count.
    """
    depth = 0
    comment = False
    for i, ch in enumerate(text):
        if comment:
            comment = ch != "\n"
        elif ch == "#":
            comment = True
        elif ch == "(":
            depth += 1
        elif ch == ")" and depth > 0:
            depth -= 1
            if depth == 0:
                return text[i + 1:].strip() != ""
    return False


def main():
    fails = 0
    for i in range(N):
        txt = gen().encode()
        ref_rc, ref_bin, ref_err = run(["capnp", "encode", "-I" + INC, SCHEMA, "Parity"], txt)
        our_rc, our_bin, our_err = run([CLI, "encode", "-I" + INC, SCHEMA, "Parity"], txt)
        bad = []
        if (ref_rc == 0) != (our_rc == 0):
            bad.append("encode status %d/%d" % (ref_rc, our_rc))
        elif ref_rc == 0 and ref_bin != our_bin:
            bad.append("encode bytes")
        if ref_rc == 0:
            seg = "--segment-size=%d" % R.randint(1, 64)
            for fmt in ["binary", "packed"]:
                a = run(["capnp", "convert", seg, "-I" + INC, "binary:" + fmt, SCHEMA, "Parity"], ref_bin)
                b = run([CLI, "convert", seg, "-I" + INC, "binary:" + fmt, SCHEMA, "Parity"], ref_bin)
                if a[0] != b[0] or a[1] != b[1]:
                    bad.append("convert %s %s rc %d/%d" % (fmt, seg, a[0], b[0]))
            for fmt in ["packed", "canonical", "text", "json", "flat"]:
                for short in ([], ["--short"]) if fmt in ("text", "json") else ([],):
                    a = run(["capnp", "convert"] + short + ["-I" + INC, "binary:" + fmt, SCHEMA, "Parity"], ref_bin)
                    b = run([CLI, "convert"] + short + ["-I" + INC, "binary:" + fmt, SCHEMA, "Parity"], ref_bin)
                    if a[0] != b[0] or a[1] != b[1]:
                        bad.append("convert %s%s rc %d/%d" % (fmt, "".join(short), a[0], b[0]))
                    if fmt in ("text", "json") and a[0] == 0 and not (
                            fmt == "text" and splits_early(a[1].decode())):
                        c = run(["capnp", "convert", "-I" + INC, fmt + ":canonical", SCHEMA, "Parity"], a[1])
                        d = run([CLI, "convert", "-I" + INC, fmt + ":canonical", SCHEMA, "Parity"], a[1])
                        if c[0] != d[0] or c[1] != d[1]:
                            bad.append("convert %s:canonical%s rc %d/%d" % (fmt, "".join(short), c[0], d[0]))
        if bad:
            fails += 1
            sys.stderr.write("case %d: %s\n%s\n" % (i, "; ".join(bad), txt.decode()))
    print("random parity: seed %d cases %d fails %d" % (SEED, N, fails))
    return min(fails, 100)


sys.exit(main())

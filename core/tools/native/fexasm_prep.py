"""Assemble FEX's 32-bit ASM unit tests for tests/native/fexrun.

For every unittests/32Bit_ASM/**.asm in the FEX checkout this writes
  <out>/<name>.bin  the test assembled exactly as FEX's CMake does
                    (BITS 32, org 10000h, DS/ES = 0x17 prologue, ret epilogue)
  <out>/<name>.cfg  the test's JSON header in fexrun's line format
and <out>/manifest.txt listing the tests, tagged known_fail / disabled
from FEX's Known_Failures and Disabled_Tests lists.

The .cfg format (one item per line):
  mode 32|64
  feat <host feature bitmask>
  region <addr> <size>
  mem <addr> <hex bytes>
  reg <name index> <value> [<value> ...]
  env <KEY> <VALUE>
Name indexes and feature bits follow FEX's Scripts/json_config_parse.py.

Usage: python fexasm_prep.py --fex D:/code/FEX --out build/native/fexasm
                             --nasm D:/tools/nasm-2.16.03/nasm.exe
       python fexasm_prep.py --suite tests/native/sysops --out build/native/sysops ...
"""

import argparse
import json
import os
import subprocess
import sys

PROLOGUE = "BITS 32\norg 10000h\nmov eax, 0x17\nmov ds, ax\nmov es, ax\n"
EPILOGUE = "ret\n"

REG_INDEX = {"RIP": 0, "RAX": 1, "RBX": 2, "RCX": 3, "RDX": 4, "RSI": 5,
             "RDI": 6, "RBP": 7, "RSP": 8, "GS": 33, "FS": 34}
REG_INDEX.update({"R%d" % n: n + 1 for n in range(8, 16)})
REG_INDEX.update({"XMM%d" % n: 17 + n for n in range(16)})
REG_INDEX.update({"MM%d" % n: 35 + n for n in range(9)})

FEATURES = ["3DNOW", "SSE4A", "AVX", "RAND", "SHA", "CLZERO", "BMI1", "BMI2",
            "CLWB", "LINUX", "AES256", "AFP", "SSSE3", "SSE4.1", "SSE4.2",
            "AES", "PCLMUL", "MOVBE", "ADX", "XSAVE", "RDPID", "CLFLOPT",
            "FSGSBASE", "EMMI", "AVX_VNNI"]
FEATURE_BIT = {name: 1 << i for i, name in enumerate(FEATURES)}


def parse_hexstring(s):
    """Same byte order (and the same 0x quirk) as FEX's parse_hexstring."""
    data = []
    for num in s.split(" "):
        if s.startswith("0x"):
            num = num[2:]
        while num:
            data.append(int(num[-2:], 16))
            num = num[:-2]
    return data


def config_lines(header):
    obj = {k.upper(): v for k, v in json.loads(header).items()}
    mode = 64
    if "MODE" in obj:
        mode = 32 if obj["MODE"].upper() == "32BIT" else 64
    lines = ["mode %d" % mode]
    feat = 0
    for name in obj.get("HOSTFEATURES", []):
        feat |= FEATURE_BIT[name.upper()]
    lines.append("feat 0x%x" % feat)
    for addr, size in obj.get("MEMORYREGIONS", {}).items():
        lines.append("region 0x%x 0x%x" % (int(addr, 0), int(size, 0)))
    for addr, text in obj.get("MEMORYDATA", {}).items():
        data = parse_hexstring(text)
        lines.append("mem 0x%x %s" % (int(addr, 0), "".join("%02x" % b for b in data)))
    for reg, val in obj.get("REGDATA", {}).items():
        vals = val if isinstance(val, list) else [val]
        lines.append("reg %d %s" % (REG_INDEX[reg.upper()],
                                    " ".join("0x%x" % int(v, 0) for v in vals)))
    for key, val in obj.get("ENV", {}).items():
        lines.append("env %s %s" % (key, val))
    return lines


def read_list(path, prefix):
    names = set()
    if not os.path.exists(path):
        return names
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and line.startswith(prefix):
                names.add(line[len(prefix):])
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fex", help="FEX checkout; the suite is its unittests/32Bit_ASM")
    ap.add_argument("--suite", help="any other directory of tests in the same format")
    ap.add_argument("--out", required=True)
    ap.add_argument("--nasm", required=True)
    args = ap.parse_args()
    if not args.fex and not args.suite:
        ap.error("one of --fex or --suite is required")

    suite = args.suite or os.path.join(args.fex, "unittests", "32Bit_ASM")
    known = read_list(os.path.join(suite, "Known_Failures"), "Test_32Bit_")
    disabled = read_list(os.path.join(suite, "Disabled_Tests"), "Test_32Bit_")

    tests = []
    for root, _, files in os.walk(suite):
        for fn in files:
            if fn.endswith(".asm"):
                tests.append(os.path.relpath(os.path.join(root, fn), suite).replace("\\", "/"))
    tests.sort()

    manifest = []
    failed = 0
    for rel in tests:
        name = rel[:-4]
        out_base = os.path.join(args.out, name)
        os.makedirs(os.path.dirname(out_base), exist_ok=True)
        with open(os.path.join(suite, rel), encoding="utf-8") as f:
            text = f.read()

        tmp = out_base + "_tmp.asm"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write(PROLOGUE + text + ("" if text.endswith("\n") else "\n") + EPILOGUE)
        r = subprocess.run([args.nasm, tmp, "-o", out_base + ".bin"],
                           capture_output=True, text=True)
        os.remove(tmp)
        if r.returncode:
            print("nasm failed: %s\n%s" % (rel, r.stderr.strip()), file=sys.stderr)
            failed += 1
            continue

        header = text.split("%ifdef CONFIG")
        lines = ["mode 64"]
        if len(header) > 1:
            lines = config_lines(header[1].split("%endif")[0].strip())
        with open(out_base + ".cfg", "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")

        tags = []
        if rel in known:
            tags.append("known_fail")
        if rel in disabled:
            tags.append("disabled")
        manifest.append(" ".join([name] + tags))

    with open(os.path.join(args.out, "manifest.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(manifest) + "\n")
    print("prepared %d tests (%d nasm failures) in %s" % (len(manifest), failed, args.out))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

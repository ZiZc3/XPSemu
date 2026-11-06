"""The core's knobs, from its source: every environment variable it reads
(getenv, the app passes them through debug.hakux.env) and every
debug.hakux.* system property, with where each is read and the comment that
says what it does (the nearest one naming it, else the one just above).

Usage: python knobs.py [--md]   (--md: a Markdown table)
"""

import argparse
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
DIRS = ["native", os.path.join("hw", "xbox", "hle")]
ENV = re.compile(r'getenv(?:_int_clamped)?\(\s*"([A-Z0-9_]+)"')
PROP = re.compile(r'"(debug\.hakux\.[a-z0-9_.]+)"')


def is_comment(line):
    s = line.lstrip()
    return s.startswith(("/*", "*", "//")) or "/*" in line or "//" in line


def clean(line):
    text = line
    m = re.search(r"/\*(.*?)(\*/|$)|//(.*)$", line)
    if m and not line.lstrip().startswith("*"):
        text = m.group(1) if m.group(1) is not None else m.group(3)
    text = re.sub(r"^\s*(/\*+|\*+|//)\s?", "", text or "")
    return re.sub(r"\*/\s*$", "", text).strip()


def comment_for(lines, i, name):
    """The comment that explains name: the closest comment line in the file
    naming it (above first: a header's, a block's), else the comment block
    just above the line that reads it."""
    short = name.split(".")[-1] if name.startswith("debug.hakux.") else name
    for order in (range(i, -1, -1), range(i + 1, len(lines))):
        for j in order:
            line = lines[j]
            if (name in line or (short != name and "debug.hakux." + short in line)) and is_comment(line):
                text = clean(line)
                if len(text) > len(name) + 3:
                    return text
    j = i - 1
    block = []
    while j >= 0 and i - j <= 15 and is_comment(lines[j]) and len(block) < 3:
        block.insert(0, clean(lines[j]))
        j -= 1
    return " ".join(b for b in block if b)


def scan():
    found = {}
    for d in DIRS:
        for base, _, files in os.walk(os.path.join(ROOT, d)):
            for f in sorted(files):
                if not f.endswith((".c", ".cpp", ".h")):
                    continue
                path = os.path.join(base, f)
                rel = os.path.relpath(path, ROOT).replace("\\", "/")
                lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
                for i, line in enumerate(lines):
                    for kind, rx in (("env", ENV), ("prop", PROP)):
                        for m in rx.finditer(line):
                            name = m.group(1)
                            entry = found.setdefault((kind, name), {"where": [], "what": ""})
                            entry["where"].append("%s:%d" % (rel, i + 1))
                            if not entry["what"]:
                                entry["what"] = comment_for(lines, i, name)
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--md", action="store_true")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    found = scan()
    keys = sorted(found, key=lambda k: (k[0] != "prop", k[1]))
    nprop = sum(1 for k in keys if k[0] == "prop")
    if args.md:
        print("%d debug.hakux properties, %d environment variables.\n" % (nprop, len(keys) - nprop))
        print("| Knob | Kind | Read at | What |\n|---|---|---|---|")
        for k in keys:
            e = found[k]
            where = e["where"][0] + (" (+%d)" % (len(e["where"]) - 1) if len(e["where"]) > 1 else "")
            print("| `%s` | %s | %s | %s |" % (k[1], k[0], where, e["what"].replace("|", "\\|")))
    else:
        for k in keys:
            e = found[k]
            print("%-34s %-4s %-42s %s" % (k[1], k[0], e["where"][0], e["what"][:90]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

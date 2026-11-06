"""Kernel import census from census.sh output.

Reads the "== <image>" and "hakuX-native-imports: title <id> <ordinals>"
lines that tools/native/census.sh prints and writes a Markdown report: per
title, how many kernel exports it imports and how many of those functions
this kernel doesn't implement (nc_kernel_impl in native/); the union over a
chosen set of titles, grouped by name prefix (subsystem); and the functions
imported but not implemented, with the titles that import them.

Usage: python imports.py census.txt [--set "Project Gotham,Fable,Otogi,Dead or Alive"]
"""

import argparse
import collections
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
TABLE = os.path.join(ROOT, "native", "kernel", "kexports_table.h")


def load_names():
    names = {}
    with open(TABLE, encoding="utf-8") as f:
        for line in f:
            m = re.match(r'NC_KEXPORT\((\d+), "([^"]+)", (\w+), (\d+)\)', line)
            if m:
                names[int(m.group(1))] = (m.group(2), m.group(3))
    return names


def load_impl():
    done = set()
    for d, _, files in os.walk(os.path.join(ROOT, "native")):
        for f in files:
            if f.endswith(".c"):
                with open(os.path.join(d, f), encoding="utf-8", errors="replace") as fh:
                    done |= set(re.findall(r'nc_kernel_impl\(\w+, "(\w+)"', fh.read()))
    return done


def parse(path):
    titles = []  # (image, title id, set(ordinals))
    image = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if line.startswith("== "):
                image = os.path.basename(line[3:])
            m = re.search(r"hakuX-native-imports: title (\w+) ([\d,]+)", line)
            if m and image:
                titles.append((image, m.group(1), {int(x) for x in m.group(2).split(",")}))
    return titles


def prefix(name):
    m = re.match(r"(Xc|Xe|Xbox|Av|Dbg|Ex|Hal|Io|Ke|Kf|Ki|Mm|Nt|Ob|Ps|Rtl|Interlocked|Kd|Phy|Write|Read|Launch|Idex|Hal)", name)
    return m.group(1) if m else "other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("census")
    ap.add_argument("--set", default="Project Gotham,Fable,Otogi,Dead or Alive")
    args = ap.parse_args()
    names = load_names()
    done = load_impl()
    titles = parse(args.census)
    picks = [p.strip().lower() for p in args.set.split(",") if p.strip()]

    def missing(o):
        name, abi = names.get(o, ("?%d" % o, "?"))
        return abi != "NC_KABI_DATA" and name not in done

    out = ["| Title | Title ID | Kernel imports | Not implemented |", "|---|---|---|---|"]
    chosen = set()
    users = collections.defaultdict(list)
    for image, tid, ords in titles:
        short = image.replace(".xiso.iso", "").replace(".iso", "")
        gaps = [o for o in ords if missing(o)]
        for o in gaps:
            users[o].append(short)
        out.append("| %s | %s | %d | %d |" % (short, tid, len(ords), len(gaps)))
        if any(p in image.lower() for p in picks):
            chosen |= ords
    every = set().union(*(o for _, _, o in titles)) if titles else set()
    out += ["", "Union over the chosen set (%s): **%d** exports; over all titles: **%d**." %
            (args.set, len(chosen), len(every)), ""]

    groups = collections.defaultdict(list)
    for o in sorted(chosen):
        name, abi = names.get(o, ("?%d" % o, "?"))
        groups[prefix(name)].append("%s (%d)" % (name, o))
    out += ["| Prefix | Count | Exports |", "|---|---|---|"]
    for g in sorted(groups, key=lambda k: -len(groups[k])):
        out.append("| %s | %d | %s |" % (g, len(groups[g]), ", ".join(groups[g])))
    out += ["", "Imported, not implemented (a call stops the title): **%d**." % len(users), "",
            "| Export | Titles | Which |", "|---|---|---|"]
    for o in sorted(users, key=lambda o: (-len(users[o]), names[o][0])):
        out.append("| %s (%d) | %d | %s |" % (names[o][0], o, len(users[o]), ", ".join(sorted(users[o]))))
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()

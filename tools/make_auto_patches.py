#!/usr/bin/env python3
# Builds ps5/ui/patches-auto.inc: the widescreen and 60 FPS patches XOG applies by itself, from Jay's
# Magic Patches (github.com/JayYardley/Xbox-Magic-Patches-by-Jay), read with plain HTTP, kept in
# K:\XOG\patches so a rebuild doesn't fetch them again. The list comes from the catalog XPSemu built.
# A file's authors stay in its text (author=). 21:9 patches are left out: the picture is 16:9.
import os
import re
import sys
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
CAT = os.path.join(HERE, "..", "ps5", "ui", "patch-catalog.h")
OUT = os.path.join(HERE, "..", "ps5", "ui", "patches-auto.inc")
CACHE = "patches-cache"
BASE = "https://raw.githubusercontent.com/JayYardley/Xbox-Magic-Patches-by-Jay/main/"

line = re.compile(r'^\s*\{ "([^"]*)", "([^"]*)", "([^"]*)", "([^"]*)", "((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)" \},')


def wanted(cat, notes, path):
    text = (cat + " " + notes + " " + path).lower()
    if "21x9" in text or "21:9" in text:
        return None
    fps = "60fps" in text.replace(" ", "")
    wide = "widescreen" in cat.lower()
    return "both" if fps and wide else "60fps" if fps else "widescreen" if wide else None


entries = []
for l in open(CAT, encoding="utf-8", errors="replace"):
    m = line.match(l)
    if not m:
        continue
    kind = wanted(m.group(2), m.group(7), m.group(6))
    if kind:
        entries.append((kind, m.group(6).replace('\\"', '"').replace("\\\\", "\\")))
print(len(entries), "patches:", sum(e[0] == "widescreen" for e in entries), "widescreen,", sum(e[0] == "60fps" for e in entries), "60 FPS,", sum(e[0] == "both" for e in entries), "both")


def fetch(e):
    kind, path = e
    local = os.path.join(CACHE, path)
    if not os.path.exists(local):
        os.makedirs(os.path.dirname(local), exist_ok=True)
        url = BASE + urllib.parse.quote(path)
        req = urllib.request.Request(url, headers={"User-Agent": "xog-build"})
        try:
            data = urllib.request.urlopen(req, timeout=30).read()
        except Exception as x:
            return (kind, path, None, str(x))
        open(local, "wb").write(data)
    return (kind, path, open(local, "rb").read(), None)


with ThreadPoolExecutor(8) as ex:
    got = list(ex.map(fetch, entries))
bad = [g for g in got if g[2] is None]
for b in bad[:20]:
    print("no file:", b[1], b[3])
out = ["// Made by tools/make_auto_patches.py. Patches by their authors (each file's author=), collected as Jay's Magic Patches.\n"]
total = 0
for kind, path, data, _ in got:
    if data is None:
        continue
    total += len(data)
    # as bytes in a C string: any text survives
    s = "".join("\\%03o" % c if (c < 32 or c > 126 or c in (34, 92, 63)) else chr(c) for c in data)
    out.append('{ "%s", "%s", "%s" },\n' % (kind, path.replace("\\", "\\\\").replace('"', '\\"'), s))
open(OUT, "w", encoding="ascii", newline="\n").write("".join(out))
print("wrote", len(got) - len(bad), "files,", total, "bytes;", len(bad), "missing")
sys.exit(0)

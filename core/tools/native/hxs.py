#!/usr/bin/env python3
"""Reads a native-core savestate (.hxs): its header and sections, records
decoded through the field tables (the .def files in native/state), NV2A
registers by offset.

  hxs.py STATE                        header and section table
  hxs.py STATE --rec SECTION TYPE     the section's first record of TYPE, field by field
                                      (records stored before it are decoded to get there)
  hxs.py STATE --nv2a OFF [OFF ...]   NV2A registers (hex offsets) from the NV2A section
  hxs.py A B --diff SECTION TYPE      the two states' records of TYPE, the fields that differ
"""

import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import statelock  # noqa: E402  (the .def parser)

FMT = {"U8": "B", "U16": "H", "U32": "I", "U64": "Q", "I8": "b", "I16": "h", "I32": "i", "I64": "q",
       "BOOL": "?", "F32": "f", "F64": "d"}


class Reader:
    def __init__(self, data, pos=0):
        self.d, self.p = data, pos

    def get(self, n):
        b = self.d[self.p:self.p + n]
        if len(b) != n:
            raise EOFError("past the end")
        self.p += n
        return b

    def u8(self):
        return self.get(1)[0]

    def u32(self):
        return struct.unpack("<I", self.get(4))[0]

    def u64(self):
        return struct.unpack("<Q", self.get(8))[0]


def cstr(b):
    return b.split(b"\0", 1)[0].decode("utf-8", "replace")


def load(path):
    d = open(path, "rb").read()
    r = Reader(d)
    magic, cont = r.u32(), r.u32()
    if magic != 0x31535848:
        raise SystemExit(f"{path}: not a state (magic {magic:08x})")
    hl = r.u32()
    h = Reader(r.get(hl))
    info = {"title_id": h.u32(), "xbe_path": cstr(h.get(128)), "xbe_fp": h.get(20)[:8].hex(),
            "disc_size": h.u64(), "disc_hash": h.u32(), "ram_size": h.u32(), "guest_ns": h.u64(),
            "wall_time": h.u64(), "point": h.u8(), "core_version": cstr(h.get(32)), "core_git": cstr(h.get(48))}
    n = r.u32()
    secs = {}
    for _ in range(n):
        tag, ver, off, ln, crc, _pad = struct.unpack("<IIQQII", r.get(32))
        name = struct.pack("<I", tag).decode().strip()
        body = d[off:off + ln]
        secs[name] = {"version": ver, "off": off, "len": ln, "crc_ok": zlib.crc32(body) & 0xFFFFFFFF == crc,
                      "data": body}
    return cont, info, secs


def defs():
    out = {}
    for s in statelock.sections().values():
        for t, size, fields in s.records:
            out[t] = (s.name, fields)
    return out


def read_rec(r, typ, recs, v):
    """A record as state.c's nc_rec_get reads it: {field: value(s)}."""
    _, fields = recs[typ]
    out = {}
    for f in fields:
        if not (v >= f.since and (not f.until or v < f.until)):
            continue
        if f.kind == "R":
            vals = [read_rec(r, f.ty, recs, v) for _ in range(f.count)]
            out[f.name] = vals[0] if f.count == 1 else vals
        else:
            fmt = FMT[f.ty]
            size = struct.calcsize("<" + fmt)
            vals = struct.unpack("<%d%s" % (f.count, fmt), r.get(size * f.count))
            out[f.name] = vals[0] if f.count == 1 else list(vals)
    return out


def show(rec, indent=""):
    for k, v in rec.items():
        if isinstance(v, dict):
            print(f"{indent}{k}:")
            show(v, indent + "  ")
        elif isinstance(v, list) and len(v) > 16:
            nz = [(i, x) for i, x in enumerate(v) if x]
            print(f"{indent}{k}: [{len(v)}] {len(nz)} non-zero" + (": " + " ".join(
                f"{i}={x:#x}" if isinstance(x, int) else f"{i}={x}" for i, x in nz[:24]) if nz else ""))
        elif isinstance(v, int) and not isinstance(v, bool):
            print(f"{indent}{k}: {v} ({v:#x})")
        else:
            print(f"{indent}{k}: {v}")


# bytes a section stores before its first record (pbstate.c: push-buffer mode on)
LEAD = {"PBD": 1}


def first_rec(secs, sec, typ):
    """The section's records in .def order up to TYPE (each stored once at the
    start: the scalar records sections begin with)."""
    recs = defs()
    s = secs[sec]
    r = Reader(s["data"], LEAD.get(sec, 0))
    order = [t for t, _, _ in statelock.sections()[sec].records]
    # the leading records, as each section's save writes them (first its state record)
    for t in order:
        rec = read_rec(r, t, recs, s["version"])
        if t == typ:
            return rec
    raise SystemExit(f"{typ} isn't in {sec}")


def nv2a_regs(secs, offs):
    s = secs["NV2A"]
    recs = defs()
    r = Reader(s["data"])
    st = read_rec(r, "NcNv2aState", recs, s["version"])
    pages = {}
    for _ in range(st["npages"]):
        pg = r.u32()
        pages[pg] = r.get(4096)
    out = {}
    for off in offs:
        pg, o = off >> 12, off & 0xFFF
        out[off] = struct.unpack_from("<I", pages[pg], o)[0] if pg in pages else 0
    return st, out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("state", nargs="+")
    ap.add_argument("--rec", nargs=2, metavar=("SECTION", "TYPE"))
    ap.add_argument("--nv2a", nargs="+")
    ap.add_argument("--diff", nargs=2, metavar=("SECTION", "TYPE"))
    a = ap.parse_args()
    cont, info, secs = load(a.state[0])
    if a.diff:
        _, _, secs_b = load(a.state[1])
        ra, rb = first_rec(secs, *a.diff), first_rec(secs_b, *a.diff)
        for k in ra:
            if ra[k] != rb.get(k):
                print(f"{k}: {ra[k]} -> {rb.get(k)}")
        return
    if a.rec:
        show(first_rec(secs, *a.rec))
        return
    if a.nv2a:
        st, regs = nv2a_regs(secs, [int(x, 16) for x in a.nv2a])
        print(f"vblanks {st['vblanks']} frame {st['frame_ns']} ns, {st['npages']} pages")
        for off, v in regs.items():
            print(f"  {off:06x}: {v:08x}")
        return
    print(f"container {cont}")
    for k, v in info.items():
        print(f"  {k}: {v:#x}" if k in ("title_id", "disc_hash") else f"  {k}: {v}")
    for name, s in secs.items():
        print(f"  {name:5} v{s['version']}  {s['len']:>10} bytes  crc {'ok' if s['crc_ok'] else 'BAD'}")


if __name__ == "__main__":
    main()

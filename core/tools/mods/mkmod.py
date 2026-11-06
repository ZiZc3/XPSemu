"""mkmod.py SPEC.py XBE [OUT]: a mod.txt from an XBE and a spec module.

The spec (a Python file) sets:
  HEADER   the mod.txt text before the patches (name, description, comments,
           the [file] line)
  ASM      the NASM source (path relative to the spec), assembled at its ORG
  ORG      where the code goes (in the XBE's header page)
  LAST     the label the patched code ends at (the variables from it on
           stay zeros in the file and out of the patches)
  HOOKS    [(va, 'old bytes', 'call' | 'jmp', label, pad)]: a 5-byte call or
           jmp to the label, then pad NOPs
  PATCHES  [(va, 'old bytes', 'new bytes')] (optional)
The header page is raised to 0x1000 bytes (sizeof_headers) when the code is
past the headers. NASM: $NASM, else nasm on PATH.
"""
import os
import runpy
import struct
import subprocess
import sys
import tempfile


def main():
    spec_path, xbe_path = sys.argv[1], sys.argv[2]
    spec = runpy.run_path(spec_path)
    d = open(xbe_path, 'rb').read()
    base = struct.unpack_from('<I', d, 0x104)[0]
    hdr = struct.unpack_from('<I', d, 0x108)[0]
    nsec, secva = struct.unpack_from('<II', d, 0x11C)

    def file_bytes(va, n):
        for i in range(nsec):
            flags, sva, vsize, raw, rsize = struct.unpack_from('<IIIII', d, secva - base + i * 0x38)
            if sva <= va < sva + rsize:
                o = raw + va - sva
                return d[o:o + n]
        if base <= va < base + max(hdr, 0x1000):     # the headers (Halo 2's are 0x2000 bytes)
            return d[va - base:va - base + n]
        raise ValueError(hex(va))

    def hx(b):
        return ' '.join('%02X' % x for x in b)

    org = spec['ORG']
    nasm = os.environ.get('NASM', 'nasm')
    src_path = os.path.join(os.path.dirname(os.path.abspath(spec_path)), spec['ASM'])
    with open(src_path) as f:
        text = f.read()
    with tempfile.TemporaryDirectory() as t:
        src, out, mp = (os.path.join(t, n) for n in ('code.asm', 'code.bin', 'code.map'))
        with open(src, 'w') as f:
            f.write(text + '\n[map symbols %s]\n' % mp.replace('\\', '/'))
        subprocess.run([nasm, '-f', 'bin'] + os.environ.get('NASMFLAGS', '').split() + ['-o', out, src], check=True)
        code = open(out, 'rb').read()
        labels = {}
        for ln in open(mp):
            p = ln.split()
            if len(p) == 3:
                try:
                    labels[p[2]] = int(p[1], 16)
                except ValueError:
                    pass
    assert org + len(code) <= spec.get('END', base + 0x1000), 'code too large: %d bytes' % len(code)
    code = code[:labels[spec['LAST']] - org]
    lines = []
    if org + len(code) > base + hdr:
        lines.append('0x%08X: %s -> %s' % (base + 0x108, hx(file_bytes(base + 0x108, 4)),
                                             hx(struct.pack('<I', 0x1000))))
        assert not any(d[max(hdr, org - base):0x1000]) or org - base < hdr
    for i in range(0, len(code), 32):
        chunk = code[i:i + 32]
        lines.append('0x%08X: %s -> %s' % (org + i, hx(file_bytes(org + i, len(chunk))), hx(chunk)))
    for va, old, kind, label, pad in spec['HOOKS']:
        n = 5 + pad
        have = hx(file_bytes(va, n))
        assert have == old, (hex(va), have, old)
        op = b'\xE8' if kind == 'call' else b'\xE9'
        new = op + struct.pack('<i', labels[label] - (va + 5)) + b'\x90' * pad
        lines.append('0x%08X: %s -> %s' % (va, old, hx(new)))
    for va, old, new in spec.get('PATCHES', []):
        have = hx(file_bytes(va, len(old.split())))
        assert have == old, (hex(va), have, old)
        lines.append('0x%08X: %s -> %s' % (va, old, new))
    out_text = spec['HEADER'] + '\n'.join(lines) + '\n'
    if len(sys.argv) > 3:
        with open(sys.argv[3], 'w', newline='\n') as f:
            f.write(out_text)
    else:
        sys.stdout.write(out_text)
    sys.stderr.write('code %d bytes at 0x%X..0x%X\n' % (len(code), org, org + len(code)))


if __name__ == '__main__':
    main()

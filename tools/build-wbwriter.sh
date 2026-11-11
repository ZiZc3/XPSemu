#!/usr/bin/env bash
# Builds the persistent write-helper payload (ps5/wbwriter/wbwriter.c) with the
# PS5 payload SDK. XPSemu streams it to elfldr (port 9021) once at start-up; it
# runs in its own process (outside the sandbox, un-throttled) and takes cache
# writes over the socket it is handed. See ps5/include/wbwriter_proto.h.
#
#   tools/build-wbwriter.sh <output.elf>
#
# Must end exactly at its section-header table: elfldr reads the ELF up to the
# end of that table and gives the rest of the stream to the helper, so trailing
# bytes would corrupt the first protocol frames. Checked here.
set -euo pipefail
B=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=${1:-$HOME/ps5/xog/build/wbwriter.elf}
sdk=$HOME/ps5/PS5_Vulkan/.deps/native/ps5-payload-sdk
cc=$sdk/bin/prospero-clang
src=$B/ps5/wbwriter/wbwriter.c
inc=$B/ps5/include

[ -x "$cc" ] || { echo "wbwriter: payload SDK clang not found at $cc" >&2; exit 2; }
mkdir -p "$(dirname "$out")"
if [ ! -f "$out" ] || [ "$src" -nt "$out" ] || [ "$inc/wbwriter_proto.h" -nt "$out" ]; then
  "$cc" -std=gnu11 -O2 -g -Wall -Wextra -Werror -I "$inc" -Wl,--build-id=none -o "$out" "$src"
fi

python3 - "$out" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
if len(d) < 64 or d[:6] != b"\x7fELF\x02\x01":
    raise SystemExit("wbwriter: not a little-endian ELF64")
shoff = struct.unpack_from('<Q', d, 40)[0]
entsize, count = struct.unpack_from('<HH', d, 58)
if entsize != 64 or count == 0 or shoff < 64:
    raise SystemExit("wbwriter: no ordinary ELF64 section headers")
if shoff + count * entsize != len(d):
    raise SystemExit("wbwriter: trailing bytes after the section table would corrupt the socket framing")
print("wbwriter: %s (%d bytes, framing ok)" % (sys.argv[1], len(d)))
PY

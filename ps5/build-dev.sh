#!/bin/bash
# XPSemu dev build: ui/xpsemu-dev.h's XPSEMU_DEV set to 1 while it builds
# (the crash journal, the stress test, the stability report), then back to 0.
# The title goes to dist-dev/PPSA97358 with:
#   dev-build.txt    the eboot's id (first 12 hex of its sha256), for the journal
#   dev-symbols.txt  "offset name" of every function, so the dashboard can name
#                    where a crash happened
# dist/ is emptied, so a dev eboot is never taken for a release.
set -e
src=$(cd "$(dirname "$0")/.." && pwd)
cd "$src"
header=ui/xpsemu-dev.h
restore() { sed -i 's/^#define XPSEMU_DEV 1$/#define XPSEMU_DEV 0/' "$header"; }
trap restore EXIT
sed -i 's/^#define XPSEMU_DEV 0$/#define XPSEMU_DEV 1/' "$header"
grep -q '^#define XPSEMU_DEV 1$' "$header"

bash ps5/build-title.sh

app=dist/PPSA97358
sha=$(sha256sum "$app/eboot.bin" | cut -c1-12)
echo "$sha" > "$app/dev-build.txt"
llvm-nm-18 -n -C --defined-only build-ps5/title/llvm-pie.elf |
    awk '$2 ~ /^[tTwW]$/ {
        a = $1; sub(/^0+/, "", a); if (a == "") a = "0"
        $1 = ""; $2 = ""; sub(/^ +/, "")
        print a " " substr($0, 1, 160)
    }' > "$app/dev-symbols.txt"
cp build-ps5/title/llvm-pie.elf "ps5/builds/llvm-pie-$sha.elf"

rm -rf dist-dev; mkdir -p dist-dev
mv "$app" dist-dev/
echo "dev build $sha: $src/dist-dev/PPSA97358 ($(wc -l < dist-dev/PPSA97358/dev-symbols.txt) symbols)"

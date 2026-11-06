#!/system/bin/sh
# Boots every disc image in a directory with xboxrun (to its first kernel
# call) and prints the frontier and kernel-import lines for each, for
# tools/native/imports.py. Pushed with xboxrun to /data/local/tmp/native.
#
#   sh census.sh [/sdcard/ROMs/xbox]
#
# Never runs while the hakuX app is up (it shares the phone with the HLE GPU
# work); checked before every title, exit status 4 if it is.
app_running() {
  pidof com.rfandango.haku_x.debug:native >/dev/null || pidof com.rfandango.haku_x >/dev/null ||
    dumpsys window | grep -m1 mCurrentFocus | grep -q haku_x
}
cd /data/local/tmp/native || exit 1
chmod 755 xboxrun
DIR=${1:-/sdcard/ROMs/xbox}
for F in "$DIR"/*.iso; do
  if app_running; then
    echo "hakuX app is running: stopping (device runs must not overlap it)"
    exit 4
  fi
  echo "== $F"
  timeout 60 ./xboxrun "$F" 2>&1 | grep -E 'hakuX-native-(frontier|imports)'
done

#!/system/bin/sh
# Runs one title with xboxrun, unless the hakuX app is up (it shares the phone
# with the HLE GPU work; exit status 4): its emulator process, the release
# package, or hakuX in the foreground. The debug UI process left cached in the
# background doesn't count. Pushed with xboxrun to /data/local/tmp/native.
#
#   sh xboxrun.sh <disc image> [option...]    (native/run/xboxrun.c)
if pidof com.rfandango.haku_x.debug:native >/dev/null || pidof com.rfandango.haku_x >/dev/null ||
   dumpsys window | grep -m1 mCurrentFocus | grep -q haku_x; then
  echo "hakuX app is running: not starting (device runs must not overlap it)"
  exit 4
fi
cd /data/local/tmp/native || exit 1
chmod 755 xboxrun
exec ./xboxrun "$@"

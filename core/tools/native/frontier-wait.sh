#!/usr/bin/env bash
# Host side (Git Bash): waits until the phone is free of the hakuX app (two
# clean checks 30 s apart), then runs the given device commands in order,
# each re-checked in the same adb shell. See tools/native/xboxrun.sh for what
# counts as the app running.
#
#   tools/native/frontier-wait.sh '<device command>' ['<device command>' ...]
export MSYS_NO_PATHCONV=1
DEV=${ANDROID_SERIAL:-39a6da54} # the Xiaomi 17: native-core runs (the Odin 3 is the HLE GPU phone)
GUARD='if pidof com.rfandango.haku_x.debug:native >/dev/null || pidof com.rfandango.haku_x >/dev/null || dumpsys window | grep -m1 mCurrentFocus | grep -q haku_x; then echo "hakuX app is running: not starting"; exit 4; fi'
busy() {
  adb -s "$DEV" shell "$GUARD" >/dev/null 2>&1
  [ $? -eq 4 ]
}
idle=0
n=0
while [ $idle -lt 2 ]; do
  n=$((n + 1))
  if busy; then idle=0; else idle=$((idle + 1)); fi
  [ $idle -lt 2 ] && sleep 30
done
echo "phone free after $n checks"
for cmd in "$@"; do
  echo "==== $cmd"
  adb -s "$DEV" shell "$GUARD; $cmd"
done

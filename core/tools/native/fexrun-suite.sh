#!/system/bin/sh
# Runs fexrun test suites on the device, in FEX CI's three JIT configurations.
# Pushed with fexrun to /data/local/tmp/fexrun (FEX suite in tests/, ours in
# sysops/).
#
#   sh fexrun-suite.sh identity|auto|diff|sysops
#
# identity: FEX suite, guest addresses are host addresses (baseline B0)
# auto:     FEX suite, guest memory behind the memory-base register
# diff:     FEX suite both ways, comparing the full CPU state and every
#           mapped guest page
# sysops:   tests/native/sysops in system mode, both ways with --diff, plus
#           the FEX suite in system mode with --diff
#
# Runs on the small cores at low priority so it doesn't disturb other device
# work, and never while the hakuX app is up (it shares the phone with the HLE
# GPU work): checked before every run, exit status 4 if it is. Prints one
# summary line per run, then the tests that need a look.
app_running() {
  pidof com.rfandango.haku_x.debug:native >/dev/null || pidof com.rfandango.haku_x >/dev/null ||
    dumpsys window | grep -m1 mCurrentFocus | grep -q haku_x
}
cd /data/local/tmp/fexrun || exit 1
chmod 755 fexrun
MODE=${1:-identity}
RUN="taskset 3f nice -n 10 ./fexrun"
for CFG in "1 0" "500 0" "500 1"; do
  if app_running; then
    echo "hakuX app is running: stopping (device runs must not overlap it)"
    exit 4
  fi
  set -- $CFG
  JIT="--maxinst $1 --multiblock $2"
  OUT=suite_${MODE}_$1_$2
  case $MODE in
  diff)
    $RUN --diff $JIT tests tests/manifest.txt > $OUT.txt 2> $OUT.err
    tail -n 1 $OUT.txt
    grep '^DIFF' $OUT.txt | grep -v gather
    ;;
  sysops)
    $RUN --sysops --diff $JIT sysops sysops/manifest.txt > $OUT.txt 2> $OUT.err
    tail -n 1 $OUT.txt
    grep -v '^SAME [^ ]* PASS/PASS' $OUT.txt | grep -v '^summary'
    $RUN --sysops --diff $JIT tests tests/manifest.txt > ${OUT}_fex.txt 2> ${OUT}_fex.err
    tail -n 1 ${OUT}_fex.txt
    grep '^DIFF' ${OUT}_fex.txt | grep -v gather
    ;;
  *)
    $RUN --base $MODE $JIT tests tests/manifest.txt > $OUT.txt 2> $OUT.err
    tail -n 1 $OUT.txt
    grep -v '^PASS' $OUT.txt | grep -v '^summary' | grep -v gather
    ;;
  esac
done

#!/bin/sh
# One M0 iteration on the E5:
#   build stub -> write boot_b -> arm misc for exactly one stub run
#   -> reboot -> wait for Android -> pull the ramoops report
#
# Usage:
#   tools/e5-cycle.sh run              # flash + collect
#   tools/e5-cycle.sh flash            # flash only (EXTRA=-DM0_FB to add defines)
#   tools/e5-cycle.sh collect          # wait + pull only
#
# Why "one run": LK boots the highest-priority slot that still has tries left and
# decrements tries_remaining every boot.  With slot B at prio 15 / tries 1 / not
# successful and slot A at prio 15 / tries 1 / successful, LK runs our stub once
# and then rolls back to Android by itself.  No manual recovery, no watchdog wait.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ADB="adb -s TS30520241202952"
MISC_TMP=/tmp/e5-misc4k.bin

need_root() {
  $ADB root >/dev/null 2>&1 || true
  $ADB wait-for-device
  sleep 1
}

flash() {
  echo "== building"
  EXTRA="$EXTRA" sh "$ROOT/boot/m0/build.sh"
  need_root
  echo "== arming misc (slot A: prio 14, slot B: prio 15 tries 2) -- matches the known-good trial record"
  $ADB exec-out "dd if=/dev/block/by-name/misc bs=4096 count=1" > "$MISC_TMP"
  python3 "$ROOT/tools/e5-misc.py" patch "$MISC_TMP" --suffix _b --a-prio 14 --a-tries 1 --a-succ 1 --b-prio 15 --b-tries 2 --b-succ 0
  $ADB push "$MISC_TMP" /data/local/tmp/misc4k.bin >/dev/null
  $ADB shell "dd if=/data/local/tmp/misc4k.bin of=/dev/block/by-name/misc bs=4096 count=1 conv=notrunc" 2>/dev/null
  echo "== writing stub to boot_b (kernel starts at 0x1000, hence seek=1)"
  $ADB push "$ROOT/boot/m0/m0.bin" /data/local/tmp/m0.bin >/dev/null
  $ADB shell "dd if=/data/local/tmp/m0.bin of=/dev/block/by-name/boot_b bs=4096 seek=1 conv=notrunc" 2>/dev/null
  echo "== reading back boot_b head to confirm"
  $ADB shell "dd if=/dev/block/by-name/boot_b bs=4096 count=2" 2>/dev/null | head -c 64 | od -An -tx1 | head -2
}

collect() {
  echo "== rebooting"
  $ADB reboot
  i=0
  while [ $i -lt 95 ]; do
    if $ADB devices 2>/dev/null | grep -q "device$"; then break; fi
    i=$((i + 1))
    sleep 5
  done
  echo "== adb back after about $((i * 5))s"
  if [ $i -ge 95 ]; then echo "device never came back"; exit 1; fi
  need_root
  echo "== waiting for Android to finish booting"
  i=0
  while [ $i -lt 60 ]; do
    b=$($ADB shell getprop sys.boot_completed 2>/dev/null | tr -d "\r")
    if [ "$b" = "1" ]; then break; fi
    i=$((i + 1))
    sleep 5
  done
  if [ "$b" != "1" ]; then echo "!! Android did not come back in time"; exit 1; fi
  echo "== Android is up after roughly $((5 + i * 2))s"
  need_root
  STAMP=$(date +%H%M%S)
  REPORT=$ROOT/dumps/m0-report-$STAMP.txt
  $ADB exec-out "cat /sys/fs/pstore/console-ramoops-0" > "$REPORT"
  echo "== report: $REPORT ($(wc -c < "$REPORT") bytes)"
  echo "----------------------------------------------------------------"
  cat "$REPORT"
  echo "----------------------------------------------------------------"
  if grep -q "M0-E5-EL1-DONE" "$REPORT"; then
    echo "== reached EL1 and finished"
  elif grep -q "M0-E5-END" "$REPORT"; then
    echo "== finished at EL2 but never made it to EL1"
  else
    echo "== report truncated (stub died early)"
  fi
}

case "${1:-run}" in
  flash) flash ;;
  collect) collect ;;
  run) flash; collect ;;
  *) echo "usage: $0 [run|flash|collect]"; exit 2 ;;
esac

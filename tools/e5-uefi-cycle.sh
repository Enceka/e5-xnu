# One UEFI iteration on the E5: arm slot B, write the payload into boot_b,
# reboot, then pull the DEBUG log back out of the ramoops console zone.
#
# Usage:
#   tools/e5-uefi-cycle.sh flash     # write payload + misc, no reboot
#   tools/e5-uefi-cycle.sh collect   # reboot, wait for Android, pull the log
#   tools/e5-uefi-cycle.sh run       # both
#
# PAYLOAD can be overridden, it defaults to uefi/build/e5-uefi.bin.
#
# The misc record is the known-good one from the M0 work: slot A prio 14 with a
# single try, slot B prio 15 with two tries.  LK boots the strictly
# higher-priority slot, decrements tries_remaining on every boot, and rolls back
# to Android on its own once slot B runs out.  tries_remaining must be 2 or more:
# LK stores the displayed value plus one, so 1 means "already used up" and it
# drops into download mode instead of booting.
set -e
ROOT=`cd \`dirname $0\`/.. && pwd`
ADB="adb -s TS30520241202952"
PAYLOAD=${PAYLOAD:-$ROOT/uefi/build/e5-uefi.bin}
MISC_TMP=/tmp/e5-misc4k.bin
HDR_TMP=/tmp/e5-hdr8.bin

need_root() {
  $ADB root >/dev/null 2>&1 || true
  $ADB wait-for-device
  sleep 1
}

flash() {
  test -f "$PAYLOAD"
  SIZE=`wc -c < "$PAYLOAD"`
  echo "== payload $PAYLOAD ($SIZE bytes)"
  need_root
  echo "== arming misc (slot A prio 14 tries 1, slot B prio 15 tries 2)"
  $ADB exec-out "dd if=/dev/block/by-name/misc bs=4096 count=1" > "$MISC_TMP"
  python3 "$ROOT/tools/e5-misc.py" patch "$MISC_TMP" --suffix _b --a-prio 14 --a-tries 1 --a-succ 1 --b-prio 15 --b-tries 2 --b-succ 0
  $ADB push "$MISC_TMP" /data/local/tmp/misc4k.bin >/dev/null
  $ADB shell "dd if=/data/local/tmp/misc4k.bin of=/dev/block/by-name/misc bs=4096 count=1 conv=notrunc" 2>/dev/null
  echo "== writing payload into boot_b (kernel starts at 0x1000, hence seek=1)"
  $ADB push "$PAYLOAD" /data/local/tmp/e5-uefi.bin >/dev/null
  $ADB shell "dd if=/data/local/tmp/e5-uefi.bin of=/dev/block/by-name/boot_b bs=4096 seek=1 conv=notrunc" 2>/dev/null
  echo "== patching the boot image header (kernel_size, ramdisk_size=0)"
  python3 -c "import struct,sys; open(sys.argv[1],\"wb\").write(struct.pack(\"<II\", int(sys.argv[2]), 0))" "$HDR_TMP" "$SIZE"
  $ADB push "$HDR_TMP" /data/local/tmp/e5-hdr8.bin >/dev/null
  $ADB shell "dd if=/data/local/tmp/e5-hdr8.bin of=/dev/block/by-name/boot_b bs=1 seek=8 conv=notrunc" 2>/dev/null
  echo "== reading back the header and the first payload words"
  $ADB shell "dd if=/dev/block/by-name/boot_b bs=4096 count=2" 2>/dev/null | od -A d -t x1 -N 32 | head -2
  $ADB shell "dd if=/dev/block/by-name/boot_b bs=4096 count=2" 2>/dev/null | od -A d -t x1 -j 4096 -N 32 | head -2
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
    b=`$ADB shell getprop sys.boot_completed 2>/dev/null | tr -d "\r"`
    if [ "$b" = "1" ]; then break; fi
    i=$((i + 1))
    sleep 5
  done
  if [ "$b" != "1" ]; then echo "!! Android did not come back in time"; exit 1; fi
  need_root
  STAMP=`date +%H%M%S`
  REPORT=$ROOT/dumps/uefi-report-$STAMP.txt
  $ADB exec-out "cat /sys/fs/pstore/console-ramoops-0" > "$REPORT"
  echo "== report: $REPORT (`wc -c < "$REPORT"` bytes)"
  echo "----------------------------------------------------------------"
  cat "$REPORT"
  echo "----------------------------------------------------------------"
  if grep -q "DXE" "$REPORT"; then
    echo "== SEC and DXE are running"
  elif grep -q "Silicium" "$REPORT"; then
    echo "== SEC started, DXE not reached"
  else
    echo "== nothing from the firmware, SEC never printed"
  fi
}

case "${1:-run}" in
  flash) flash ;;
  collect) collect ;;
  run) flash; collect ;;
  *) echo "usage: $0 [run|flash|collect]"; exit 2 ;;
esac

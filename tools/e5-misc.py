#!/usr/bin/env python3
"""Patch the 32-byte A/B slot record LK keeps at misc offset 0x800.

Layout (verified against the stock record and against
boot-linux-slotb's misc hex from the e5-linux repo):

  [0:4]   slot_suffix, e.g. "_a\0\0"
  [4:8]   b"BCAB"
  [8]     struct version (1)
  [9]     nb_slot (2)
  [10]    recovery_tries
  [11]    merge_status
  [12:20] slot[0..3], 2 bytes each:
            b0 = prio[3:0] | tries[2:0] << 4 | successful << 7
            b1 = verity_corrupt
  [20:28] reserved
  [28:32] crc32 over [0:28], little endian

LK boots the slot with the highest priority that still has tries left, and
decrements tries_remaining on every boot.  So to get exactly one run of our
stub and then an automatic rollback to Android:

    slot A: prio 15, tries 1, successful 1
    slot B: prio 15, tries 1, successful 0

Usage:
    e5-misc.py show  <misc-first-4k.bin>
    e5-misc.py patch <misc-first-4k.bin> --a-prio 15 --a-tries 1 --b-prio 15 --b-tries 1
"""
import struct
import sys
import zlib

REC_OFF = 0x800
REC_LEN = 32


def pack_slot(prio, tries, succ):
    return ((prio & 0xF) | ((tries & 0x7) << 4) | ((succ & 1) << 7)) & 0xFF


def unpack_slot(b):
    return (b & 0xF, (b >> 4) & 0x7, (b >> 7) & 1)


def build(slots, version=1, nb_slot=2, suffix=b"_a\0\0"):
    rec = bytearray(REC_LEN)
    rec[0:4] = suffix[:4].ljust(4, b"\0")
    rec[4:8] = b"BCAB"
    rec[8] = version
    rec[9] = nb_slot
    for i, (prio, tries, succ) in enumerate(slots):
        rec[12 + i * 2] = pack_slot(prio, tries, succ)
    rec[28:32] = struct.pack("<I", zlib.crc32(bytes(rec[0:28])))
    return bytes(rec)


def decode(rec):
    ok = struct.unpack("<I", rec[28:32])[0] == zlib.crc32(bytes(rec[0:28]))
    out = ["magic=%r version=%d nb_slot=%d crc_ok=%s"
           % (bytes(rec[4:8]), rec[8], rec[9], ok)]
    for i in range(rec[9]):
        prio, tries, succ = unpack_slot(rec[12 + i * 2])
        out.append("  slot %d: prio=%d tries=%d successful=%d raw=0x%02x"
                   % (i, prio, tries, succ, rec[12 + i * 2]))
    return "\n".join(out)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, path = argv[1], argv[2]
    blob = bytearray(open(path, "rb").read())
    if len(blob) < REC_OFF + REC_LEN:
        raise SystemExit("file too short, need at least 0x820 bytes")
    cur = bytes(blob[REC_OFF:REC_OFF + REC_LEN])

    if cmd == "show":
        print(decode(cur))
        return 0
    if cmd != "patch":
        print(__doc__)
        return 2

    a = [15, 1, 1]
    b = [15, 1, 0]
    suffix = b"_a\0\0"
    args = argv[3:]
    while args:
        k = args.pop(0)
        v = args.pop(0)
        if k == "--suffix":
            suffix = v.encode().ljust(4, b"\0")[:4]
            continue
        v = int(v, 0)
        if k == "--a-prio":
            a[0] = v
        elif k == "--a-tries":
            a[1] = v
        elif k == "--a-succ":
            a[2] = v
        elif k == "--b-prio":
            b[0] = v
        elif k == "--b-tries":
            b[1] = v
        elif k == "--b-succ":
            b[2] = v
        else:
            raise SystemExit("unknown option " + k)

    rec = build([tuple(a), tuple(b)], suffix=suffix)
    blob[REC_OFF:REC_OFF + REC_LEN] = rec
    open(path, "wb").write(blob)
    print("patched:")
    print(decode(rec))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

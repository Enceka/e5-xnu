"""Assemble the E5 boot payload: boot shim followed by the firmware volume.

The shim is linked at 0x80080000, which is where LK always drops the boot
partition kernel, and it copies the firmware volume to 0x80200000 before
branching into it.  The two blobs are simply concatenated, exactly like the
Silicium build_uefi.py does for other devices.

One extra step: the AArch64 Image header at the start of the shim carries the
total image size at offset 16.  It is zero when the shim is linked, because the
size of the firmware volume is not known until now, so patch it in here.

Usage: mk-boot-payload.py <shim.bin> <firmware.fd> <output.bin>
"""

import struct
import sys

IMAGE_SIZE_OFFSET = 16


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2

    shim_path, fd_path, out_path = argv[1], argv[2], argv[3]

    shim = bytearray(open(shim_path, "rb").read())
    fd = open(fd_path, "rb").read()

    if len(shim) < IMAGE_SIZE_OFFSET + 8:
        print("shim is too small to hold an Image header: %d bytes" % len(shim))
        return 1

    if struct.unpack_from("<I", shim, 56)[0] != 0x644D5241:
        print("shim has no ARMd magic at offset 56")
        return 1

    total = len(shim) + len(fd)
    struct.pack_into("<Q", shim, IMAGE_SIZE_OFFSET, total)

    if len(shim) % 16:
        print("warning: payload starts at 0x%x, not 16 byte aligned" % len(shim))

    with open(out_path, "wb") as out:
        out.write(shim)
        out.write(fd)

    print("shim %d bytes, firmware %d bytes, payload %d bytes" % (len(shim), len(fd), total))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

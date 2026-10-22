#!/usr/bin/env python3
"""Generate the Apple-format device tree (ADT) for the Rongyue E5 (Unisoc UMS9621).

The ADT is consumed by DarwinBoot / XNU, not by Linux and not by UEFI. Its
encoding is NOT the standard FDT encoding:

    node := u32 property_count | u32 child_count
            property_count * { char name[32]; u32 length; u8 value[length]; pad to 4 }
            child_count * node

Every value is little-endian (a 64-bit address is 2 LE u32 cells, low cell
first). Standard FDT, by contrast, is big-endian and uses a string table.

Facts encoded here all come from measured sources, never from guesses:
  * timebase 26 MHz                        -> M0 report, CNTFRQ_EL0
  * dram base/size 0x80000000 / 0x7ffff000 -> LK boot log, /memory node
  * GICD 0x12000000 / GICR 0x12040000      -> vendor FDT, M0 report
  * UART1 0x20210000                       -> vendor FDT, M0 report
  * 8 CPUs, cpuN reg = N << 8              -> vendor FDT /cpus/cpu@N00

The cpu `reg` encoding is the one unresolved input: the vendor FDT numbers the
CPUs by Affinity1 (0x000, 0x100, ... 0x700) but the GICR_TYPER Affinity_Value
of the eight redistributors reads back as 0..7, which is Affinity0 numbering.
Use --cpu-affinity to pick; it must be settled by reading MPIDR_EL1 on a
secondary CPU before SMP can work.
"""
import argparse
import os
import struct
import sys
import uuid
import zlib

TIMEBASE = 26000000
DRAM_BASE = 0x80000000
DRAM_SIZE = 0x7FFFF000
CPU_COUNT = 8

# One bus window for arm-io: 0x12000000 .. 0x32000000. It covers the GIC, the
# SPRD UARTs, the eMMC/SDIO hosts and the display block. Child `reg` values are
# offsets inside this window. Anything outside it must be addressed by a driver
# through a board constant.
ARM_IO_BASE = 0x12000000
ARM_IO_SIZE = 0x20000000
GICD_OFFSET = 0x00000000
GICD_SIZE = 0x00020000
GICR_OFFSET = 0x00040000
GICR_SIZE = 0x00100000
UART1_OFFSET = 0x20210000 - ARM_IO_BASE
UART1_SIZE = 0x00001000
EMMC_OFFSET = 0x22200000 - ARM_IO_BASE
EMMC_SIZE = 0x00010000

CHIP_ID = 0x9621


def node(name, properties=(), children=()):
    props = [("name", name.encode() + b"\0"), *properties]
    out = struct.pack("<II", len(props), len(children))
    for key, data in props:
        out += struct.pack("<32sI", key.encode(), len(data))
        out += data + bytes(-len(data) % 4)
    return out + b"".join(children)


def empty_nvram_proxy():
    """A fresh volatile CHRP bank, the minimum IODTNVRAM will accept."""

    def header(name, size):
        raw = bytearray(struct.pack("<BBH12s", 0x70, 0, size // 16, name))
        checksum = sum(raw)
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        raw[1] = checksum
        return raw

    image = bytearray(8192)
    image[:16] = header(b"nvram", 32)
    image[32:48] = header(b"common", 16 + 2048)
    free_at = 32 + 16 + 2048
    image[free_at:free_at + 16] = header(b"free", len(image) - free_at)
    struct.pack_into("<I", image, 16, zlib.adler32(image[20:]))
    return [
        ("nvram-bank-size", struct.pack("<I", len(image))),
        ("nvram-bank-count", struct.pack("<I", 1)),
        ("nvram-current-bank", struct.pack("<I", 0)),
        ("nvram-proxy-data", bytes(image)),
    ]


def cpu_node(index, affinity_mode, state):
    if affinity_mode == "fdt":
        reg = index << 8
    else:
        reg = index
    return node(
        "cpu%d" % index,
        [
            ("state", state.encode() + b"\0"),
            ("reg", struct.pack("<I", reg)),
            ("device_type", b"cpu\0"),
            ("compatible", b"arm,armv8\0"),
            ("timebase-frequency", struct.pack("<I", TIMEBASE)),
        ],
    )


def build(args):
    root_matching = []
    if args.root_uuid is not None:
        xml = (
            "<dict><key>IOProviderClass</key><string>IOMedia</string>"
            "<key>IOPropertyMatch</key><dict><key>UUID</key><string>"
            + str(args.root_uuid).upper()
            + "</string></dict></dict>\0"
        )
        root_matching = [("root-matching", xml.encode())]

    chosen_props = [
        ("random-seed", os.urandom(64)),
        ("unique-chip-id", struct.pack("<Q", args.unique_chip_id)),
        ("chip-id", struct.pack("<I", CHIP_ID)),
        ("dram-base", struct.pack("<Q", DRAM_BASE)),
        ("dram-size", struct.pack("<Q", DRAM_SIZE)),
        ("osenvironment", args.osenvironment.encode() + b"\0"),
        ("crypto-hash-method", b"sha2-384\0"),
        ("darwinboot-memory-size", struct.pack("<Q", args.darwinboot_memory_size)),
        *root_matching,
        *empty_nvram_proxy(),
    ]
    memory_map = node(
        "memory-map",
        [
            ("RAMDisk", bytes(16)),
            ("PackedRAMDisk", bytes(16)),
            ("TrustCache", bytes(16)),
        ],
    )

    cpus = node(
        "cpus",
        children=[cpu_node(0, args.cpu_affinity, "running")]
        + [
            cpu_node(index, args.cpu_affinity, "waiting")
            for index in range(1, args.cpus)
        ],
    )

    arm_io = node(
        "arm-io",
        [
            ("device_type", b"arm-io\0"),
            ("#address-cells", struct.pack("<I", 2)),
            ("#size-cells", struct.pack("<I", 2)),
            ("ranges", struct.pack("<QQQ", 0, ARM_IO_BASE, ARM_IO_SIZE)),
        ],
        [
            node(
                "intc",
                [
                    ("compatible", b"arm,gic-v3\0"),
                    ("reg", struct.pack("<QQQQ", GICD_OFFSET, GICD_SIZE, GICR_OFFSET, GICR_SIZE)),
                    ("interrupt-controller", b""),
                    ("#interrupt-cells", struct.pack("<I", 3)),
                ],
            ),
            node(
                "uart",
                [
                    ("compatible", b"sprd,sc9836-uart\0"),
                    ("reg", struct.pack("<QQ", UART1_OFFSET, UART1_SIZE)),
                ],
            ),
            node(
                "emmc",
                [
                    ("compatible", b"sprd,sdhci-r11\0"),
                    ("reg", struct.pack("<QQ", EMMC_OFFSET, EMMC_SIZE)),
                ],
            ),
        ],
    )

    return node(
        "device-tree",
        [
            ("model", args.model.encode() + b"\0"),
            ("compatible", b"unisoc,ums9621\0"),
            ("#address-cells", struct.pack("<I", 2)),
            ("#size-cells", struct.pack("<I", 2)),
        ],
        [
            node("chosen", chosen_props, [memory_map]),
            cpus,
            node("psci", [("compatible", b"arm,psci-1.0\0"), ("method", b"smc\0")]),
            node("defaults", [("sleep-disabled", struct.pack("<I", 1))]),
            node("options"),
            node("product", [("product-name", b"Rongyue E5\0"), ("model", b"ums9158_1h10\0")]),
            node("buttons", [("home-button-type", b"0\0")]),
            arm_io,
        ],
    )


def dump(blob):
    """Decode an ADT back into an indented listing, to prove the encoding."""

    def walk(offset, depth):
        count, children = struct.unpack_from("<II", blob, offset)
        offset += 8
        lines = []
        for _ in range(count):
            raw_name, length = struct.unpack_from("<32sI", blob, offset)
            offset += 36
            value = blob[offset:offset + length]
            offset += length + (-length % 4)
            name = raw_name.split(b"\0")[0].decode(errors="replace")
            if name == "name":
                lines.append((depth, "node %s" % value.split(b"\0")[0].decode(errors="replace"), None))
            else:
                shown = value.hex() if length <= 16 else "%d bytes" % length
                lines.append((depth, name, shown))
        for _ in range(children):
            child_lines, offset = walk(offset, depth + 1)
            lines.extend(child_lines)
        return lines, offset

    lines, end = walk(0, 0)
    for depth, name, value in lines:
        if value is None:
            print("  " * depth + name)
        else:
            print("  " * depth + "%-24s %s" % (name, value))
    if not end == len(blob):
        print("WARNING: %d trailing bytes" % (len(blob) - end), file=sys.stderr)
        return 1
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", default="uefi/build/e5.adt")
    parser.add_argument("--cpus", type=int, default=CPU_COUNT)
    parser.add_argument("--cpu-affinity", choices=["fdt", "index"], default="fdt")
    parser.add_argument("--model", default="e5")
    parser.add_argument("--osenvironment", choices=["main", "recoveryos", "ramdisk"], default="main")
    parser.add_argument("--darwinboot-memory-size", type=lambda v: int(v, 0), default=0x20000000)
    parser.add_argument("--unique-chip-id", type=lambda v: int(v, 0), default=0x0000000000096210)
    parser.add_argument("--root-uuid", type=uuid.UUID, default=None)
    parser.add_argument("--dump", action="store_true")
    args = parser.parse_args()

    if not 0x10000000 <= args.darwinboot_memory_size <= 0x200000000 or args.darwinboot_memory_size % 0x200000:
        parser.error("darwinboot-memory-size must be 256 MiB .. 8 GiB in 2 MiB steps")
    if not 1 <= args.cpus <= CPU_COUNT:
        parser.error("cpus must be 1..8")

    blob = build(args)
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as handle:
        handle.write(blob)
    print("E5 ADT: %d bytes -> %s" % (len(blob), args.output))
    if args.dump:
        return dump(blob)
    return 0


if __name__ == "__main__":
    sys.exit(main())

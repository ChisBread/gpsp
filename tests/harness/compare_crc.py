#!/usr/bin/env python3
"""
Compare CRC logs from ESP32 device and x86 dual simulation.

Format: each record is 4x uint32: {frame_nr, oam_crc, io0_crc, pixel_crc}

Usage:
    python3 compare_crc.py crc_dual.bin dump_crc.bin [skip]

    skip = device DUMP_SKIP value (default: 2000)
"""

import sys
import struct
import os

REC_SIZE = 16  # 4 x uint32
REC_FMT = "<IIII"


def load_crc(path):
    data = open(path, "rb").read()
    n = len(data) // REC_SIZE
    records = []
    for i in range(n):
        rec = struct.unpack_from(REC_FMT, data, i * REC_SIZE)
        records.append(rec)  # (frame_nr, oam_crc, io0_crc, pixel_crc)
    return records


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <x86_crc.bin> <device_crc.bin> [skip]")
        sys.exit(1)

    x86_path = sys.argv[1]
    dev_path = sys.argv[2]
    skip = int(sys.argv[3]) if len(sys.argv) > 3 else 2000

    x86_recs = load_crc(x86_path)
    dev_recs = load_crc(dev_path)

    print(f"x86:    {len(x86_recs)} records")
    print(f"Device: {len(dev_recs)} records (skip={skip})")

    # Compare matching frame numbers
    oam_diff = 0
    io_diff = 0
    pixel_diff = 0
    compared = 0
    first_oam_diff = None
    first_io_diff = None

    for dev_rec in dev_recs:
        dev_frame = dev_rec[0]
        # Find matching x86 frame
        if dev_frame >= len(x86_recs):
            break

        x86_rec = x86_recs[dev_frame]
        x86_frame = x86_rec[0]

        if x86_frame != dev_frame:
            print(f"  Frame number mismatch: x86={x86_frame}, dev={dev_frame}")
            continue

        compared += 1

        if x86_rec[1] != dev_rec[1]:
            oam_diff += 1
            if first_oam_diff is None:
                first_oam_diff = dev_frame

        if x86_rec[2] != dev_rec[2]:
            io_diff += 1
            if first_io_diff is None:
                first_io_diff = dev_frame

        if x86_rec[3] != dev_rec[3]:
            pixel_diff += 1

    print(f"\nCompared {compared} frames:")
    print(f"  OAM CRC mismatches:   {oam_diff:6d} (first: frame {first_oam_diff})")
    print(f"  IO0 CRC mismatches:   {io_diff:6d} (first: frame {first_io_diff})")
    print(f"  Pixel CRC mismatches: {pixel_diff:6d}")

    if oam_diff > 0:
        print("\n>>> OAM DIFFERS — game state has diverged upstream!")
        print("    Sprite positions are wrong because the game wrote")
        print("    different OAM data. Root cause is NOT in PPU pipeline rendering.")
    elif pixel_diff > 0 and oam_diff == 0:
        print("\n>>> OAM MATCHES but pixels differ — rendering bug!")
        print("    The snapshot data is identical but rendering produces")
        print("    different output. Bug is in update_scanline() or redirect logic.")
    else:
        print("\n>>> Everything matches!")


if __name__ == "__main__":
    main()

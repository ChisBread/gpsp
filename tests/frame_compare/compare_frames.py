#!/usr/bin/env python3
"""
Frame Comparison Tool — compare pixel output of single-core vs dual-core.

Usage:
    python3 compare_frames.py frames_single.bin frames_dual.bin [--verbose]

Each .bin file contains N frames of raw 240×160 u16 pixel data (76800 bytes/frame).
Reports the first frame and scanline where pixels diverge.
"""

import sys
import struct
import os

WIDTH  = 240
HEIGHT = 160
FRAME_BYTES = WIDTH * HEIGHT * 2  # u16 per pixel


def read_frame(fp):
    """Read one frame (240×160 u16). Returns bytes or None at EOF."""
    data = fp.read(FRAME_BYTES)
    if len(data) < FRAME_BYTES:
        return None
    return data


def compare_scanline(ref_data, test_data, y):
    """Compare one scanline (240 u16). Returns list of (x, ref_val, test_val)."""
    offset = y * WIDTH * 2
    diffs = []
    for x in range(WIDTH):
        i = offset + x * 2
        rv = struct.unpack_from('<H', ref_data, i)[0]
        tv = struct.unpack_from('<H', test_data, i)[0]
        if rv != tv:
            diffs.append((x, rv, tv))
    return diffs


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <ref.bin> <test.bin> [--verbose]")
        sys.exit(1)

    ref_path  = sys.argv[1]
    test_path = sys.argv[2]
    verbose   = '--verbose' in sys.argv

    ref_size  = os.path.getsize(ref_path)
    test_size = os.path.getsize(test_path)
    ref_frames  = ref_size  // FRAME_BYTES
    test_frames = test_size // FRAME_BYTES
    num_frames  = min(ref_frames, test_frames)

    print(f"Reference:  {ref_path}  ({ref_frames} frames)")
    print(f"Test:       {test_path} ({test_frames} frames)")
    print(f"Comparing {num_frames} frames...")

    total_diff_frames = 0
    first_diff_frame  = -1

    with open(ref_path, 'rb') as fp_ref, open(test_path, 'rb') as fp_test:
        for frame in range(num_frames):
            ref_data  = read_frame(fp_ref)
            test_data = read_frame(fp_test)

            if ref_data is None or test_data is None:
                break

            if ref_data == test_data:
                continue

            # Found a divergence
            total_diff_frames += 1
            if first_diff_frame < 0:
                first_diff_frame = frame

            if total_diff_frames <= 10 or verbose:
                print(f"\n=== Frame {frame} DIFFERS ===")

                for y in range(HEIGHT):
                    diffs = compare_scanline(ref_data, test_data, y)
                    if diffs:
                        print(f"  Line {y:3d}: {len(diffs)} pixels differ", end="")
                        if len(diffs) <= 8:
                            for x, rv, tv in diffs:
                                print(f"  [x={x}: {rv:#06x}→{tv:#06x}]", end="")
                        print()

            elif total_diff_frames == 11 and not verbose:
                print("\n  ... (use --verbose to see all differences)")

    # Summary
    print(f"\n{'='*60}")
    if total_diff_frames == 0:
        print("PASS — all frames match perfectly!")
    else:
        print(f"FAIL — {total_diff_frames} / {num_frames} frames differ")
        print(f"First divergence at frame {first_diff_frame}")

    if ref_frames != test_frames:
        print(f"WARNING: frame count mismatch ({ref_frames} vs {test_frames})")

    sys.exit(0 if total_diff_frames == 0 else 1)


if __name__ == '__main__':
    main()

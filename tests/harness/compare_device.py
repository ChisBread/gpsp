#!/usr/bin/env python3
"""
Compare device-dumped pixels against x86 single-core reference.

Usage:
    python3 compare_device.py frames_single.bin dump_pixels.bin [skip]

    skip = number of frames skipped on device before recording (default: 2000)

Output: lists every divergent frame with pixel diff count and saves
        the first divergent frame pair as PNG for visual inspection.
"""

import sys
import struct
import numpy as np

W, H = 240, 160
FRAME_BYTES = W * H * 2  # u16 per pixel


def rgb565_to_rgb888(frame_u16):
    r = ((frame_u16 >> 11) & 0x1F) << 3
    g = ((frame_u16 >> 5) & 0x3F) << 2
    b = (frame_u16 & 0x1F) << 3
    return np.stack([r, g, b], axis=-1).astype(np.uint8)


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <ref_single.bin> <device_dump.bin> [skip]")
        sys.exit(1)

    ref_path = sys.argv[1]
    dev_path = sys.argv[2]
    skip = int(sys.argv[3]) if len(sys.argv) > 3 else 2000

    import os
    ref_size = os.path.getsize(ref_path)
    dev_size = os.path.getsize(dev_path)
    ref_frames = ref_size // FRAME_BYTES
    dev_frames = dev_size // FRAME_BYTES

    print(f"Reference: {ref_path} — {ref_frames} frames")
    print(f"Device:    {dev_path} — {dev_frames} frames (skip={skip})")

    if ref_frames < skip + dev_frames:
        print(f"WARNING: reference has only {ref_frames} frames, "
              f"need {skip + dev_frames} to cover device range")
        dev_frames = min(dev_frames, ref_frames - skip)

    compare_count = dev_frames
    print(f"Comparing {compare_count} frames (device frames {skip}..{skip + compare_count - 1})")

    ref_fp = open(ref_path, "rb")
    dev_fp = open(dev_path, "rb")

    # Seek reference to the skip offset
    ref_fp.seek(skip * FRAME_BYTES)

    divergent = []
    first_diff_ref = None
    first_diff_dev = None
    first_diff_frame = None

    for i in range(compare_count):
        ref_data = ref_fp.read(FRAME_BYTES)
        dev_data = dev_fp.read(FRAME_BYTES)

        if len(ref_data) < FRAME_BYTES or len(dev_data) < FRAME_BYTES:
            print(f"  Unexpected EOF at frame {i}")
            break

        if ref_data != dev_data:
            ref_arr = np.frombuffer(ref_data, dtype=np.uint16)
            dev_arr = np.frombuffer(dev_data, dtype=np.uint16)
            diff_count = int(np.sum(ref_arr != dev_arr))
            frame_nr = skip + i
            divergent.append((frame_nr, diff_count))

            if first_diff_ref is None:
                first_diff_ref = ref_arr.copy()
                first_diff_dev = dev_arr.copy()
                first_diff_frame = frame_nr

    ref_fp.close()
    dev_fp.close()

    if not divergent:
        print("\nRESULT: All frames IDENTICAL — no divergence detected!")
        return

    print(f"\nRESULT: {len(divergent)} / {compare_count} frames differ")
    print(f"First divergent frame: {divergent[0][0]} ({divergent[0][1]} pixels differ)")

    # Show first 20 divergent frames
    print("\nFirst divergent frames:")
    for frame_nr, diff_count in divergent[:20]:
        pct = 100.0 * diff_count / (W * H)
        print(f"  Frame {frame_nr:6d}: {diff_count:6d} pixels differ ({pct:.1f}%)")

    if len(divergent) > 20:
        print(f"  ... and {len(divergent) - 20} more")

    # Summary statistics
    diff_counts = [d for _, d in divergent]
    print(f"\nDiff pixel stats: min={min(diff_counts)}, max={max(diff_counts)}, "
          f"mean={np.mean(diff_counts):.0f}")

    # Check if divergence is continuous or intermittent
    frame_nrs = [f for f, _ in divergent]
    gaps = [frame_nrs[i+1] - frame_nrs[i] for i in range(len(frame_nrs)-1)]
    if gaps and max(gaps) == 1:
        print(f"Divergence is CONTINUOUS from frame {frame_nrs[0]} onward")
    elif gaps:
        print(f"Divergence pattern: gap min={min(gaps)}, max={max(gaps)}")

    # Save first diff as PNG
    try:
        from PIL import Image
        ref_img = rgb565_to_rgb888(first_diff_ref.reshape(H, W))
        dev_img = rgb565_to_rgb888(first_diff_dev.reshape(H, W))

        # Side by side: ref | device | diff mask
        diff_mask = (first_diff_ref != first_diff_dev).reshape(H, W)
        diff_vis = np.zeros((H, W, 3), dtype=np.uint8)
        diff_vis[diff_mask] = [255, 0, 0]  # red where different

        combined = np.concatenate([ref_img, dev_img, diff_vis], axis=1)
        img = Image.fromarray(combined)
        out_name = f"diff_frame_{first_diff_frame}.png"
        img.save(out_name)
        print(f"\nSaved visual diff: {out_name} (ref | device | diff_mask)")
    except ImportError:
        print("\n(Install Pillow for visual diff output: pip install Pillow)")


if __name__ == "__main__":
    main()

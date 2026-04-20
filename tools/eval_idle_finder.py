#!/usr/bin/env python3
"""
Evaluate idle_loop_finder.py against gba_over.h ground truth.

1. Parse gba_over.h to extract {game_code: idle_loop_target_pc} ground truth
2. Scan ROM directory to build {game_code: rom_path} map
3. Run idle_loop_finder on all ROMs (multiprocessing)
4. Compare: recall (how many gba_over.h entries did we find?), precision, etc.
5. Output detailed report and CSV

Usage:
  python tools/eval_idle_finder.py /mnt/planet/data/GBA/ALLROMS/roms_sorted/ -j 8
"""

import argparse
import csv
import json
import multiprocessing as mp
import os
import re
import struct
import sys
import time
from collections import defaultdict
from pathlib import Path

# Add tools dir to path so we can import the finder
sys.path.insert(0, os.path.dirname(__file__))
from idle_loop_finder import IdleLoopFinder

# ─── Parse gba_over.h ───────────────────────────────────────────────────────

def parse_gba_over(path: str) -> dict:
    """Parse gba_over.h and return {game_code: idle_loop_target_pc} for non-zero entries."""
    ground_truth = {}
    with open(path, 'r') as f:
        text = f.read()

    # Match each entry block: "CODE" ... idle_loop_target_pc
    # Pattern: "XXXX" as gamepak_code, then 0x... as idle_loop_target_pc
    entries = re.findall(
        r'//\s*(.*?)\n\s*"(\w{4})".*?gamepak_code.*?\n'
        r'\s*.*?/\*\s*flags.*?\n'
        r'\s*(0x[\da-fA-F]+|0)\s*,\s*/\*\s*idle_loop_target_pc',
        text
    )
    for comment, code, pc_str in entries:
        pc = int(pc_str, 0)
        if pc != 0:
            ground_truth[code] = pc
    return ground_truth


# ─── ROM scanning ────────────────────────────────────────────────────────────

def get_game_code(rom_path: str) -> str:
    """Read the 4-byte game code from a GBA ROM header at offset 0xAC."""
    try:
        with open(rom_path, 'rb') as f:
            f.seek(0xAC)
            code = f.read(4)
            if len(code) == 4:
                return code.decode('ascii', errors='replace')
    except:
        pass
    return ""


def scan_roms(rom_dir: str) -> dict:
    """Recursively find all .gba files and map game_code → [rom_paths]."""
    code_to_paths = defaultdict(list)
    for dirpath, _, filenames in os.walk(rom_dir):
        for fn in filenames:
            if fn.lower().endswith('.gba'):
                full = os.path.join(dirpath, fn)
                code = get_game_code(full)
                if code and len(code) == 4 and code.isprintable():
                    code_to_paths[code].append(full)
    return dict(code_to_paths)


# ─── Worker function ─────────────────────────────────────────────────────────

def analyze_single(args):
    """Worker: analyze one ROM, return (game_code, rom_path, [(pc, confidence, mode, io_reads, rank)])."""
    rom_path, game_code = args
    try:
        with open(rom_path, 'rb') as f:
            data = f.read()
        finder = IdleLoopFinder(data)
        candidates = finder.find()
        results = []
        for i, c in enumerate(candidates):
            results.append((c.pc, c.confidence, c.mode,
                           c.io_reads, i + 1, c.loop_size,
                           '; '.join(c.reasons)))
        return (game_code, rom_path, results, None)
    except Exception as e:
        return (game_code, rom_path, [], str(e))


# ─── Main ────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description='Evaluate idle_loop_finder vs gba_over.h')
    ap.add_argument('rom_dir', help='Directory with GBA ROMs (recursive)')
    ap.add_argument('-j', '--jobs', type=int, default=max(1, mp.cpu_count() - 2),
                    help='Number of parallel workers')
    ap.add_argument('--gba-over', type=str,
                    default=os.path.join(os.path.dirname(__file__), '..', 'gba_over.h'),
                    help='Path to gba_over.h')
    ap.add_argument('--top', type=int, default=5,
                    help='Consider top-N candidates when checking recall')
    ap.add_argument('--csv', type=str, default='idle_eval_results.csv',
                    help='Output CSV file')
    ap.add_argument('--report', type=str, default='idle_eval_report.txt',
                    help='Output report file')
    ap.add_argument('--missed-json', type=str, default='idle_missed.json',
                    help='JSON of missed entries for further analysis')
    ap.add_argument('--extra-json', type=str, default='idle_extra.json',
                    help='JSON of extra findings not in gba_over.h')
    args = ap.parse_args()

    # 1. Parse ground truth
    print(f"Parsing {args.gba_over}...")
    gt = parse_gba_over(args.gba_over)
    print(f"  Ground truth: {len(gt)} entries with non-zero idle_loop_target_pc")

    # 2. Scan ROMs
    print(f"Scanning {args.rom_dir}...")
    code_map = scan_roms(args.rom_dir)
    print(f"  Found {sum(len(v) for v in code_map.values())} ROMs, "
          f"{len(code_map)} unique game codes")

    # Check coverage
    gt_found = {code: pc for code, pc in gt.items() if code in code_map}
    gt_missing_roms = {code: pc for code, pc in gt.items() if code not in code_map}
    print(f"  Ground truth codes with ROMs: {len(gt_found)}/{len(gt)}")
    if gt_missing_roms:
        print(f"  Ground truth codes without ROMs: {list(gt_missing_roms.keys())[:10]}...")

    # 3. Build work list: for GT entries, pick first matching ROM
    #    Also analyze ALL ROMs for extra findings
    work_gt = []
    for code, pc in gt_found.items():
        work_gt.append((code_map[code][0], code))

    # For extra discovery: analyze all unique ROMs (one per code)
    work_all = []
    for code, paths in code_map.items():
        work_all.append((paths[0], code))

    # 4. Run analysis with multiprocessing
    print(f"\nPhase 1: Analyzing {len(work_gt)} ground-truth ROMs ({args.jobs} workers)...")
    t0 = time.time()

    with mp.Pool(args.jobs) as pool:
        gt_results = list(pool.imap_unordered(analyze_single, work_gt))

    t1 = time.time()
    print(f"  Done in {t1-t0:.1f}s")

    print(f"\nPhase 2: Analyzing all {len(work_all)} unique ROMs ({args.jobs} workers)...")
    with mp.Pool(args.jobs) as pool:
        all_results = list(pool.imap_unordered(analyze_single, work_all))

    t2 = time.time()
    print(f"  Done in {t2-t1:.1f}s")

    # 5. Evaluate recall
    gt_result_map = {r[0]: r for r in gt_results}

    hit_top1 = 0
    hit_topN = 0
    hit_any = 0
    missed = []
    rank_histogram = defaultdict(int)

    report_lines = []
    report_lines.append(f"Idle Loop Finder Evaluation Report")
    report_lines.append(f"{'='*64}")
    report_lines.append(f"Ground truth entries (non-zero idle_loop_target_pc): {len(gt)}")
    report_lines.append(f"ROMs found for GT entries: {len(gt_found)}")
    report_lines.append(f"Top-N threshold: {args.top}")
    report_lines.append(f"")

    for code, expected_pc in sorted(gt_found.items()):
        r = gt_result_map.get(code)
        if not r:
            missed.append({'code': code, 'expected_pc': f'0x{expected_pc:08x}',
                          'reason': 'no result'})
            continue

        _, rom_path, candidates, err = r
        if err:
            missed.append({'code': code, 'expected_pc': f'0x{expected_pc:08x}',
                          'reason': f'error: {err}'})
            continue

        found_rank = None
        for pc, conf, mode, io_reads, rank, loop_size, reasons in candidates:
            if pc == expected_pc:
                found_rank = rank
                break

        if found_rank is not None:
            hit_any += 1
            rank_histogram[found_rank] += 1
            if found_rank == 1:
                hit_top1 += 1
            if found_rank <= args.top:
                hit_topN += 1
            report_lines.append(f"  ✓ {code}  0x{expected_pc:08x}  rank=#{found_rank}")
        else:
            # Check if we have any candidates at all
            n_cands = len(candidates)
            top_pcs = [f"0x{c[0]:08x}({c[1]:.0%})" for c in candidates[:3]]
            missed.append({
                'code': code,
                'expected_pc': f'0x{expected_pc:08x}',
                'rom_path': rom_path,
                'reason': f'not in {n_cands} candidates',
                'top_candidates': top_pcs
            })
            report_lines.append(
                f"  ✗ {code}  0x{expected_pc:08x}  MISSED "
                f"({n_cands} cands, top: {', '.join(top_pcs[:3])})")

    total = len(gt_found)
    report_lines.append(f"")
    report_lines.append(f"{'='*64}")
    report_lines.append(f"RECALL SUMMARY")
    report_lines.append(f"  Top-1 recall: {hit_top1}/{total} = {hit_top1/total:.1%}")
    report_lines.append(f"  Top-{args.top} recall: {hit_topN}/{total} = {hit_topN/total:.1%}")
    report_lines.append(f"  Any-rank recall: {hit_any}/{total} = {hit_any/total:.1%}")
    report_lines.append(f"  Missed: {total - hit_any}/{total}")
    report_lines.append(f"")
    report_lines.append(f"Rank histogram:")
    for rank in sorted(rank_histogram.keys()):
        report_lines.append(f"  Rank #{rank}: {rank_histogram[rank]}")

    # 6. Collect extra findings (candidates for ROMs NOT in gba_over.h)
    all_result_map = {r[0]: r for r in all_results}
    extras = []
    for code, (_, rom_path, candidates, err) in sorted(all_result_map.items()):
        if err or not candidates:
            continue
        if code in gt:
            continue  # already in ground truth
        best = candidates[0]
        pc, conf, mode, io_reads, rank, loop_size, reasons = best
        if conf >= 0.5:  # only report high-confidence extras
            extras.append({
                'code': code,
                'rom_path': rom_path,
                'idle_loop_pc': f'0x{pc:08x}',
                'confidence': conf,
                'mode': mode,
                'io_reads': io_reads,
                'loop_size': loop_size,
                'reasons': reasons
            })

    report_lines.append(f"")
    report_lines.append(f"EXTRA FINDINGS (not in gba_over.h, conf >= 0.5)")
    report_lines.append(f"  Total: {len(extras)} ROMs")

    # Print report
    report_text = '\n'.join(report_lines)
    print(f"\n{report_text}")

    # Write report
    with open(args.report, 'w') as f:
        f.write(report_text + '\n')
    print(f"\nReport written to {args.report}")

    # Write CSV of all results
    with open(args.csv, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['game_code', 'rom_path', 'expected_pc', 'found_pc',
                     'confidence', 'rank', 'mode', 'io_reads', 'loop_size',
                     'match', 'reasons'])
        for code, expected_pc in sorted(gt_found.items()):
            r = gt_result_map.get(code)
            if not r:
                w.writerow([code, '', f'0x{expected_pc:08x}', '', '', '', '', '', '', 'NO_ROM', ''])
                continue
            _, rom_path, candidates, err = r
            if err:
                w.writerow([code, rom_path, f'0x{expected_pc:08x}', '', '', '', '', '', '', f'ERROR:{err}', ''])
                continue
            found = False
            for pc, conf, mode, io_reads, rank, loop_size, reasons in candidates[:20]:
                match = 'YES' if pc == expected_pc else 'no'
                w.writerow([code, rom_path, f'0x{expected_pc:08x}', f'0x{pc:08x}',
                           f'{conf:.2f}', rank, mode, ';'.join(io_reads),
                           loop_size, match, reasons])
                if pc == expected_pc:
                    found = True
            if not found:
                w.writerow([code, rom_path, f'0x{expected_pc:08x}', 'NOT_FOUND',
                           '', '', '', '', '', 'MISSED', ''])

    print(f"CSV written to {args.csv}")

    # Write missed JSON
    with open(args.missed_json, 'w') as f:
        json.dump(missed, f, indent=2)
    print(f"Missed entries written to {args.missed_json}")

    # Write extras JSON
    with open(args.extra_json, 'w') as f:
        json.dump(extras, f, indent=2)
    print(f"Extra findings written to {args.extra_json}")


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Full idle loop detection pipeline: static analysis + harness validation.

For each ROM:
  1. Run idle_loop_finder.py to get top-N candidates
  2. Validate each candidate with the harness (speedup + frame match)
  3. Pick the best valid candidate (highest speedup)

Output: JSON file mapping game_code → validated idle_loop_target_pc

Usage:
  python3 idle_loop_pipeline.py                       # all ROMs, top-3
  python3 idle_loop_pipeline.py --top 5               # top-5 candidates
  python3 idle_loop_pipeline.py --code BKME           # single ROM
  python3 idle_loop_pipeline.py --frames 600          # more frames
  python3 idle_loop_pipeline.py --workers 8           # parallelism
  python3 idle_loop_pipeline.py --resume results.json # resume from partial
"""
import argparse, hashlib, json, os, re, struct, subprocess, sys, tempfile, time
import multiprocessing as mp
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HARNESS = os.path.join(ROOT, 'tests/harness/test_gba_x86')
BIOS = os.path.join(ROOT, 'JC4880P443C_I_W/gba_bios.bin')
ROM_DIR = '/mnt/planet/data/GBA/ALLROMS/roms_sorted/'
SPEEDUP_THRESH = 1.05

sys.path.insert(0, os.path.join(ROOT, 'tools'))

def get_game_code(rom_path):
    try:
        with open(rom_path, 'rb') as f:
            f.seek(0xAC)
            code = f.read(4)
            if len(code) == 4:
                s = code.decode('ascii', errors='replace')
                if s.isprintable() and '\x00' not in s:
                    return s
    except:
        pass
    return ""

def build_rom_map(rom_dir):
    code_map = {}
    for dirpath, _, filenames in os.walk(rom_dir):
        for fn in filenames:
            if fn.lower().endswith('.gba'):
                full = os.path.join(dirpath, fn)
                code = get_game_code(full)
                if code and len(code) == 4 and code not in code_map:
                    code_map[code] = full
    return code_map

def file_md5(path):
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()

def parse_fps(output):
    for line in output.split('\n'):
        if 'Effective FPS' in line:
            parts = line.split(':')
            if len(parts) >= 2:
                try:
                    return float(parts[-1].strip())
                except ValueError:
                    pass
    return None

def run_harness(rom_path, frames, idle_loop_pc=None, workdir=None):
    cmd = [HARNESS]
    if idle_loop_pc is not None:
        cmd += ['--idle-loop', f'0x{idle_loop_pc:08x}']
    cmd += [BIOS, rom_path, str(frames)]

    out_file = os.path.join(workdir, 'frames_single.bin')
    if os.path.exists(out_file):
        os.unlink(out_file)

    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=300, cwd=workdir)
    except subprocess.TimeoutExpired:
        return None, None, 'timeout'
    except Exception as e:
        return None, None, str(e)

    if result.returncode != 0:
        return None, None, f'exit {result.returncode}: {result.stderr[:200]}'

    if not os.path.exists(out_file):
        return None, None, 'no output file'

    fps = parse_fps(result.stdout)
    return file_md5(out_file), fps, None

def process_one_rom(args):
    """Worker: analyze + validate one ROM.
    Returns (code, rom_path, results_list).
    results_list items: (pc, speedup, frames_match, confidence, error)
    """
    code, rom_path, top_n, frames = args
    
    # Step 1: Static analysis
    from idle_loop_finder import IdleLoopFinder
    try:
        with open(rom_path, 'rb') as f:
            data = f.read()
        finder = IdleLoopFinder(data)
        cands = finder.find()[:top_n]
    except Exception as e:
        return (code, rom_path, [], f'analysis error: {e}')

    if not cands:
        return (code, rom_path, [], None)

    # Collect unique PCs to test (from candidates + alt_pcs)
    pcs_to_test = []
    seen = set()
    for c in cands:
        if c.pc not in seen:
            pcs_to_test.append((c.pc, c.confidence))
            seen.add(c.pc)
        # Also test top alt_pcs (loop body addresses)
        for alt in c.alt_pcs[:3]:
            if alt not in seen:
                pcs_to_test.append((alt, c.confidence * 0.9))
                seen.add(alt)

    # Step 2: Harness validation
    results = []
    with tempfile.TemporaryDirectory(prefix=f'idle_{code}_') as td:
        # Baseline run
        base_md5, base_fps, err = run_harness(rom_path, frames, workdir=td)
        if err:
            return (code, rom_path, [], f'baseline: {err}')

        # Test each PC
        for pc, confidence in pcs_to_test:
            idle_md5, idle_fps, err = run_harness(
                rom_path, frames, idle_loop_pc=pc, workdir=td)
            if err:
                results.append((pc, None, None, confidence, err))
                continue

            frames_match = (base_md5 == idle_md5)
            speedup = (idle_fps / base_fps) if base_fps and idle_fps else None
            results.append((pc, speedup, frames_match, confidence, None))

    return (code, rom_path, results, None)

def main():
    parser = argparse.ArgumentParser(description='Full idle loop detection pipeline')
    parser.add_argument('--code', type=str, help='Single game code')
    parser.add_argument('--top', type=int, default=3, help='Top N candidates per ROM')
    parser.add_argument('--frames', type=int, default=600, help='Frames to run')
    parser.add_argument('--workers', type=int, default=8, help='Parallel workers')
    parser.add_argument('--resume', type=str, help='Resume from partial results JSON')
    parser.add_argument('--output', type=str, default='idle_loop_results.json',
                        help='Output file (default: idle_loop_results.json)')
    parser.add_argument('--rom-dir', type=str, default=ROM_DIR,
                        help=f'ROM directory to scan (default: {ROM_DIR})')
    args = parser.parse_args()

    if not os.path.exists(HARNESS):
        print(f'ERROR: harness not found at {HARNESS}')
        print('Build: cd tests/harness && make ARCH=x86 DYNAREC=0')
        return 1

    # Load existing results for resume
    done = {}
    if args.resume and os.path.exists(args.resume):
        with open(args.resume) as f:
            prev = json.load(f)
        for entry in prev:
            done[entry['code']] = entry
        print(f'Resuming: {len(done)} games already done')

    # Build ROM map
    code_map = build_rom_map(args.rom_dir)
    print(f'Found {len(code_map)} unique ROMs')

    if args.code:
        if args.code not in code_map:
            print(f'ROM not found for code {args.code}')
            return 1
        codes = [args.code]
    else:
        codes = sorted(code_map.keys())

    # Filter already done
    work = []
    for code in codes:
        if code not in done:
            work.append((code, code_map[code], args.top, args.frames))

    print(f'Processing {len(work)} ROMs ({len(done)} already done)')
    if not work:
        print('Nothing to do')
        return 0

    nworkers = min(args.workers, len(work))
    t0 = time.time()

    all_results = list(done.values())  # start with previous results

    # Process in batches for progress reporting and incremental save
    BATCH = 50
    for batch_start in range(0, len(work), BATCH):
        batch = work[batch_start:batch_start + BATCH]
        
        if nworkers <= 1:
            batch_results = [process_one_rom(w) for w in batch]
        else:
            with mp.Pool(nworkers) as pool:
                batch_results = pool.map(process_one_rom, batch)

        for code, rom_path, results, error in batch_results:
            entry = {
                'code': code,
                'rom': os.path.basename(rom_path),
                'best_pc': None,
                'best_speedup': None,
                'n_candidates_tested': len(results),
                'n_valid': 0,
                'error': error,
                'all_valid': [],
            }

            if results:
                valid = [(pc, sp, conf) for pc, sp, fm, conf, err in results
                         if fm and sp and sp >= SPEEDUP_THRESH and not err]
                entry['n_valid'] = len(valid)
                if valid:
                    # Pick highest speedup
                    best = max(valid, key=lambda x: x[1])
                    entry['best_pc'] = f'0x{best[0]:08x}'
                    entry['best_speedup'] = round(best[1], 3)
                    entry['all_valid'] = [
                        {'pc': f'0x{pc:08x}', 'speedup': round(sp, 3), 'confidence': round(conf, 3)}
                        for pc, sp, conf in sorted(valid, key=lambda x: -x[1])
                    ]

            all_results.append(entry)

        elapsed = time.time() - t0
        done_count = batch_start + len(batch)
        rate = done_count / elapsed if elapsed > 0 else 0
        remaining = (len(work) - done_count) / rate if rate > 0 else 0
        found = sum(1 for r in all_results if r['best_pc'])
        print(f'  [{done_count}/{len(work)}] {elapsed:.0f}s elapsed, '
              f'~{remaining:.0f}s remaining, {found} games with idle loop')

        # Incremental save
        out_path = os.path.join(ROOT, args.output)
        with open(out_path, 'w') as f:
            json.dump(all_results, f, indent=2)

    # Final report
    found = sum(1 for r in all_results if r['best_pc'])
    errors = sum(1 for r in all_results if r['error'])
    print(f'\n{"="*60}')
    print(f'Total ROMs:     {len(all_results)}')
    print(f'Idle loop found: {found} ({found/len(all_results):.1%})')
    print(f'No idle loop:   {len(all_results) - found - errors}')
    print(f'Errors:         {errors}')
    print(f'Results saved to {os.path.join(ROOT, args.output)}')

if __name__ == '__main__':
    sys.exit(main() or 0)

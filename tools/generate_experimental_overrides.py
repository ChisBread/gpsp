#!/usr/bin/env python3
"""Generate gba_over_experimental.h from pipeline results.

Reads idle_loop_results.json and gba_over.h, then generates an experimental
overrides table containing only entries NOT already present in gba_over.h
(or present with idle_loop=0).

Usage:
  python3 generate_experimental_overrides.py                    # dry-run
  python3 generate_experimental_overrides.py --write            # write file
  python3 generate_experimental_overrides.py --min-speedup 1.1  # higher threshold
"""
import argparse, json, os, re, sys
from collections import OrderedDict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse_existing_idle_loops(path):
    """Parse gba_over.h and return set of codes that already have idle_loop != 0."""
    codes_with_idle = set()
    with open(path) as f:
        text = f.read()

    pattern = re.compile(
        r'"(\w{4})".*?gamepak_code.*?\n'
        r'(?:\s*/\*.*?\*/\s*\n)*'
        r'\s*(.*?),\s*(?:/\*\s*flags.*?\n|\n)'
        r'\s*(0x[\da-fA-F]+|0)\s*,\s*/\*\s*idle_loop'
    )
    for m in pattern.finditer(text):
        code, flags, idle_pc = m.groups()
        if int(idle_pc, 0) != 0:
            codes_with_idle.add(code)
    return codes_with_idle


def format_entry(code, pc, comment, speedup=None, commented=False):
    """Format one experimental entry."""
    pc_str = f'0x{pc:08x}'
    sp_str = f' [{speedup:.2f}x]' if speedup else ''
    if commented:
        return (
            f'   // {{ // {comment}{sp_str}\n'
            f'   //    "{code}",  {pc_str},\n'
            f'   // }}'
        )
    return (
        f'   {{ // {comment}{sp_str}\n'
        f'      "{code}",  {pc_str},\n'
        f'   }}'
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--write', action='store_true',
                        help='Write gba_over_experimental.h')
    parser.add_argument('--results', type=str, default='idle_loop_results.json',
                        help='Pipeline results JSON')
    parser.add_argument('--min-speedup', type=float, default=1.05,
                        help='Min speedup to include (default 1.05)')
    args = parser.parse_args()

    gba_over_path = os.path.join(ROOT, 'gba_over.h')
    out_path = os.path.join(ROOT, 'gba_over_experimental.h')
    results_path = (args.results if os.path.isabs(args.results)
                    else os.path.join(ROOT, args.results))

    # Get codes already covered in gba_over.h
    existing_idle = parse_existing_idle_loops(gba_over_path)
    print(f'Existing entries with idle_loop in gba_over.h: {len(existing_idle)}')

    # Load pipeline results
    with open(results_path) as f:
        pipeline = json.load(f)

    # Collect new entries: valid idle loop AND not already in gba_over.h
    new_entries = []
    for r in pipeline:
        if not r.get('best_pc'):
            continue
        if r.get('best_speedup', 0) < args.min_speedup:
            continue
        code = r['code']
        if code in existing_idle:
            continue
        pc = int(r['best_pc'], 16)
        rom_name = r.get('rom', code).replace('.gba', '')
        new_entries.append((code, pc, rom_name, r['best_speedup']))

    new_entries.sort(key=lambda x: x[0])  # sort by game code

    print(f'Pipeline valid results (not in gba_over.h): {len(new_entries)}')

    # Speedup stats
    if new_entries:
        speedups = [s for _, _, _, s in new_entries]
        print(f'  Speedup range: {min(speedups):.2f}x - {max(speedups):.2f}x')
        print(f'  Median: {sorted(speedups)[len(speedups)//2]:.2f}x')
        gt2 = sum(1 for s in speedups if s >= 2.0)
        gt3 = sum(1 for s in speedups if s >= 3.0)
        print(f'  >2.0x: {gt2}, >3.0x: {gt3}')

    # Split into active (>= 1.1x) and commented (< 1.1x)
    COMMENT_THRESH = 1.1
    active_entries = [(c, p, cm, s) for c, p, cm, s in new_entries if s >= COMMENT_THRESH]
    commented_entries = [(c, p, cm, s) for c, p, cm, s in new_entries if s < COMMENT_THRESH]

    print(f'  Active (>= {COMMENT_THRESH}x): {len(active_entries)}')
    print(f'  Commented (< {COMMENT_THRESH}x): {len(commented_entries)}')

    # Generate output
    entries_parts = []
    for code, pc, comment, speedup in active_entries:
        entries_parts.append(format_entry(code, pc, comment, speedup))

    entries_str = ',\n'.join(entries_parts)

    # Commented-out entries appended after the array
    commented_parts = []
    for code, pc, comment, speedup in commented_entries:
        commented_parts.append(format_entry(code, pc, comment, speedup, commented=True))

    output = (
        '// Auto-generated experimental idle loop overrides.\n'
        '// These entries are discovered by the idle_loop_pipeline tool and validated\n'
        '// via harness FPS speedup measurement. They are only applied when\n'
        '// use_experimental_overrides is enabled at runtime.\n'
        f'// Total entries: {len(active_entries)} active, {len(commented_entries)} commented (<{COMMENT_THRESH}x)\n'
        '\n'
        'static const ini_experimental_t gbaover_experimental[] = {\n'
    )
    if entries_str:
        output += entries_str + ',\n'
    else:
        output += '   { "",  0 },  // placeholder\n'
    output += '};\n'

    if commented_parts:
        output += '\n// Low-speedup entries (< ' + f'{COMMENT_THRESH}x' + ') — uncomment to enable:\n'
        output += ',\n'.join(commented_parts) + '\n'

    if args.write:
        with open(out_path, 'w') as f:
            f.write(output)
        print(f'\nWrote {out_path} ({len(new_entries)} entries)')
    else:
        print(f'\nDry run. Use --write to generate gba_over_experimental.h')
        # Show sample entries
        print(f'\nSample entries:')
        for code, pc, comment, speedup in new_entries[:15]:
            print(f'  {code}: 0x{pc:08x} ({speedup:.2f}x) — {comment}')
        if len(new_entries) > 15:
            print(f'  ... and {len(new_entries) - 15} more')


if __name__ == '__main__':
    sys.exit(main() or 0)

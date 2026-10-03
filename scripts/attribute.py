#!/usr/bin/env python3
"""Attribute cost to source-level categories for one book version.

Combines two views of the same replay:
  * perf cycles samples aggregated by source line (real time, but sampling has
    skid without PEBS: a stall is charged to the instruction after it)
  * cachegrind per-line counts (deterministic Ir / D1 read / D1 write misses)

usage: scripts/attribute.py <perf.data> <cachegrind.out> [book.hpp]
Categories are defined by file and, inside the book header, by the line ranges
of its functions (found by parsing the header), so the script follows the code.
"""
import collections
import re
import subprocess
import sys

perf_data, cg_out = sys.argv[1], sys.argv[2]
book_hdr = sys.argv[3] if len(sys.argv) > 3 else None

# ---- function line ranges in the book header -----------------------------------
ranges = []  # (first_line, name)
if book_hdr:
    pat = re.compile(r'^  (?:\[\[nodiscard\]\] )?(?:void|auto|template <[^>]*>\s*auto) ([a-z_]+)\(')
    for n, line in enumerate(open(book_hdr), 1):
        m = pat.match(line)
        if m:
            ranges.append((n, m.group(1)))
        elif line.startswith('  template <Side TakerSide>'):
            ranges.append((n, 'match'))
GROUP = {
    'push': 'public op body (push/cancel/modify, inline index lookups)',
    'cancel': 'public op body (push/cancel/modify, inline index lookups)',
    'modify': 'public op body (push/cancel/modify, inline index lookups)',
    'slot_of': 'grid: slot_of / price_of / level_of',
    'price_of': 'grid: slot_of / price_of / level_of',
    'level_of': 'grid: slot_of / price_of / level_of',
    'anchor': 'grid: slot_of / price_of / level_of',
    'alloc_node': 'node pool + level list',
    'free_node': 'node pool + level list',
    'push_back': 'node pool + level list',
    'unlink': 'node pool + level list',
    'add': 'match (incl. add)',
    'match': 'match (incl. add)',
    'rest': 'rest',
    'erase': 'erase',
}


def book_category(line):
    if line == 0:
        return 'book.hpp:0 (no line info)'
    name = None
    for first, fn in ranges:
        if first <= line:
            name = fn
    return GROUP.get(name, f'book.hpp other ({name})')


def category(path, line):
    f = path.rsplit('/', 1)[-1]
    if 'boost/unordered' in path or f in ('pair.h',):
        return 'index: boost flat_map (probe/insert/erase, slot copy)'
    if f == 'book.hpp' and book_hdr:
        return book_category(line)
    if f == 'hier_bitmap.hpp':
        return 'grid: hierarchical bitmap'
    if f == 'vector.h':
        return 'node pool + level list'
    if f == 'events.hpp':
        return 'event sink (ChecksumSink)'
    if f in ('replay.hpp', 'main.cxx'):
        return 'replay loop / dispatch'
    if 'malloc' in path:
        return 'malloc'
    return f'other: {f}'


# ---- perf: cycles by source line ---------------------------------------------------
out = subprocess.run(
    ['perf', 'report', '-i', perf_data, '--stdio', '--no-children', '-g', 'none',
     '--sort', 'srcline', '--percent-limit', '0', '-F', 'overhead,srcline', '--full-source-path'],
    capture_output=True, text=True).stdout
perf_cat = collections.Counter()
for line in out.splitlines():
    m = re.match(r'\s+([\d.]+)%\s+(\S+):(\d+)', line)
    if m:
        perf_cat[category(m.group(2), int(m.group(3)))] += float(m.group(1))
    else:
        m = re.match(r'\s+([\d.]+)%\s+(\S+)', line)
        if m:
            perf_cat['other: ' + m.group(2)[:40]] += float(m.group(1))

# ---- cachegrind: Ir, D1mr, D1mw by source line -------------------------------------
cg_cat = collections.defaultdict(lambda: [0, 0, 0])
events = None
fl = None
for line in open(cg_out):
    if line.startswith('events:'):
        events = line.split()[1:]
        continue
    if line.startswith('fl='):
        fl = line[3:].strip()
        continue
    if line.startswith(('fn=', 'desc:', 'cmd:', 'summary:')) or not line[:1].isdigit():
        continue
    parts = line.split()
    vals = dict(zip(events, map(int, parts[1:])))
    c = cg_cat[category(fl, int(parts[0]))]
    c[0] += vals.get('Ir', 0)
    c[1] += vals.get('D1mr', 0)
    c[2] += vals.get('D1mw', 0)
tot = [sum(v[i] for v in cg_cat.values()) for i in range(3)]

cats = sorted(set(perf_cat) | set(cg_cat), key=lambda c: -perf_cat.get(c, 0))
print(f"{'category':<62}{'cycles%':>9}{'Ir%':>8}{'D1mr%':>8}{'D1mw%':>8}")
for c in cats:
    p = perf_cat.get(c, 0)
    v = cg_cat.get(c, [0, 0, 0])
    if p < 0.3 and v[0] < 0.003 * tot[0]:
        continue
    print(f"{c[:61]:<62}{p:>8.1f}%" + ''.join(f"{100 * v[i] / max(tot[i], 1):>7.1f}%" for i in range(3)))
print(f"{'(cachegrind totals: Ir / D1mr / D1mw)':<62}{'':>9}{tot[0]:>8,} {tot[1]:,} {tot[2]:,}")

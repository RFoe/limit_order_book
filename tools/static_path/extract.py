#!/usr/bin/env python3
"""Reconstruct the dominant execution path of a code region for static analysis.

Inputs: a callgrind profile taken with --dump-instr=yes --collect-jumps=yes and
the same binary. From a start address it walks the code: conditional branches
follow the more frequently executed direction (from callgrind's jcnd counts),
direct jumps/calls are followed, indirect jumps take their most frequent
target, ret returns to the caller. The result is a straight-line instruction
sequence (jmp/call/ret dropped, conditional branches kept as not-taken so
macro-fusion is preserved) that uiCA / llvm-mca can analyse.

  extract.py PROFILE BINARY --list-hot FUNC_SUBSTRING        # find start points
  extract.py PROFILE BINARY --start 0xADDR [--stop 0xADDR] [--take 0xSRC=0xDST ...] -o path.s

Caveat: the path is a "typical" path stitched from per-branch majorities; it
is not guaranteed to be a single executed trace (branch outcomes may be
correlated).
"""
import argparse
import collections
import re
import subprocess
import sys


def parse_callgrind(path):
    ir = collections.Counter()  # addr -> executions
    jcnd = {}  # src -> (taken, executed, target)
    jumps = collections.defaultdict(collections.Counter)  # src -> {target: count} (unconditional / indirect)
    fn_of = {}
    cur = 0
    fn = None
    names = {}
    pending = None  # ('jcnd', taken, total, target) or ('jump', count, target) or ('call',)

    def resolve(spec, base):
        if spec == '*':
            return base
        if spec.startswith(('+', '-')):
            return base + int(spec, 0)
        return int(spec, 0)

    for line in open(path):
        line = line.rstrip('\n')
        if not line:
            continue
        if line.startswith(('fn=', 'cfn=')):
            # name compression: "(id) name" defines id, "(id)" refers to it; the
            # definition may appear first in either fn= or cfn=
            m = re.match(r'c?fn=\((\d+)\)(?: (.*))?', line)
            if m:
                if m.group(2):
                    names[m.group(1)] = m.group(2)
                if line.startswith('fn='):
                    fn = names.get(m.group(1), m.group(1))
            continue
        if line.startswith(('jcnd=', 'jump=')):
            kind, rest = line.split('=', 1)
            parts = rest.split()
            if kind == 'jcnd':
                taken, total = map(int, parts[0].split('/'))
                pending = ('jcnd', taken, total, resolve(parts[1], cur))
            else:
                pending = ('jump', int(parts[0]), resolve(parts[1], cur))
            continue
        if line.startswith('calls='):
            pending = ('call',)
            continue
        if line[0] in '0123456789+-*' and not line.startswith(('-- ',)):
            parts = line.split()
            addr = resolve(parts[0], cur)
            cur = addr
            if pending is not None:
                if pending[0] == 'jcnd':
                    _, taken, total, tgt = pending
                    t0, e0, _ = jcnd.get(addr, (0, 0, tgt))
                    jcnd[addr] = (t0 + taken, e0 + total, tgt)
                elif pending[0] == 'jump':
                    _, cnt, tgt = pending
                    jumps[addr][tgt] += cnt
                # a 'call' line's cost is inclusive: do not count it as executions
                was_call = pending[0] == 'call'
                pending = None
                if was_call or len(parts) < 3:
                    continue
            if len(parts) >= 3:
                ir[addr] += int(parts[2])
                fn_of[addr] = fn
    return ir, jcnd, jumps, fn_of


def disassemble(binary):
    out = subprocess.run(['llvm-objdump-23', '-d', '--no-show-raw-insn', '--print-imm-hex', binary],
                         capture_output=True, text=True).stdout
    ins = {}
    order = []
    for line in out.splitlines():
        m = re.match(r'\s+([0-9a-f]+):\s+(.*)$', line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        text = m.group(2).split('#')[0].rstrip()
        if not text:
            continue
        ins[addr] = text
        order.append(addr)
    nxt = {a: b for a, b in zip(order, order[1:])}
    return ins, nxt


def branch_target(text):
    m = re.match(r'(j\w+|call\w*)\s+(0x[0-9a-f]+|[0-9a-f]+)\b', text)
    if m:
        return int(m.group(2), 16)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile')
    ap.add_argument('binary')
    ap.add_argument('--list-hot')
    ap.add_argument('--start')
    ap.add_argument('--stop', action='append', default=[])
    ap.add_argument('--take', action='append', default=[], help='force src=dst for a branch')
    ap.add_argument('--max', type=int, default=2000)
    ap.add_argument('-o')
    a = ap.parse_args()

    ir, jcnd, jumps, fn_of = parse_callgrind(a.profile)
    ins, nxt = disassemble(a.binary)

    if a.list_hot:
        rows = [(addr, cnt) for addr, cnt in ir.items() if a.list_hot in (fn_of.get(addr) or '')]
        rows.sort()
        print(f"{len(rows)} executed instructions in functions matching {a.list_hot!r}")
        for addr, cnt in rows:
            t = ins.get(addr, '?')
            extra = ''
            if addr in jcnd:
                tk, ex, tg = jcnd[addr]
                extra = f'   taken {tk}/{ex} -> {tg:#x}'
            elif addr in jumps:
                extra = '   -> ' + ', '.join(f'{t2:#x}:{c}' for t2, c in jumps[addr].most_common(4))
            if cnt >= 100_000 or extra:
                print(f"{addr:#8x} {cnt:>10} {t}{extra}")
        return

    forced = {int(k, 0): int(v, 0) for k, v in (x.split('=') for x in a.take)}
    stops = {int(x, 0) for x in a.stop}
    addr = int(a.start, 0)
    stack = []
    path = []
    seen_steps = 0
    while seen_steps < a.max:
        seen_steps += 1
        if addr in stops and path:
            break
        text = ins.get(addr)
        if text is None:
            print(f"stop: no instruction at {addr:#x}", file=sys.stderr)
            break
        op = text.split()[0]
        cnt = ir.get(addr, 0)
        if op.startswith('ret'):
            if not stack:
                break
            addr = stack.pop()
            continue
        if op.startswith('call'):
            tgt = branch_target(text)
            if tgt is None or tgt not in ins or ir.get(tgt, 0) == 0:
                path.append((addr, cnt, text + '   # call not followed'))
                addr = nxt[addr]
                continue
            stack.append(nxt[addr])
            addr = tgt
            continue
        if op.startswith('jmp'):
            tgt = forced.get(addr) or branch_target(text)
            if tgt is None and jumps.get(addr):  # indirect: most frequent target
                tgt = jumps[addr].most_common(1)[0][0]
            if tgt is None:
                break
            addr = tgt
            continue
        if op.startswith('j'):  # conditional
            tgt = branch_target(text)
            tk, ex, _ = jcnd.get(addr, (0, 0, tgt))
            take = (forced[addr] == tgt) if addr in forced else (tk * 2 > ex)
            path.append((addr, cnt, f'{op} 1b   # cond: taken {tk}/{ex}, path {"takes" if take else "falls through"}'))
            addr = tgt if take else nxt[addr]
            continue
        path.append((addr, cnt, text))
        addr = nxt[addr]

    lines = [f'# path from {a.start}: {len(path)} instructions (jmp/call/ret dropped)', '.att_syntax', '1:']
    for addr_, cnt, text in path:
        text = re.sub(r'\s+<[^>]*>', '', text)  # drop symbol annotations
        lines.append(f'    {text:<60} # {addr_:#x} x{cnt}')
    out = '\n'.join(lines) + '\n'
    if a.o:
        open(a.o, 'w').write(out)
    print(out)


if __name__ == '__main__':
    main()

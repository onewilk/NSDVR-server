#!/usr/bin/env python3
"""Worst-case stack depth from GCC -fcallgraph-info=su files (static frames; recursion reported)."""
import glob, re, sys
nodes, edges = {}, {}
for f in glob.glob(sys.argv[1] + '/*.ci'):
    for line in open(f):
        m = re.match(r'node: \{ title: "([^"]+)" label: "([^"]*)"', line)
        if m:
            title, label = m.groups()
            s = re.search(r'\\n(\d+) bytes \((\w[\w,]*)\)', label)
            if s:
                nodes[title] = (int(s.group(1)), s.group(2))
            continue
        m = re.match(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"', line)
        if m:
            edges.setdefault(m.group(1), set()).add(m.group(2))

def resolve(t):
    if t in nodes:
        return t
    # static functions are "file:name"; external references use the bare name
    return t

memo, cycles, dyn = {}, set(), set()
def depth(fn, stack):
    if fn in stack:
        cycles.add(fn)
        return 0, [fn + ' (recursion)']
    if fn in memo:
        return memo[fn]
    size, kind = nodes.get(fn, (0, 'unknown'))
    if 'dynamic' in kind:
        dyn.add((fn, kind))
    best, path = 0, []
    for t in edges.get(fn, ()):
        d, p = depth(resolve(t), stack | {fn})
        if d > best:
            best, path = d, p
    memo[fn] = (size + best, [f'{fn} [{size}]'] + path)
    return memo[fn]

for root in sys.argv[2:]:
    memo.clear()
    d, p = depth(root, frozenset())
    print(f'{root}: {d} bytes worst case (static frames)')
    for x in p:
        print('   ', x)
print('recursive functions:', sorted(cycles))
print('dynamic frames:', sorted(dyn))

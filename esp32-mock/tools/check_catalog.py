#!/usr/bin/env python3
"""Checks that firmware-core/src/catalog.cpp matches protocol.json effects.table (ids, names, categories, parameter names/ranges/defaults).
Cost/mem are not in the protocol and are not checked. Exit 1 on any difference."""
import json, re, sys, os
here = os.path.dirname(os.path.abspath(__file__))
tab = json.load(open(os.path.join(here, '..', '..', 'protocol.json')))['effects']['table']
src = open(os.path.join(here, '..', 'firmware-core', 'src', 'catalog.cpp')).read()
rows = re.findall(r'^\s*\{(\d+), "(\w+)", "(\w+)", (\d+), (\d+), (\d+), \{(.*)\}\},$', src, re.M)
bad = []
if len(rows) != len(tab): bad.append(f'count {len(rows)} != {len(tab)}')
for r, f in zip(rows, tab):
    i, name, cat, cost, mem, np_, ps = r
    got = [(n, int(a), int(b), int(c)) for n, a, b, c in re.findall(r'\{"(\w+)", (-?\d+), (-?\d+), (-?\d+)\}', ps)]
    want = [(p['name'], p['min'], p['max'], p['default']) for p in f['parameters']]
    if (int(i), name, cat) != (f['id'], f['name'], f['category']) or got != want or int(np_) != len(want):
        bad.append(f"id {f['id']} {f['name']}: catalog {i} {name} {cat} {got}")
print('catalog.cpp matches protocol.json' if not bad else '\n'.join(bad)); sys.exit(1 if bad else 0)

# SPDX-License-Identifier: GPL-3.0-or-later
"""Census of a directory of Skyrim PEX files: opcodes, native functions and
the calls that reach them, engine events, fragment scripts.

    bethconv extract --source <Misc.bsa>... --from pex-list.txt -o pex/
    python3 pex_census.py pex/

Call targets are resolved through declared types of locals, parameters,
object variables and `self`, then up the class hierarchy.
"""
import collections
import pathlib
import sys

import pex

FRAGMENT_PREFIXES = ('tif__', 'qf_', 'sf_', 'pf_', 'prkf_')


def load(root):
    classes = {}
    for path in sorted(pathlib.Path(root).rglob('*.pex')):
        p = pex.Pex(path.read_bytes())
        assert p.r.p == len(p.r.b), f'{path}: {len(p.r.b) - p.r.p} bytes left over'
        for o in p.objects:
            classes[o['name'].lower()] = o
    return classes


def functions(o):
    for state, fs in o['states']:
        for f in fs:
            yield state, f
    for prop in o['props']:
        for f in prop['funcs']:
            yield '', f


def main(root):
    classes = load(root)

    def parent(c):
        o = classes.get(c)
        return o['parent'].lower() if o and o['parent'] else None

    natives = collections.defaultdict(set)
    for c, o in classes.items():
        for _, f in functions(o):
            if f['flags'] & 2:
                natives[c].add(f['name'].lower())

    def resolve(c, name):
        for _ in range(64):
            if c is None or c not in classes:
                return None
            if name in natives.get(c, ()):
                return c
            if any(f['name'].lower() == name for _, f in functions(classes[c])):
                return 'script'
            c = parent(c)
        return None

    ops = collections.Counter()
    calls = collections.Counter()
    per_script = {}
    unresolved = 0
    script_calls = 0
    for c, o in classes.items():
        if c in natives:
            continue
        used = set()
        members = {a.lower(): b.lower() for a, b in o['vars']}
        for _, f in functions(o):
            types = dict(members)
            types.update({a.lower(): b.lower() for a, b in f['params'] + f['locals']})
            types['self'] = c
            for op, args, _ in f['code']:
                ops[pex.OPS[op][0]] += 1
                if op == 23:
                    target = types.get(args[1][1].lower()) if args[1][0] == 'id' else None
                    name = args[0][1].lower()
                elif op == 24:
                    target, name = parent(c), args[0][1].lower()
                elif op == 25:
                    target, name = args[0][1].lower(), args[1][1].lower()
                else:
                    continue
                if target is None or target.endswith('[]'):
                    unresolved += 1
                    continue
                where = resolve(target, name)
                if where == 'script':
                    script_calls += 1
                elif where is None:
                    unresolved += 1
                else:
                    calls[f'{where}.{name}'] += 1
                    used.add(f'{where}.{name}')
        per_script[c] = used

    declared = sum(len(v) for v in natives.values())
    sites = sum(calls.values())
    print(f'scripts {len(classes)}, native-declaring {len(natives)}, natives declared {declared}')
    print(f'instructions {sum(ops.values())}, opcodes used {len(ops)} of {len(pex.OPS)}')
    print('  ' + ', '.join(f'{k} {v}' for k, v in ops.most_common()))
    print(f'calls: native {sites}, script {script_calls}, unresolved {unresolved}')
    print(f'natives called {len(calls)} of {declared}')

    print('  ' + ', '.join(f'{k} {v}' for k, v in calls.most_common(12)))
    ranked = [k for k, _ in calls.most_common()]
    total = 0
    marks = iter((0.5, 0.8, 0.9, 0.95, 0.99, 1.0))
    mark = next(marks)
    for n, k in enumerate(ranked, 1):
        total += calls[k]
        while mark is not None and total >= mark * sites - 1e-9:
            top = set(ranked[:n])
            whole = sum(1 for u in per_script.values() if u <= top)
            plain = sum(1 for c, u in per_script.items()
                        if u <= top and not c.startswith(FRAGMENT_PREFIXES))
            print(f'  {mark:4.0%} of call sites: {n:3d} natives; '
                  f'{whole} of {len(per_script)} scripts ({plain} not fragments) '
                  f'call no other native directly')
            mark = next(marks, None)

    events = {f['name'].lower() for c in natives for _, f in functions(classes[c])
              if not f['flags'] & 2 and f['name'].lower().startswith('on')}
    overridden = collections.Counter(
        f['name'].lower() for c, o in classes.items() if c not in natives
        for _, f in functions(o) if f['name'].lower() in events)
    print(f'engine events {len(events)}, handled by vanilla scripts {len(overridden)}')
    print('  ' + ', '.join(f'{k} {v}' for k, v in overridden.most_common(12)))

    kinds = collections.Counter(next((p for p in FRAGMENT_PREFIXES if c.startswith(p)), 'other')
                                for c in per_script)
    print('scripts by kind: ' + ', '.join(f'{k} {v}' for k, v in kinds.most_common()))
    props = [p for o in classes.values() for p in o['props']]
    print(f'properties {len(props)}, auto {sum(1 for p in props if p["flags"] & 4)}')
    print(f'functions in named states {sum(1 for o in classes.values() for s, _ in functions(o) if s)}')


if __name__ == '__main__':
    main(sys.argv[1])

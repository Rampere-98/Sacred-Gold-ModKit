#!/usr/bin/env python
"""Reads the quest journal of each running agent copy (SacredA1..A8) from the game's memory, read only, and prints
per quest how many copies have it done / failed / open, with its name and kind from docs/QUESTS.md.

Journal (docs/AGENT_ADDRESSES.md): records of 0x174 bytes from [0xAAD3A4] to [0xAAD3A8]; +0 kind (3 main), +4 state
(1, 2 open; 100 done; 101 failed), +8 number (DQ_<n> is n; R<rr>NQ<qq> is 100 + 50*rr + qq), +0x18/+0x1C goal tile.
Usage: python -I docs/Misiones-medir.py [--json out.json]
"""
import ctypes, struct, subprocess, re, os, sys, json, collections

k = ctypes.windll.kernel32
k.OpenProcess.restype = ctypes.c_void_p
HERE = os.path.dirname(os.path.abspath(__file__))


def copies():
    out = subprocess.check_output(['powershell', '-NoProfile', '-Command',
                                   "Get-Process Sacred -ErrorAction SilentlyContinue | % { \"$($_.Id)|$($_.Path)\" }"])
    for line in out.decode(errors='ignore').split():
        pid, _, path = line.partition('|')
        m = re.search(r'SacredA(\d)', path)
        if m:
            yield int(m.group(1)), int(pid)


def journal(pid):
    h = k.OpenProcess(0x0010 | 0x0400, False, pid)
    if not h:
        return None

    def rd(a, n):
        b = ctypes.create_string_buffer(n)
        g = ctypes.c_size_t()
        return b.raw if k.ReadProcessMemory(ctypes.c_void_p(h), ctypes.c_void_p(a), b, n, ctypes.byref(g)) else None

    try:
        p = rd(0xAAD3A4, 8)
        if not p:
            return None
        begin, end = struct.unpack('<II', p)
        n = (end - begin) // 0x174 if end > begin else 0
        if not n or n > 512:
            return []
        rows = []
        for i in range(n):
            r = rd(begin + i * 0x174, 0x30)
            if not r:
                continue
            kind, state, number = struct.unpack_from('<iii', r, 0)
            tx, ty = struct.unpack_from('<ii', r, 0x18)
            rows.append({'kind': kind, 'state': state, 'number': number, 'tx': tx, 'ty': ty})
        return rows
    finally:
        k.CloseHandle(ctypes.c_void_p(h))


def names():
    """number -> (name, kind) from QUESTS.md headings: «### Name — clave `DQ_15005`» and the «Tipo» line under it."""
    out = {}
    try:
        text = open(os.path.join(HERE, 'QUESTS.md'), encoding='utf8').read()
    except OSError:
        return out
    for m in re.finditer(r'^### (.+?) — clave `([A-Za-z0-9_]+)`\s*\n(?:- \*\*Tipo\*\*: ([^\n]*))?', text, re.M):
        key = m.group(2)
        mq = re.match(r'DQ_(\d+)$', key)
        mr = re.match(r'R(\d+)[a-z]?NQ(\d+)$', key)
        num = int(mq.group(1)) if mq else 100 + 50 * int(mr.group(1)) + int(mr.group(2)) if mr else None
        if num is not None:
            out.setdefault(num, (m.group(1).strip(), (m.group(3) or '').split(':')[0].strip(), key))
    return out


def main():
    nm = names()
    per = {}
    for a, pid in sorted(copies()):
        j = journal(pid)
        if j is not None:
            per[a] = j
    table = collections.defaultdict(lambda: {'done': [], 'failed': [], 'open': []})
    for a, rows in per.items():
        seen = {}
        for r in rows:      # a quest has one record per step: its worst open state wins, then failed, then done
            s = 'done' if r['state'] == 100 else 'failed' if r['state'] == 101 else 'open' if r['state'] in (1, 2) else None
            if s is None:
                continue
            prev = seen.get(r['number'])
            order = {'open': 3, 'failed': 2, 'done': 1}
            if prev is None or order[s] > order[prev]:
                seen[r['number']] = s
        for num, s in seen.items():
            table[num][s].append(a)
    print(f"copias leídas: {sorted(per)}")
    print(f"{'nº':>6} {'hechas':>6} {'fall.':>5} {'abier.':>6}  misión (tipo) [copias abiertas/falladas]")
    for num in sorted(table, key=lambda n: (-len(table[n]['open']) - len(table[n]['failed']), n)):
        t = table[num]
        name, kind, key = nm.get(num, ('?', '', ''))
        extra = ''
        if t['open'] or t['failed']:
            extra = ' [abierta: ' + ','.join(f'A{a}' for a in t['open']) + ('; fallada: ' + ','.join(f'A{a}' for a in t['failed']) if t['failed'] else '') + ']'
        print(f"{num:>6} {len(t['done']):>6} {len(t['failed']):>5} {len(t['open']):>6}  {key} {name} ({kind}){extra}")
    if '--json' in sys.argv:
        json.dump({'per': per, 'table': table}, open(sys.argv[sys.argv.index('--json') + 1], 'w'), indent=1)


main()

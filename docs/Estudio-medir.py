import re, os, collections
ROOT = "D:/SteamLibrary/steamapps/common"
hp = re.compile(r'^(\d\d:\d\d:\d\d).*HeroProfile: (\S+(?: \S+)?) \(class \d+\), level (\d+)')
qb = re.compile(r'^(\d\d:\d\d:\d\d).*quest book: (\d+) goals \((\d+) done, (\d+) failed\)')
intent = re.compile(r'Intent: (\w+)')
fight = re.compile(r'^(\d\d:\d\d:\d\d).*CombatLearn: fight over, (\d+) s, (\d+) enemies, (\d+) killed.*?(\d+) potions, (\d+) experience')
look = re.compile(r'nothing around, going to look')
def sec(t):
    h, m, s = map(int, t.split(':')); return h*3600+m*60+s
for i in range(1, 9):
    first = last = None; q0 = q1 = None; intents = collections.Counter(); xp = 0; fights = 0; fsec = 0; looks = 0
    t0 = t1 = None
    for name in ("SacredBild.prev2.log", "SacredBild.prev.log", "SacredBild.log"):
        p = f"{ROOT}/SacredA{i}/{name}"
        if not os.path.exists(p): continue
        for line in open(p, encoding="utf8", errors="ignore"):
            m = hp.search(line)
            if m:
                first = first or (m[1], m[2], int(m[3])); last = (m[1], m[2], int(m[3]))
            m = qb.search(line)
            if m:
                q0 = q0 or (int(m[3]), int(m[4])); q1 = (int(m[3]), int(m[4]), int(m[2]))
            m = intent.search(line)
            if m: intents[m[1]] += 1
            m = fight.search(line)
            if m: xp += int(m[6]); fights += 1; fsec += int(m[2])
            if look.search(line): looks += 1
            mt = re.match(r'(\d\d:\d\d:\d\d)', line)
            if mt:
                t0 = t0 or mt[1]; t1 = mt[1]
    span = (sec(t1) - sec(t0)) % 86400 / 3600 if t0 else 0
    print(f"A{i} {first[1] if first else '?'}: lvl {first[2] if first else '?'}->{last[2] if last else '?'} in {span:.1f} h; "
          f"quests done/failed {q0}->{q1}; fights {fights} ({fsec/60:.0f} min fighting), xp {xp} ({xp/max(span,0.01):.0f}/h); "
          f"'going to look' {looks}; intents {dict(intents.most_common(5))}")

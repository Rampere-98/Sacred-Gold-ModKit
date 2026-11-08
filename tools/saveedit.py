# saveedit.py - Sacred Gold save (GAMExx.PAK) hero editor for testing; respects the game's anti-cheat.
# Layout found by RE (2026-11-08):
#   header "AMS\x1b"; directory from 0x100: records (type, file offset, size) of 3 dwords.
#   compressed chunk at offset: "DE C0 AD BA" + u32 compressed size + 24 zero bytes + zlib stream;
#   its directory size = decompressed size.
#   chunk 0xA0 = the hero. Stats (hero+0x3A8) at STATS: +0x0C exp, +0x46 (anti-cheat field), +0x56 level (u16);
#   learned arts: u16 count at ARTS, then 22-byte entries: u32 kind (1 spell, 2 move), u16 id, u16 level, floats.
#   chunk 0xA1 holds the anti-cheat copies (writer 0x4671e0): 0xF00DBABE, key, exp^key, field^key, level^key.
#   On load a hero that does not match them is set to 1 and killed (checker 0x4656a0).
# For test copies only (never the save you play): write to a new file and keep a backup.
# Usage: saveedit.py in.PAK out.PAK [exp N] [level N] [art IDX KIND IDHEX LEVEL]... [addart KIND IDHEX LEVEL]...
import struct, sys, zlib

MARK = b'\xde\xc0\xad\xba'
STATS, ARTS = 0x3D9, 0x4CF


def directory(d):
    return [(o,) + struct.unpack_from('<3I', d, o) for o in range(0x100, 0x400, 12)
            if struct.unpack_from('<I', d, o + 4)[0]]


def load(d, off):
    assert d[off:off + 4] == MARK
    csize = struct.unpack_from('<I', d, off + 4)[0]
    return bytearray(zlib.decompress(bytes(d[off + 0x20:off + 0x20 + csize]))), csize


def main():
    src, dst, args = sys.argv[1], sys.argv[2], sys.argv[3:]
    d = bytearray(open(src, 'rb').read())
    dirs = {t: (o, off) for o, t, off, size in directory(d)}
    hero, _ = load(d, dirs[0xA0][1])
    guard, _ = load(d, dirs[0xA1][1])
    g = guard.find(struct.pack('<I', 0xF00DBABE)) + 4
    key = struct.unpack_from('<I', guard, g)[0]
    exp, field, level = (x ^ key for x in struct.unpack_from('<3I', guard, g + 4))
    assert exp == struct.unpack_from('<I', hero, STATS + 0x0C)[0], 'guard does not match the hero'
    count = struct.unpack_from('<H', hero, ARTS)[0]
    i = 0
    while i < len(args):
        a = args[i]
        if a == 'exp':
            exp = int(args[i + 1]); i += 2
        elif a == 'level':
            level = int(args[i + 1]); i += 2
        elif a == 'art':
            e = ARTS + 2 + 22 * int(args[i + 1])
            struct.pack_into('<IHH', hero, e, int(args[i + 2]), int(args[i + 3], 16), int(args[i + 4])); i += 5
        elif a == 'addart':
            e = bytearray(22)
            struct.pack_into('<IHH', e, 0, int(args[i + 1]), int(args[i + 2], 16), int(args[i + 3]))
            struct.pack_into('<f', e, 0x0E, 1.0)
            pos = ARTS + 2 + 22 * count
            hero[pos:pos] = e
            count += 1; i += 4
        else:
            raise SystemExit('unknown ' + a)
    struct.pack_into('<H', hero, ARTS, count)
    struct.pack_into('<I', hero, STATS + 0x0C, exp)
    struct.pack_into('<H', hero, STATS + 0x56, level)
    struct.pack_into('<3I', guard, g + 4, exp ^ key, field ^ key, level ^ key)
    for j in range(count):
        kind, art, lv = struct.unpack_from('<IHH', hero, ARTS + 2 + 22 * j)
        print('art', j, 'kind', kind, 'id %x' % art, 'level', lv)
    print('exp', exp, 'level', level)
    # Rebuild: replace the chunks from the last to the first, shifting the directory offsets after each.
    out = bytearray(d)
    for t, blob in sorted(((0xA0, hero), (0xA1, guard)), key=lambda x: -dirs[x[0]][1]):
        o, off = dirs[t]
        csize = struct.unpack_from('<I', out, off + 4)[0]
        comp = zlib.compress(bytes(blob), 9)
        delta = len(comp) - csize
        old = bytes(out)
        out[off + 0x20:off + 0x20 + csize] = comp
        struct.pack_into('<I', out, off + 4, len(comp))
        struct.pack_into('<I', out, o + 8, len(blob))
        for o2, t2, off2, size2 in directory(old):
            if off2 > off:
                struct.pack_into('<I', out, o2 + 4, off2 + delta)
    open(dst, 'wb').write(out)
    print('written', dst, len(out))


main()

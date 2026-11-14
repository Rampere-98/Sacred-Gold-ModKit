#include "game/world_tiles.h"
#include "log.h"

#include <windows.h>
#include <zlib.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    // sectors.keyx: a 0x100 byte header (+4 the record count), then 0x300 byte records: +0x20 sy*100 + sx (the
    // sector's place in the 100x100 grid, sx = x >> 6), +0xEC / +0xF0 offset and size of its zlib block in
    // sectors.wldx. A block inflates to 0x20144 bytes: +0x20 the 64x64 tiles of 0x20 bytes, row by row.
    constexpr size_t kKeyHeader = 0x100;
    constexpr size_t kKeyRecord = 0x300;
    constexpr int kGrid = 100;                  // sectors per side
    constexpr int kSectorTiles = 64;
    constexpr size_t kBlockSize = 0x20144;
    constexpr size_t kTilesAt = 0x20;
    constexpr size_t kTileSize = 0x20;
    // 4 KB each. A far route's box is up to 18 x 18 sectors: with 256 slots its own rows threw each other out, each
    // miss a read and an inflate under the lock.
    constexpr int kCacheSlots = 1024;

    struct Block
    {
        uint32_t offset = 0, size = 0;          // size 0: no sector there
    };

    struct Slot
    {
        int code = -1;
        uint32_t lastUse = 0;
        std::array<uint8_t, kSectorTiles * kSectorTiles> walk{};     // 1 walkable, 0 not
    };

    std::mutex g_lock;
    bool g_tried = false;
    bool g_ok = false;
    HANDLE g_wld = INVALID_HANDLE_VALUE;
    std::vector<Block> g_blocks;                // by sy*100 + sx
    std::vector<int16_t> g_slotOf;              // by sy*100 + sx: its cache slot, -1 none
    std::vector<Slot> g_slots;
    uint32_t g_clock = 0;
    bool g_warned = false;

    std::wstring worldDir()
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe;
        const size_t cut = dir.find_last_of(L"\\/");
        dir.resize(cut == std::wstring::npos ? 0 : cut + 1);
        return dir + L"WORLD\\";
    }

    HANDLE openRead(const std::wstring& path)
    {
        return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    bool readAt(HANDLE h, uint64_t offset, void* out, DWORD size)
    {
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(offset);
        DWORD got = 0;
        return SetFilePointerEx(h, at, nullptr, FILE_BEGIN) && ReadFile(h, out, size, &got, nullptr) && got == size;
    }

    // Under g_lock. Reads the index once; false (and every answer -1) without the files.
    bool open()
    {
        if (g_tried)
        {
            return g_ok;
        }
        g_tried = true;
        const std::wstring dir = worldDir();
        HANDLE key = openRead(dir + L"sectors.keyx");
        if (key == INVALID_HANDLE_VALUE)
        {
            LOG("World tiles: no WORLD\\sectors.keyx ({}), far tiles unknown", GetLastError());
            return false;
        }
        LARGE_INTEGER len{};
        std::vector<uint8_t> data;
        if (GetFileSizeEx(key, &len) && len.QuadPart >= static_cast<LONGLONG>(kKeyHeader) && len.QuadPart < (64 << 20))
        {
            data.resize(static_cast<size_t>(len.QuadPart));
            if (!readAt(key, 0, data.data(), static_cast<DWORD>(data.size())))
            {
                data.clear();
            }
        }
        CloseHandle(key);
        if (data.size() < kKeyHeader || std::memcmp(data.data(), "WLK", 3) != 0)
        {
            LOG("World tiles: WORLD\\sectors.keyx unreadable, far tiles unknown");
            return false;
        }
        uint32_t count = 0;
        std::memcpy(&count, data.data() + 4, 4);
        if (kKeyHeader + static_cast<size_t>(count) * kKeyRecord > data.size())
        {
            LOG("World tiles: WORLD\\sectors.keyx short ({} records, {} bytes)", count, data.size());
            return false;
        }
        g_blocks.assign(kGrid * kGrid, Block{});
        int sectors = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint8_t* r = data.data() + kKeyHeader + i * kKeyRecord;
            uint32_t code = 0;
            Block b;
            std::memcpy(&code, r + 0x20, 4);
            std::memcpy(&b.offset, r + 0xEC, 4);
            std::memcpy(&b.size, r + 0xF0, 4);
            if (code < static_cast<uint32_t>(kGrid * kGrid) && b.size)
            {
                g_blocks[code] = b;
                ++sectors;
            }
        }
        g_wld = openRead(dir + L"sectors.wldx");
        if (g_wld == INVALID_HANDLE_VALUE)
        {
            LOG("World tiles: no WORLD\\sectors.wldx ({}), far tiles unknown", GetLastError());
            return false;
        }
        g_slotOf.assign(kGrid * kGrid, -1);
        g_slots.resize(kCacheSlots);
        g_ok = true;
        LOG("World tiles: {} sectors indexed from WORLD\\sectors.keyx", sectors);
        return true;
    }

    // Under g_lock. The sector's walkable map, inflated into the least recently used slot; nullptr if unreadable.
    const Slot* sector(int code)
    {
        const int16_t cached = g_slotOf[code];
        if (cached >= 0)
        {
            g_slots[cached].lastUse = ++g_clock;
            return &g_slots[cached];
        }
        const Block& b = g_blocks[code];
        std::vector<uint8_t> packed(b.size);
        std::vector<uint8_t> raw(kBlockSize);
        uLongf rawLen = static_cast<uLongf>(raw.size());
        if (!readAt(g_wld, b.offset, packed.data(), b.size) ||
            uncompress(raw.data(), &rawLen, packed.data(), static_cast<uLong>(packed.size())) != Z_OK ||
            rawLen < kTilesAt + kSectorTiles * kSectorTiles * kTileSize)
        {
            if (!g_warned)
            {
                g_warned = true;
                LOG("World tiles: sector {},{} unreadable in WORLD\\sectors.wldx", code % kGrid, code / kGrid);
            }
            return nullptr;
        }
        int victim = 0;
        for (int i = 1; i < kCacheSlots; ++i)
        {
            if (g_slots[i].lastUse < g_slots[victim].lastUse)
            {
                victim = i;
            }
        }
        Slot& s = g_slots[victim];
        if (s.code >= 0)
        {
            g_slotOf[s.code] = -1;
        }
        for (int t = 0; t < kSectorTiles * kSectorTiles; ++t)
        {
            // walkable 0x636C10: blocked if the low nibble of +0x1F is 1 or 2, or +0x1E bit 3 is set. Bit 2 of
            // +0x1E (an object or barrier decides) is unknown offline: walkable.
            const uint8_t* tile = raw.data() + kTilesAt + t * kTileSize;
            const uint8_t kind = tile[0x1F] & 0xF;
            // Bit 0 walkable; bits 1-4 the ground (block R2: the high nibble of +0x1F).
            s.walk[t] = static_cast<uint8_t>((kind == 1 || kind == 2 || (tile[0x1E] & 8) ? 0 : 1) | ((tile[0x1F] >> 4) << 1));
        }
        s.code = code;
        s.lastUse = ++g_clock;
        g_slotOf[code] = static_cast<int16_t>(victim);
        return &s;
    }
}

// The tile's byte (walkable bit 0, ground bits 1-4); -1 not known, -2 outside the world. The lock held by the caller.
static int tileByteLocked(int layer, int tileX, int tileY);

static int tileByte(int layer, int tileX, int tileY)
{
    std::lock_guard<std::mutex> lock(g_lock);
    return tileByteLocked(layer, tileX, tileY);
}

void WorldTiles::fill(int tileX, int tileY, int w, int h, int8_t* out)
{
    // One lock a row (the code reviewer: a lock a tile held up a big route by 100 ms; one for the whole box held the
    // other tile tests for seconds on a far route).
    for (int y = 0; y < h; ++y)
    {
        {
            std::lock_guard<std::mutex> lock(g_lock);
            for (int x = 0; x < w; ++x)
            {
                const int b = tileByteLocked(0, tileX + x, tileY + y);
                out[static_cast<size_t>(y) * w + x] = static_cast<int8_t>(b == -1 ? -1 : b < 0 ? 0 : b);
            }
        }
        // The lock is not fair: taken again at once for the next row, the frame's own tile tests waited on it for
        // the whole box (2026-11-14: up to 9.6 s without a frame, the presenting thread in tileByte's lock). Every
        // few rows the others get their turn.
        if ((y & 3) == 3)
        {
            Sleep(0);
        }
    }
}

int WorldTiles::walkable(int layer, int tileX, int tileY)
{
    const int b = tileByte(layer, tileX, tileY);
    return b == -1 ? -1 : b < 0 ? 0 : (b & 1);
}

int WorldTiles::ground(int layer, int tileX, int tileY)
{
    const int b = tileByte(layer, tileX, tileY);
    return b < 0 ? -1 : (b >> 1) & 0xF;
}

static int tileByteLocked(int layer, int tileX, int tileY)
{
    if (layer != 0)
    {
        return -1;
    }
    if (!open())
    {
        return -1;
    }
    if (tileX < 0 || tileY < 0 || tileX >= kGrid * kSectorTiles || tileY >= kGrid * kSectorTiles)
    {
        return -2;
    }
    const int sx = tileX / kSectorTiles, sy = tileY / kSectorTiles;
    const int code = sy * kGrid + sx;
    if (!g_blocks[code].size)
    {
        return -2;  // no sector there: outside the world
    }
    const Slot* s = sector(code);
    if (!s)
    {
        return -1;
    }
    return s->walk[(tileY - sy * kSectorTiles) * kSectorTiles + (tileX - sx * kSectorTiles)];
}

#include "game/map_memory.h"
#include "log.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
    constexpr double kTileSize = 53.66563;
    constexpr double kCell = 4 * kTileSize;

    struct Cell
    {
        uint16_t visits = 0;
        uint16_t blocked = 0;
    };
    std::unordered_map<uint64_t, Cell> g_cells;
    std::mutex g_lock;
    bool g_loaded = false, g_dirty = false;
    uint32_t g_savedAt = 0;
    uint64_t g_lastKey = ~0ull;

    uint64_t key(int layer, int cx, int cy)
    {
        return (static_cast<uint64_t>(layer & 0xFF) << 48) | (static_cast<uint64_t>(static_cast<uint16_t>(cx)) << 16) |
               static_cast<uint16_t>(cy);
    }

    uint64_t keyAt(int layer, int32_t x, int32_t y)
    {
        return key(layer, static_cast<int>(std::floor(x / kCell)), static_cast<int>(std::floor(y / kCell)));
    }

    std::wstring path()
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring p = exe;
        return p.substr(0, p.find_last_of(L"\\/") + 1) + L"SacredBild-map.txt";
    }

    // Under g_lock.
    void load()
    {
        g_loaded = true;
        FILE* f = _wfopen(path().c_str(), L"r");
        if (!f)
        {
            return;
        }
        int layer, cx, cy, visits, blocked;
        while (std::fscanf(f, "%d %d %d %d %d", &layer, &cx, &cy, &visits, &blocked) == 5)
        {
            g_cells[key(layer, cx, cy)] = {static_cast<uint16_t>(visits), static_cast<uint16_t>(blocked)};
        }
        std::fclose(f);
        LOG("MapMemory: {} cells known", g_cells.size());
    }
}

void MapMemory::visit(int layer, int32_t x, int32_t y)
{
    const std::lock_guard lock(g_lock);
    if (!g_loaded)
    {
        load();
    }
    const uint64_t k = keyAt(layer, x, y);
    if (k == g_lastKey)
    {
        return;     // one visit per entering a cell, not per look
    }
    g_lastKey = k;
    Cell& c = g_cells[k];
    c.visits = static_cast<uint16_t>(std::min(c.visits + 1, 60000));
    // Reached: whatever blocked it before was the way, not the place.
    c.blocked = c.blocked > 0 ? static_cast<uint16_t>(c.blocked - 1) : 0;
    g_dirty = true;
}

void MapMemory::blocked(int layer, int32_t x, int32_t y)
{
    const std::lock_guard lock(g_lock);
    if (!g_loaded)
    {
        load();
    }
    Cell& c = g_cells[keyAt(layer, x, y)];
    c.blocked = static_cast<uint16_t>(std::min(c.blocked + 1, 1000));
    g_dirty = true;
}

float MapMemory::cost(int layer, int32_t x, int32_t y)
{
    const std::lock_guard lock(g_lock);
    if (!g_loaded)
    {
        load();
    }
    const auto it = g_cells.find(keyAt(layer, x, y));
    if (it == g_cells.end())
    {
        return 0.0f;
    }
    return std::log2(1.0f + it->second.visits) + 3.0f * it->second.blocked;
}

int MapMemory::cells()
{
    const std::lock_guard lock(g_lock);
    return static_cast<int>(g_cells.size());
}

int MapMemory::cellsOn(int layer)
{
    const std::lock_guard lock(g_lock);
    int n = 0;
    for (const auto& [k, c] : g_cells)
    {
        n += static_cast<int>(k >> 48) == (layer & 0xFF) && c.visits > 0 ? 1 : 0;
    }
    return n;
}

void MapMemory::save(uint32_t now, uint32_t minMs)
{
    const std::lock_guard lock(g_lock);
    if (!g_dirty || (g_savedAt && now - g_savedAt < minMs))
    {
        return;
    }
    const std::wstring p = path(), temp = p + L".tmp";
    FILE* f = _wfopen(temp.c_str(), L"w");
    if (!f)
    {
        return;
    }
    for (const auto& [k, c] : g_cells)
    {
        std::fprintf(f, "%d %d %d %u %u\n", static_cast<int>(k >> 48), static_cast<int16_t>((k >> 16) & 0xFFFF),
                     static_cast<int16_t>(k & 0xFFFF), c.visits, c.blocked);
    }
    std::fclose(f);
    MoveFileExW(temp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_dirty = false;
    g_savedAt = now;
}

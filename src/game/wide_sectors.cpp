#include "game/wide_sectors.h"
#include "config/render.h"
#include "game/modkit_menu.h"
#include "log.h"
#include "patch.h"
#include "sacred/world.h"
#include "sig.h"

#include <imgui.h>
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>

namespace
{
    using namespace Sacred;
    using WideSectors::kCount;
    using WideSectors::kSide;

    // Read by the patched game code: its address is in their instructions.
    Sector* g_table[kCount] = {};
    // Ring sectors this module set the keep flag on (cleared again once they leave the ring), by sector id + 1 (0:
    // none) in the map they were kept in: loading a game clears the map, which frees its sectors, so pointers kept
    // from before would be stale (clearing the flag through them wrote into freed memory).
    uint32_t g_kept[kCount] = {};
    cMapData* g_keptMap = nullptr;
    bool g_active = false;
    cMapData* g_map = nullptr;
    uint32_t g_mismatchLogs = 0;

    using GetSector = Sector*(__fastcall*)(cMapData* self, void* edx, uint32_t id);
    GetSector g_getSector = nullptr;

    uintptr_t s_cmpCx9 = 0, s_readCx = 0, s_cmpWord9 = 0, s_readDx = 0, s_leaDx = 0, s_stepUp = 0,
              s_stepUpRight = 0, s_waterA = 0, s_waterB = 0, s_getSector = 0;

    // ENG addresses for reference: 0062AFF6, 0062AFFF, 0062B033, 0062B044, 0062B04B, 0062D29D, 0062D2BB
    // (renderTileRow), 0062DDE4, 0062E0F2 (drawWaterTiles), 00634340 (cMapData::getSector).
    const Sig::Entry kSigs[] = {
        {"wide sectors: cmp cx, 9", "66 83 F9 09 7D ?? 0F BF C9 8B 94 8D B0 70 09 00", 0, Sig::Take::Match, &s_cmpCx9},
        {"wide sectors: sector read", "8B 94 8D B0 70 09 00 3B D6 74 05 8B", 0, Sig::Take::Match, &s_readCx},
        {"wide sectors: cmp word, 9", "66 83 7C 24 34 09 0F 8D ?? ?? ?? ?? 0F BF 54 24 34 8B 8C 95 B0 70 09 00", 0,
            Sig::Take::Match, &s_cmpWord9},
        {"wide sectors: sector read 2", "8B 8C 95 B0 70 09 00 8D 84 95 B0 70 09 00", 0, Sig::Take::Match, &s_readDx},
        {"wide sectors: sector slot", "8D 84 95 B0 70 09 00 85 C9 89 44 24", 0, Sig::Take::Match, &s_leaDx},
        {"wide sectors: step up", "83 EA 03 81 C7 C1 0F 00 00 89 54 24", 0, Sig::Take::Match, &s_stepUp},
        {"wide sectors: step up right", "83 E9 02 89 4C 24 34 EB 0C 8B 54 24", 0, Sig::Take::Match, &s_stepUpRight},
        {"wide sectors: water sector", "8B 94 87 B0 70 09 00 8A 41 1F 24 F0", 0, Sig::Take::Match, &s_waterA},
        {"wide sectors: water sector 2", "8B 8C 8F B0 70 09 00 24 F0 3C 90 75", 0, Sig::Take::Match, &s_waterB},
        {"wide sectors: getSector", "83 EC 08 53 8B 5C 24 10 56 8B F1 3B 9E 84 82 00 00 0F 83", 0, Sig::Take::Match,
            &s_getSector},
    };

    struct Edit
    {
        uintptr_t* site;
        uint8_t expected[7];
        size_t size;
        uint8_t modrm, sib;     // 0: keep
        bool table;             // the disp32 at +3 becomes the table's address
        size_t immOffset;       // 0: none
        uint8_t imm;
    };

    // mov/lea r32, [base + index*4 + 0x970B0] -> [index*4 + table]: mod 10 -> 00 with SIB base 101 (no base,
    // disp32), the same length. The cmp/sub imm8 operands change in place.
    const Edit kEdits[] = {
        {&s_cmpCx9, {0x66, 0x83, 0xF9, 0x09}, 4, 0, 0, false, 3, 0x19},
        {&s_cmpWord9, {0x66, 0x83, 0x7C, 0x24, 0x34, 0x09}, 6, 0, 0, false, 5, 0x19},
        {&s_readCx, {0x8B, 0x94, 0x8D, 0xB0, 0x70, 0x09, 0x00}, 7, 0x14, 0, true, 0, 0},
        {&s_readDx, {0x8B, 0x8C, 0x95, 0xB0, 0x70, 0x09, 0x00}, 7, 0x0C, 0, true, 0, 0},
        {&s_leaDx, {0x8D, 0x84, 0x95, 0xB0, 0x70, 0x09, 0x00}, 7, 0x04, 0, true, 0, 0},
        {&s_stepUp, {0x83, 0xEA, 0x03}, 3, 0, 0, false, 2, 0x05},
        {&s_stepUpRight, {0x83, 0xE9, 0x02}, 3, 0, 0, false, 2, 0x04},
        {&s_waterA, {0x8B, 0x94, 0x87, 0xB0, 0x70, 0x09, 0x00}, 7, 0x14, 0x85, true, 0, 0},
        {&s_waterB, {0x8B, 0x8C, 0x8F, 0xB0, 0x70, 0x09, 0x00}, 7, 0x0C, 0x8D, true, 0, 0},
    };

    bool matches(const Edit& e)
    {
        return *e.site && std::memcmp(reinterpret_cast<const void*>(*e.site), e.expected, e.size) == 0;
    }

    bool apply(const Edit& e)
    {
        uint8_t bytes[7];
        std::memcpy(bytes, e.expected, e.size);
        if (e.modrm)
        {
            bytes[1] = e.modrm;
        }
        if (e.sib)
        {
            bytes[2] = e.sib;
        }
        if (e.table)
        {
            const auto address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_table[0]));
            std::memcpy(bytes + 3, &address, 4);
        }
        if (e.immOffset)
        {
            bytes[e.immOffset] = e.imm;
        }
        return Patch::write(*e.site, bytes, e.size);
    }

    bool inner3x3(int slot)
    {
        const int r = slot / kSide, c = slot % kSide;
        return r >= 1 && r <= 3 && c >= 1 && c <= 3;
    }

    // The menu edits the next start's choice: the patch is applied once, at start.
    bool g_wanted = true;

    void drawMenu()
    {
        ImGui::Checkbox("Sectores amplios (5x5)", &g_wanted);
        ImGui::SetItemTooltip("Carga el mundo en 5x5 sectores para que las esquinas de la pantalla ancha no queden negras.");
        ImGui::TextDisabled(g_wanted == g_active ? "Ahora: %s." : "Ahora: %s. Guarda y reinicia el juego para cambiarlo.",
            g_active ? "activos" : "inactivos");
    }

    void saveMenu(const std::wstring& ini)
    {
        ModkitMenu::write(ini, L"Render", L"WideSectors", g_wanted);
    }
}

void WideSectors::install()
{
    g_wanted = Config::render.wideSectors;
    ModkitMenu::add({"Sectores amplios", &drawMenu, &saveMenu});
    if (!Config::render.wideSectors)
    {
        return;
    }
    if (Sig::resolve(kSigs) != 0)
    {
        LOG("Wide sectors: off, the game code is not the one expected");
        return;
    }
    for (const Edit& e : kEdits)
    {
        if (!matches(e))
        {
            LOG("Wide sectors: off, unexpected bytes at {:08x}", *e.site);
            return;
        }
    }
    for (const Edit& e : kEdits)
    {
        if (!apply(e))
        {
            // Some sites may be patched already: the walk would look sectors up in a table nobody fills.
            LOG("Wide sectors: patching {:08x} failed", *e.site);
            return;
        }
    }
    g_getSector = reinterpret_cast<GetSector>(s_getSector);
    g_active = true;
    LOG("Wide sectors: 5x5 map sectors around the camera (table at {:08x})",
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_table[0])));
}

bool WideSectors::active()
{
    return g_active;
}

void WideSectors::update(cWorldView* view)
{
    if (!g_active || !view)
    {
        return;
    }
    Sector* next[kCount] = {};
    uint32_t ids[kCount] = {};  // ring sectors' ids + 1
    for (int s = 0; s < 9; ++s)
    {
        next[fromGame(s)] = view->sectors[s];
    }
    // Neighbor ids by (row, column) offset, in the order of Sector::neighbors.
    static constexpr int kDr[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
    static constexpr int kDc[8] = {-1, 0, 1, 1, 1, 0, -1, -1};
    cMapData* map = view->mapData;
    g_map = map;
    for (int slot = 0; slot < kCount && map; ++slot)
    {
        if (inner3x3(slot))
        {
            continue;
        }
        // One step out from the nearest sector of the 3x3.
        const int r = slot / kSide, c = slot % kSide;
        const int ir = std::clamp(r, 1, 3), ic = std::clamp(c, 1, 3);
        const Sector* inner = next[ir * kSide + ic];
        if (!inner)
        {
            continue;
        }
        int dir = 0;
        while (dir < 8 && !(kDr[dir] == r - ir && kDc[dir] == c - ic))
        {
            ++dir;
        }
        const uint32_t id = inner->neighbors[dir];
        Sector* s = g_getSector(map, nullptr, id);
        if (!s || !(s->flags & Sector::loaded) || !s->tiles)
        {
            continue;
        }
        const int32_t wantX = inner->originX + 64 * (c - ic), wantY = inner->originY + 64 * (r - ir);
        if (s->originX != wantX || s->originY != wantY)
        {
            if (g_mismatchLogs++ < 10)
            {
                LOG("Wide sectors: neighbor {} of the sector at ({}, {}) lies at ({}, {}), expected ({}, {})", dir,
                    inner->originX, inner->originY, s->originX, s->originY, wantX, wantY);
            }
            continue;
        }
        next[slot] = s;
        ids[slot] = id + 1;
    }

    // Keep the ring loaded while it is in the table; let go of what left it (unless the game's 3x3 has it now).
    if (g_keptMap != map)
    {
        std::fill(std::begin(g_kept), std::end(g_kept), 0u);
        g_keptMap = map;
    }
    uint32_t nowKept[kCount] = {};
    int n = 0;
    for (int slot = 0; slot < kCount; ++slot)
    {
        Sector* s = next[slot];
        if (!s || inner3x3(slot) || !ids[slot])
        {
            continue;
        }
        const bool ours = std::find(std::begin(g_kept), std::end(g_kept), ids[slot]) != std::end(g_kept);
        if (ours || !(s->flags & Sector::keep))
        {
            s->flags |= Sector::keep;
            nowKept[n++] = ids[slot];
        }
    }
    for (uint32_t old : g_kept)
    {
        Sector* s = old && map ? map->sector(old - 1) : nullptr;
        if (s && std::find(nowKept, nowKept + n, old) == nowKept + n &&
            std::find(view->sectors, view->sectors + 9, s) == view->sectors + 9)
        {
            s->flags &= ~Sector::keep;
        }
    }
    std::copy(std::begin(nowKept), std::end(nowKept), std::begin(g_kept));
    std::copy(std::begin(next), std::end(next), std::begin(g_table));
}

void WideSectors::release()
{
    if (!g_active || !g_keptMap)
    {
        return;
    }
    for (uint32_t& id : g_kept)
    {
        if (Sector* s = id ? g_keptMap->sector(id - 1) : nullptr)
        {
            s->flags &= ~Sector::keep;
        }
        id = 0;
    }
}

Sector* WideSectors::at(int slot)
{
    return slot >= 0 && slot < kCount ? g_table[slot] : nullptr;
}

cMapData* WideSectors::map()
{
    return g_map;
}

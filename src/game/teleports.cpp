#include "game/teleports.h"
#include "game/auto_attack.h"
#include "game/click_walk.h"
#include "game/sacred_addr.h"
#include "log.h"
#include "patch.h"
#include "sacred/ui.h"

#include <windows.h>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace
{
    std::vector<Teleports::Interior> g_interiors;
    bool g_loaded = false;

    std::wstring gameDir()
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe;
        return path.substr(0, path.find_last_of(L"\\/") + 1);
    }

    void load()
    {
        g_loaded = true;
        static const wchar_t* const kClasses[] = {L"TYPE_NPC_ELVE",      L"TYPE_NPC_GLADIATOR", L"TYPE_NPC_SERAPHIM",
                                                  L"TYPE_NPC_MAGICIAN",  L"TYPE_NPC_DARKELVE",  L"TYPE_NPC_ZWERG",
                                                  L"TYPE_NPC_DAEMONIN",  L"TYPE_NPC_VAMPIRELADY"};
        // Every class's DefPos, merged (the stuck solver, 2026-11-13: only the first found was read, the Wood Elf's;
        // the colosseum's ways out tpstart_g_03/04 are in the Gladiator's alone, and a new Gladiator walked its yard
        // 66 min with no way out).
        std::map<std::string, std::pair<int, int>> start, target, in, out;
        size_t n = 0;
        for (const wchar_t* cls : kClasses)
        {
            std::vector<char> data;
            const std::wstring file = gameDir() + L"bin\\" + cls + L"\\DefPos.bin";
            if (FILE* f = _wfopen(file.c_str(), L"rb"))
            {
                std::fseek(f, 0, SEEK_END);
                const long size = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                data.resize(size > 0 ? static_cast<size_t>(size) : 0);
                const size_t got = std::fread(data.data(), 1, data.size(), f);
                std::fclose(f);
                data.resize(got);
            }
            n += data.size();
            // Each name, NUL-led, then x and y at +0x40/+0x44 of its record.
            for (size_t i = 1; i + 0x48 <= data.size(); ++i)
            {
                if (data[i - 1] != 0 || data[i] != 't' || data[i + 1] != 'p')
                {
                    continue;
                }
                const size_t len = strnlen(data.data() + i, 0x40);
                if (len >= 0x40)
                {
                    continue;
                }
                const std::string name(data.data() + i, len);
                int x = 0, y = 0;
                std::memcpy(&x, data.data() + i + 0x40, 4);
                std::memcpy(&y, data.data() + i + 0x44, 4);
                if (x <= 0 || y <= 0 || x >= 8192 || y >= 8192)
                {
                    continue;
                }
                auto take = [&](const char* prefix, std::map<std::string, std::pair<int, int>>& into) {
                    const size_t p = std::strlen(prefix);
                    if (name.compare(0, p, prefix) == 0)
                    {
                        into.emplace(name.substr(p), std::pair{x, y});   // the first class's wins
                        return true;
                    }
                    return false;
                };
                take("tpstart_", start) || take("tptarget_", target) || take("tpreinz", in) || take("tprausz", out);
            }
        }
        for (const auto& [suffix, o] : out)
        {
            if (const auto it = in.find(suffix); it != in.end())
            {
                g_interiors.push_back({it->second.first, it->second.second, o.first, o.second, false});
            }
        }
        for (const auto& [suffix, s] : start)
        {
            if (const auto it = target.find(suffix); it != target.end())
            {
                g_interiors.push_back({it->second.first, it->second.second, s.first, s.second, true});
            }
            else
            {
                // A way out whose landing has another name (the colosseum's g_03 and g_04 land on g_02): the tile to
                // step on is known all the same, for the ways out of where the hero is shut in.
                g_interiors.push_back({0, 0, s.first, s.second, true});
            }
        }
        LOG("Teleports: {} interiors and jumps read ({} bytes of DefPos)", g_interiors.size(), n);
    }

    struct Node
    {
        uint16_t balance, height;
        uint32_t key, value;
        Node *left, *right, *parent;
    };
}

const std::vector<Teleports::Interior>& Teleports::interiors()
{
    if (!g_loaded)
    {
        load();
    }
    return g_interiors;
}

std::vector<std::pair<int, int>> Teleports::anchorsNear(int layer, int tx, int ty, int radius)
{
    std::vector<std::pair<int, int>> out;
    const Node* root = *reinterpret_cast<Node* const*>(0xAB4498);
    // A walk of the whole tree (some thousands of nodes), each node checked readable: a wrong root gives nothing.
    std::vector<const Node*> stack;
    if (root)
    {
        stack.push_back(root);
    }
    int visited = 0;
    while (!stack.empty() && ++visited < 20000)
    {
        const Node* node = stack.back();
        stack.pop_back();
        if (IsBadReadPtr(node, sizeof(Node)))
        {
            continue;
        }
        const uint32_t v = node->value;
        const int ax = static_cast<int>(v & 0x1FFF), ay = static_cast<int>((v >> 13) & 0x1FFF);
        const int al = static_cast<int>(v >> 26);
        if (al == layer && std::abs(ax - tx) <= radius && std::abs(ay - ty) <= radius)
        {
            bool seen = false;
            for (const auto& [x, y] : out)
            {
                seen = seen || (x == ax && y == ay);
            }
            if (!seen)
            {
                out.emplace_back(ax, ay);
            }
        }
        if (node->left)
        {
            stack.push_back(node->left);
        }
        if (node->right)
        {
            stack.push_back(node->right);
        }
    }
    return out;
}

bool Teleports::heroAt(int& tx, int& ty, int& layer)
{
    const uintptr_t om = *reinterpret_cast<const uintptr_t*>(0xAD5C40);
    const uintptr_t player = *reinterpret_cast<const uintptr_t*>(0x182EBE8);
    if (!om || !player || IsBadReadPtr(reinterpret_cast<const void*>(om), 8) ||
        IsBadReadPtr(reinterpret_cast<const void*>(player + 0x14), 4))
    {
        return false;
    }
    const uintptr_t table = *reinterpret_cast<const uintptr_t*>(om + 4);
    const uint32_t id = *reinterpret_cast<const uint32_t*>(player + 0x14);
    if (!table || IsBadReadPtr(reinterpret_cast<const void*>(table + 4 * id), 4))
    {
        return false;
    }
    const uintptr_t hero = *reinterpret_cast<const uintptr_t*>(table + 4 * id);
    if (!hero || IsBadReadPtr(reinterpret_cast<const void*>(hero + 0x1C), 9))
    {
        return false;
    }
    tx = static_cast<int>(*reinterpret_cast<const int32_t*>(hero + 0x1C) / kWorldPerTile);
    ty = static_cast<int>(*reinterpret_cast<const int32_t*>(hero + 0x20) / kWorldPerTile);
    layer = *reinterpret_cast<const uint8_t*>(hero + 0x24);
    return true;
}

namespace
{
    // A teleport whose ground was not loaded when clicked: once its sectors are, the hero goes to the nearest walkable
    // tile if it stands on one that is not (water, a rock, a wall).
    struct PendingSnap
    {
        int tx = 0, ty = 0, layer = 0;
        DWORD since = 0;
    } g_snap;

    // The walkable tile nearest (tx, ty) within 40 tiles, ring by ring (the nearest of the first ring with one):
    // 1 found, 0 none there, -1 not known. Beyond the loaded sectors ClickWalk::walkable answers from the world files
    // (surface only), so the player's jump lands on a walkable tile worked out before it.
    int nearestWalkable(int layer, int tx, int ty, int& ox, int& oy)
    {
        const int here = ClickWalk::walkable(layer, tx, ty);
        if (here != 0)
        {
            ox = tx;
            oy = ty;
            return here;
        }
        for (int r = 1; r <= 40; ++r)
        {
            double best = 1e9;
            bool unknown = false;
            for (int dy = -r; dy <= r; ++dy)
            {
                for (int dx = -r; dx <= r; ++dx)
                {
                    if (std::abs(dx) != r && std::abs(dy) != r)
                    {
                        continue;
                    }
                    const int w = ClickWalk::walkable(layer, tx + dx, ty + dy);
                    unknown = unknown || w < 0;
                    if (w == 1 && std::hypot(dx, dy) < best)
                    {
                        best = std::hypot(dx, dy);
                        ox = tx + dx;
                        oy = ty + dy;
                    }
                }
            }
            if (best < 1e9)
            {
                return 1;
            }
            if (unknown)
            {
                return -1;
            }
        }
        return 0;
    }

    bool writeHero(int tx, int ty, int layer)
    {
        const uintptr_t om = *reinterpret_cast<const uintptr_t*>(0xAD5C40);
        const uintptr_t player = *reinterpret_cast<const uintptr_t*>(0x182EBE8);
        int x = 0, y = 0, l = 0;
        if (!Teleports::heroAt(x, y, l))
        {
            return false;
        }
        const uintptr_t hero = *reinterpret_cast<const uintptr_t*>(
            *reinterpret_cast<const uintptr_t*>(om + 4) + 4 * *reinterpret_cast<const uint32_t*>(player + 0x14));
        *reinterpret_cast<int32_t*>(hero + 0x1C) = static_cast<int32_t>((tx + 0.5) * Teleports::kWorldPerTile);
        *reinterpret_cast<int32_t*>(hero + 0x20) = static_cast<int32_t>((ty + 0.5) * Teleports::kWorldPerTile);
        *reinterpret_cast<uint8_t*>(hero + 0x24) = static_cast<uint8_t>(layer);
        return true;
    }
}

void Teleports::onFrame()
{
    if (!g_snap.since)
    {
        return;
    }
    if (GetTickCount() - g_snap.since > 10000)
    {
        LOG("Teleports: the ground at {},{} not loaded in 10 s: left as it is", g_snap.tx, g_snap.ty);
        g_snap.since = 0;
        return;
    }
    int ox = 0, oy = 0;
    const int found = nearestWalkable(g_snap.layer, g_snap.tx, g_snap.ty, ox, oy);
    if (found < 0)
    {
        return;     // not loaded yet
    }
    g_snap.since = 0;
    if (found == 1 && (ox != g_snap.tx || oy != g_snap.ty) && writeHero(ox, oy, g_snap.layer))
    {
        LOG("Teleports: {},{} not walkable: the hero to {},{}, the nearest walkable", g_snap.tx, g_snap.ty, ox, oy);
    }
}

bool Teleports::playerTeleport(int tx, int ty, int layer)
{
    const uintptr_t om = *reinterpret_cast<const uintptr_t*>(0xAD5C40);
    const uintptr_t player = *reinterpret_cast<const uintptr_t*>(0x182EBE8);
    int x = 0, y = 0, l = 0;
    if (!heroAt(x, y, l) || tx <= 0 || ty <= 0 || tx >= 8192 || ty >= 8192 || layer < 0 || layer > 63)
    {
        return false;
    }
    (void)om;
    (void)player;
    // The tile clicked or the nearest walkable one, worked out before the jump where the ground is loaded; else on
    // arrival, as soon as it loads.
    int ox = tx, oy = ty;
    const int found = nearestWalkable(layer, tx, ty, ox, oy);
    if (found == 0)
    {
        return false;   // nothing walkable within 40 tiles
    }
    if (!writeHero(ox, oy, layer))
    {
        return false;
    }
    g_snap = {};
    if (found < 0)
    {
        g_snap = {tx, ty, layer, GetTickCount()};
    }
    else if (ox != tx || oy != ty)
    {
        LOG("Teleports: {},{} not walkable: to {},{}, the nearest walkable", tx, ty, ox, oy);
    }
    tx = ox;
    ty = oy;
    LOG("Teleports: the player teleported the hero from {},{} (layer {}) to {},{} (layer {})", x, y, l, tx, ty, layer);
    return true;
}

namespace
{
    using namespace Sacred;

    // cUI_Megamap::receiveEvent (the M key's map, UI window 5; vtable 0x896724's entry), thiscall(event) -> bool used.
    using MegamapEventFn = uint32_t(__fastcall*)(void* self, void* edx, cEvent* event);
    MegamapEventFn g_origMegamapEvent = nullptr;

    // The player's Alt+click on the map: the game has its own behind a debug flag ([0x182EE38] & 0x2000000, off);
    // this one writes the hero's position (Manolito, block O: the map's screen to tile, as the game's 0x6C834F).
    uint32_t __fastcall hookMegamapEvent(void* self, void* edx, cEvent* event)
    {
        const auto* map = static_cast<const uint8_t*>(self);
        if (event && event->vtable == Addr::cEventMouseDown_vtable && !AutoAttack::on() &&
            *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(event) + 4) == 0 &&
            (GetAsyncKeyState(VK_MENU) & 0x8000) && (*reinterpret_cast<const uint32_t*>(map + 0x10) & 5) == 5)
        {
            const auto* mouse = static_cast<const cEventMouse*>(event);
            const double scrollX = *reinterpret_cast<const int32_t*>(map + 0xC016C);
            const double scrollY = *reinterpret_cast<const int32_t*>(map + 0xC0170);
            const double X = (mouse->x + scrollX - 1018.75) / 0.007446;
            const double Y = (mouse->y + scrollY + 186.947) / 0.014914;
            const double wx = (X / 0.894427 + Y / 0.447214) / 2.0;
            const double wy = (Y / 0.447214 - X / 0.894427) / 2.0;
            const int tx = static_cast<int>(std::floor(wx / Teleports::kWorldPerTile));
            const int ty = static_cast<int>(std::floor(wy / Teleports::kWorldPerTile));
            if (!Teleports::playerTeleport(tx, ty, 0))
            {
                LOG("Teleports: Alt+click on the map at {},{} (tile {},{}): refused", mouse->x, mouse->y, tx, ty);
            }
            return 1;   // used: the map does not take the click too
        }
        return g_origMegamapEvent(self, edx, event);
    }
}

void Teleports::install()
{
    Patch::hook(g_origMegamapEvent, static_cast<uintptr_t>(0x6C8140), &hookMegamapEvent, "cUI_Megamap::receiveEvent");
}

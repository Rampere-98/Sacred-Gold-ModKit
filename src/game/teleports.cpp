#include "game/teleports.h"
#include "log.h"

#include <windows.h>
#include <cstdio>
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
        std::vector<char> data;
        for (const wchar_t* cls : kClasses)
        {
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
                break;
            }
        }
        // Each name, NUL-led, then x and y at +0x40/+0x44 of its record.
        std::map<std::string, std::pair<int, int>> start, target, in, out;
        const size_t n = data.size();
        for (size_t i = 1; i + 0x48 <= n; ++i)
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
                    into[name.substr(p)] = {x, y};
                    return true;
                }
                return false;
            };
            take("tpstart_", start) || take("tptarget_", target) || take("tpreinz", in) || take("tprausz", out);
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

bool Teleports::playerTeleport(int tx, int ty, int layer)
{
    const uintptr_t om = *reinterpret_cast<const uintptr_t*>(0xAD5C40);
    const uintptr_t player = *reinterpret_cast<const uintptr_t*>(0x182EBE8);
    int x = 0, y = 0, l = 0;
    if (!heroAt(x, y, l) || tx <= 0 || ty <= 0 || tx >= 8192 || ty >= 8192 || layer < 0 || layer > 63)
    {
        return false;
    }
    const uintptr_t hero = *reinterpret_cast<const uintptr_t*>(
        *reinterpret_cast<const uintptr_t*>(om + 4) + 4 * *reinterpret_cast<const uint32_t*>(player + 0x14));
    *reinterpret_cast<int32_t*>(hero + 0x1C) = static_cast<int32_t>((tx + 0.5) * kWorldPerTile);
    *reinterpret_cast<int32_t*>(hero + 0x20) = static_cast<int32_t>((ty + 0.5) * kWorldPerTile);
    *reinterpret_cast<uint8_t*>(hero + 0x24) = static_cast<uint8_t>(layer);
    LOG("Teleports: the player teleported the hero from {},{} (layer {}) to {},{} (layer {})", x, y, l, tx, ty, layer);
    return true;
}

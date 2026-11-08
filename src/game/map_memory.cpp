#include "game/map_memory.h"
#include "game/frame_hooks.h"
#include "game/modkit_menu.h"
#include "log.h"
#include "sacred/render.h"

#include <windows.h>
#include <ddraw.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
    // One cell per tile (a map coordinate).
    constexpr double kTileSize = 53.66563;

    struct Cell
    {
        uint16_t visits = 0;
        uint16_t blocked = 0;
        uint8_t r = 0, g = 0, b = 0;
        uint8_t samples = 0;    // ground color samples averaged (0: none yet)
    };
    std::unordered_map<uint64_t, Cell> g_cells;
    std::mutex g_lock;
    bool g_loaded = false, g_dirty = false;
    uint32_t g_savedAt = 0;
    uint64_t g_lastKey = ~0ull;

    // The ground sample asked for.
    bool g_want = false;
    uint64_t g_wantKey = 0;
    float g_wantX = 0.0f, g_wantY = 0.0f;
    DWORD g_sampledAt = 0;
    constexpr DWORD kSampleEveryMs = 700;   // a back buffer read stalls the GPU a little: not more often

    int g_heroLayer = 0, g_heroTx = 0, g_heroTy = 0;
    bool g_heroKnown = false;

    int tileOf(int32_t v)
    {
        return static_cast<int>(std::floor(v / kTileSize));
    }

    uint64_t key(int layer, int tx, int ty)
    {
        return (static_cast<uint64_t>(layer & 0xFF) << 48) | (static_cast<uint64_t>(static_cast<uint16_t>(tx)) << 16) |
               static_cast<uint16_t>(ty);
    }

    uint64_t keyAt(int layer, int32_t x, int32_t y)
    {
        return key(layer, tileOf(x), tileOf(y));
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
        char line[128];
        while (std::fgets(line, sizeof(line), f))
        {
            int layer, tx, ty, visits, blocked;
            unsigned color = 0, samples = 0;
            const int n = std::sscanf(line, "%d %d %d %d %d %x %u", &layer, &tx, &ty, &visits, &blocked, &color, &samples);
            if (n < 5)
            {
                continue;
            }
            Cell c;
            c.visits = static_cast<uint16_t>(visits);
            c.blocked = static_cast<uint16_t>(blocked);
            if (n == 7)
            {
                c.r = static_cast<uint8_t>(color >> 16);
                c.g = static_cast<uint8_t>(color >> 8);
                c.b = static_cast<uint8_t>(color);
                c.samples = static_cast<uint8_t>(std::min(samples, 255u));
            }
            g_cells[key(layer, tx, ty)] = c;
        }
        std::fclose(f);
        LOG("MapMemory: {} map squares known", g_cells.size());
    }

    void ensureLoaded()
    {
        if (!g_loaded)
        {
            load();
        }
    }

    // The ground's color below the hero's feet, from the back buffer: of points on a ring around the feet, the median by
    // brightness (the hero, enemies and effects stand out from the ground; most of the ring is ground).
    bool sample(float sx, float sy, uint8_t& r, uint8_t& g, uint8_t& b)
    {
        Sacred::dxDriver7* driver = FrameHooks::dxDriver();
        IDirectDrawSurface7* back = driver ? driver->back : nullptr;
        if (!back)
        {
            return false;
        }
        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        if (FAILED(back->Lock(nullptr, &desc, DDLOCK_READONLY | DDLOCK_WAIT | DDLOCK_NOSYSLOCK, nullptr)))
        {
            return false;
        }
        const DDPIXELFORMAT& pf = desc.ddpfPixelFormat;
        const bool bgrx = pf.dwRBitMask == 0xFF0000 && pf.dwGBitMask == 0xFF00 && pf.dwBBitMask == 0xFF;
        const bool rgbx = pf.dwRBitMask == 0xFF && pf.dwGBitMask == 0xFF00 && pf.dwBBitMask == 0xFF0000;
        bool ok = false;
        if (pf.dwRGBBitCount == 32 && (bgrx || rgbx) && desc.lpSurface)
        {
            const int red = bgrx ? 2 : 0, blue = 2 - red;
            struct Px
            {
                uint8_t r, g, b;
                int lum;
            };
            Px px[12];
            int n = 0;
            const float fx = sx, fy = sy + 28.0f;   // the feet, below the rect's middle
            for (int k = 0; k < 12; ++k)
            {
                const float a = k * 3.14159265f / 6.0f;
                const int x = static_cast<int>(fx + std::cos(a) * 46.0f);
                const int y = static_cast<int>(fy + std::sin(a) * 23.0f);     // isometric: a flat ellipse
                if (x < 0 || y < 0 || x >= static_cast<int>(desc.dwWidth) || y >= static_cast<int>(desc.dwHeight))
                {
                    continue;
                }
                const uint8_t* p = static_cast<const uint8_t*>(desc.lpSurface) + static_cast<ptrdiff_t>(y) * desc.lPitch +
                                   static_cast<ptrdiff_t>(x) * 4;
                px[n++] = {p[red], p[1], p[blue], p[red] * 3 + p[1] * 6 + p[blue]};
            }
            if (n >= 5)
            {
                std::nth_element(px, px + n / 2, px + n, [](const Px& a, const Px& b) { return a.lum < b.lum; });
                r = px[n / 2].r;
                g = px[n / 2].g;
                b = px[n / 2].b;
                ok = true;
            }
        }
        back->Unlock(nullptr);
        return ok;
    }

    // Dark ground is lifted so that it never reads as unexplored black.
    ImU32 shown(const Cell& c)
    {
        int r = c.r, g = c.g, b = c.b;
        const int top = std::max({r, g, b});
        constexpr int kMin = 56;
        if (top < kMin)
        {
            if (top == 0)
            {
                r = g = b = kMin;
            }
            else
            {
                r = r * kMin / top;
                g = g * kMin / top;
                b = b * kMin / top;
            }
        }
        return IM_COL32(r, g, b, 255);
    }

    int g_zoom = 4;     // pixels per square

    void drawMenu()
    {
        ImGui::TextWrapped("Lo explorado alrededor del héroe, una casilla por coordenada: en el color del suelo que pisó; "
                           "transparente donde no consiguió pasar; negro lo inexplorado.");
        ImGui::SliderInt("Tamaño de casilla (px)", &g_zoom, 2, 10);
        const std::lock_guard lock(g_lock);
        ensureLoaded();
        if (!g_heroKnown)
        {
            ImGui::TextDisabled("Sin héroe en el mundo.");
            return;
        }
        int seen = 0;
        for (const auto& [k, c] : g_cells)
        {
            seen += static_cast<int>(k >> 48) == g_heroLayer && c.visits ? 1 : 0;
        }
        ImGui::Text("Capa %d: %d casillas pisadas (%d en total). Casilla del héroe: %d, %d.", g_heroLayer, seen,
                    static_cast<int>(g_cells.size()), g_heroTx, g_heroTy);
        const float size = 300.0f;
        const int half = static_cast<int>(size / g_zoom / 2);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin, ImVec2(origin.x + size, origin.y + size), IM_COL32(0, 0, 0, 255));
        // The world's x/y axes run diagonally on the screen in Sacred; the map shows them as rows and columns, north up
        // as the tiles' own grid.
        for (int dy = -half; dy < half; ++dy)
        {
            for (int dx = -half; dx < half; ++dx)
            {
                const auto it = g_cells.find(key(g_heroLayer, g_heroTx + dx, g_heroTy + dy));
                if (it == g_cells.end())
                {
                    continue;   // unexplored: the black
                }
                const Cell& c = it->second;
                const ImVec2 a(origin.x + (dx + half) * g_zoom, origin.y + (dy + half) * g_zoom);
                const ImVec2 b(a.x + g_zoom, a.y + g_zoom);
                if (!c.visits)
                {
                    // Never reached (blocked): clear, the menu's background showing through a faint outline.
                    dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_WindowBg));
                    continue;
                }
                dl->AddRectFilled(a, b, c.samples ? shown(c) : IM_COL32(90, 90, 90, 255));
            }
        }
        const ImVec2 mid(origin.x + half * g_zoom + g_zoom * 0.5f, origin.y + half * g_zoom + g_zoom * 0.5f);
        dl->AddCircleFilled(mid, std::max(2.0f, g_zoom * 0.8f), IM_COL32(255, 40, 40, 255));
        ImGui::Dummy(ImVec2(size, size));
    }

    void saveMenu(const std::wstring&)
    {
    }
}

void MapMemory::install()
{
    static std::once_flag once;
    std::call_once(once, [] { ModkitMenu::add({"Mapa explorado", &drawMenu, &saveMenu}); });
}

void MapMemory::setHero(int layer, int32_t x, int32_t y)
{
    const std::lock_guard lock(g_lock);
    g_heroLayer = layer;
    g_heroTx = tileOf(x);
    g_heroTy = tileOf(y);
    g_heroKnown = true;
}

void MapMemory::visit(int layer, int32_t x, int32_t y)
{
    install();
    setHero(layer, x, y);
    const std::lock_guard lock(g_lock);
    ensureLoaded();
    const uint64_t k = keyAt(layer, x, y);
    if (k == g_lastKey)
    {
        return;     // one visit per entering a square, not per look
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
    ensureLoaded();
    Cell& c = g_cells[keyAt(layer, x, y)];
    c.blocked = static_cast<uint16_t>(std::min(c.blocked + 1, 1000));
    g_dirty = true;
}

float MapMemory::cost(int layer, int32_t x, int32_t y)
{
    const std::lock_guard lock(g_lock);
    ensureLoaded();
    // Over the 3x3 squares around: how much was seen there, and how much of it was blocked.
    const int tx = tileOf(x), ty = tileOf(y);
    float visits = 0.0f, blocked = 0.0f;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            const auto it = g_cells.find(key(layer, tx + dx, ty + dy));
            if (it != g_cells.end())
            {
                visits += it->second.visits;
                blocked += it->second.blocked;
            }
        }
    }
    return std::log2(1.0f + visits) + 1.5f * blocked;
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

void MapMemory::wantGround(int layer, int32_t x, int32_t y, float sx, float sy)
{
    const std::lock_guard lock(g_lock);
    const uint64_t k = keyAt(layer, x, y);
    const auto it = g_cells.find(k);
    if (it != g_cells.end() && it->second.samples >= 3)
    {
        return;
    }
    g_want = true;
    g_wantKey = k;
    g_wantX = sx;
    g_wantY = sy;
}

void MapMemory::afterWorld()
{
    const DWORD now = GetTickCount();
    float sx, sy;
    uint64_t k;
    {
        const std::lock_guard lock(g_lock);
        if (!g_want || (g_sampledAt && now - g_sampledAt < kSampleEveryMs))
        {
            return;
        }
        g_want = false;
        g_sampledAt = now;
        sx = g_wantX;
        sy = g_wantY;
        k = g_wantKey;
    }
    uint8_t r, g, b;
    if (!sample(sx, sy, r, g, b))
    {
        return;
    }
    const std::lock_guard lock(g_lock);
    Cell& c = g_cells[k];
    const int n = c.samples;
    c.r = static_cast<uint8_t>((c.r * n + r) / (n + 1));
    c.g = static_cast<uint8_t>((c.g * n + g) / (n + 1));
    c.b = static_cast<uint8_t>((c.b * n + b) / (n + 1));
    c.samples = static_cast<uint8_t>(std::min(n + 1, 255));
    g_dirty = true;
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
        std::fprintf(f, "%d %d %d %u %u %06x %u\n", static_cast<int>(k >> 48), static_cast<int16_t>((k >> 16) & 0xFFFF),
                     static_cast<int16_t>(k & 0xFFFF), c.visits, c.blocked, (c.r << 16) | (c.g << 8) | c.b, c.samples);
    }
    std::fclose(f);
    MoveFileExW(temp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_dirty = false;
    g_savedAt = now;
}

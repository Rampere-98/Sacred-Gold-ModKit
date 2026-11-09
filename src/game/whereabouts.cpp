#include "game/whereabouts.h"
#include "game/click_walk.h"
#include "log.h"

#include <imgui.h>
#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <vector>

namespace
{
    constexpr int kSide = 2 * Whereabouts::kRadius + 1;
    Whereabouts::Area g_area;
    std::vector<uint8_t> g_reached;     // kSide*kSide, 1 reached
    DWORD g_fillAt = 0;

    void fill(int layer, int hx, int hy)
    {
        g_reached.assign(kSide * kSide, 0);
        Whereabouts::Area a;
        a.valid = true;
        a.layer = layer;
        a.heroX = hx;
        a.heroY = hy;
        a.minX = a.maxX = hx;
        a.minY = a.maxY = hy;
        bool open = false;
        std::vector<int> queue;
        queue.reserve(kSide * kSide);
        const int start = Whereabouts::kRadius * kSide + Whereabouts::kRadius;
        g_reached[start] = 1;
        queue.push_back(start);
        static const int kDx[4] = {1, -1, 0, 0}, kDy[4] = {0, 0, 1, -1};
        for (size_t i = 0; i < queue.size(); ++i)
        {
            const int cx = queue[i] % kSide, cy = queue[i] / kSide;
            const int tx = hx + cx - Whereabouts::kRadius, ty = hy + cy - Whereabouts::kRadius;
            ++a.tiles;
            a.minX = std::min(a.minX, tx);
            a.maxX = std::max(a.maxX, tx);
            a.minY = std::min(a.minY, ty);
            a.maxY = std::max(a.maxY, ty);
            for (int d = 0; d < 4; ++d)
            {
                const int nx = cx + kDx[d], ny = cy + kDy[d];
                if (nx < 0 || ny < 0 || nx >= kSide || ny >= kSide)
                {
                    open = true;    // the fill ran to its edge: the way goes on further out
                    continue;
                }
                const int n = ny * kSide + nx;
                if (g_reached[n])
                {
                    continue;
                }
                const int w = ClickWalk::walkable(layer, hx + nx - Whereabouts::kRadius, hy + ny - Whereabouts::kRadius);
                if (w < 0)
                {
                    open = true;    // beyond the loaded sectors: not known to be shut
                    continue;
                }
                if (w)
                {
                    g_reached[n] = 1;
                    queue.push_back(n);
                }
            }
        }
        // The hero's own tile not walkable by the test (standing on an object's edge): no verdict.
        a.shutIn = !open && a.tiles > 1;
        const bool changed = a.shutIn != g_area.shutIn || !g_area.valid || a.layer != g_area.layer;
        g_area = a;
        if (changed)
        {
            if (a.shutIn)
            {
                LOG("Whereabouts: shut in, {} tiles reachable (tiles {}..{} x {}..{}, layer {})", a.tiles, a.minX, a.maxX,
                    a.minY, a.maxY, a.layer);
            }
            else
            {
                LOG("Whereabouts: open ground around tile {},{} (layer {})", hx, hy, layer);
            }
        }
    }
}

void Whereabouts::update(int layer, int tileX, int tileY)
{
    const DWORD now = GetTickCount();
    const bool moved = !g_area.valid || layer != g_area.layer || tileX != g_area.heroX || tileY != g_area.heroY;
    if ((moved && now - g_fillAt > 1000) || now - g_fillAt > 5000)
    {
        g_fillAt = now;
        fill(layer, tileX, tileY);
    }
}

const Whereabouts::Area& Whereabouts::area()
{
    return g_area;
}

int Whereabouts::reachable(int x, int y)
{
    if (!g_area.valid)
    {
        return -1;
    }
    const int cx = x - g_area.heroX + kRadius, cy = y - g_area.heroY + kRadius;
    if (cx < 0 || cy < 0 || cx >= kSide || cy >= kSide)
    {
        return -1;
    }
    return g_reached[cy * kSide + cx] ? 1 : 0;
}

bool Whereabouts::nearestReachable(int x, int y, int& outX, int& outY)
{
    if (!g_area.valid)
    {
        return false;
    }
    long best = -1;
    for (int cy = 0; cy < kSide; ++cy)
    {
        for (int cx = 0; cx < kSide; ++cx)
        {
            if (!g_reached[cy * kSide + cx])
            {
                continue;
            }
            const int tx = g_area.heroX + cx - kRadius, ty = g_area.heroY + cy - kRadius;
            const long d = static_cast<long>(tx - x) * (tx - x) + static_cast<long>(ty - y) * (ty - y);
            if (best < 0 || d < best)
            {
                best = d;
                outX = tx;
                outY = ty;
            }
        }
    }
    return best >= 0;
}

void Whereabouts::drawMenu()
{
    const Area& a = g_area;
    if (!a.valid)
    {
        ImGui::TextDisabled("Aún sin mirar alrededor.");
        return;
    }
    ImGui::Text("Casilla %d,%d, capa %d", a.heroX, a.heroY, a.layer);
    if (a.shutIn)
    {
        ImGui::Text("Recinto cerrado: %d casillas alcanzables (%d..%d x %d..%d)", a.tiles, a.minX, a.maxX, a.minY,
                    a.maxY);
    }
    else
    {
        ImGui::Text("Terreno abierto (%d casillas alcanzables a %d de radio)", a.tiles, kRadius);
    }
}

#include "game/map_memory.h"
#include "game/combat_learn.h"
#include "game/frame_hooks.h"
#include "game/modkit_menu.h"
#include "overlay/overlay.h"
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
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // One cell per tile (a map coordinate).
    constexpr double kTileSize = 53.66563;

    // 6 bytes a square (the game is 32 bit: the whole world explored must fit beside it). All zero: unexplored.
    struct Cell
    {
        uint8_t visits = 0;     // saturating
        uint8_t blocked = 0;    // saturating
        uint8_t r = 0, g = 0, b = 0;
        uint8_t samples = 0;    // ground color samples averaged (0: none yet)
        bool known() const { return visits || blocked || samples; }
    };
    // Squares in chunks of 32x32 (6 KB), only where something is known: a lookup is one hash and an index, and the
    // memory is what was explored, not a node per square.
    constexpr int kChunkShift = 5, kChunkSide = 1 << kChunkShift;
    struct Chunk
    {
        Cell cells[kChunkSide * kChunkSide];
    };
    std::unordered_map<uint64_t, std::unique_ptr<Chunk>> g_chunks;
    int g_known = 0;                                // squares known, all layers
    std::unordered_map<int, int> g_visitedOn;       // squares stepped on, per layer
    std::mutex g_lock;
    bool g_loaded = false, g_dirty = false;
    uint32_t g_savedAt = 0;
    uint64_t g_lastKey = ~0ull;

    // What the agent found and marks on its map (kept in the map file).
    struct Landmark
    {
        MapMemory::Mark kind;
        uint8_t layer;
        int16_t tx, ty;
    };
    std::vector<Landmark> g_landmarks;

    // The encyclopedia on the map: where each enemy kind was met (12 bytes a place; at most kSightingsPerKind places
    // a kind, kSightings in all, the oldest dropped).
    struct Sighting
    {
        uint32_t type;
        uint8_t layer;
        int16_t tx, ty;
    };
    constexpr size_t kSightingsPerKind = 48, kSightings = 6000;
    std::deque<Sighting> g_sightings;
    std::unordered_map<uint32_t, std::string> g_kindNames;
    // Its filters (ModKit menu, [Map] in SacredBild.ini).
    bool g_showFoes = true;
    float g_minDanger = 0.0f;           // % of the hero's health per second; above 0 the kinds not yet learned hide
    bool g_onlyKilled = false;
    std::unordered_set<uint32_t> g_hiddenKinds;

    // The ground sample asked for.
    bool g_want = false;
    uint64_t g_wantKey = 0;
    float g_wantX = 0.0f, g_wantY = 0.0f;
    DWORD g_sampledAt = 0;
    constexpr DWORD kSampleEveryMs = 700;   // a back buffer read stalls the GPU a little: not more often

    int g_heroLayer = 0, g_heroTx = 0, g_heroTy = 0;
    int32_t g_heroX = 0, g_heroY = 0;
    struct BlipAt
    {
        MapMemory::Blip kind;
        int32_t x, y;
    };
    std::vector<BlipAt> g_blips, g_blipsBuilding;
    bool g_route = false;
    int32_t g_routeX = 0, g_routeY = 0;
    // Where the hero walked lately: a point every half tile, none older than kTrailMs and at most kTrailPoints.
    struct TrailPoint
    {
        int32_t x, y;
        DWORD at;
    };
    constexpr DWORD kTrailMs = 45000;
    constexpr size_t kTrailPoints = 60;
    std::deque<TrailPoint> g_trail;
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

    int layerOfKey(uint64_t k) { return static_cast<int>(k >> 48); }
    int txOfKey(uint64_t k) { return static_cast<int16_t>((k >> 16) & 0xFFFF); }
    int tyOfKey(uint64_t k) { return static_cast<int16_t>(k & 0xFFFF); }

    uint64_t chunkKey(int layer, int tx, int ty)
    {
        return key(layer, tx >> kChunkShift, ty >> kChunkShift);
    }

    // The square, or null where nothing is known (no chunk made). Under g_lock.
    const Cell* find(int layer, int tx, int ty)
    {
        const auto it = g_chunks.find(chunkKey(layer, tx, ty));
        if (it == g_chunks.end())
        {
            return nullptr;
        }
        const Cell& c = it->second->cells[((ty & (kChunkSide - 1)) << kChunkShift) | (tx & (kChunkSide - 1))];
        return c.known() ? &c : nullptr;
    }

    const Cell* find(uint64_t k)
    {
        return find(layerOfKey(k), txOfKey(k), tyOfKey(k));
    }

    // Changes a square (made if new), keeping the counts. Under g_lock.
    template <typename Fn>
    void change(uint64_t k, Fn fn)
    {
        const int layer = layerOfKey(k), tx = txOfKey(k), ty = tyOfKey(k);
        std::unique_ptr<Chunk>& chunk = g_chunks[chunkKey(layer, tx, ty)];
        if (!chunk)
        {
            chunk = std::make_unique<Chunk>();
        }
        Cell& c = chunk->cells[((ty & (kChunkSide - 1)) << kChunkShift) | (tx & (kChunkSide - 1))];
        const bool known = c.known(), visited = c.visits > 0;
        fn(c);
        g_known += (c.known() ? 1 : 0) - (known ? 1 : 0);
        g_visitedOn[layer] += (c.visits > 0 ? 1 : 0) - (visited ? 1 : 0);
        g_dirty = true;
    }

    // Each known square with its key. Under g_lock.
    template <typename Fn>
    void forEach(Fn fn)
    {
        for (const auto& [ck, chunk] : g_chunks)
        {
            const int layer = layerOfKey(ck), cx = txOfKey(ck), cy = tyOfKey(ck);
            for (int i = 0; i < kChunkSide * kChunkSide; ++i)
            {
                if (chunk->cells[i].known())
                {
                    fn(key(layer, cx * kChunkSide + (i & (kChunkSide - 1)), cy * kChunkSide + (i >> kChunkShift)),
                       chunk->cells[i]);
                }
            }
        }
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
            int layer, tx, ty, visits, blocked, kind;
            unsigned type = 0;
            if (std::sscanf(line, "E %u %d %d %d", &type, &layer, &tx, &ty) == 4)
            {
                g_sightings.push_back({type, static_cast<uint8_t>(layer), static_cast<int16_t>(tx), static_cast<int16_t>(ty)});
                continue;
            }
            char name[96];
            if (std::sscanf(line, "N %u %95s", &type, name) == 2)
            {
                g_kindNames[type] = name;
                continue;
            }
            if (std::sscanf(line, "L %d %d %d %d", &kind, &layer, &tx, &ty) == 4)
            {
                if (kind >= 0 && kind < static_cast<int>(MapMemory::Mark::Count))
                {
                    g_landmarks.push_back({static_cast<MapMemory::Mark>(kind), static_cast<uint8_t>(layer),
                                           static_cast<int16_t>(tx), static_cast<int16_t>(ty)});
                }
                continue;
            }
            unsigned color = 0, samples = 0;
            const int n = std::sscanf(line, "%d %d %d %d %d %x %u", &layer, &tx, &ty, &visits, &blocked, &color, &samples);
            if (n < 5)
            {
                continue;
            }
            change(key(layer, tx, ty), [&](Cell& c) {
                c.visits = static_cast<uint8_t>(std::clamp(visits, 0, 255));
                c.blocked = static_cast<uint8_t>(std::clamp(blocked, 0, 255));
                if (n == 7)
                {
                    c.r = static_cast<uint8_t>(color >> 16);
                    c.g = static_cast<uint8_t>(color >> 8);
                    c.b = static_cast<uint8_t>(color);
                    c.samples = static_cast<uint8_t>(std::min(samples, 255u));
                }
            });
        }
        std::fclose(f);
        g_dirty = false;
        LOG("MapMemory: {} map squares known in {} chunks ({} KB), {} landmarks", g_known, g_chunks.size(),
            g_chunks.size() * sizeof(Chunk) / 1024, g_landmarks.size());
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

    // An enemy kind's color on the map by how hard it hits (green 0.25 % of the hero's health a second or less, red
    // 4 % or more), grey if not learned yet; 0 if the filters hide it.
    ImU32 foeColor(uint32_t type)
    {
        if (g_hiddenKinds.count(type))
        {
            return 0;
        }
        float rate = -1.0f;
        int killed = 0;
        CombatLearn::kind(type, rate, killed);
        const float percent = rate * 100.0f;
        if ((g_onlyKilled && !killed) || (g_minDanger > 0.0f && (rate < 0.0f || percent < g_minDanger)))
        {
            return 0;
        }
        if (rate < 0.0f)
        {
            return IM_COL32(170, 170, 170, 255);
        }
        const float t = std::clamp(std::log2(std::max(percent, 0.01f) / 0.25f) / 4.0f, 0.0f, 1.0f);
        return t < 0.5f ? IM_COL32(static_cast<int>(510 * t), 220, 40, 255)
                        : IM_COL32(255, static_cast<int>(220 * (1.0f - t) * 2.0f), 40, 255);
    }

    // The encyclopedia's filters on the map. Under g_lock.
    void drawFoeFilters()
    {
        ImGui::SeparatorText("Enciclopedia en el mapa");
        ImGui::Checkbox("Mostrar dónde se encontró a cada enemigo", &g_showFoes);
        ImGui::SetItemTooltip("Una cruz por tipo y lugar: de verde (pega poco) a rojo (pega fuerte); gris si aún no se sabe.");
        ImGui::SliderFloat("Peligro mínimo (% de vida/s)", &g_minDanger, 0.0f, 5.0f, "%.1f");
        ImGui::SetItemTooltip("Por encima de 0 se ocultan los tipos que aún no ha aprendido.");
        ImGui::Checkbox("Solo tipos que ya ha vencido", &g_onlyKilled);
        std::unordered_map<uint32_t, int> places;
        for (const Sighting& s : g_sightings)
        {
            ++places[s.type];
        }
        if (places.empty())
        {
            ImGui::TextDisabled("Aún no ha visto enemigos.");
            return;
        }
        if (ImGui::SmallButton("Todos"))
        {
            g_hiddenKinds.clear();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Ninguno"))
        {
            for (const auto& [type, n] : places)
            {
                g_hiddenKinds.insert(type);
            }
        }
        ImGui::BeginChild("##foekinds", ImVec2(0, 160));
        for (const auto& [type, n] : places)
        {
            float rate = -1.0f;
            int killed = 0;
            CombatLearn::kind(type, rate, killed);
            const auto name = g_kindNames.find(type);
            bool shown = !g_hiddenKinds.count(type);
            char label[160];
            if (rate >= 0.0f)
            {
                std::snprintf(label, sizeof(label), "%s  (%d lugares, %.2f %%/s, %d vencidos)##%u",
                              name != g_kindNames.end() ? name->second.c_str() : "?", n, rate * 100.0f, killed, type);
            }
            else
            {
                std::snprintf(label, sizeof(label), "%s  (%d lugares, sin aprender)##%u",
                              name != g_kindNames.end() ? name->second.c_str() : "?", n, type);
            }
            if (ImGui::Checkbox(label, &shown))
            {
                if (shown)
                {
                    g_hiddenKinds.erase(type);
                }
                else
                {
                    g_hiddenKinds.insert(type);
                }
            }
        }
        ImGui::EndChild();
    }

    int g_zoom = 4;     // pixels per square
    bool g_hud = true;  // the map on the screen's side, the menu closed too

    // The map around the hero as a round radar, `size` pixels across, at the cursor of the current ImGui window, turned
    // as the screen is: the game's view is isometric, a world step (dx, dy) in tiles goes (dx - dy, (dx + dy) / 2)
    // on the screen (seen live: 41, -5 tiles went 254, 98 pixels right and down), so squares show as the ground's
    // diamonds. Under g_lock.
    void drawMap(float size)
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float radius = size * 0.5f;
        const ImVec2 mid(origin.x + radius, origin.y + radius);
        const float s = g_zoom * 0.75f;     // half a diamond's width
        // Tile coordinates (fractional, from the hero's position) to the radar.
        const double hx = g_heroX / kTileSize, hy = g_heroY / kTileSize;
        const auto project = [&](double tx, double ty) {
            const double dx = tx - hx, dy = ty - hy;
            return ImVec2(mid.x + static_cast<float>((dx - dy) * s), mid.y + static_cast<float>((dx + dy) * s * 0.5));
        };
        const auto at = [&](int32_t x, int32_t y) { return project(x / kTileSize, y / kTileSize); };
        const auto inside = [&](const ImVec2& p, float margin) {
            const float ex = p.x - mid.x, ey = p.y - mid.y;
            return ex * ex + ey * ey <= (radius - margin) * (radius - margin);
        };
        dl->AddCircleFilled(mid, radius, IM_COL32(0, 0, 0, 255), 64);
        // Squares of a few pixels: without the anti-aliased fringe (it is all that shows of them, and seams between).
        const ImDrawListFlags flags = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        // The tiles whose diamond lies inside the circle: |dx|, |dy| up to 1.5 radius / s.
        const int reach = static_cast<int>(1.5f * radius / s) + 1;
        uint64_t lastChunk = ~0ull;
        const Chunk* chunk = nullptr;
        for (int dy = -reach; dy <= reach; ++dy)
        {
            for (int dx = -reach; dx <= reach; ++dx)
            {
                const int tx = g_heroTx + dx, ty = g_heroTy + dy;
                // One hash per chunk, not per square.
                const uint64_t ck = chunkKey(g_heroLayer, tx, ty);
                if (ck != lastChunk)
                {
                    lastChunk = ck;
                    const auto it = g_chunks.find(ck);
                    chunk = it == g_chunks.end() ? nullptr : it->second.get();
                }
                if (!chunk)
                {
                    continue;   // unexplored: the black
                }
                const Cell& c = chunk->cells[((ty & (kChunkSide - 1)) << kChunkShift) | (tx & (kChunkSide - 1))];
                if (!c.known())
                {
                    continue;
                }
                const ImVec2 centre = project(tx + 0.5, ty + 0.5);
                if (!inside(centre, s))
                {
                    continue;
                }
                const ImVec2 n = project(tx, ty), e = project(tx + 1, ty), so = project(tx + 1, ty + 1),
                             w = project(tx, ty + 1);
                if (!c.visits)
                {
                    // Never reached (blocked): clear, a hollow diamond.
                    dl->AddQuad(n, e, so, w, IM_COL32(120, 120, 140, 160));
                    continue;
                }
                dl->AddQuadFilled(n, e, so, w, c.samples ? shown(c) : IM_COL32(90, 90, 90, 255));
            }
        }
        dl->Flags = flags;
        dl->AddCircle(mid, radius, IM_COL32(200, 200, 200, 200), 64, 2.0f);
        const int half = reach;
        dl->PushClipRect(origin, ImVec2(origin.x + size, origin.y + size), true);
        // The trail: where it walked lately, fading with age; points past kTrailMs are gone.
        const DWORD now = GetTickCount();
        while (!g_trail.empty() && now - g_trail.front().at > kTrailMs)
        {
            g_trail.pop_front();
        }
        for (size_t i = 1; i < g_trail.size(); ++i)
        {
            const float fresh = 1.0f - static_cast<float>(now - g_trail[i].at) / kTrailMs;
            const ImVec2 a = at(g_trail[i - 1].x, g_trail[i - 1].y), b = at(g_trail[i].x, g_trail[i].y);
            if (inside(a, 1.0f) && inside(b, 1.0f))
            {
                dl->AddLine(a, b, IM_COL32(255, 255, 255, static_cast<int>(30 + 110 * std::max(0.0f, fresh))), 1.0f);
            }
        }
        // The route: where it is heading now.
        if (g_route)
        {
            const ImVec2 to = at(g_routeX, g_routeY);
            dl->AddLine(mid, to, IM_COL32(255, 220, 40, 230), 2.0f);
            dl->AddCircle(to, std::max(3.0f, g_zoom * 0.9f), IM_COL32(255, 220, 40, 230), 0, 1.5f);
        }
        const float dot = std::max(2.5f, g_zoom * 0.75f);
        // What it found: merchant a gold diamond, way in or out a cyan triangle, portal a violet ring.
        const float mark = std::max(4.0f, g_zoom * 1.4f);
        // The encyclopedia: a cross where each kind was met, green to red by how hard it hits, grey not yet learned.
        if (g_showFoes)
        {
            std::unordered_map<uint32_t, ImU32> colors;     // per kind this frame; 0: filtered out
            const float arm = mark * 0.6f;
            for (const Sighting& e : g_sightings)
            {
                if (e.layer != (g_heroLayer & 0xFF) || std::abs(e.tx - g_heroTx) > half + 2 ||
                    std::abs(e.ty - g_heroTy) > half + 2)
                {
                    continue;
                }
                auto it = colors.find(e.type);
                if (it == colors.end())
                {
                    it = colors.emplace(e.type, foeColor(e.type)).first;
                }
                if (!it->second)
                {
                    continue;
                }
                const ImVec2 p = project(e.tx + 0.5, e.ty + 0.5);
                if (!inside(p, arm))
                {
                    continue;
                }
                dl->AddLine(ImVec2(p.x - arm, p.y - arm), ImVec2(p.x + arm, p.y + arm), IM_COL32(0, 0, 0, 255), 3.5f);
                dl->AddLine(ImVec2(p.x - arm, p.y + arm), ImVec2(p.x + arm, p.y - arm), IM_COL32(0, 0, 0, 255), 3.5f);
                dl->AddLine(ImVec2(p.x - arm, p.y - arm), ImVec2(p.x + arm, p.y + arm), it->second, 2.0f);
                dl->AddLine(ImVec2(p.x - arm, p.y + arm), ImVec2(p.x + arm, p.y - arm), it->second, 2.0f);
            }
        }
        for (const Landmark& l : g_landmarks)
        {
            if (l.layer != (g_heroLayer & 0xFF) || std::abs(l.tx - g_heroTx) > half + 2 || std::abs(l.ty - g_heroTy) > half + 2)
            {
                continue;
            }
            const ImVec2 p = project(l.tx + 0.5, l.ty + 0.5);
            if (!inside(p, mark))
            {
                continue;
            }
            switch (l.kind)
            {
            case MapMemory::Mark::Merchant:
                dl->AddQuadFilled(ImVec2(p.x, p.y - mark), ImVec2(p.x + mark, p.y), ImVec2(p.x, p.y + mark),
                                  ImVec2(p.x - mark, p.y), IM_COL32(255, 200, 40, 255));
                dl->AddQuad(ImVec2(p.x, p.y - mark), ImVec2(p.x + mark, p.y), ImVec2(p.x, p.y + mark),
                            ImVec2(p.x - mark, p.y), IM_COL32(0, 0, 0, 255), 1.5f);
                break;
            case MapMemory::Mark::Entrance:
                dl->AddTriangleFilled(ImVec2(p.x - mark, p.y - mark * 0.8f), ImVec2(p.x + mark, p.y - mark * 0.8f),
                                      ImVec2(p.x, p.y + mark), IM_COL32(40, 230, 230, 255));
                dl->AddTriangle(ImVec2(p.x - mark, p.y - mark * 0.8f), ImVec2(p.x + mark, p.y - mark * 0.8f),
                                ImVec2(p.x, p.y + mark), IM_COL32(0, 0, 0, 255), 1.5f);
                break;
            case MapMemory::Mark::Portal:
                dl->AddCircle(p, mark, IM_COL32(200, 90, 255, 255), 0, 2.5f);
                dl->AddCircleFilled(p, mark * 0.35f, IM_COL32(200, 90, 255, 255));
                break;
            default:
                break;
            }
        }
        for (const BlipAt& b : g_blips)
        {
            const ImVec2 p = at(b.x, b.y);
            if (!inside(p, dot))
            {
                continue;
            }
            switch (b.kind)
            {
            case MapMemory::Blip::Enemy:
                dl->AddCircleFilled(p, dot, IM_COL32(255, 140, 20, 255));
                break;
            case MapMemory::Blip::Target:
                dl->AddCircleFilled(p, dot * 1.2f, IM_COL32(255, 30, 30, 255));
                dl->AddCircle(p, dot * 2.2f, IM_COL32(255, 30, 30, 255), 0, 1.5f);
                break;
            case MapMemory::Blip::Hireling:
                dl->AddCircleFilled(p, dot, IM_COL32(60, 150, 255, 255));
                break;
            }
        }
        // The hero: white with a dark rim, to stand out from the route and the enemies.
        dl->AddCircleFilled(mid, dot * 1.2f, IM_COL32(255, 255, 255, 255));
        dl->AddCircle(mid, dot * 1.2f, IM_COL32(0, 0, 0, 255), 0, 1.5f);
        dl->PopClipRect();
        ImGui::Dummy(ImVec2(size, size));
    }

    int seenOnLayer()
    {
        const auto it = g_visitedOn.find(g_heroLayer & 0xFF);
        return it == g_visitedOn.end() ? 0 : it->second;
    }

    // A landmark, unless one of its kind is already within a few squares. Under g_lock.
    void addLandmark(MapMemory::Mark kind, int layer, int tx, int ty)
    {
        for (const Landmark& l : g_landmarks)
        {
            if (l.kind == kind && l.layer == (layer & 0xFF) && std::abs(l.tx - tx) <= 4 && std::abs(l.ty - ty) <= 4)
            {
                return;
            }
        }
        g_landmarks.push_back({kind, static_cast<uint8_t>(layer), static_cast<int16_t>(tx), static_cast<int16_t>(ty)});
        g_dirty = true;
        static const char* const names[] = {"merchant", "way in or out", "portal"};
        LOG("MapMemory: {} found at {},{} on layer {}", names[static_cast<int>(kind)], tx, ty, layer);
    }

    void drawMenu()
    {
        ImGui::TextWrapped("Lo explorado alrededor del héroe, una casilla por coordenada: en el color del suelo que pisó; "
                           "transparente donde no consiguió pasar; negro lo inexplorado.");
        ImGui::Checkbox("Mostrar en un lado de la pantalla", &g_hud);
        ImGui::SliderInt("Tamaño de casilla (px)", &g_zoom, 2, 10);
        const std::lock_guard lock(g_lock);
        ensureLoaded();
        if (!g_heroKnown)
        {
            ImGui::TextDisabled("Sin héroe en el mundo.");
            return;
        }
        ImGui::Text("Capa %d: %d casillas pisadas (%d en total, %u KB). Casilla del héroe: %d, %d.", g_heroLayer,
                    seenOnLayer(), g_known, static_cast<unsigned>(g_chunks.size() * sizeof(Chunk) / 1024), g_heroTx,
                    g_heroTy);
        ImGui::TextDisabled("Hallazgos: %d. Rombo dorado: mercader. Triángulo cian: entrada o salida. Anillo violeta: "
                            "portal.", static_cast<int>(g_landmarks.size()));
        if (!g_landmarks.empty() && ImGui::SmallButton("Olvidar hallazgos"))
        {
            g_landmarks.clear();
            g_dirty = true;
        }
        drawMap(300.0f);
        drawFoeFilters();
    }

    // The agent's view in the screen's bottom right corner, clear of the game's windows.
    void drawHud()
    {
        if (!g_hud)
        {
            return;
        }
        const std::lock_guard lock(g_lock);
        if (!g_heroKnown)
        {
            return;
        }
        const ImGuiIO& io = ImGui::GetIO();
        const float size = std::round(io.DisplaySize.y * 0.22f);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 8.0f, io.DisplaySize.y - 8.0f), ImGuiCond_Always,
                                ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);     // only the round radar and its title show
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;
        if (ImGui::Begin("##agentmap", nullptr, flags))
        {
            ImGui::Text("Visión del agente  capa %d  %d casillas", g_heroLayer, seenOnLayer());
            drawMap(size);
        }
        ImGui::End();
    }

    void saveMenu(const std::wstring& ini)
    {
        ModkitMenu::write(ini, L"Map", L"Hud", g_hud);
        ModkitMenu::write(ini, L"Map", L"Zoom", g_zoom);
        ModkitMenu::write(ini, L"Map", L"Foes", g_showFoes);
        ModkitMenu::write(ini, L"Map", L"FoeMinDanger", static_cast<int>(std::lround(g_minDanger * 10.0f)));
        ModkitMenu::write(ini, L"Map", L"FoeOnlyKilled", g_onlyKilled);
        std::wstring hidden;
        {
            const std::lock_guard lock(g_lock);
            for (uint32_t type : g_hiddenKinds)
            {
                hidden += (hidden.empty() ? L"" : L",") + std::to_wstring(type);
            }
        }
        WritePrivateProfileStringW(L"Map", L"FoeHidden", hidden.c_str(), ini.c_str());
    }
}

void MapMemory::install()
{
    static std::once_flag once;
    std::call_once(once, [] {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring ini = exe;
        ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"SacredBild.ini";
        g_hud = GetPrivateProfileIntW(L"Map", L"Hud", 1, ini.c_str()) != 0;
        g_zoom = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Map", L"Zoom", 4, ini.c_str())), 2, 10);
        g_showFoes = GetPrivateProfileIntW(L"Map", L"Foes", 1, ini.c_str()) != 0;
        g_minDanger = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Map", L"FoeMinDanger", 0, ini.c_str())), 0, 50) / 10.0f;
        g_onlyKilled = GetPrivateProfileIntW(L"Map", L"FoeOnlyKilled", 0, ini.c_str()) != 0;
        wchar_t hidden[2048] = {};
        GetPrivateProfileStringW(L"Map", L"FoeHidden", L"", hidden, 2048, ini.c_str());
        for (wchar_t* p = hidden; *p;)
        {
            wchar_t* end = nullptr;
            const unsigned long type = std::wcstoul(p, &end, 10);
            if (end == p)
            {
                break;
            }
            g_hiddenKinds.insert(static_cast<uint32_t>(type));
            p = *end == L',' ? end + 1 : end;
        }
        ModkitMenu::add({"Mapa explorado", &drawMenu, &saveMenu});
        Overlay::setHud(&drawHud);
        // The whole map in memory from the start: the agent always has it at hand.
        const std::lock_guard lock(g_lock);
        ensureLoaded();
    });
}

void MapMemory::forgetHero()
{
    const std::lock_guard lock(g_lock);
    g_heroKnown = false;
    g_trail.clear();
    g_lastKey = ~0ull;
}

void MapMemory::sighting(uint32_t type, const std::string& name, int layer, int32_t x, int32_t y)
{
    install();
    const std::lock_guard lock(g_lock);
    const int tx = tileOf(x), ty = tileOf(y);
    size_t ofKind = 0;
    auto oldest = g_sightings.end();
    for (auto it = g_sightings.begin(); it != g_sightings.end(); ++it)
    {
        if (it->type != type)
        {
            continue;
        }
        if (it->layer == (layer & 0xFF) && std::abs(it->tx - tx) <= 6 && std::abs(it->ty - ty) <= 6)
        {
            return;     // this kind is already marked here
        }
        if (oldest == g_sightings.end())
        {
            oldest = it;
        }
        ++ofKind;
    }
    if (ofKind >= kSightingsPerKind)
    {
        g_sightings.erase(oldest);
    }
    else if (g_sightings.size() >= kSightings)
    {
        g_sightings.pop_front();
    }
    g_sightings.push_back({type, static_cast<uint8_t>(layer), static_cast<int16_t>(tx), static_cast<int16_t>(ty)});
    if (!name.empty() && name.find(' ') == std::string::npos)
    {
        g_kindNames.emplace(type, name);
    }
    g_dirty = true;
}

void MapMemory::mark(Mark kind, int layer, int32_t x, int32_t y)
{
    install();
    const std::lock_guard lock(g_lock);
    addLandmark(kind, layer, tileOf(x), tileOf(y));
}

void MapMemory::setHero(int layer, int32_t x, int32_t y)
{
    const std::lock_guard lock(g_lock);
    // A way into another layer (a door, stairs, a cave) or a jump on the same one (a portal): both ends marked.
    // A jump is one no walk or ride covers in the time since the last update (over 30 tiles and over 8 tiles a
    // second): updates pause meanwhile at times (a collect, a loading screen), and a walk then looks like a jump.
    static DWORD heroAt = 0;
    const DWORD now = GetTickCount();
    if (g_heroKnown)
    {
        const double jx = static_cast<double>(x) - g_heroX, jy = static_cast<double>(y) - g_heroY;
        const double tiles = std::sqrt(jx * jx + jy * jy) / kTileSize;
        const bool otherLayer = layer != g_heroLayer;
        if (otherLayer || (tiles > 30.0 && tiles > 8.0 * (now - heroAt) / 1000.0))
        {
            const MapMemory::Mark kind = otherLayer ? MapMemory::Mark::Entrance : MapMemory::Mark::Portal;
            addLandmark(kind, g_heroLayer, g_heroTx, g_heroTy);
            addLandmark(kind, layer, tileOf(x), tileOf(y));
        }
    }
    g_heroLayer = layer;
    g_heroTx = tileOf(x);
    g_heroTy = tileOf(y);
    g_heroX = x;
    g_heroY = y;
    g_heroKnown = true;
    heroAt = now;
    // The trail, a point every half tile or so; a jump (teleport, another layer) starts it anew.
    if (!g_trail.empty())
    {
        const double dx = x - g_trail.back().x, dy = y - g_trail.back().y;
        if (dx * dx + dy * dy > (30 * kTileSize) * (30 * kTileSize))
        {
            g_trail.clear();
        }
        else if (dx * dx + dy * dy < 0.25 * kTileSize * kTileSize)
        {
            return;
        }
    }
    g_trail.push_back({x, y, GetTickCount()});
    if (g_trail.size() > kTrailPoints)
    {
        g_trail.pop_front();
    }
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
    change(k, [](Cell& c) {
        c.visits = static_cast<uint8_t>(std::min(c.visits + 1, 255));
        // Reached: whatever blocked it before was the way, not the place.
        c.blocked = c.blocked > 0 ? static_cast<uint8_t>(c.blocked - 1) : 0;
    });
}

void MapMemory::blocked(int layer, int32_t x, int32_t y)
{
    const std::lock_guard lock(g_lock);
    ensureLoaded();
    change(keyAt(layer, x, y), [](Cell& c) { c.blocked = static_cast<uint8_t>(std::min(c.blocked + 1, 255)); });
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
            if (const Cell* c = find(layer, tx + dx, ty + dy))
            {
                visits += c->visits;
                blocked += c->blocked;
            }
        }
    }
    return std::log2(1.0f + visits) + 1.5f * blocked;
}

int MapMemory::cells()
{
    const std::lock_guard lock(g_lock);
    return g_known;
}

int MapMemory::cellsOn(int layer)
{
    const std::lock_guard lock(g_lock);
    const auto it = g_visitedOn.find(layer & 0xFF);
    return it == g_visitedOn.end() ? 0 : it->second;
}

void MapMemory::wantGround(int layer, int32_t x, int32_t y, float sx, float sy)
{
    const std::lock_guard lock(g_lock);
    const uint64_t k = keyAt(layer, x, y);
    const Cell* c = find(k);
    if (c && c->samples >= 3)
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
    change(k, [&](Cell& c) {
        const int n = c.samples;
        c.r = static_cast<uint8_t>((c.r * n + r) / (n + 1));
        c.g = static_cast<uint8_t>((c.g * n + g) / (n + 1));
        c.b = static_cast<uint8_t>((c.b * n + b) / (n + 1));
        c.samples = static_cast<uint8_t>(std::min(n + 1, 255));
    });
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
    forEach([f](uint64_t k, const Cell& c) {
        std::fprintf(f, "%d %d %d %u %u %06x %u\n", layerOfKey(k), txOfKey(k), tyOfKey(k), c.visits, c.blocked,
                     (c.r << 16) | (c.g << 8) | c.b, c.samples);
    });
    for (const Landmark& l : g_landmarks)
    {
        std::fprintf(f, "L %d %d %d %d\n", static_cast<int>(l.kind), l.layer, l.tx, l.ty);
    }
    for (const auto& [type, name] : g_kindNames)
    {
        std::fprintf(f, "N %u %s\n", type, name.c_str());
    }
    for (const Sighting& s : g_sightings)
    {
        std::fprintf(f, "E %u %d %d %d\n", s.type, s.layer, s.tx, s.ty);
    }
    std::fclose(f);
    MoveFileExW(temp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_dirty = false;
    g_savedAt = now;
}

void MapMemory::beginRadar()
{
    g_blipsBuilding.clear();
}

void MapMemory::addBlip(Blip kind, int32_t x, int32_t y)
{
    g_blipsBuilding.push_back({kind, x, y});
}

void MapMemory::setRoute(bool route, int32_t x, int32_t y)
{
    const std::lock_guard lock(g_lock);
    g_route = route;
    g_routeX = x;
    g_routeY = y;
}

void MapMemory::endRadar()
{
    const std::lock_guard lock(g_lock);
    g_blips.swap(g_blipsBuilding);
}

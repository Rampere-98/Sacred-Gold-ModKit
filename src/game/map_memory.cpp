#include "game/map_memory.h"
#include "game/agent_save.h"
#include "game/combat_learn.h"
#include "game/click_walk.h"
#include "game/hero_profile.h"
#include "game/world_tiles.h"
#include <queue>
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
#include <cstdlib>
#include <cstdio>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>
#include <format>
#include <string>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <vector>

namespace
{
    // One cell per tile (a map coordinate).
    constexpr double kTileSize = 53.66563;

    // 8 bytes a square (the game is 32 bit: the whole world explored must fit beside it). All zero: unexplored.
    struct Cell
    {
        uint8_t visits = 0;     // saturating
        uint8_t blocked = 0;    // saturating
        uint8_t r = 0, g = 0, b = 0;
        uint8_t samples = 0;    // ground color samples averaged (0: none yet)
        uint8_t scan = 0;       // the game's tile test, run over the loaded sectors: 0 not yet, 1 walkable, 2 not
        uint8_t kind = 0;       // ClickWalk::TileKind, read from the game's tile data (0: not read)
        bool known() const { return visits || blocked || samples || scan || kind; }
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
    bool g_showQuests = true;
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
    std::vector<MapMemory::QuestMark> g_questMarks;
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
        return p.substr(0, p.find_last_of(L"\\/") + 1) + L"SacredBild-map2.txt";  // map2, 2026-11-13: begun again (the player: "borra el registro actual")
    }

    // Up to `max` whitespace separated integers (decimal, or hex for the one at `hexAt`); how many were read.
    int numbers(const char* p, long* out, int max, int hexAt = -1)
    {
        int n = 0;
        while (n < max)
        {
            while (*p == ' ' || *p == '	')
            {
                ++p;
            }
            char* end = nullptr;
            out[n] = n == hexAt ? static_cast<long>(std::strtoul(p, &end, 16)) : std::strtol(p, &end, 10);
            if (end == p)
            {
                break;
            }
            p = end;
            ++n;
        }
        return n;
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
            // sscanf took over a second for a large file: the fields read by hand.
            int layer, tx, ty, visits, blocked, kind;
            unsigned type = 0;
            long v[8];
            if (line[0] == 'E')
            {
                if (numbers(line + 1, v, 4) == 4)
                {
                    type = static_cast<unsigned>(v[0]);
                    g_sightings.push_back({type, static_cast<uint8_t>(v[1]), static_cast<int16_t>(v[2]), static_cast<int16_t>(v[3])});
                }
                continue;
            }
            if (line[0] == 'N')
            {
                char name[96];
                if (std::sscanf(line, "N %u %95s", &type, name) == 2)
                {
                    g_kindNames[type] = name;
                }
                continue;
            }
            if (line[0] == 'L')
            {
                if (numbers(line + 1, v, 4) == 4)
                {
                    kind = static_cast<int>(v[0]);
                    if (kind >= 0 && kind < static_cast<int>(MapMemory::Mark::Count))
                    {
                        g_landmarks.push_back({static_cast<MapMemory::Mark>(kind), static_cast<uint8_t>(v[1]),
                                               static_cast<int16_t>(v[2]), static_cast<int16_t>(v[3])});
                    }
                }
                continue;
            }
            const int n = numbers(line, v, 9, 5);
            if (n < 5)
            {
                continue;
            }
            layer = static_cast<int>(v[0]);
            tx = static_cast<int>(v[1]);
            ty = static_cast<int>(v[2]);
            visits = static_cast<int>(v[3]);
            blocked = static_cast<int>(v[4]);
            const unsigned color = n > 5 ? static_cast<unsigned>(v[5]) : 0;
            const unsigned samples = n > 6 ? static_cast<unsigned>(v[6]) : 0;
            const unsigned scan = n > 7 ? static_cast<unsigned>(v[7]) : 0;
            const unsigned tileKind = n > 8 ? static_cast<unsigned>(v[8]) : 0;
            change(key(layer, tx, ty), [&](Cell& c) {
                c.visits = static_cast<uint8_t>(std::clamp(visits, 0, 255));
                c.blocked = static_cast<uint8_t>(std::clamp(blocked, 0, 255));
                if (n >= 8)
                {
                    c.scan = static_cast<uint8_t>(std::min(scan, 2u));
                }
                if (n >= 9)
                {
                    c.kind = static_cast<uint8_t>(tileKind & 0x1F);
                }
                if (n >= 7)
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
        ImGui::Checkbox("Mostrar misiones (?! dar, ! informar; amarillo ya disponible, blanco aún no)", &g_showQuests);
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
    // The map in the menu takes clicks: the hero walks there (ClickWalk, a route over this map when far).
    void drawMap(float size, bool clickable = false)
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
                    if (c.blocked)
                    {
                        // Tried and never reached: a hollow diamond (over the wall colour when the game says so).
                        if (c.scan == 2)
                        {
                            dl->AddQuadFilled(n, e, so, w, IM_COL32(140, 52, 36, 255));
                        }
                        dl->AddQuad(n, e, so, w, IM_COL32(255, 120, 200, 230), 1.5f);
                    }
                    else if (c.scan == 1)
                    {
                        // Walkable by the game's tile test, not walked yet: dim, ground colour if sampled.
                        dl->AddQuadFilled(n, e, so, w, c.samples ? (shown(c) & 0x00FFFFFF) | 0x80000000
                                                                 : IM_COL32(52, 58, 66, 255));
                    }
                    else if ((c.kind & 0xF) >= 2 && (c.kind & 0xF) <= 9 && (c.kind & 0xF) != 8)
                    {
                        // What it is, by the game's tile data (ClickWalk::TileKind): water blue, lava orange, a
                        // building brown, a rock or tree brick red, a door open green or shut yellow, a scripted
                        // barrier violet.
                        static constexpr ImU32 kKindColour[10] = {
                            0, 0, IM_COL32(40, 90, 170, 255), IM_COL32(220, 110, 20, 255), IM_COL32(110, 70, 40, 255),
                            IM_COL32(140, 52, 36, 255), IM_COL32(60, 170, 70, 255), IM_COL32(220, 190, 40, 255), 0,
                            IM_COL32(150, 60, 190, 255)};
                        dl->AddQuadFilled(n, e, so, w, kKindColour[c.kind & 0xF]);
                    }
                    else if (c.scan == 2)
                    {
                        // Not walkable (tree, rock, water, wall): a brick red that stands out from the black of
                        // the unexplored (the player, 2026-11-12: «las colisiones no están siendo pintadas»; it was
                        // 28,16,16, unseen on black).
                        dl->AddQuadFilled(n, e, so, w, IM_COL32(140, 52, 36, 255));
                    }
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
        // Loot under the creatures: an item a small light blue square, a chest a brown box with a gold rim, a hiding
        // place (or well, gold) a pink plus.
        for (const BlipAt& b : g_blips)
        {
            const ImVec2 p = at(b.x, b.y);
            const float r = dot * 0.9f;
            if (!inside(p, dot * 1.5f))
            {
                continue;
            }
            switch (b.kind)
            {
            case MapMemory::Blip::Item:
                dl->AddRectFilled(ImVec2(p.x - r * 0.7f, p.y - r * 0.7f), ImVec2(p.x + r * 0.7f, p.y + r * 0.7f),
                                  IM_COL32(140, 220, 255, 255));
                break;
            case MapMemory::Blip::Chest:
                dl->AddRectFilled(ImVec2(p.x - r * 1.3f, p.y - r), ImVec2(p.x + r * 1.3f, p.y + r), IM_COL32(150, 90, 30, 255));
                dl->AddRect(ImVec2(p.x - r * 1.3f, p.y - r), ImVec2(p.x + r * 1.3f, p.y + r), IM_COL32(255, 210, 60, 255), 0.0f,
                            0, 1.5f);
                break;
            case MapMemory::Blip::Hiding:
                dl->AddLine(ImVec2(p.x - r * 1.5f, p.y), ImVec2(p.x + r * 1.5f, p.y), IM_COL32(255, 90, 200, 255), 3.0f);
                dl->AddLine(ImVec2(p.x, p.y - r * 1.5f), ImVec2(p.x, p.y + r * 1.5f), IM_COL32(255, 90, 200, 255), 3.0f);
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
            default:
                break;
            }
        }
        // Quests: the journal's targets as rings (at the rim when off the radar), the givers' and receivers' signs.
        {
            const ImU32 yellow = IM_COL32(255, 215, 30, 255), white = IM_COL32(235, 235, 235, 255);
            const float fontSize = std::max(22.0f, mark * 3.0f);
            ImFont* font = ImGui::GetFont();
            for (const MapMemory::QuestMark& q : g_questMarks)
            {
                if (!g_showQuests)
                {
                    break;
                }
                if (q.layer != (g_heroLayer & 0xFF))
                {
                    continue;
                }
                ImVec2 p = at(q.x, q.y);
                const ImU32 color = q.yellow ? yellow : white;
                if (q.glyph == MapMemory::Glyph::Target)
                {
                    const float ring = mark * 1.5f;
                    if (!inside(p, ring))
                    {
                        // Off the radar: a dot on the rim, toward it.
                        const float ex = p.x - mid.x, ey = p.y - mid.y, len = std::sqrt(ex * ex + ey * ey);
                        if (len < 1.0f)
                        {
                            continue;
                        }
                        const float r = radius - ring - 2.0f;
                        p = ImVec2(mid.x + ex / len * r, mid.y + ey / len * r);
                        dl->AddCircleFilled(p, ring * 0.6f, color);
                        dl->AddCircle(p, ring * 0.6f, IM_COL32(0, 0, 0, 255), 0, 1.5f);
                        continue;
                    }
                    dl->AddCircle(p, ring, IM_COL32(0, 0, 0, 255), 0, 4.0f);
                    dl->AddCircle(p, ring, color, 0, 2.5f);
                    continue;
                }
                if (!inside(p, fontSize * 0.5f))
                {
                    continue;
                }
                const char* text = q.glyph == MapMemory::Glyph::Offer ? "?!" : q.glyph == MapMemory::Glyph::Report ? "!" : "?";
                const ImVec2 extent = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
                const ImVec2 corner(p.x - extent.x * 0.5f, p.y - extent.y * 0.5f);
                for (int ox = -1; ox <= 1; ++ox)
                {
                    for (int oy = -1; oy <= 1; ++oy)
                    {
                        if (ox || oy)
                        {
                            dl->AddText(font, fontSize, ImVec2(corner.x + ox * 1.5f, corner.y + oy * 1.5f),
                                        IM_COL32(0, 0, 0, 255), text);
                        }
                    }
                }
                dl->AddText(font, fontSize, corner, color, text);
            }
        }
        // The hero: white with a dark rim, to stand out from the route and the enemies.
        dl->AddCircleFilled(mid, dot * 1.2f, IM_COL32(255, 255, 255, 255));
        dl->AddCircle(mid, dot * 1.2f, IM_COL32(0, 0, 0, 255), 0, 1.5f);
        dl->PopClipRect();
        if (!clickable)
        {
            ImGui::Dummy(ImVec2(size, size));
            return;
        }
        ImGui::InvisibleButton("##agentmapclick", ImVec2(size, size));
        if (ImGui::IsItemHovered() && inside(ImGui::GetIO().MousePos, 0.0f))
        {
            // Back from the radar to tiles: ex = (dx - dy) s, ey = (dx + dy) s / 2.
            const float ex = (ImGui::GetIO().MousePos.x - mid.x) / s, ey = (ImGui::GetIO().MousePos.y - mid.y) / (s * 0.5f);
            const double tx = hx + (ex + ey) * 0.5, ty = hy + (ey - ex) * 0.5;
            ImGui::SetTooltip("Clic: ir allí (%.0f casillas)", std::max(std::abs(tx - hx), std::abs(ty - hy)));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                ClickWalk::walkTo(static_cast<int32_t>(tx * kTileSize), static_cast<int32_t>(ty * kTileSize));
            }
        }
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
                            "portal. Radar: cuadradito azul, objeto en el suelo; caja marrón, cofre; cruz rosa, escondite "
                            "mágico, pozo u oro.", static_cast<int>(g_landmarks.size()));
        if (!g_landmarks.empty() && ImGui::SmallButton("Olvidar hallazgos"))
        {
            g_landmarks.clear();
            g_dirty = true;
        }
        ImGui::TextDisabled("Clic en el mapa: el héroe va allí, por lo conocido, sin límite de distancia.");
        drawMap(300.0f, true);
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
        // Alt held (the player's wish, 2026-11-09): the radar larger, 55% of the screen's height instead of 22%, for a
        // closer look; only while the game has the focus (Alt-Tab elsewhere leaves it as it is).
        DWORD owner = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &owner);
        const bool large = owner == GetCurrentProcessId() && (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        const float size = std::round(io.DisplaySize.y * (large ? 0.55f : 0.22f));
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 8.0f, io.DisplaySize.y - 8.0f), ImGuiCond_Always,
                                ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);     // only the round radar and its title show: no frame, no padding
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 4.0f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoBackground;
        if (ImGui::Begin("##agentmap", nullptr, flags))
        {
            // The title centered over the radar, with a shadow to read on any ground.
            // Whose vision it is (the player, 2026-11-10: the character's name at the top; with eight agents side by
            // side each window tells its class and level).
            static const char* const kNames[] = {"Héroe",     "Serafín",  "Gladiador", "Mago de batalla", "Elfo oscuro",
                                                 "Elfa del bosque", "Vampiresa", "Vampiresa (noche)", "Enano", "Daemon"};
            const HeroProfile::Data hero = HeroProfile::current();
            const uint32_t cls = static_cast<uint32_t>(hero.cls);
            char title[128];
            if (hero.valid)
            {
                std::snprintf(title, sizeof(title), "%s · nivel %d · capa %d", kNames[cls < 10 ? cls : 0], hero.level,
                              g_heroLayer);
            }
            else
            {
                std::snprintf(title, sizeof(title), "Visión del agente · capa %d", g_heroLayer);
            }
            const ImVec2 extent = ImGui::CalcTextSize(title);
            const float width = std::max(size, extent.x);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 at(origin.x + std::round((width - extent.x) * 0.5f), origin.y);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 230), title);
            dl->AddText(at, IM_COL32(235, 235, 235, 255), title);
            ImGui::Dummy(ImVec2(width, extent.y));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::round((width - size) * 0.5f));
            drawMap(size);
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
    }

    void saveMenu(const std::wstring& ini)
    {
        ModkitMenu::write(ini, L"Map", L"Hud", g_hud);
        ModkitMenu::write(ini, L"Map", L"Zoom", g_zoom);
        ModkitMenu::write(ini, L"Map", L"Foes", g_showFoes);
        ModkitMenu::write(ini, L"Map", L"Quests", g_showQuests);
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
        g_showQuests = GetPrivateProfileIntW(L"Map", L"Quests", 1, ini.c_str()) != 0;
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
        // The whole map in memory from the start (preload() has usually read it by now).
        MapMemory::preload();
    });
}

void MapMemory::preload()
{
    // Read on a thread of its own from the game's start, while the menus and movies run: on the presenting thread a
    // large file held the first frames of a game for over a second (seen with the hitch watch).
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            const std::lock_guard lock(g_lock);
            ensureLoaded();
        }).detach();
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

std::vector<std::pair<int, int>> MapMemory::waysOut(int layer, int tx, int ty, int radius)
{
    std::vector<std::pair<int, int>> out;
    const std::lock_guard lock(g_lock);
    for (const Landmark& l : g_landmarks)
    {
        if (l.kind != MapMemory::Mark::Merchant && l.layer == (layer & 0xFF) && std::abs(l.tx - tx) <= radius &&
            std::abs(l.ty - ty) <= radius)
        {
            out.emplace_back(l.tx, l.ty);
        }
    }
    return out;
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
        // Not while a save loads (the hero «jumps» to where it was saved: A5's cave got two false portals, 2026-11-12).
        if (!AgentSave::worldChanging() && (otherLayer || (tiles > 30.0 && tiles > 8.0 * (now - heroAt) / 1000.0)))
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

namespace
{
    // The terrain scan: the game's tile test over the square around the hero, a slice a frame, so the map shows
    // what can be walked (and what not) without walking it. Restarts when the hero moves on, and every 15 s where
    // he stays: the test changes with the world (a door opened or shut, a house's floor read through its shut
    // door), so the latest reading wins; only a square stood on stays walkable whatever it says.
    constexpr int kScanReach = 96;              // tiles each way (the loaded sectors reach ~160)
    constexpr int kScanPerFrame = 2500;
    constexpr int kScanRestart = 24;            // tiles the hero moves before the square is scanned again
    constexpr DWORD kScanAgainMs = 15000;       // a scan done is run again after this long
    int g_scanLayer = -1, g_scanCx = 0, g_scanCy = 0, g_scanAt = 0;
    bool g_scanDone = false;
    DWORD g_scanDoneAt = 0;
    int g_kindSeen[10] = {};    // tile kinds read in the pass under way
    int g_indoorSeen = 0;       // and those with the indoors bit
    int g_obstacleGround[17] = {};  // the obstacles' ground class (tile +0x1F >> 4; 16: from the world files)

    // Under g_lock, on the game thread.
    void scanStep(int layer, int tx, int ty)
    {
        if (layer != g_scanLayer || std::abs(tx - g_scanCx) > kScanRestart || std::abs(ty - g_scanCy) > kScanRestart)
        {
            g_scanLayer = layer;
            g_scanCx = tx;
            g_scanCy = ty;
            g_scanAt = 0;
            g_scanDone = false;
            std::fill(std::begin(g_kindSeen), std::end(g_kindSeen), 0);
        }
        if (g_scanDone)
        {
            if (GetTickCount() - g_scanDoneAt < kScanAgainMs)
            {
                return;
            }
            g_scanAt = 0;
            g_scanDone = false;
        }
        constexpr int side = 2 * kScanReach + 1;
        for (int budget = kScanPerFrame; budget > 0 && g_scanAt < side * side; ++g_scanAt)
        {
            const int sx = g_scanCx - kScanReach + g_scanAt % side, sy = g_scanCy - kScanReach + g_scanAt / side;
            const uint64_t k = key(layer, sx, sy);
            const Cell* c = find(k);
            if (c && c->visits)
            {
                continue;   // stood on: walkable for good
            }
            --budget;
            const int kind = ClickWalk::tileKind(layer, sx, sy);
            if (kind > 0 && (kind & 0xF) < 10)
            {
                ++g_kindSeen[kind & 0xF];
                g_indoorSeen += (kind & ClickWalk::kIndoors) != 0;
                if ((kind & 0xF) == 5)
                {
                    const int ground = ClickWalk::tileGround(layer, sx, sy);
                    ++g_obstacleGround[ground < 0 ? 16 : ground & 0xF];
                }
            }
            const uint8_t now = ClickWalk::kindWalkable(kind) ? 1 : 2;
            if (kind > 0 && (!c || c->scan != now || c->kind != kind))
            {
                change(k, [now, kind](Cell& cell) {
                    cell.scan = now;
                    cell.kind = static_cast<uint8_t>(kind);
                });
            }
        }
        if (g_scanAt >= side * side && !g_scanDone)
        {
            g_scanDone = true;
            g_scanDoneAt = GetTickCount();
            // What the pass read, once a minute (to check the reading against the game).
            static DWORD s_toldAt = 0;
            if (!s_toldAt || g_scanDoneAt - s_toldAt > 60000)
            {
                s_toldAt = g_scanDoneAt;
                LOG("MapMemory: tiles read around {},{} layer {}: floor {}, water {}, lava {}, building {}, obstacle {}, "
                    "door open {}, door shut {}, barrier open {}, barrier shut {}; indoors bit {}",
                    g_scanCx, g_scanCy, layer, g_kindSeen[1], g_kindSeen[2], g_kindSeen[3], g_kindSeen[4],
                    g_kindSeen[5], g_kindSeen[6], g_kindSeen[7], g_kindSeen[8], g_kindSeen[9], g_indoorSeen);
            }
            std::fill(std::begin(g_kindSeen), std::end(g_kindSeen), 0);
            g_indoorSeen = 0;
            if (g_scanDoneAt == s_toldAt)
            {
                std::string grounds;
                for (int g = 0; g < 17; ++g)
                {
                    if (g_obstacleGround[g])
                    {
                        grounds += std::format(" {:x}:{}", g, g_obstacleGround[g]);
                    }
                }
                LOG("MapMemory: the obstacles' ground classes (10 = from the world files):{}", grounds);
            }
            std::fill(std::begin(g_obstacleGround), std::end(g_obstacleGround), 0);
        }
    }
}

void MapMemory::visit(int layer, int32_t x, int32_t y)
{
    install();
    setHero(layer, x, y);
    const std::lock_guard lock(g_lock);
    ensureLoaded();
    scanStep(layer, tileOf(x), tileOf(y));
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
        // Stood on: walkable, whatever the tile test said (a house's floor tested with its door shut reads as not
        // walkable; the player, 2026-11-13: the Seraphim kept from interiors so marked).
        if (c.scan == 2)
        {
            c.scan = 1;
        }
    });
}

namespace
{
    // Route lengths, worked out on a thread of their own (a search over a big box takes up to a second or more).
    struct RouteJob
    {
        int layer, fromX, fromY, toX, toY;
        uint64_t key;
    };
    std::mutex g_lenLock;
    std::condition_variable g_lenWake;
    std::deque<RouteJob> g_lenJobs;
    std::unordered_map<uint64_t, std::pair<float, DWORD>> g_lenKnown;   // length (-2 no route), when
    std::unordered_set<uint64_t> g_lenPending;
    bool g_lenThread = false;
    // The last length worked out to each goal (by layer and goal within 4 tiles), from where: while a new start is
    // worked out, that one plus the way between the starts (the goals' order does not swap back and forth meanwhile).
    struct LastLength
    {
        int fromX, fromY;
        float tiles;
        DWORD at;
    };
    std::unordered_map<uint64_t, LastLength> g_lenLast;

    uint64_t goalKey(int layer, int toX, int toY)
    {
        return (static_cast<uint64_t>(layer & 0xFF) << 32) | (static_cast<uint64_t>((toX >> 2) & 0xFFFF) << 16) |
               static_cast<uint64_t>((toY >> 2) & 0xFFFF);
    }

    uint64_t lengthKey(int layer, int fromX, int fromY, int toX, int toY)
    {
        // Ends within 4 tiles share one: 12 bits a coordinate (tiles below 16384).
        const auto q = [](int v) { return static_cast<uint64_t>((v >> 2) & 0xFFF); };
        return (static_cast<uint64_t>(layer & 0xFF) << 48) | (q(fromX) << 36) | (q(fromY) << 24) | (q(toX) << 12) |
               q(toY);
    }

    void lengthWorker()
    {
        // Below the game's threads, and a breath between searches (the player, 2026-11-11: «ahora todos tienen
        // tirones»: eight copies, each searching all the time, held the processor at 100 %).
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        for (;;)
        {
            Sleep(40);
            RouteJob job;
            {
                std::unique_lock lock(g_lenLock);
                g_lenWake.wait(lock, [] { return !g_lenJobs.empty(); });
                job = g_lenJobs.front();
                g_lenJobs.pop_front();
            }
            std::vector<std::pair<int, int>> cells;
            float tiles = -2.0f;
            if (MapMemory::route(job.layer, job.fromX, job.fromY, job.toX, job.toY, cells))
            {
                tiles = 0.0f;
                for (size_t i = 1; i < cells.size(); ++i)
                {
                    const bool diagonal = cells[i].first != cells[i - 1].first && cells[i].second != cells[i - 1].second;
                    tiles += diagonal ? 1.4142f : 1.0f;
                }
            }
            const std::lock_guard lock(g_lenLock);
            g_lenKnown[job.key] = {tiles, GetTickCount()};
            g_lenPending.erase(job.key);
            if (tiles >= 0.0f)
            {
                g_lenLast[goalKey(job.layer, job.toX, job.toY)] = {job.fromX, job.fromY, tiles, GetTickCount()};
            }
        }
    }
}

float MapMemory::routeTiles(int layer, int fromX, int fromY, int toX, int toY)
{
    // Far goals keep the straight line: a search over hundreds of tiles costs much, and that far the difference
    // tells little.
    if (std::abs(toX - fromX) > 300 || std::abs(toY - fromY) > 300)
    {
        return -2.0f;
    }
    const uint64_t key = lengthKey(layer, fromX, fromY, toX, toY);
    const DWORD now = GetTickCount();
    const std::lock_guard lock(g_lenLock);
    if (const auto it = g_lenKnown.find(key); it != g_lenKnown.end() && now - it->second.second < 120000)
    {
        return it->second.first;
    }
    const auto fromLast = [&]() -> float {
        const auto it = g_lenLast.find(goalKey(layer, toX, toY));
        if (it == g_lenLast.end() || now - it->second.at >= 120000)
        {
            return -1.0f;
        }
        const float moved = static_cast<float>(std::hypot(fromX - it->second.fromX, fromY - it->second.fromY));
        return moved <= 24.0f ? it->second.tiles + moved : -1.0f;
    };
    if (g_lenPending.count(key))
    {
        return fromLast();
    }
    // A short queue: the newest asked first, the oldest dropped (asked again if still wanted).
    if (g_lenJobs.size() >= 8)
    {
        g_lenPending.erase(g_lenJobs.back().key);
        g_lenJobs.pop_back();
    }
    g_lenJobs.push_front({layer, fromX, fromY, toX, toY, key});
    g_lenPending.insert(key);
    if (g_lenKnown.size() > 4000)
    {
        for (auto it = g_lenKnown.begin(); it != g_lenKnown.end();)
        {
            it = now - it->second.second >= 120000 ? g_lenKnown.erase(it) : std::next(it);
        }
    }
    if (!g_lenThread)
    {
        g_lenThread = true;
        std::thread(lengthWorker).detach();
    }
    g_lenWake.notify_one();
    if (g_lenLast.size() > 4000)
    {
        for (auto it = g_lenLast.begin(); it != g_lenLast.end();)
        {
            it = now - it->second.at >= 120000 ? g_lenLast.erase(it) : std::next(it);
        }
    }
    return fromLast();
}

namespace
{
    std::unordered_map<uint64_t, bool> g_doorTiles;   // under g_lock: the doors seen, by layer and tile: opens

    uint64_t doorKey(int layer, int x, int y)
    {
        return (static_cast<uint64_t>(layer & 0xFF) << 40) | (static_cast<uint64_t>(x & 0xFFFFF) << 20) |
               static_cast<uint64_t>(y & 0xFFFFF);
    }
}

void MapMemory::noteDoor(int layer, int tileX, int tileY, bool opens)
{
    const std::lock_guard lock(g_lock);
    g_doorTiles[doorKey(layer, tileX, tileY)] = opens;
}

bool MapMemory::doorInto(int layer, int goalX, int goalY, int heroX, int heroY, int& doorX, int& doorY)
{
    // The room: the walkable squares reached from the goal without crossing a door, a wall or a building, within
    // 24 tiles. Reaching the box's edge or an unread square means open ground, not indoors.
    constexpr int kReach = 24, kSide = 2 * kReach + 1;
    const std::lock_guard lock(g_lock);
    ensureLoaded();
    const auto kindAt = [&](int x, int y) -> int {
        const Cell* c = find(layer, x, y);
        if (!c)
        {
            return -1;
        }
        if (c->kind)
        {
            return c->kind & 0xF;
        }
        return c->visits || c->scan == 1 ? 1 : c->scan == 2 ? 5 : -1;
    };
    const auto isDoor = [](int k) { return k == 6 || k == 7; };
    std::vector<uint8_t> seen(kSide * kSide, 0);
    std::vector<std::pair<int, int>> todo{{goalX, goalY}}, doors;
    seen[kReach * kSide + kReach] = 1;
    bool heroIn = false;
    int walls = 0;
    for (size_t i = 0; i < todo.size(); ++i)
    {
        const auto [x, y] = todo[i];
        heroIn = heroIn || (x == heroX && y == heroY);
        static constexpr std::pair<int, int> kSteps[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto [dx, dy] : kSteps)
        {
            const int nx = x + dx, ny = y + dy;
            const int ix = nx - goalX + kReach, iy = ny - goalY + kReach;
            if (ix < 0 || iy < 0 || ix >= kSide || iy >= kSide)
            {
                return false;   // out of the box: open ground
            }
            uint8_t& s = seen[iy * kSide + ix];
            if (s)
            {
                continue;
            }
            s = 1;
            const int k = kindAt(nx, ny);
            if (k < 0)
            {
                return false;   // not read: it may be the way out
            }
            if (isDoor(k))
            {
                doors.emplace_back(nx, ny);
            }
            else if (ClickWalk::kindWalkable(k))
            {
                todo.emplace_back(nx, ny);
            }
            else
            {
                walls += k == 4 || k == 5;
            }
        }
        if (todo.size() > 900)
        {
            return false;   // too wide for a room
        }
    }
    // (The tile's +0x1E bit 2 is the safe zone of a settlement, not a roof: Manolito, 2026-11-13. Not used here.)
    if (heroIn || doors.empty() || walls < 4)
    {
        return false;
    }
    // The door on the hero's way in: the shortest walk hero -> door -> goal.
    double best = 1e9;
    for (const auto& [x, y] : doors)
    {
        const double d = std::hypot(x - heroX, y - heroY) + std::hypot(x - goalX, y - goalY);
        if (d < best)
        {
            best = d;
            doorX = x;
            doorY = y;
        }
    }
    return true;
}

void MapMemory::doorState(int layer, int tileX, int tileY, bool open)
{
    // The tile test behind a shut door reads the house's floor as not walkable, and a square's test was kept for
    // good: once a door is seen open, the squares within 14 tiles so marked are tested again (once each time it is
    // seen open after shut).
    static std::unordered_map<uint64_t, bool> s_seenOpen;
    const std::lock_guard lock(g_lock);
    const uint64_t k = doorKey(layer, tileX, tileY);
    bool& was = s_seenOpen[k];
    if (!open || was)
    {
        was = open;
        return;
    }
    was = true;
    int cleared = 0;
    for (int dy = -14; dy <= 14; ++dy)
    {
        for (int dx = -14; dx <= 14; ++dx)
        {
            const Cell* c = find(layer, tileX + dx, tileY + dy);
            if (c && c->scan == 2)
            {
                change(key(layer, tileX + dx, tileY + dy), [](Cell& cell) { cell.scan = 0; });
                ++cleared;
            }
        }
    }
    if (cleared)
    {
        g_scanAt = 0;
        g_scanDone = false;
        LOG("MapMemory: door at {},{} open: {} squares marked not walkable around it to be tested again", tileX, tileY,
            cleared);
    }
}

std::vector<std::pair<int, int>> MapMemory::doorsThatOpen(int layer, int x0, int y0, int x1, int y1)
{
    std::vector<std::pair<int, int>> out;
    const std::lock_guard lock(g_lock);
    for (const auto& [key, opens] : g_doorTiles)
    {
        const int l = static_cast<int>((key >> 40) & 0xFF);
        const int x = static_cast<int>((key >> 20) & 0xFFFFF), y = static_cast<int>(key & 0xFFFFF);
        if (opens && l == (layer & 0xFF) && x >= x0 && y >= y0 && x < x1 && y < y1)
        {
            out.emplace_back(x, y);
        }
    }
    return out;
}

bool MapMemory::route(int layer, int fromX, int fromY, int toX, int toY, std::vector<std::pair<int, int>>& out)
{
    out.clear();
    // A box around both ends; too far apart for one search (32 bit: keep it to a few MB), no route.
    constexpr int kMargin = 48, kMaxSide = 1100;
    const int x0 = std::min(fromX, toX) - kMargin, y0 = std::min(fromY, toY) - kMargin;
    const int w = std::abs(toX - fromX) + 2 * kMargin + 1, h = std::abs(toY - fromY) + 2 * kMargin + 1;
    if (w > kMaxSide || h > kMaxSide)
    {
        return false;
    }
    // The lock only while the agent's own map is copied (the player, 2026-11-11: «ya empiezan a dar tirones»: the
    // world files read for the roads and the search over up to 1100x1100 tiles held it for seconds, and the frame's
    // drawing waited on it, 2-3 s without a frame).
    std::unique_lock lock(g_lock);
    ensureLoaded();
    // Per tile, the step's cost: walked or walkable by the tile test 10, never seen 22 (may be anything), not
    // walkable or blocked again and again: no way.
    std::vector<uint8_t> step(static_cast<size_t>(w) * h, 22);
    for (int cy = (y0 >> kChunkShift); cy <= ((y0 + h - 1) >> kChunkShift); ++cy)
    {
        for (int cx = (x0 >> kChunkShift); cx <= ((x0 + w - 1) >> kChunkShift); ++cx)
        {
            const auto it = g_chunks.find(chunkKey(layer, cx << kChunkShift, cy << kChunkShift));
            if (it == g_chunks.end())
            {
                continue;
            }
            for (int i = 0; i < kChunkSide * kChunkSide; ++i)
            {
                const int tx = (cx << kChunkShift) + (i & (kChunkSide - 1)), ty = (cy << kChunkShift) + (i >> kChunkShift);
                if (tx < x0 || ty < y0 || tx >= x0 + w || ty >= y0 + h)
                {
                    continue;
                }
                const Cell& c = it->second->cells[i];
                uint8_t v = 22;
                if (c.visits || c.scan == 1)
                {
                    v = 10;
                }

                else if (c.blocked >= 2)
                {
                    v = 0;
                }
                else if ((c.kind & 0xF) == static_cast<uint8_t>(ClickWalk::TileKind::GateShut))
                {
                    v = 40;  // a shut door or gate: opened on the way (dear, it may be locked)
                }
                else if (c.kind && !ClickWalk::kindWalkable(c.kind))
                {
                    v = 0;   // water, lava, a building, a rock, a scripted barrier: by the game's own data
                }
                else if (c.scan == 2)
                {
                    v = 1;  // the tile test alone: closed, unless a door that opens is near (below)
                }
                step[static_cast<size_t>(ty - y0) * w + (tx - x0)] = v;
            }
        }
    }
    // The doors seen in the box: a way through (cost 12) or a wall (0), applied after the world files below.
    std::vector<std::tuple<int, int, bool>> doors;
    for (const auto& [k, opens] : g_doorTiles)
    {
        if (static_cast<int>(k >> 40) != layer)
        {
            continue;
        }
        const int dx = static_cast<int>((k >> 20) & 0xFFFFF), dy = static_cast<int>(k & 0xFFFFF);
        if (dx >= x0 && dy >= y0 && dx < x0 + w && dy < y0 + h)
        {
            doors.push_back({dx, dy, opens});
        }
    }
    lock.unlock();
    // Barred by the tile test alone within 12 tiles of a door that opens: a house's floor, tested with its door shut,
    // dear (40) but a way, so a route into it exists; a real wall there gets blocked on walking and closes then.
    for (size_t i = 0; i < step.size(); ++i)
    {
        if (step[i] != 1)
        {
            continue;
        }
        const int x = x0 + static_cast<int>(i % w), y = y0 + static_cast<int>(i / w);
        bool nearDoor = false;
        for (const auto& [dx, dy, opens] : doors)
        {
            nearDoor = nearDoor || (opens && std::abs(dx - x) <= 12 && std::abs(dy - y) <= 12);
        }
        step[i] = nearDoor ? 40 : 0;
    }
    // The roads first (the player, 2026-11-10: «las carreteras mapeadas y el agente se moviera por ellas»): on the
    // surface, by the world files (Manolito, block R2), a road tile costs 6, so far walks follow them; ground never
    // seen is judged by the files too (walkable 14, not walkable no way), unknown stays 22.
    if (layer == 0)
    {
        std::vector<int8_t> files(static_cast<size_t>(w) * h);
        WorldTiles::fill(x0, y0, w, h, files.data());
        for (size_t i = 0; i < files.size(); ++i)
        {
            uint8_t& v = step[i];
            const int b = files[i];
            if (!v || b < 0)
            {
                continue;   // seen blocked, or the files do not know
            }
            if ((b & 1) && WorldTiles::road((b >> 1) & 0xF))
            {
                v = 6;
            }
            else if (v == 22)
            {
                v = (b & 1) ? 14 : 0;
            }
        }
    }
    for (const auto& [dx, dy, opens] : doors)
    {
        for (int oy = -1; oy <= 1; ++oy)
        {
            for (int ox = -1; ox <= 1; ++ox)
            {
                const int x = dx + ox, y = dy + oy;
                if (x >= x0 && y >= y0 && x < x0 + w && y < y0 + h)
                {
                    step[static_cast<size_t>(y - y0) * w + (x - x0)] = opens ? 12 : 0;
                }
            }
        }
    }
    const auto index = [&](int x, int y) { return (y - y0) * w + (x - x0); };
    const int startCell = index(fromX, fromY), goalCell = index(toX, toY);
    step[startCell] = 10;
    step[goalCell] = std::max<uint8_t>(step[goalCell], 10);
    std::vector<int32_t> cost(static_cast<size_t>(w) * h, INT32_MAX);
    std::vector<int8_t> came(static_cast<size_t>(w) * h, -1);
    static constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static constexpr int kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    // Weighted a little past admissible: fast over open unknown ground, the route still close to the shortest.
    const auto estimate = [&](int x, int y) {
        const int dx = std::abs(x - toX), dy = std::abs(y - toY);
        // 8 straight, 12 diagonal: nearer the roads' 6 (the code reviewer: 12/17 made the roads count for little).
        return 8 * (dx + dy) - 4 * std::min(dx, dy);
    };
    using Entry = std::pair<int32_t, int32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    cost[startCell] = 0;
    open.push({estimate(fromX, fromY), startCell});
    bool reached = false;
    while (!open.empty())
    {
        const auto [f, cell] = open.top();
        open.pop();
        const int x = x0 + cell % w, y = y0 + cell / w;
        if (f - estimate(x, y) > cost[cell])
        {
            continue;
        }
        if (cell == goalCell)
        {
            reached = true;
            break;
        }
        for (int d = 0; d < 8; ++d)
        {
            const int nx = x + kDx[d], ny = y + kDy[d];
            if (nx < x0 || ny < y0 || nx >= x0 + w || ny >= y0 + h)
            {
                continue;
            }
            const int n = index(nx, ny);
            if (!step[n] || (d >= 4 && (!step[index(nx, y)] || !step[index(x, ny)])))
            {
                continue;
            }
            const int32_t c = cost[cell] + (d < 4 ? step[n] : step[n] * 14 / 10);
            if (c < cost[n])
            {
                cost[n] = c;
                came[n] = static_cast<int8_t>(d);
                open.push({c + estimate(nx, ny), n});
            }
        }
    }
    if (!reached)
    {
        return false;
    }
    for (int cell = goalCell; cell != startCell;)
    {
        const int x = x0 + cell % w, y = y0 + cell / w;
        out.push_back({x, y});
        const int d = came[cell];
        cell = index(x - kDx[d], y - kDy[d]);
    }
    out.push_back({fromX, fromY});
    std::reverse(out.begin(), out.end());
    return true;
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
                blocked += c->blocked + (c->scan == 2 ? 2 : 0);
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

namespace
{
    // What a save writes, copied under g_lock (a few MB of chunks: milliseconds) and written by a thread of its own:
    // formatting and writing the whole file (13 MB after a few hours) on the presenting thread froze the game and the
    // map for a moment each minute (seen live 2026-11-09).
    struct SaveCopy
    {
        std::vector<std::pair<uint64_t, Chunk>> chunks;
        std::vector<Landmark> landmarks;
        std::vector<std::pair<uint32_t, std::string>> names;
        std::vector<Sighting> sightings;
    };
    std::atomic<bool> g_saving{false};

    void writeSave(const SaveCopy& copy)
    {
        const std::wstring p = path(), temp = p + L".tmp";
        FILE* f = _wfopen(temp.c_str(), L"w");
        if (!f)
        {
            return;
        }
        std::vector<char> buffer(1 << 20);
        std::setvbuf(f, buffer.data(), _IOFBF, buffer.size());
        for (const auto& [ck, chunk] : copy.chunks)
        {
            const int layer = layerOfKey(ck), cx = txOfKey(ck), cy = tyOfKey(ck);
            for (int i = 0; i < kChunkSide * kChunkSide; ++i)
            {
                const Cell& c = chunk.cells[i];
                if (c.known())
                {
                    std::fprintf(f, "%d %d %d %u %u %06x %u %u %u\n", layer, cx * kChunkSide + (i & (kChunkSide - 1)),
                                 cy * kChunkSide + (i >> kChunkShift), c.visits, c.blocked, (c.r << 16) | (c.g << 8) | c.b,
                                 c.samples, c.scan, c.kind);
                }
            }
        }
        for (const Landmark& l : copy.landmarks)
        {
            std::fprintf(f, "L %d %d %d %d\n", static_cast<int>(l.kind), l.layer, l.tx, l.ty);
        }
        for (const auto& [type, name] : copy.names)
        {
            std::fprintf(f, "N %u %s\n", type, name.c_str());
        }
        for (const Sighting& s : copy.sightings)
        {
            std::fprintf(f, "E %u %d %d %d\n", s.type, s.layer, s.tx, s.ty);
        }
        std::fclose(f);
        MoveFileExW(temp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

void MapMemory::save(uint32_t now, uint32_t minMs)
{
    if (g_saving)
    {
        return;
    }
    auto copy = std::make_shared<SaveCopy>();
    {
        const std::lock_guard lock(g_lock);
        if (!g_dirty || (g_savedAt && now - g_savedAt < minMs))
        {
            return;
        }
        copy->chunks.reserve(g_chunks.size());
        for (const auto& [ck, chunk] : g_chunks)
        {
            copy->chunks.emplace_back(ck, *chunk);
        }
        copy->landmarks = g_landmarks;
        copy->names.assign(g_kindNames.begin(), g_kindNames.end());
        copy->sightings.assign(g_sightings.begin(), g_sightings.end());
        g_dirty = false;
        g_savedAt = now;
    }
    if (minMs == 0)
    {
        writeSave(*copy);   // asked now (leaving): written before returning
        return;
    }
    g_saving = true;
    std::thread([copy] {
        writeSave(*copy);
        g_saving = false;
    }).detach();
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

void MapMemory::setQuestMarks(std::vector<QuestMark> marks)
{
    const std::lock_guard lock(g_lock);
    g_questMarks.swap(marks);
}

void MapMemory::endRadar()
{
    const std::lock_guard lock(g_lock);
    g_blips.swap(g_blipsBuilding);
}

bool& MapMemory::hudShown()
{
    return g_hud;
}

bool& MapMemory::foesShown()
{
    return g_showFoes;
}

bool& MapMemory::questsShown()
{
    return g_showQuests;
}

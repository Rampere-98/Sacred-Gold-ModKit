#include "game/agent_overlay.h"
#include "game/map_memory.h"
#include "game/modkit_menu.h"
#include "overlay/overlay.h"
#include "log.h"

#include <windows.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace
{
    constexpr float kTile = 53.66563f;

    struct Settings
    {
        bool enabled = true;
        bool intent = true, route = true, target = true, aggro = true, doors = true;
    } g_settings;

    struct Prim
    {
        enum Kind : uint8_t
        {
            Poly,       // closed polyline (points)
            Line,
            Dot,
            Text,
            Circle,     // a screen circle at points[0], radius r
            Way,        // from points[0] to points[1] (cut at the screen's edge), a dot there, text its label
        } kind;
        uint32_t color;
        float thickness = 1.5f, r = 0.0f;
        std::vector<ImVec2> points;
        std::string text;
    };

    struct Frame
    {
        std::vector<Prim> prims;
        uint32_t at = 0;
    };

    std::mutex g_lock;
    Frame g_shown;          // the last frame (under g_lock)
    Frame g_building;       // render thread only
    int32_t g_hx = 0, g_hy = 0;
    float g_hsx = 0.0f, g_hsy = 0.0f;
    float g_scale = 0.0f;   // pixels per isometric world unit, 0 until measured
    bool g_building_on = false;

    ImVec2 project(double x, double y)
    {
        const double dx = x - g_hx, dy = y - g_hy;
        return ImVec2(static_cast<float>(g_hsx + g_scale * 0.894427 * (dx - dy)),
                      static_cast<float>(g_hsy + g_scale * 0.447214 * (dx + dy)));
    }

    void drawHud()
    {
        if (!g_settings.enabled)
        {
            return;
        }
        Frame frame;
        {
            const std::lock_guard lock(g_lock);
            if (!g_shown.at || GetTickCount() - g_shown.at > 600)
            {
                return;     // the agent drew nothing lately (menus, loading, another layer)
            }
            frame = g_shown;
        }
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        const ImU32 shadow = IM_COL32(0, 0, 0, 200);
        for (const Prim& p : frame.prims)
        {
            switch (p.kind)
            {
            case Prim::Poly:
                dl->AddPolyline(p.points.data(), static_cast<int>(p.points.size()), shadow, ImDrawFlags_Closed,
                                p.thickness + 1.5f);
                dl->AddPolyline(p.points.data(), static_cast<int>(p.points.size()), p.color, ImDrawFlags_Closed,
                                p.thickness);
                break;
            case Prim::Line:
                dl->AddLine(p.points[0], p.points[1], shadow, p.thickness + 1.5f);
                dl->AddLine(p.points[0], p.points[1], p.color, p.thickness);
                break;
            case Prim::Dot:
                dl->AddCircleFilled(p.points[0], p.r + 1.5f, shadow);
                dl->AddCircleFilled(p.points[0], p.r, p.color);
                break;
            case Prim::Circle:
                dl->AddCircle(p.points[0], p.r, shadow, 0, p.thickness + 1.5f);
                dl->AddCircle(p.points[0], p.r, p.color, 0, p.thickness);
                break;
            case Prim::Way:
            {
                const ImVec2 from = p.points[0];
                ImVec2 to = p.points[1];
                const ImVec2 size = ImGui::GetIO().DisplaySize;
                const auto cut = [](float a, float b, float lo, float hi) {
                    return b < lo ? (lo - a) / (b - a) : b > hi ? (hi - a) / (b - a) : 1.0f;
                };
                const float f = std::clamp(std::min(cut(from.x, to.x, 8.0f, size.x - 8.0f),
                                                    cut(from.y, to.y, 8.0f, size.y - 8.0f)), 0.0f, 1.0f);
                to = ImVec2(from.x + (to.x - from.x) * f, from.y + (to.y - from.y) * f);
                dl->AddLine(from, to, shadow, 3.5f);
                dl->AddLine(from, to, p.color, 2.0f);
                dl->AddCircleFilled(to, 6.5f, shadow);
                dl->AddCircleFilled(to, 5.0f, p.color);
                if (!p.text.empty())
                {
                    const ImVec2 ts = ImGui::CalcTextSize(p.text.c_str());
                    const ImVec2 at(std::clamp(to.x - ts.x * 0.5f, 4.0f, size.x - ts.x - 4.0f), to.y - ts.y - 10.0f);
                    dl->AddRectFilled(ImVec2(at.x - 4, at.y - 2), ImVec2(at.x + ts.x + 4, at.y + ts.y + 2),
                                      IM_COL32(0, 0, 0, 140), 3.0f);
                    dl->AddText(at, p.color, p.text.c_str());
                }
                break;
            }
            case Prim::Text:
            {
                const ImVec2 size = ImGui::CalcTextSize(p.text.c_str());
                const ImVec2 at(p.points[0].x - size.x * 0.5f, p.points[0].y - size.y);
                dl->AddRectFilled(ImVec2(at.x - 4, at.y - 2), ImVec2(at.x + size.x + 4, at.y + size.y + 2),
                                  IM_COL32(0, 0, 0, 140), 3.0f);
                dl->AddText(at, p.color, p.text.c_str());
                break;
            }
            }
        }
    }

    void drawMenu()
    {
        ImGui::Checkbox("Overlays del agente sobre el mundo", &g_settings.enabled);
        ImGui::SetItemTooltip("Dibujado encima del juego, alrededor del héroe; cada capa por separado.");
        ImGui::BeginDisabled(!g_settings.enabled);
        ImGui::Indent();
        ImGui::Checkbox("Intención y motivo sobre el héroe", &g_settings.intent);
        ImGui::Checkbox("Camino: adónde va (paso, misión, tienda)", &g_settings.route);
        ImGui::Checkbox("Objetivo: enemigo que ataca o botín que recoge", &g_settings.target);
        ImGui::Checkbox("Radio de aggro de los enemigos (11 casillas)", &g_settings.aggro);
        ImGui::SetItemTooltip("Rojo: ya van a por el héroe. Naranja: aún no; el agente los rodea al explorar.");
        ImGui::Checkbox("Puertas y entradas a las que va", &g_settings.doors);
        ImGui::Unindent();
        ImGui::EndDisabled();
        if (g_scale <= 0.0f)
        {
            ImGui::TextDisabled("Escala aún sin medir: falta ver una criatura en pantalla.");
        }
        ImGui::SeparatorText("Otros overlays del agente");
        ImGui::Checkbox("Radar «Visión del agente» en un lado", &MapMemory::hudShown());
        ImGui::Checkbox("Enemigos vistos, en el mapa", &MapMemory::foesShown());
        ImGui::Checkbox("Misiones, en el mapa", &MapMemory::questsShown());
    }

    void saveMenu(const std::wstring& ini)
    {
        ModkitMenu::write(ini, L"AgentOverlay", L"Enabled", g_settings.enabled);
        ModkitMenu::write(ini, L"AgentOverlay", L"Intent", g_settings.intent);
        ModkitMenu::write(ini, L"AgentOverlay", L"Route", g_settings.route);
        ModkitMenu::write(ini, L"AgentOverlay", L"Target", g_settings.target);
        ModkitMenu::write(ini, L"AgentOverlay", L"Aggro", g_settings.aggro);
        ModkitMenu::write(ini, L"AgentOverlay", L"Doors", g_settings.doors);
        ModkitMenu::write(ini, L"Map", L"Hud", MapMemory::hudShown());
        ModkitMenu::write(ini, L"Map", L"Foes", MapMemory::foesShown());
        ModkitMenu::write(ini, L"Map", L"Quests", MapMemory::questsShown());
    }
}

void AgentOverlay::install()
{
    static std::once_flag once;
    std::call_once(once, [] {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring ini = exe;
        ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"SacredBild.ini";
        const auto read = [&](const wchar_t* key, int fallback) {
            return GetPrivateProfileIntW(L"AgentOverlay", key, fallback, ini.c_str()) != 0;
        };
        // All on by default (the player, 2026-11-10: intent, way, target and doors always; the aggro rings only on
        // real hostiles).
        g_settings.enabled = read(L"Enabled", 1);
        g_settings.intent = read(L"Intent", 1);
        g_settings.route = read(L"Route", 1);
        g_settings.target = read(L"Target", 1);
        g_settings.aggro = read(L"Aggro", 1);
        g_settings.doors = read(L"Doors", 1);
        ModkitMenu::add({"Overlays del agente", &drawMenu, &saveMenu});
        Overlay::addHud(&drawHud);
        LOG("AgentOverlay: {} (intent {}, route {}, target {}, aggro {}, doors {})", g_settings.enabled ? "on" : "off",
            g_settings.intent, g_settings.route, g_settings.target, g_settings.aggro, g_settings.doors);
    });
}

bool AgentOverlay::shown(Layer layer)
{
    if (!g_settings.enabled)
    {
        return false;
    }
    switch (layer)
    {
    case Layer::Intent: return g_settings.intent;
    case Layer::Route: return g_settings.route;
    case Layer::Target: return g_settings.target;
    case Layer::Aggro: return g_settings.aggro;
    case Layer::Doors: return g_settings.doors;
    }
    return false;
}

void AgentOverlay::begin(int32_t heroX, int32_t heroY, float heroScreenX, float heroScreenY)
{
    g_building.prims.clear();
    g_building_on = g_settings.enabled;
    g_hx = heroX;
    g_hy = heroY;
    g_hsx = heroScreenX;
    g_hsy = heroScreenY;
}

void AgentOverlay::calibrate(int32_t x, int32_t y, float screenX, float screenY)
{
    const double dx = x - g_hx, dy = y - g_hy;
    const double ix = 0.894427 * (dx - dy), iy = 0.447214 * (dx + dy);
    const double iso = std::hypot(ix, iy);
    const double px = std::hypot(screenX - g_hsx, screenY - g_hsy);
    if (iso < 2.0 * kTile || px < 40.0)
    {
        return;     // too close to tell the scale
    }
    // The same way on screen as in the isometric frame, or it is not the projection (logged once).
    if ((ix * (screenX - g_hsx) + iy * (screenY - g_hsy)) / (iso * px) < 0.8)
    {
        static bool told = false;
        if (!told)
        {
            told = true;
            LOG("AgentOverlay: a creature off the expected isometric bearing (world {:.0f},{:.0f}, screen {:.0f},{:.0f})",
                dx, dy, screenX - g_hsx, screenY - g_hsy);
        }
        return;
    }
    const float s = static_cast<float>(px / iso);
    g_scale = g_scale <= 0.0f ? s : g_scale * 0.9f + s * 0.1f;
}

void AgentOverlay::ring(int32_t x, int32_t y, float tiles, uint32_t color, float thickness)
{
    if (!g_building_on || g_scale <= 0.0f)
    {
        return;
    }
    Prim p{Prim::Poly, color, thickness};
    for (int i = 0; i < 40; ++i)
    {
        const double a = i * 3.14159265 * 2.0 / 40.0;
        p.points.push_back(project(x + std::cos(a) * tiles * kTile, y + std::sin(a) * tiles * kTile));
    }
    g_building.prims.push_back(std::move(p));
}

void AgentOverlay::way(int32_t x, int32_t y, uint32_t color, const std::string& label)
{
    if (!g_building_on || g_scale <= 0.0f)
    {
        return;
    }
    g_building.prims.push_back({Prim::Way, color, 2.0f, 0.0f, {ImVec2(g_hsx, g_hsy), project(x, y)}, label});
}

void AgentOverlay::around(float screenX, float screenY, float radius, uint32_t color)
{
    if (!g_building_on)
    {
        return;
    }
    g_building.prims.push_back({Prim::Circle, color, 2.5f, radius, {ImVec2(screenX, screenY)}});
}

void AgentOverlay::heroText(const std::string& text, uint32_t color)
{
    if (!g_building_on || text.empty())
    {
        return;
    }
    g_building.prims.push_back({Prim::Text, color, 0.0f, 0.0f, {ImVec2(g_hsx, g_hsy - 70.0f)}, text});
}

void AgentOverlay::end(uint32_t now)
{
    if (!g_building_on)
    {
        return;
    }
    g_building.at = now ? now : 1;
    const std::lock_guard lock(g_lock);
    std::swap(g_shown, g_building);
}

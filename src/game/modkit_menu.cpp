#include "game/modkit_menu.h"
#include "config.h"
#include "game/focus.h"
#include "log.h"
#include "overlay/overlay.h"

#include <imgui.h>
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    constexpr int kMenuKey = VK_HOME;
    constexpr DWORD kSavedNoteMs = 3000;

    std::vector<ModkitMenu::Section>& sections()
    {
        static std::vector<ModkitMenu::Section> list;
        return list;
    }

    std::atomic<bool> g_closeRequested{false};
    bool g_keyWasDown = false;
    bool g_first = false;
    DWORD g_savedAt = 0;
    bool g_saveFailed = false;

    void save()
    {
        const std::wstring& ini = ConfigFile::path();
        g_saveFailed = ini.empty();
        if (!g_saveFailed)
        {
            for (const ModkitMenu::Section& s : sections())
            {
                if (s.save)
                {
                    s.save(ini);
                }
            }
        }
        g_savedAt = GetTickCount();
        LOG("ModKit menu: settings {}", g_saveFailed ? "not saved (no ini)" : "saved to SacredBild.ini");
    }

    bool draw()
    {
        if (g_closeRequested.exchange(false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            LOG("ModKit menu: closed");
            return false;
        }
        ImGuiIO& io = ImGui::GetIO();
        const float s = Overlay::scale();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
            ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(560.0f * s, 600.0f * s), ImGuiCond_Appearing);
        ImGui::SetNextWindowBgAlpha(0.92f);
        if (g_first)
        {
            ImGui::SetNextWindowFocus();
            g_first = false;
        }
        bool open = true;
        ImGui::Begin("Sacred Gold ModKit##menu", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
        ImGui::BeginChild("sections", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 1.4f));
        if (sections().empty())
        {
            ImGui::TextDisabled("Ningún mod instalado tiene ajustes.");
        }
        for (const ModkitMenu::Section& section : sections())
        {
            if (ImGui::CollapsingHeader(section.title, ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::PushID(section.title);
                ImGui::Indent();
                section.draw();
                ImGui::Unindent();
                ImGui::PopID();
                ImGui::Spacing();
            }
        }
        ImGui::EndChild();
        ImGui::Separator();
        if (ImGui::Button("Guardar en SacredBild.ini"))
        {
            save();
        }
        ImGui::SetItemTooltip("Los cambios ya se aplican al momento; guardarlos los mantiene al volver a arrancar.");
        ImGui::SameLine();
        if (ImGui::Button("Cerrar"))
        {
            open = false;
        }
        ImGui::SameLine();
        if (g_savedAt && GetTickCount() - g_savedAt < kSavedNoteMs)
        {
            ImGui::TextUnformatted(g_saveFailed ? "No se pudo guardar." : "Guardado.");
        }
        else
        {
            ImGui::TextDisabled("Inicio o Esc: cerrar");
        }
        ImGui::End();
        if (!open)
        {
            LOG("ModKit menu: closed");
        }
        return open;
    }
}

void ModkitMenu::add(const Section& section)
{
    sections().push_back(section);
}

void ModkitMenu::write(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int value)
{
    WritePrivateProfileStringW(section, key, std::to_wstring(value).c_str(), ini.c_str());
}

void ModkitMenu::onFrame()
{
    // Menus present from the UI thread, the game from the render thread; around loading both may.
    static std::mutex frameLock;
    const std::unique_lock lock(frameLock, std::try_to_lock);
    if (!lock)
    {
        return;
    }
    // The physical key, only while the game has the focus.
    // Bit 0: pressed since the last poll, so a tap shorter than a frame (slow frames while loading) still counts.
    const SHORT state = GetAsyncKeyState(kMenuKey);
    const bool foreground = Focus::foreground();
    const bool down = foreground && (state & 0x8000) != 0;
    const bool pressed = (down && !g_keyWasDown) || (foreground && !down && (state & 0x1) != 0);
    g_keyWasDown = down;
    if (!pressed)
    {
        return;
    }
    if (Overlay::isOpen())
    {
        g_closeRequested = true;
        return;
    }
    if (!Overlay::available())
    {
        LOG("ModKit menu: needs [DDraw] Backend=d3d9");
        return;
    }
    g_closeRequested = false;
    g_first = true;
    Overlay::open(&draw);
    LOG("ModKit menu: opened");
}

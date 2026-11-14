#include "game/act.h"
#include "game/modkit_menu.h"
#include "log.h"

#include <imgui.h>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace
{
    constexpr int kCount = static_cast<int>(Act::Kind::Count);

    struct Entry
    {
        const char* key;        // ini key and log name
        const char* label;      // the menu, UTF-8 Spanish
    };
    constexpr Entry kEntries[kCount] = {
        {"Walk", "Andar (explorar, ir a misiones)"},
        {"Attack", "Elegir enemigo y atacar"},
        {"Arts", "Lanzar artes y combos"},
        {"ArtBar", "Cambiar las artes de la barra"},
        {"LifePotion", "Beber poción de vida"},
        {"OtherPotions", "Otras pociones (antídoto, mentor…)"},
        {"PickUp", "Recoger botín"},
        {"Chests", "Abrir cofres, escondites y lápidas"},
        {"Doors", "Abrir puertas"},
        {"Talk", "Hablar y responder diálogos"},
        {"Mount", "Montar a caballo"},
        {"Dismount", "Bajar del caballo"},
        {"CallHorse", "Llamar al caballo"},
        {"Equip", "Equipar objetos mejores"},
        {"WeaponSlots", "Llenar y cambiar ranuras de arma"},
        {"Sell", "Vender"},
        {"Buy", "Comprar"},
        {"BuyHorse", "Comprar caballo"},
        {"Smith", "Engarzar en el herrero"},
        {"Combos", "Maestro de combos (combos y canje de runas)"},
        {"Runes", "Leer runas"},
        {"Scrolls", "Leer pergaminos y libros"},
        {"Attributes", "Repartir puntos de atributo"},
        {"Skills", "Subir y aprender habilidades"},
        {"SaveLoad", "Guardar y cargar partida"},
        {"Windows", "Cerrar ventanas"},
        {"Retreat", "Retirarse y esquivar"},
    };

    std::atomic<unsigned char> g_mode[kCount] = {};
    // Suggest: what the agent wants (the latest reason a kind gave) and the player's «do it» (valid for a while).
    std::mutex g_lock;
    std::string g_why[kCount];
    DWORD g_askedAt[kCount] = {};
    std::atomic<DWORD> g_grantUntil[kCount] = {};
    DWORD g_loggedAt[kCount] = {};
    unsigned char g_loggedAs[kCount] = {};
    std::atomic<bool> g_stop{false};
    std::atomic<DWORD> g_stopCheckedAt{0};
    bool g_copy = false;        // a training copy (SacredA<n>)
    std::wstring g_dir;         // the exe's folder, with the last slash

    constexpr DWORD kGrantMs = 30000;       // a «do it» of the player's lasts this long
    constexpr DWORD kAskShownMs = 20000;    // a suggestion stays in the menu this long after it was last asked
    constexpr DWORD kLogEveryMs = 10000;    // one line a kind while nothing changes

    void findFolder()
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe;
        const size_t slash = path.find_last_of(L"\\/");
        g_dir = path.substr(0, slash + 1);
        const size_t before = slash == std::wstring::npos || slash == 0 ? std::wstring::npos
                                                                         : path.find_last_of(L"\\/", slash - 1);
        const std::wstring name = before == std::wstring::npos ? L"" : path.substr(before + 1, slash - before - 1);
        g_copy = name.size() == 8 && name.compare(0, 7, L"SacredA") == 0 && name[7] >= L'1' && name[7] <= L'8';
    }

    bool stopped()
    {
        const DWORD now = GetTickCount();
        const DWORD at = g_stopCheckedAt.load(std::memory_order_relaxed);
        if (!at || now - at > 1000)
        {
            g_stopCheckedAt.store(now ? now : 1, std::memory_order_relaxed);
            const bool stop = GetFileAttributesW((g_dir + L"SacredBild-stop").c_str()) != INVALID_FILE_ATTRIBUTES;
            if (stop != g_stop.exchange(stop))
            {
                LOG("Act: {}", stop ? "the stop file is there: nothing is allowed" : "the stop file is gone");
            }
        }
        return g_stop.load(std::memory_order_relaxed);
    }

    // One JSON line: {"t":ms,"kind":"Equip","mode":"Auto","done":true,"why":"..."}; a line a kind while it repeats.
    void note(int i, Act::Mode mode, bool done, const char* why)
    {
        const DWORD now = GetTickCount();
        const unsigned char as = static_cast<unsigned char>((static_cast<int>(mode) << 1) | (done ? 1 : 0)) + 1;
        {
            const std::lock_guard lock(g_lock);
            if (g_loggedAs[i] == as && now - g_loggedAt[i] < kLogEveryMs)
            {
                return;
            }
            g_loggedAs[i] = as;
            g_loggedAt[i] = now;
        }
        std::string text = why ? why : "";
        for (char& c : text)
        {
            if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20)
            {
                c = ' ';
            }
        }
        SYSTEMTIME t;
        GetLocalTime(&t);
        FILE* f = nullptr;
        if (_wfopen_s(&f, (g_dir + L"SacredBild-act.jsonl").c_str(), L"ab") == 0 && f)
        {
            std::fprintf(f, "{\"time\":\"%02d:%02d:%02d\",\"kind\":\"%s\",\"mode\":\"%s\",\"done\":%s,\"why\":\"%s\"}\n",
                         t.wHour, t.wMinute, t.wSecond, kEntries[i].key,
                         mode == Act::Mode::Auto ? "Auto" : mode == Act::Mode::Suggest ? "Suggest" : "No",
                         done ? "true" : "false", text.c_str());
            std::fclose(f);
        }
    }

    void preset(Act::Mode all)
    {
        for (auto& m : g_mode)
        {
            m.store(static_cast<unsigned char>(all));
        }
    }

    void drawMenu()
    {
        ImGui::TextWrapped("Lo que el agente puede hacer por su cuenta. «No»: nunca. «Sugerir»: lo propone aquí y "
                           "espera a que pulses «Hazlo». «Auto»: lo hace solo.");
        if (g_stop.load())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Parado por el fichero SacredBild-stop: nada permitido.");
        }
        if (ImGui::Button("Manual (todo No)"))
        {
            preset(Act::Mode::No);
        }
        ImGui::SameLine();
        if (ImGui::Button("Sugerir todo"))
        {
            preset(Act::Mode::Suggest);
        }
        ImGui::SameLine();
        if (ImGui::Button("Autónomo (todo Auto)"))
        {
            preset(Act::Mode::Auto);
        }
        const DWORD now = GetTickCount();
        const char* const names[] = {"No", "Sugerir", "Auto"};
        for (int i = 0; i < kCount; ++i)
        {
            ImGui::PushID(i);
            int m = g_mode[i].load();
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::Combo("##mode", &m, names, 3))
            {
                g_mode[i].store(static_cast<unsigned char>(m));
                LOG("Act: {} set to {} (menu)", kEntries[i].key, names[m]);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(kEntries[i].label);
            if (m == static_cast<int>(Act::Mode::Suggest))
            {
                std::string why;
                DWORD askedAt = 0;
                {
                    const std::lock_guard lock(g_lock);
                    why = g_why[i];
                    askedAt = g_askedAt[i];
                }
                if (askedAt && now - askedAt < kAskShownMs)
                {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Hazlo"))
                    {
                        g_grantUntil[i].store((now + kGrantMs) | 1);
                        LOG("Act: {} granted by the player for {} s ({})", kEntries[i].key, kGrantMs / 1000, why);
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("propone: %s", why.empty() ? "…" : why.c_str());
                }
            }
            ImGui::PopID();
        }
    }

    void saveMenu(const std::wstring& ini)
    {
        for (int i = 0; i < kCount; ++i)
        {
            const std::string key = kEntries[i].key;
            ModkitMenu::write(ini, L"Permisos", std::wstring(key.begin(), key.end()).c_str(), g_mode[i].load());
        }
    }
}

void Act::install()
{
    static std::once_flag once;
    std::call_once(once, [] {
        findFolder();
        const std::wstring ini = g_dir + L"SacredBild.ini";
        // The player's own folder: nothing unless he says so (the potion of life as he had it: its own switch too).
        // A training copy: everything, as it played until now.
        int autos = 0;
        for (int i = 0; i < kCount; ++i)
        {
            const std::string key = kEntries[i].key;
            const int byDefault = g_copy || i == static_cast<int>(Kind::LifePotion) ? 2 : 0;
            int m = GetPrivateProfileIntW(L"Permisos", std::wstring(key.begin(), key.end()).c_str(), byDefault, ini.c_str());
            m = m < 0 ? 0 : m > 2 ? 2 : m;
            g_mode[i].store(static_cast<unsigned char>(m));
            autos += m == 2;
        }
        LOG("Act: permissions read ({} of {} on Auto; {})", autos, kCount,
            g_copy ? "a training copy" : "the player's folder: No unless set");
        ModkitMenu::add({"Permisos del agente", &drawMenu, &saveMenu});
    });
}

Act::Mode Act::mode(Kind kind)
{
    install();
    return static_cast<Mode>(g_mode[static_cast<int>(kind)].load(std::memory_order_relaxed));
}

const char* Act::name(Kind kind)
{
    return kEntries[static_cast<int>(kind)].key;
}

bool Act::allowed(Kind kind, const char* why)
{
    install();
    const int i = static_cast<int>(kind);
    if (stopped())
    {
        return false;
    }
    const Mode m = static_cast<Mode>(g_mode[i].load(std::memory_order_relaxed));
    if (m == Mode::Auto)
    {
        note(i, m, true, why);
        return true;
    }
    if (m == Mode::No)
    {
        return false;
    }
    const DWORD now = GetTickCount();
    const DWORD until = g_grantUntil[i].load(std::memory_order_relaxed);
    if (until && static_cast<int>(until - now) > 0)
    {
        note(i, m, true, why);
        return true;
    }
    {
        const std::lock_guard lock(g_lock);
        g_why[i] = why ? why : "";
        g_askedAt[i] = now ? now : 1;
    }
    note(i, m, false, why);
    return false;
}

bool Act::automatic(Kind kind)
{
    install();
    if (stopped())
    {
        return false;
    }
    const int i = static_cast<int>(kind);
    const Mode m = static_cast<Mode>(g_mode[i].load(std::memory_order_relaxed));
    if (m != Mode::Suggest)
    {
        return m == Mode::Auto;
    }
    const DWORD until = g_grantUntil[i].load(std::memory_order_relaxed);
    return until && static_cast<int>(until - GetTickCount()) > 0;
}

bool Act::may(Kind kind)
{
    install();
    return !stopped() && g_mode[static_cast<int>(kind)].load(std::memory_order_relaxed) != static_cast<unsigned char>(Mode::No);
}

bool Act::nothing()
{
    install();
    if (stopped())
    {
        return true;
    }
    for (const auto& m : g_mode)
    {
        if (m.load(std::memory_order_relaxed) != static_cast<unsigned char>(Mode::No))
        {
            return false;
        }
    }
    return true;
}

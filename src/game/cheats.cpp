#include "game/cheats.h"
#include "game/modkit_menu.h"
#include "input/inject.h"
#include "log.h"
#include "sacred/object.h"

#include <imgui.h>
#include <windows.h>
#include <atomic>
#include <format>
#include <mutex>
#include <string>

using namespace Sacred;

namespace
{
    constexpr int kCount = static_cast<int>(Cheats::Id::Count);

    struct Entry
    {
        const char* key;        // ini key, the log's name
        const char* category;   // the menu's heading, UTF-8 Spanish
        const char* label;
        const char* note;       // what it does and how far it is tried
    };
    constexpr Entry kEntries[kCount] = {
        {"Oro", "Economía", "Oro sin fin", "Rellena el oro hasta un millón con la función del propio juego."},
        {"Vida", "Personaje", "Vida sin fin (experimental)",
         "Devuelve la vida al máximo en cada fotograma. Escritura directa: puede no gustarle a las copias de control del juego."},
    };

    // The catalogue still to be built: shown greyed, by category, so that what is possible is in sight. Each needs
    // its addresses read from the game first (Manolito) and a try in A4 or A5.
    struct Planned
    {
        const char* category;
        const char* label;
    };
    constexpr Planned kPlanned[] = {
        {"Economía", "Precios a cero en las tiendas"},
        {"Economía", "Vender a precio de compra"},
        {"Personaje", "Maná y recarga de artes al instante"},
        {"Personaje", "Experiencia multiplicada"},
        {"Personaje", "Subir de nivel / puntos de atributo y de habilidad"},
        {"Personaje", "Todas las artes y runas"},
        {"Combate", "Daño multiplicado"},
        {"Combate", "Matar de un golpe"},
        {"Combate", "Los enemigos no atacan"},
        {"Objetos", "Crear un objeto por tipo y rareza"},
        {"Objetos", "Pociones sin fin"},
        {"Objetos", "Mochila sin límite"},
        {"Mundo", "Teletransporte al punto del mapa"},
        {"Mundo", "Velocidad de movimiento multiplicada"},
        {"Mundo", "Mapa descubierto entero"},
        {"Mundo", "Velocidad del juego"},
        {"Misiones", "Completar la misión seguida"},
        {"Misiones", "Reabrir una misión fallada"},
        {"Compañeros", "Mercenarios inmortales"},
        {"Compañeros", "Caballo sin requisito de montar"},
    };

    std::atomic<bool> g_on[kCount] = {};
    bool g_available = false;
    bool g_owners = false;      // SacredA7 / SacredA8: the player's own choice (a menu); A4 / A5: the ini only
    std::wstring g_ini;

    std::wstring g_dir;     // the exe's folder as it is on the disk (junctions resolved), with the last slash

    // The folder the exe runs from, as the disk has it: a junction or a link named SacredA4 that leads to the player's
    // folder gives the player's folder. The folder is opened, not the exe (an exe that is a hard link has many names).
    std::wstring realFolder()
    {
        wchar_t exe[MAX_PATH] = {};
        const DWORD got = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (!got || got >= MAX_PATH)
        {
            return L"";
        }
        std::wstring path = exe;
        path.resize(path.find_last_of(L"\\/") + 1);
        HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return L"";
        }
        wchar_t real[MAX_PATH] = {};
        const DWORD n = GetFinalPathNameByHandleW(h, real, MAX_PATH, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        CloseHandle(h);
        if (!n || n >= MAX_PATH)
        {
            return L"";
        }
        std::wstring dir = real;   // \\?\D:\...\SacredA4
        if (dir.back() != L'\\')
        {
            dir += L'\\';
        }
        return dir;
    }

    void findFolder()
    {
        g_dir = realFolder();
        const size_t slash = g_dir.size() >= 2 ? g_dir.find_last_of(L'\\', g_dir.size() - 2) : std::wstring::npos;
        const std::wstring name = slash == std::wstring::npos ? L"" : g_dir.substr(slash + 1, g_dir.size() - slash - 2);
        const bool copy = name.size() == 8 && _wcsnicmp(name.c_str(), L"SacredA", 7) == 0;
        const wchar_t n = copy ? name[7] : L'0';
        const bool lab = n == L'4' || n == L'5' || n == L'7' || n == L'8';
        // The witness: SacredBild-laboratorio.txt, its first line the folder's own name. A folder renamed or copied
        // (to A1, A9, the player's) keeps a witness naming another folder, and nothing is on.
        bool witnessed = false;
        if (lab)
        {
            wchar_t first[32] = {};
            if (FILE* f = _wfopen((g_dir + L"SacredBild-laboratorio.txt").c_str(), L"r, ccs=UTF-8"))
            {
                if (fgetws(first, 32, f))
                {
                    first[wcscspn(first, L"\r\n")] = 0;
                }
                fclose(f);
            }
            witnessed = _wcsicmp(first, name.c_str()) == 0;
            if (!witnessed)
            {
                LOG("Cheats: off: {} has no SacredBild-laboratorio.txt naming it", std::string(name.begin(), name.end()));
            }
        }
        g_available = lab && witnessed && _wcsicmp(name.c_str(), L"Sacred Gold") != 0;
        g_owners = g_available && (n == L'7' || n == L'8');
        g_ini = g_dir + L"SacredBild.ini";
    }

    void drawMenu()
    {
        ImGui::TextWrapped("Trampas de laboratorio de esta copia. No son del agente: él juega igual que en cualquier otra. "
                           "Lo que enciendas queda apuntado en el registro.");
        const char* category = nullptr;
        const auto heading = [&category](const char* c) {
            if (!category || std::string(category) != c)
            {
                category = c;
                ImGui::SeparatorText(c);
            }
        };
        for (int i = 0; i < kCount; ++i)
        {
            heading(kEntries[i].category);
            bool v = g_on[i].load();
            if (ImGui::Checkbox(kEntries[i].label, &v))
            {
                g_on[i].store(v);
                LOG("Cheats: {} turned {} (menu)", kEntries[i].key, v ? "on" : "off");
            }
            ImGui::SetItemTooltip("%s", kEntries[i].note);
            // The planned ones of the same category right under it.
            ImGui::BeginDisabled();
            for (const Planned& p : kPlanned)
            {
                bool off = false;
                if (std::string(p.category) == kEntries[i].category &&
                    (i + 1 == kCount || std::string(kEntries[i + 1].category) != kEntries[i].category))
                {
                    ImGui::Checkbox(p.label, &off);
                }
            }
            ImGui::EndDisabled();
        }
        // The categories with nothing built yet.
        ImGui::BeginDisabled();
        for (const Planned& p : kPlanned)
        {
            bool built = false;
            for (const Entry& e : kEntries)
            {
                built |= std::string(e.category) == p.category;
            }
            if (!built)
            {
                heading(p.category);
                bool off = false;
                ImGui::Checkbox(p.label, &off);
            }
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("En gris: por construir (faltan las direcciones del juego y la prueba en A4 o A5).");
    }

    void saveMenu(const std::wstring& ini)
    {
        for (int i = 0; i < kCount; ++i)
        {
            const std::string key = kEntries[i].key;
            ModkitMenu::write(ini, L"Trampas", std::wstring(key.begin(), key.end()).c_str(), g_on[i].load() ? 1 : 0);
        }
    }

    // A save file of this folder that is another folder's too (a hard link: the copies once shared the player's
    // GAME01..05): a cheat on with one of them loaded and a save from the game's menu would write the player's game
    // (the code reviewer, T1, 2026-11-14). No cheat while any is linked.
    bool sharesSaves()
    {
        for (int n = 1; n <= 5; ++n)
        {
            const std::wstring path = g_dir + std::format(L"save\\GAME{:02}.PAK", n);
            HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                continue;
            }
            BY_HANDLE_FILE_INFORMATION info = {};
            const bool linked = GetFileInformationByHandle(h, &info) && info.nNumberOfLinks > 1;
            CloseHandle(h);
            if (linked)
            {
                return true;
            }
        }
        return false;
    }

    void install()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            findFolder();
            if (g_available && sharesSaves())
            {
                g_available = false;
                LOG("Cheats: off: a GAME01..05.PAK of this copy is a hard link (the player's own saves): break the links first");
            }
            if (!g_available)
            {
                return;
            }
            // The gold without end A7 and A8 had from [Instance] Rol=Laboratorio stays on unless the key says no.
            wchar_t role[32] = {};
            GetPrivateProfileStringW(L"Instance", L"Rol", L"", role, 32, g_ini.c_str());
            const bool lab = g_owners && _wcsicmp(role, L"Laboratorio") == 0;
            std::string told;
            for (int i = 0; i < kCount; ++i)
            {
                const std::string key = kEntries[i].key;
                const int byDefault = lab && i == static_cast<int>(Cheats::Id::GoldNoEnd) ? 1 : 0;
                const bool v = GetPrivateProfileIntW(L"Trampas", std::wstring(key.begin(), key.end()).c_str(), byDefault, g_ini.c_str()) != 0;
                g_on[i].store(v);
                if (v)
                {
                    told += (told.empty() ? "" : ", ") + key;
                }
            }
            LOG("Cheats: a laboratory copy ({}); on: {}", g_owners ? "the player's choice, in the menu" : "the coordinator's, by the ini",
                told.empty() ? "none" : told);
            if (g_owners)
            {
                ModkitMenu::add({"Laboratorio: trampas", &drawMenu, &saveMenu});
            }
        });
    }

    std::atomic<bool> g_queued{false};     // an apply() waits in the engine's queue: no second one

    // On the engine's thread, an ordinary frame.
    void apply(void*)
    {
        g_queued.store(false);
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        if (!hero)
        {
            return;
        }
        if (g_on[static_cast<int>(Cheats::Id::HealthNoEnd)].load() && hero->health > 0 && hero->health < hero->maxHealth)
        {
            hero->health = hero->maxHealth;
        }
    }
}

bool Cheats::available()
{
    install();
    return g_available;
}

bool Cheats::on(Id id)
{
    install();
    return g_available && g_on[static_cast<int>(id)].load(std::memory_order_relaxed);
}

bool Cheats::taints()
{
    install();
    if (!g_available)
    {
        return false;
    }
    for (int i = 0; i < kCount; ++i)
    {
        if (i != static_cast<int>(Id::GoldNoEnd) && g_on[i].load(std::memory_order_relaxed))
        {
            return true;
        }
    }
    return false;
}

void Cheats::onFrame()
{
    install();
    if (g_available && g_on[static_cast<int>(Id::HealthNoEnd)].load(std::memory_order_relaxed) && !g_queued.exchange(true))
    {
        Inject::engineCall(&apply, nullptr);
    }
}

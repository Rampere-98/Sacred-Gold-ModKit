#include "game/hero_profile.h"
#include "game/modkit_menu.h"
#include "log.h"
#include "sacred/object.h"

#include <imgui.h>
#include <windows.h>
#include <mutex>

using namespace Sacred;

namespace
{
    constexpr size_t kStats = 0x3A8;
    HeroProfile::Data g_last;

    // Addresses from Manolito's catalogue (docs/AGENT_ADDRESSES.md «Arma equipada, vampiresa y montura», ENG exe).
    constexpr uintptr_t kItemTypes = 0xAAB5E4;      // [ ] + type*0x80: +0x18 u16 weapon-info index
    constexpr uintptr_t kWeaponInfo = 0x563004;     // [[kItemTypes] + this] + index*0x102: +0x1E u8 subclass
    constexpr uintptr_t kObjects = 0xAD5C40;        // [[ ]+4] + 4*(id & 0xFFFF): the object

    template <class T>
    T read(uintptr_t a)
    {
        T v{};
        __try
        {
            v = *reinterpret_cast<const T*>(a);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return v;
    }

    uint8_t kindOf(uint32_t id)
    {
        const uintptr_t objects = read<uintptr_t>(kObjects);
        const uintptr_t table = objects ? read<uintptr_t>(objects + 4) : 0;
        const uintptr_t item = id && table ? read<uintptr_t>(table + 4 * (id & 0xFFFF)) : 0;
        const uintptr_t types = read<uintptr_t>(kItemTypes);
        if (!item || !types)
        {
            return 0;
        }
        const uint32_t type = read<uint32_t>(item + 0x10);
        if (type > 0x10000)
        {
            return 0;
        }
        const uint16_t index = read<uint16_t>(types + type * 0x80 + 0x18);
        const uintptr_t infos = read<uintptr_t>(types + kWeaponInfo);
        return infos ? read<uint8_t>(infos + index * 0x102 + 0x1E) : 0;
    }

    uint8_t weaponKind(uintptr_t hero, uint32_t slot)
    {
        return kindOf(read<uint32_t>(hero + 0x1A4 + slot * 4));
    }
}

uint8_t HeroProfile::itemWeaponKind(uint32_t id)
{
    return kindOf(id);
}

bool HeroProfile::rangedKind(uint8_t kind)
{
    return kind == 6 || kind == 7 || kind == 12 || kind == 13 || kind == 14;
}

HeroProfile::Data HeroProfile::current()
{
    Data d;
    cObjectManager* objects = cObjectManager::instance();
    cCreature* hero = objects ? objects->hero() : nullptr;
    if (!hero)
    {
        return d;
    }
    const auto* st = reinterpret_cast<const uint8_t*>(hero) + kStats;
    const uint32_t cls = *reinterpret_cast<const uint32_t*>(st + 8);
    d.valid = true;
    d.heroId = hero->id;
    d.cls = cls <= 9 ? static_cast<Class>(cls) : Class::Unknown;
    d.level = *reinterpret_cast<const uint16_t*>(st + 0x56);
    d.health = hero->health;
    d.maxHealth = hero->maxHealth;
    const auto h = reinterpret_cast<uintptr_t>(hero);
    d.weaponKind = weaponKind(h, 13);
    if (!d.weaponKind)
    {
        d.weaponKind = weaponKind(h, 12);
    }
    d.ranged = rangedKind(d.weaponKind);
    d.mounted = read<uint32_t>(h + 0x1EC) != 0;
    return d;
}

const char* HeroProfile::className(Class cls)
{
    switch (cls)
    {
    case Class::Seraphim: return "Seraphim";
    case Class::Gladiator: return "Gladiador";
    case Class::BattleMage: return "Mago de batalla";
    case Class::DarkElf: return "Elfo oscuro";
    case Class::WoodElf: return "Elfa del bosque";
    case Class::Vampiress: return "Vampiresa";
    case Class::VampiressNight: return "Vampiresa (otra forma)";
    case Class::Dwarf: return "Enano";
    case Class::Daemon: return "Daemon";
    default: return "desconocida";
    }
}

void HeroProfile::onFrame()
{
    static std::once_flag menu;
    std::call_once(menu, [] { ModkitMenu::add({"Personaje controlado", &drawMenu, nullptr}); });
    const Data d = current();
    if (!d.valid)
    {
        return;
    }
    if (d.heroId != g_last.heroId || d.cls != g_last.cls || d.level != g_last.level ||
        d.weaponKind != g_last.weaponKind || d.mounted != g_last.mounted)
    {
        LOG("HeroProfile: {} (class {}), level {}, health {} of {}, weapon kind {} ({}){}", className(d.cls),
            static_cast<uint32_t>(d.cls), d.level, d.health, d.maxHealth, d.weaponKind,
            d.ranged ? "a distancia" : "cuerpo a cuerpo", d.mounted ? ", mounted" : "");
    }
    g_last = d;
}

void HeroProfile::drawMenu()
{
    const Data& d = g_last;
    if (!d.valid)
    {
        ImGui::TextDisabled("Sin personaje en juego.");
        return;
    }
    ImGui::Text("Clase: %s", className(d.cls));
    ImGui::Text("Nivel: %d", d.level);
    ImGui::Text("Vida: %d / %d", d.health, d.maxHealth);
    ImGui::Text("Arma: tipo %d, %s", d.weaponKind, d.ranged ? "a distancia" : "cuerpo a cuerpo");
    ImGui::Text("Montado: %s", d.mounted ? "sí" : "no");
}

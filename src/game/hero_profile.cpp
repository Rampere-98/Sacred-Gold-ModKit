#include "game/hero_profile.h"
#include "game/modkit_menu.h"
#include "log.h"
#include "sacred/object.h"

#include <imgui.h>
#include <mutex>

using namespace Sacred;

namespace
{
    constexpr size_t kStats = 0x3A8;
    HeroProfile::Data g_last;
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
    d.cls = cls <= 8 ? static_cast<Class>(cls) : Class::Unknown;
    d.level = *reinterpret_cast<const uint16_t*>(st + 0x56);
    d.health = hero->health;
    d.maxHealth = hero->maxHealth;
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
    if (d.heroId != g_last.heroId || d.cls != g_last.cls || d.level != g_last.level)
    {
        LOG("HeroProfile: {} (class {}), level {}, health {} of {}", className(d.cls), static_cast<uint32_t>(d.cls),
            d.level, d.health, d.maxHealth);
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
}

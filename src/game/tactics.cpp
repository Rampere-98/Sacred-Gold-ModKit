#include "game/tactics.h"
#include "config.h"
#include "config/tactics.h"
#include "game/frame_hooks.h"
#include "game/modkit_menu.h"
#include "input/inject.h"
#include "log.h"
#include "sacred/engine.h"
#include "sacred/object.h"
#include "sacred/ui.h"
#include "sig.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Sacred;

namespace
{
    using Art = Config::Tactics::Art;
    constexpr int kSlots = Config::Tactics::slotCount;

    // --- The game's side -------------------------------------------------------------------------------------------

    // usePotion (ENG 00612FA0): thiscall on the engine (type, toHirelings) -> whether one was drunk. Types: 1 healing
    // (Space; B gives it to the hirelings), 2 Undead Death (Q), 3 Mentor (W), 4 Antidote (E), 5 Concentration (R).
    using UsePotion = uint8_t(__fastcall*)(cEngine* self, void* edx, int type, int toHirelings);
    // The value the combat art order carries at +0x30 (ENG 00552370): thiscall on the caster (art id, 0).
    using ArtValue = uint32_t(__fastcall*)(cCreature* self, void* edx, uint32_t art, uint32_t zero);

    uintptr_t g_usePotion = 0;
    uintptr_t g_artValue = 0;
    uintptr_t g_players = 0;        // the address of the players' data pointer (ENG 00AD4448)

    const Sig::Entry kSigs[] = {
        {"usePotion", "8B 0D ?? ?? ?? ?? 83 EC 08 53 55 56 57 E8 ?? ?? ?? ?? 8B F8 85 FF 0F 84 ?? ?? ?? ?? 8B 47 0C", 0,
            Sig::Take::Match, &g_usePotion},
        {"artValue", "83 EC 08 8B 41 10 8B 54 24 0C 83 F8 09 53 89 4C 24 08", 0, Sig::Take::Match, &g_artValue},
        {"players", "A1 ?? ?? ?? ?? 85 C0 75 ?? 56 68 08 30 02 00", 1, Sig::Take::Abs32, &g_players},
    };
    bool g_ready = false;

    enum Potion
    {
        Healing = 1,
        UndeadDeath = 2,
        Mentor = 3,
        Antidote = 4,
        Concentration = 5,
    };

    // The players' data (32 of 0x1180 bytes, ENG 005DEBC0 / 005DEC90). For each, the combat art slots (keys 6-0):
    // 20-byte entries at +0xC90, two per slot (the second for the alternate bar, +0x1162 bit 1), a kind word (0 =
    // empty) and the art at +4; the selected slot of each group at +0xD58 (group 1, +0xD5A, the arts).
    constexpr size_t kPlayerSize = 0x1180;
    constexpr size_t kArtEntries = 0xC90;
    constexpr size_t kArtEntrySize = 20;
    constexpr size_t kAltBar = 0x1162;
    constexpr size_t kSelectedArt = 0xD5A;

    // The engine's art the right click casts (set as a slot is selected, ENG 00619286): a kind word at +0x58 and
    // the art at +0x5A, cast only below 0x4B0 (the world mouse handler, ENG 00617BC7).
    constexpr size_t kEngineArtKind = 0x58;
    constexpr size_t kEngineArt = 0x5A;
    constexpr uint16_t kArtLimit = 0x4B0;

    // An object's position as the orders take it (cObject +0x18): a word, x, y (world), the layer byte at +0x24.
    constexpr size_t kPosition = 0x18;
    constexpr size_t kLayer = 0x24;

    // The order a right click on the world gives with a combat art selected (type 0x105, built at ENG 00617C2C and
    // handed to cEngine::sendOrder).
    struct ArtOrder
    {
        uintptr_t vtable = Addr::cOrder_vtable;
        uint32_t type = 0x105;
        uint32_t _08 = 0, _0C = 0;
        uint32_t target = 0;        // the creature clicked, 0 for the ground
        uint32_t position = 0;      // the position's word
        int32_t x = 0, y = 0;
        uint32_t _20 = 0, _24 = 0;
        uint32_t power = 100;
        uint32_t kind = 0;          // the engine's art kind
        uint32_t value = 0;         // ArtValue(art)
        uint32_t layer = 0;
        uint32_t _38 = 0, _3C = 0, _40 = 0;
    };
    static_assert(sizeof(ArtOrder) == sizeof(cOrder));
    static_assert(offsetof(ArtOrder, target) == 0x10 && offsetof(ArtOrder, power) == 0x28);
    static_assert(offsetof(ArtOrder, value) == 0x30 && offsetof(ArtOrder, layer) == 0x34);

    template <class T>
    T at(const void* base, size_t offset)
    {
        return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(base) + offset);
    }

    const uint8_t* playerData()
    {
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        // The player index is the id's low word (usePotion, ENG 00612FBC).
        const uint32_t index = hero ? hero->id & 0xFFFF : 0x20;
        if (!g_players || index >= 0x20)
        {
            return nullptr;
        }
        const uint8_t* players = *reinterpret_cast<const uint8_t* const*>(g_players);
        return players ? players + index * kPlayerSize : nullptr;
    }

    struct SlotArt
    {
        uint16_t kind = 0;
        uint32_t art = 0;       // what identifies the art, also when it moves to another slot
    };

    SlotArt slotArt(const uint8_t* player, int slot)
    {
        const int alt = (at<uint16_t>(player, kAltBar) & 2) ? 1 : 0;
        const size_t entry = kArtEntries + static_cast<size_t>(slot * 2 + alt) * kArtEntrySize;
        return {at<uint16_t>(player, entry), at<uint32_t>(player, entry + 4)};
    }

    int slotKey(int slot)
    {
        return slot == 4 ? '0' : '6' + slot;
    }

    void tap(int vk)
    {
        Inject::key(vk, true);
        Inject::key(vk, false);
    }

    bool drink(Potion type, bool toHirelings)
    {
        cEngine* engine = FrameHooks::engine();
        if (!g_usePotion || !engine)
        {
            return false;
        }
        return reinterpret_cast<UsePotion>(g_usePotion)(engine, nullptr, type, toHirelings ? 1 : 0) != 0;
    }

    // --- What the arts do, learned -------------------------------------------------------------------------------

    struct Learned
    {
        int heal = 0, area = 0, single = 0, none = 0;   // what each cast seemed to do
        DWORD readyAt = 0;                              // not cast again before (recharging)
        DWORD lastCast = 0;

        Art kind() const
        {
            if (heal > 0 && heal >= area && heal >= single)
            {
                return Art::Heal;
            }
            if (area >= 2 || (area > 0 && area >= single))
            {
                return Art::Area;
            }
            if (single > 0)
            {
                return Art::Single;
            }
            return none >= 4 ? Art::Buff : Art::Auto;   // Auto: not known yet
        }
    };
    std::unordered_map<uint32_t, Learned> g_learned;

    const char* artName(Art art)
    {
        switch (art)
        {
        case Art::Heal: return "curación";
        case Art::Area: return "área";
        case Art::Single: return "un objetivo";
        case Art::Buff: return "mejora";
        case Art::Off: return "no usar";
        default: return "sin saber aún";
        }
    }

    // What was learned stays for the next start: [Tactics.Learned] Art<id>=heal|area|single|buff.
    Learned& learned(uint32_t art)
    {
        const auto it = g_learned.find(art);
        if (it != g_learned.end())
        {
            return it->second;
        }
        Learned& l = g_learned[art];
        const std::wstring& ini = ConfigFile::path();
        if (!ini.empty())
        {
            wchar_t key[24], value[16] = {};
            swprintf_s(key, L"Art%X", art);
            GetPrivateProfileStringW(L"Tactics.Learned", key, L"", value, 16, ini.c_str());
            switch (Config::parseArt(value))
            {
            case Art::Heal: l.heal = 2; break;
            case Art::Area: l.area = 2; break;
            case Art::Single: l.single = 2; break;
            case Art::Buff: l.none = 4; break;
            default: break;
            }
        }
        return l;
    }

    void remember(uint32_t art, Art kind)
    {
        const std::wstring& ini = ConfigFile::path();
        if (ini.empty() || kind == Art::Auto)
        {
            return;
        }
        wchar_t key[24];
        swprintf_s(key, L"Art%X", art);
        WritePrivateProfileStringW(L"Tactics.Learned", key, Config::artKey(kind), ini.c_str());
    }

    // --- The fight, as seen ------------------------------------------------------------------------------------------

    // Enemies' maximum health seen lately: what an ordinary enemy has here.
    std::vector<int> g_seenMaxima;
    std::unordered_map<uint32_t, int> g_lastHealth;     // each enemy's health at the last look
    // The plain attack's damage per second at the target (while no art is cast), a running estimate.
    float g_dps = 0.0f;
    float g_dpsSeconds = 0.0f;
    DWORD g_lastThink = 0;

    std::unordered_map<uint32_t, DWORD> g_friendSince;  // non-hostile creatures and since when they are about
    int g_drainTicks = 0;           // looks in a row the hero lost health with no enemy about
    int g_prevHealth = 0;
    bool g_artsRecharging = false;  // an art was wanted but all were recharging

    // --- The art under way -------------------------------------------------------------------------------------

    enum class Phase
    {
        Idle,
        Selecting,  // the slot key went out; the engine takes the art a frame or two later
        Casting,    // the order went out; its effect is measured
    };
    Phase g_phase = Phase::Idle;
    DWORD g_phaseAt = 0;
    int g_slot = -1;
    uint32_t g_art = 0;
    uint32_t g_castTarget = 0;
    bool g_castClose = false;       // the target stood close as the art went out: an art with no effect is one
    int g_restoreSlot = -1;         // the slot the player had selected
    int g_healthAtCast = 0, g_bestHealth = 0, g_maxHealth = 0;
    std::unordered_map<uint32_t, int> g_foesAtCast;
    DWORD g_nextArtAt = 0;          // a little time between arts, as a player takes
    DWORD g_nextPotionAt = 0;
    DWORD g_potionReady[6] = {};    // per type: not before this tick (cool down, or none left)

    constexpr DWORD kSelectMs = 120;
    constexpr DWORD kBusyMs = 1100;     // the order's animation: no other order meanwhile
    constexpr DWORD kMeasureMs = 1600;

    DWORD jitter(DWORD lo, DWORD hi)
    {
        static uint32_t state = GetTickCount() | 1;
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return lo + state % (hi - lo + 1);
    }

    bool sendArt(uint32_t target)
    {
        cEngine* engine = FrameHooks::engine();
        cObjectManager* objects = cObjectManager::instance();
        if (!engine || !objects || !g_artValue)
        {
            return false;
        }
        cCreature* self = objects->creature(engine->controlled);
        cObject* at_ = target ? objects->object(target) : self;
        if (!self || !at_)
        {
            return false;
        }
        const uint16_t art = at<uint16_t>(engine, kEngineArt);
        if (art >= kArtLimit)
        {
            LOG("Tactics: no art selected (engine art {:x})", art);
            return false;
        }
        ArtOrder order;
        order.target = target;
        order.position = at<uint16_t>(at_, kPosition);
        order.x = at_->x;
        order.y = at_->y;
        order.layer = at<uint8_t>(at_, kLayer);
        order.kind = at<uint16_t>(engine, kEngineArtKind);
        order.value = reinterpret_cast<ArtValue>(g_artValue)(self, nullptr, art, 0);
        engine->sendOrder(self, reinterpret_cast<cOrder&>(order));
        return true;
    }

    void startArt(int slot, uint32_t art, uint32_t target, const Tactics::Situation& s, const char* why)
    {
        const uint8_t* player = playerData();
        if (!player)
        {
            return;
        }
        g_restoreSlot = at<uint16_t>(player, kSelectedArt);
        g_slot = slot;
        g_art = art;
        g_castTarget = target;
        g_phase = Phase::Selecting;
        g_phaseAt = s.now;
        g_healthAtCast = g_bestHealth = s.health;
        g_maxHealth = s.maxHealth;
        g_foesAtCast.clear();
        g_castClose = !target;
        for (int i = 0; i < s.count; ++i)
        {
            const AimAssist::Foe& f = s.foes[i];
            g_foesAtCast[f.id] = f.health;
            const float dx = f.x - s.hx, dy = f.y - s.hy;
            if (f.id == target && dx * dx + dy * dy < (s.range * 0.45f) * (s.range * 0.45f))
            {
                g_castClose = true;
            }
        }
        if (g_restoreSlot != slot)
        {
            tap(slotKey(slot));
        }
        LOG("Tactics: art slot {} ({:x}, {}) at {:x}: {}", slot + 1, art, artName(learned(art).kind()), target, why);
    }

    // What the cast did, from the health of the hero and the enemies before and after; false if nothing was seen.
    bool measure()
    {
        Learned& l = learned(g_art);
        const Art before = l.kind();
        const int gain = g_bestHealth - g_healthAtCast;
        int hurtOthers = 0;
        bool hurtTarget = false;
        for (const auto& [id, before] : g_foesAtCast)
        {
            const auto now = g_lastHealth.find(id);
            const bool dropped = now == g_lastHealth.end() ? id == g_castTarget : now->second < before;
            if (!dropped)
            {
                continue;
            }
            if (id == g_castTarget)
            {
                hurtTarget = true;
            }
            else
            {
                ++hurtOthers;
            }
        }
        const char* seen;
        if (gain >= std::max(8, g_maxHealth * 6 / 100))
        {
            ++l.heal;
            seen = "healed";
        }
        else if (hurtOthers >= 2)
        {
            ++l.area;
            seen = "hurt several";
        }
        else if (hurtTarget)
        {
            ++l.single;
            seen = "hurt the target";
        }
        else if (g_castClose)
        {
            ++l.none;
            seen = "did nothing seen";
        }
        else
        {
            seen = "did nothing seen, the target far";
        }
        if (l.kind() != before)
        {
            remember(g_art, l.kind());
        }
        LOG("Tactics: art {:x} {} (gain {}, others hurt {}) -> {}", g_art, seen, gain, hurtOthers, artName(l.kind()));
        return gain > 0 || hurtOthers > 0 || hurtTarget;
    }

    // --- Choosing ------------------------------------------------------------------------------------------------

    Art slotUse(int slot, uint32_t art)
    {
        const Art set = Config::tactics.slots[slot];
        return set != Art::Auto ? set : learned(art).kind();
    }

    int median(std::vector<int> v)
    {
        if (v.empty())
        {
            return 0;
        }
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    }

    void chooseArt(const Tactics::Situation& s)
    {
        const uint8_t* player = playerData();
        if (!player || static_cast<int>(s.now - g_nextArtAt) < 0)
        {
            return;
        }
        const AimAssist::Foe* target = nullptr;
        int close = 0;
        for (int i = 0; i < s.count; ++i)
        {
            const float dx = s.foes[i].x - s.hx, dy = s.foes[i].y - s.hy;
            if (dx * dx + dy * dy < (s.range * 0.45f) * (s.range * 0.45f))
            {
                ++close;
            }
            if (s.foes[i].id == s.target)
            {
                target = &s.foes[i];
            }
        }
        const int missing = s.maxHealth - s.health;
        const bool danger = s.health * 100 <= s.maxHealth * 45 || (s.hurt && close >= 3);
        // Around the target: how many enemies an area art would catch.
        int around = 0;
        bool strong = false, easy = false;
        if (target)
        {
            const float r = s.range * 0.3f;
            for (int i = 0; i < s.count; ++i)
            {
                const float dx = s.foes[i].x - target->x, dy = s.foes[i].y - target->y;
                if (dx * dx + dy * dy <= r * r)
                {
                    ++around;
                }
            }
            const int typical = g_seenMaxima.size() >= 5 ? median(g_seenMaxima) : 0;
            const float seconds = g_dps > 0.0f && g_dpsSeconds >= 3.0f ? target->health / g_dps : 0.0f;
            strong = (typical > 0 && target->maxHealth * 10 >= typical * 18) || seconds > 8.0f;
            easy = (seconds > 0.0f && seconds < 3.0f) || target->health * 10 < target->maxHealth * 2;
        }

        // Each slot by its use; the best one that is ready.
        int pick = -1, pickRank = 0;
        uint32_t pickArt = 0, pickTarget = 0;
        const char* why = "";
        bool wantedButRecharging = false;
        for (int slot = 0; slot < kSlots; ++slot)
        {
            const SlotArt sa = slotArt(player, slot);
            if (!sa.kind || !sa.art)
            {
                continue;
            }
            const Art use = slotUse(slot, sa.art);
            int rank = 0;
            uint32_t at_ = 0;
            const char* reason = "";
            switch (use)
            {
            case Art::Heal:
                if (missing * 100 >= s.maxHealth * 35 || (danger && missing * 100 >= s.maxHealth * 25))
                {
                    rank = 5;
                    reason = "health low";
                }
                break;
            case Art::Area:
                if (target && !easy && (around >= 3 || (danger && around >= 2)))
                {
                    rank = 4;
                    at_ = target->id;
                    reason = "several enemies together";
                }
                break;
            case Art::Single:
                if (target && !easy && (strong || danger))
                {
                    rank = 3;
                    at_ = target->id;
                    reason = strong ? "a strong enemy" : "hard pressed";
                }
                break;
            case Art::Buff:
                if ((close >= 3 || strong || danger) && s.threat &&
                    static_cast<int>(s.now - learned(sa.art).lastCast) > 45000)
                {
                    rank = 2;
                    reason = "a hard fight starts";
                }
                break;
            case Art::Auto:
                // Not known yet: tried where any art would be worth it, to learn what it does.
                if (target && !easy && (strong || danger || around >= 3))
                {
                    rank = 1;
                    at_ = target->id;
                    reason = "trying it out";
                }
                break;
            default:
                break;
            }
            if (!rank)
            {
                continue;
            }
            if (static_cast<int>(s.now - learned(sa.art).readyAt) < 0)
            {
                wantedButRecharging = true;
                continue;
            }
            if (rank > pickRank)
            {
                pick = slot;
                pickRank = rank;
                pickArt = sa.art;
                pickTarget = at_;
                why = reason;
            }
        }
        g_artsRecharging = wantedButRecharging && pick < 0 && (danger || close >= 3 || strong);
        if (pick >= 0)
        {
            startArt(pick, pickArt, pickTarget, s, why);
        }
    }

    // --- Consumables ---------------------------------------------------------------------------------------------

    bool potionReady(Potion type, DWORD now)
    {
        return static_cast<int>(now - g_potionReady[type]) >= 0;
    }

    void tryPotion(Potion type, bool toHirelings, DWORD now, DWORD coolDownMs, const char* name, const char* why)
    {
        if (drink(type, toHirelings))
        {
            g_potionReady[type] = now + coolDownMs;
            LOG("Tactics: drank {} ({})", name, why);
        }
        else
        {
            g_potionReady[type] = now + 60000;  // none left: a look again in a while
            LOG("Tactics: no {} to drink ({})", name, why);
        }
        g_nextPotionAt = now + jitter(900, 1600);
    }

    void chooseConsumable(const Tactics::Situation& s)
    {
        const Config::Tactics& c = Config::tactics;
        if (!c.consumables || static_cast<int>(s.now - g_nextPotionAt) < 0)
        {
            return;
        }
        int close = 0;
        bool strong = false;
        const int typical = g_seenMaxima.size() >= 5 ? median(g_seenMaxima) : 0;
        for (int i = 0; i < s.count; ++i)
        {
            const float dx = s.foes[i].x - s.hx, dy = s.foes[i].y - s.hy;
            if (dx * dx + dy * dy < (s.range * 0.45f) * (s.range * 0.45f))
            {
                ++close;
            }
            if (s.foes[i].id == s.target && typical > 0 && s.foes[i].maxHealth * 10 >= typical * 18)
            {
                strong = true;
            }
        }
        const bool danger = s.health * 100 <= s.maxHealth * 45 || (s.hurt && close >= 3);
        const bool bigFight = s.count >= 4 || strong;

        // Poison: health draining with no enemy about.
        if (s.count == 0 && g_prevHealth && s.health < g_prevHealth)
        {
            ++g_drainTicks;
        }
        else if (s.count > 0 || s.health >= g_prevHealth)
        {
            g_drainTicks = 0;
        }
        g_prevHealth = s.health;

        if (c.antidote && g_drainTicks >= 4 && potionReady(Antidote, s.now))
        {
            g_drainTicks = 0;
            tryPotion(Antidote, false, s.now, 15000, "an antidote", "health drains with no enemy about");
            return;
        }
        if (c.undeadDeath && danger && close >= 3 && potionReady(UndeadDeath, s.now))
        {
            tryPotion(UndeadDeath, false, s.now, 60000, "Undead Death", "hard pressed by several");
            return;
        }
        if (c.concentration && g_artsRecharging && potionReady(Concentration, s.now))
        {
            tryPotion(Concentration, false, s.now, 60000, "Concentration", "the arts wanted are recharging");
            return;
        }
        if (c.mentor && bigFight && s.threat && potionReady(Mentor, s.now))
        {
            tryPotion(Mentor, false, s.now, 120000, "Mentor", "a big fight");
            return;
        }
        if (c.healHirelings && potionReady(Healing, s.now))
        {
            // Hirelings: creatures that are no enemies and stay about the hero.
            AimAssist::Foe friends[16];
            const int n = AimAssist::friends(s.range, friends, 16);
            std::unordered_map<uint32_t, DWORD> since;
            bool hurtHireling = false;
            for (int i = 0; i < n; ++i)
            {
                const auto it = g_friendSince.find(friends[i].id);
                const DWORD from = it != g_friendSince.end() ? it->second : s.now;
                since[friends[i].id] = from;
                if (s.now - from > 20000 && friends[i].maxHealth > 0 &&
                    friends[i].health * 100 < friends[i].maxHealth * 45)
                {
                    hurtHireling = true;
                }
            }
            g_friendSince = std::move(since);
            if (hurtHireling)
            {
                tryPotion(Healing, true, s.now, 8000, "a healing potion to the hirelings", "a hireling is badly hurt");
            }
        }
    }

    // --- Menu ----------------------------------------------------------------------------------------------------

    void drawMenu()
    {
        Config::Tactics& c = Config::tactics;
        if (!g_ready)
        {
            ImGui::TextDisabled("No disponible: el ejecutable no es el esperado.");
            return;
        }
        ImGui::Checkbox("Usar artes de combate", &c.arts);
        ImGui::SetItemTooltip("Con el ataque automático: curas solo si falta vida, área solo con varios enemigos,\n"
                              "golpes fuertes solo contra enemigos que lo merecen, mejoras al empezar una pelea dura.");
        const char* uses[] = {"Automático", "Curación", "Área", "Un objetivo", "Mejora", "No usar"};
        const uint8_t* player = playerData();
        for (int slot = 0; slot < kSlots; ++slot)
        {
            ImGui::PushID(slot);
            int use = static_cast<int>(c.slots[slot]);
            ImGui::SetNextItemWidth(160.0f * ImGui::GetFontSize() / 16.0f);
            if (ImGui::Combo("##use", &use, uses, IM_ARRAYSIZE(uses)))
            {
                c.slots[slot] = static_cast<Art>(use);
            }
            ImGui::SameLine();
            const SlotArt sa = player ? slotArt(player, slot) : SlotArt{};
            if (!sa.kind || !sa.art)
            {
                ImGui::Text("Ranura %d (tecla %c): vacía", slot + 1, slotKey(slot));
            }
            else
            {
                ImGui::Text("Ranura %d (tecla %c): %s", slot + 1, slotKey(slot), artName(learned(sa.art).kind()));
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
        ImGui::Checkbox("Usar consumibles", &c.consumables);
        ImGui::SetItemTooltip("Las pociones de curación del héroe son de «Pociones automáticas».");
        ImGui::Indent();
        ImGui::Checkbox("Muerte de no-muertos (Q): con varios enemigos encima y poca vida", &c.undeadDeath);
        ImGui::Checkbox("Mentor (W): en peleas grandes", &c.mentor);
        ImGui::Checkbox("Antídoto (E): si pierdes vida sin enemigos cerca (veneno)", &c.antidote);
        ImGui::Checkbox("Concentración (R): si las artes que hacen falta se están recargando", &c.concentration);
        ImGui::Checkbox("Curar a los mercenarios (B): si uno está muy herido", &c.healHirelings);
        ImGui::Unindent();
    }

    void saveMenu(const std::wstring& ini)
    {
        const Config::Tactics& c = Config::tactics;
        ModkitMenu::write(ini, L"Tactics", L"Arts", c.arts);
        for (int i = 0; i < kSlots; ++i)
        {
            const std::wstring key = L"Slot" + std::to_wstring(i + 1);
            WritePrivateProfileStringW(L"Tactics", key.c_str(), Config::artKey(c.slots[i]), ini.c_str());
        }
        ModkitMenu::write(ini, L"Tactics", L"Consumables", c.consumables);
        ModkitMenu::write(ini, L"Tactics", L"UndeadDeath", c.undeadDeath);
        ModkitMenu::write(ini, L"Tactics", L"Mentor", c.mentor);
        ModkitMenu::write(ini, L"Tactics", L"Antidote", c.antidote);
        ModkitMenu::write(ini, L"Tactics", L"Concentration", c.concentration);
        ModkitMenu::write(ini, L"Tactics", L"HealHirelings", c.healHirelings);
    }
}

void Tactics::install()
{
    g_ready = Sig::resolve(kSigs) == 0;
    LOG("Tactics: {}", g_ready ? "ready" : "off, the game code is not the one expected");
    ModkitMenu::add({"Tácticas de combate", &drawMenu, &saveMenu});
}

void Tactics::think(const Situation& s)
{
    if (!g_ready)
    {
        return;
    }
    // The plain attack's damage per second at the target, while no art is cast.
    const float dt = g_lastThink ? static_cast<float>(s.now - g_lastThink) / 1000.0f : 0.0f;
    g_lastThink = s.now;
    std::unordered_map<uint32_t, int> health;
    for (int i = 0; i < s.count; ++i)
    {
        const AimAssist::Foe& f = s.foes[i];
        health[f.id] = f.health;
        if (!g_lastHealth.count(f.id) && f.maxHealth > 0)
        {
            g_seenMaxima.push_back(f.maxHealth);
            if (g_seenMaxima.size() > 64)
            {
                g_seenMaxima.erase(g_seenMaxima.begin());
            }
        }
        if (f.id == s.target && g_phase == Phase::Idle && dt > 0.0f && dt < 1.0f)
        {
            const auto last = g_lastHealth.find(f.id);
            if (last != g_lastHealth.end() && last->second >= f.health)
            {
                const float rate = static_cast<float>(last->second - f.health) / dt;
                g_dps = g_dpsSeconds > 0.0f ? g_dps * 0.9f + rate * 0.1f : rate;
                g_dpsSeconds += dt;
            }
        }
    }
    g_lastHealth = std::move(health);

    if (g_phase != Phase::Idle)
    {
        return;
    }
    if (Config::tactics.arts && s.target)
    {
        chooseArt(s);
    }
    else
    {
        g_artsRecharging = false;
    }
    if (g_phase == Phase::Idle)
    {
        chooseConsumable(s);
    }
}

void Tactics::onFrame(DWORD now)
{
    if (g_phase == Phase::Idle)
    {
        return;
    }
    int health = 0, maximum = 0;
    if (AimAssist::heroHealth(health, maximum))
    {
        g_bestHealth = std::max(g_bestHealth, health);
    }
    const int elapsed = static_cast<int>(now - g_phaseAt);
    if (g_phase == Phase::Selecting)
    {
        if (elapsed < static_cast<int>(kSelectMs))
        {
            return;
        }
        if (!sendArt(g_castTarget))
        {
            reset();
            learned(g_art).readyAt = now + 5000;
            return;
        }
        learned(g_art).lastCast = now;
        g_phase = Phase::Casting;
        g_phaseAt = now;
        return;
    }
    if (elapsed < static_cast<int>(kMeasureMs))
    {
        return;
    }
    const bool seen = measure();
    Learned& l = learned(g_art);
    // A cast with no effect from an art known to have one was likely refused while it recharges: wait longer.
    const bool nothing = !seen && l.heal + l.area + l.single > 0;
    l.readyAt = now + (nothing ? 8000 : jitter(3000, 5000));
    g_nextArtAt = now + jitter(1200, 2200);
    if (g_restoreSlot >= 0 && g_restoreSlot < kSlots && g_restoreSlot != g_slot)
    {
        tap(slotKey(g_restoreSlot));
    }
    g_phase = Phase::Idle;
}

bool Tactics::busy(DWORD now)
{
    return g_phase == Phase::Selecting || (g_phase == Phase::Casting && static_cast<int>(now - g_phaseAt) <
                                                                         static_cast<int>(kBusyMs));
}

void Tactics::reset()
{
    if (g_phase != Phase::Idle && g_restoreSlot >= 0 && g_restoreSlot < kSlots && g_restoreSlot != g_slot)
    {
        tap(slotKey(g_restoreSlot));
    }
    g_phase = Phase::Idle;
}

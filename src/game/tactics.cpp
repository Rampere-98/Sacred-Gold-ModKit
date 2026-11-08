#include "game/tactics.h"
#include "config.h"
#include "config/tactics.h"
#include "game/combat_state.h"
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
        // The recharge, timed from the end of the last cast taken (0: not seen yet). A cast taken after a gap says the
        // recharge is no longer than that; one refused, that it is longer. Gaps over kMaxRechargeMs say nothing (a
        // cast that failed for another reason: the target died, the hero was hit). Each cast taken brings the estimate
        // down a little, so that one too long comes down again until the game refuses a cast.
        DWORD lastEnd = 0;
        DWORD rechargeMs = 0;
        static constexpr DWORD kMaxRechargeMs = 30000;

        // How long after its last cast ended the art is tried again.
        DWORD recharge() const
        {
            return rechargeMs ? rechargeMs : 3000;
        }

        void taken(DWORD now)
        {
            if (lastEnd)
            {
                const DWORD gap = now - lastEnd;
                if (gap <= kMaxRechargeMs)
                {
                    rechargeMs = rechargeMs ? std::min(rechargeMs, gap) : gap;
                    rechargeMs -= rechargeMs / 20;
                }
            }
        }

        void refused(DWORD now)
        {
            if (lastEnd)
            {
                const DWORD gap = now - lastEnd;
                if (gap <= kMaxRechargeMs)
                {
                    rechargeMs = std::max(rechargeMs, std::min<DWORD>(kMaxRechargeMs, gap + 1000));
                }
            }
        }

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

    int g_drainTicks = 0;           // looks in a row the hero lost health with no enemy about
    int g_prevHealth = 0;
    bool g_artsRecharging = false;  // an art was wanted but all were recharging

    // --- The art under way -------------------------------------------------------------------------------------

    enum class Phase
    {
        Idle,
        Selecting,  // the slot key went out; the engine takes the art a frame or two later
        Confirming, // the order went out; does the hero start the art (action 9), or does the game refuse it?
        Casting,    // the art is under way: no other order, its effect is measured until it is done
    };
    Phase g_phase = Phase::Idle;
    DWORD g_phaseAt = 0;
    DWORD g_castAt = 0;             // when the order went out
    DWORD g_doneAt = 0;             // when the hero's action left the art, 0 while it goes on
    int g_slot = -1;
    uint32_t g_art = 0;
    uint32_t g_castTarget = 0;
    bool g_castClose = false;       // the target stood close as the art went out: an art with no effect is one
    bool g_retapped = false;        // the slot key went out a second time
    int g_restoreSlot = -1;         // the slot the player had selected
    int g_healthAtCast = 0, g_bestHealth = 0, g_maxHealth = 0;
    std::unordered_map<uint32_t, int> g_foesAtCast;
    DWORD g_nextArtAt = 0;          // a little time between arts, as a player takes
    DWORD g_refusedAt = 0;          // the game last refused an art (recharging)
    DWORD g_nextPotionAt = 0;
    DWORD g_potionReady[6] = {};    // per type: not before this tick (cool down, or none left)
    int g_potionMisses[6] = {};     // per type: tries in a row that found none

    constexpr DWORD kSelectMs = 120;
    constexpr DWORD kSelectMaxMs = 600;     // the engine has not taken the art by then: given up
    constexpr DWORD kConfirmMs = 1000;      // the hero has not started the art by then: the game refused it
    constexpr DWORD kConfirmWalkMs = 3000;  // ... unless it is walking up to the target first, at most this long
    constexpr DWORD kCastMaxMs = 7000;      // an art lasting longer is not waited for
    constexpr DWORD kSettleMs = 200;        // after the art is done, its last blow's damage shows

    // The hero, also when mounted (the rider casts).
    cCreature* heroCreature()
    {
        cObjectManager* objects = cObjectManager::instance();
        return objects ? objects->hero() : nullptr;
    }

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
        g_retapped = false;
        // AutoPotion waits with drinks that are not urgent until the art is measured (a bound, moved up as it ends).
        CombatState::measuringUntil = s.now + kSelectMaxMs + kConfirmWalkMs + kCastMaxMs;
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

    // What a cast the game took did, from the health of the hero and the enemies before and after (the art's whole
    // time, with no plain blows of the hero's meanwhile); false if nothing was seen.
    bool measure(DWORD now)
    {
        Learned& l = learned(g_art);
        const Art before = l.kind();
        // A healing potion drunk around the cast (it heals over a second): the health regained is the potion's.
        const DWORD potion = CombatState::potionAt.load();
        const bool potionMeanwhile = potion && static_cast<int>(potion - g_castAt) >= -1500 &&
                                     static_cast<int>(now - potion) >= 0;
        const int gain = potionMeanwhile ? 0 : g_bestHealth - g_healthAtCast;
        int hurtOthers = 0;
        bool hurtTarget = false;
        cObjectManager* objects = cObjectManager::instance();
        for (const auto& [id, healthBefore] : g_foesAtCast)
        {
            // Read now: the looks at the situation pause while the art goes on. Gone or dead counts as hurt for the
            // target (the art finished it), not for the others (they may only have left the screen).
            const cCreature* foe = objects ? objects->creature(id) : nullptr;
            const bool dropped = !foe || foe->health <= 0 ? id == g_castTarget : foe->health < healthBefore;
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
        const bool healed = gain >= std::max(8, g_maxHealth * 6 / 100);
        const char* seen;
        if (healed)
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
            seen = potionMeanwhile ? "did nothing seen (a potion was drunk meanwhile)" : "did nothing seen";
        }
        else
        {
            seen = "did nothing seen, the target far";
        }
        // Taken for a heal (once, perhaps, from a potion's healing) but this cast healed nothing: less sure of it, so
        // that what the art does wins in the end.
        if (!healed && !potionMeanwhile && l.heal > 0 && before == Art::Heal)
        {
            --l.heal;
        }
        // Taken for a buff (nothing seen, the art cast at the hero) but it hurt the enemy: no buff.
        if ((hurtTarget || hurtOthers >= 2) && l.none > 0)
        {
            l.none = 0;
        }
        if (l.kind() != before)
        {
            remember(g_art, l.kind());
        }
        LOG("Tactics: art {:x} {} (gain {}, others hurt {}) -> {}", g_art, seen, gain, hurtOthers, artName(l.kind()));
        return gain > 0 || hurtOthers > 0 || hurtTarget;
    }

    // Forgets what was learned of every art (the ModKit menu's button): [Tactics.Learned] goes too.
    void forgetLearned()
    {
        g_learned.clear();
        const std::wstring& ini = ConfigFile::path();
        if (!ini.empty())
        {
            WritePrivateProfileStringW(L"Tactics.Learned", nullptr, nullptr, ini.c_str());
        }
        LOG("Tactics: what the arts do is forgotten, learned again from their next casts");
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
        if (!player || (g_nextArtAt && static_cast<int>(s.now - g_nextArtAt) < 0))
        {
            return;
        }
        // Mounted, the arts are not cast (seen: the order does nothing).
        cEngine* engine = FrameHooks::engine();
        cCreature* hero = heroCreature();
        if (!engine || !hero || engine->controlled != hero->id)
        {
            return;
        }
        const Config::Tactics& c = Config::tactics;
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
        // Arts at the target only once it is near: further away the hero first walks there, and the art goes out late
        // or not at all (the plain attack brings the hero close meanwhile).
        bool inReach = false;
        if (target)
        {
            const float dx = target->x - s.hx, dy = target->y - s.hy;
            const float reach = static_cast<float>(c.artRange);
            inReach = c.artRange <= 0 || dx * dx + dy * dy <= reach * reach;
        }
        const bool aimable = target && inReach;

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
                    rank = 10;
                    reason = "health low";
                }
                break;
            case Art::Area:
                if (aimable && !easy && (around >= 3 || (danger && around >= 2)))
                {
                    rank = 8;
                    at_ = target->id;
                    reason = "several enemies together";
                }
                break;
            case Art::Single:
                if (aimable && !easy && (strong || danger || c.singleAlways))
                {
                    rank = 6;
                    at_ = target->id;
                    reason = strong ? "a strong enemy" : danger ? "hard pressed" : "the main attack";
                }
                break;
            case Art::Buff:
                if ((close >= 3 || strong || danger) && s.threat &&
                    (!learned(sa.art).lastCast ||
                     static_cast<int>(s.now - learned(sa.art).lastCast) > c.buffEverySeconds * 1000))
                {
                    rank = 4;
                    // At the enemy fought when there is one near: a buff still lands on the hero, and an art taken
                    // for a buff by mistake (one that strikes) shows what it does.
                    at_ = aimable ? target->id : 0;
                    reason = "a hard fight starts";
                }
                break;
            case Art::Auto:
                // Not known yet: tried where any art would be worth it, to learn what it does. Never cast yet, before
                // the main attack (else an art put in a slot later waits for the main one's recharge to be tried).
                if (aimable && !easy && (strong || danger || around >= 3 || c.singleAlways))
                {
                    const Learned& l = learned(sa.art);
                    rank = l.heal + l.area + l.single + l.none == 0 && !danger ? 7 : 2;
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
            if ((learned(sa.art).readyAt && static_cast<int>(s.now - learned(sa.art).readyAt) < 0))
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
        // Concentration only for recharges the game showed (a cast refused lately), not for the waits this side keeps.
        g_artsRecharging = wantedButRecharging && pick < 0 && (danger || close >= 3 || strong) &&
                           CombatState::within(g_refusedAt, s.now, 20000);
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
            g_potionMisses[type] = 0;
            LOG("Tactics: drank {} ({})", name, why);
        }
        else
        {
            // None left: a look again in a while, ever later while none turns up (1, 2, 4, ... up to 16 minutes);
            // one bought or found meanwhile is drunk at the next look.
            const int misses = std::min(++g_potionMisses[type], 5);
            const DWORD wait = 60000u << (misses - 1);
            g_potionReady[type] = now + wait;
            if (misses <= 2)
            {
                LOG("Tactics: no {} to drink ({}), next look in {} min", name, why, wait / 60000);
            }
        }
        g_nextPotionAt = now + jitter(900, 1600);
    }

    void chooseConsumable(const Tactics::Situation& s)
    {
        const Config::Tactics& c = Config::tactics;
        if (!c.consumables || (g_nextPotionAt && static_cast<int>(s.now - g_nextPotionAt) < 0))
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
        if (c.healHirelings && potionReady(Healing, s.now) && s.hirelingCount > 0)
        {
            // Hirelings: the creatures the auto attack saw follow the hero.
            cObjectManager* objects = cObjectManager::instance();
            bool hurtHireling = false;
            for (int i = 0; objects && i < s.hirelingCount; ++i)
            {
                const cCreature* h = objects->creature(s.hirelings[i]);
                if (h && h->maxHealth > 0 && h->health > 0 && h->health * 100 < h->maxHealth * 45)
                {
                    hurtHireling = true;
                }
            }
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
        if (ImGui::Button("Olvidar lo aprendido"))
        {
            forgetLearned();
        }
        ImGui::SetItemTooltip("Borra lo que se aprendió de cada arte (curación, área, un objetivo, mejora) y su recarga;\n"
                              "se vuelve a aprender de los próximos lanzamientos.");
        ImGui::Checkbox("Artes de un objetivo como ataque principal", &c.singleAlways);
        ImGui::SetItemTooltip("Contra cualquier enemigo que aguante más de unos golpes, no solo contra los fuertes.\n"
                              "Para personajes que pelean sobre todo con sus artes.");
        ImGui::SetNextItemWidth(200.0f * ImGui::GetFontSize() / 16.0f);
        ImGui::SliderInt("Alcance de las artes (px)", &c.artRange, 0, 1500);
        ImGui::SetItemTooltip("Artes contra el objetivo solo si está a esta distancia o menos (0 = a cualquiera);\n"
                              "más lejos, el ataque normal acerca al héroe primero.");
        ImGui::SetNextItemWidth(200.0f * ImGui::GetFontSize() / 16.0f);
        ImGui::SliderInt("Mejoras: repetir cada (s)", &c.buffEverySeconds, 5, 600);
        ImGui::Checkbox("Retirarse sin pociones", &c.retreat);
        ImGui::SetItemTooltip("Con poca vida, sin pociones de curación y enemigos cerca: el héroe se aparta de ellos\n"
                              "en vez de seguir peleando, hasta recuperarse o perderlos de vista.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f * ImGui::GetFontSize() / 16.0f);
        ImGui::SliderInt("con vida (%)", &c.retreatPercent, 5, 60);
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
        ModkitMenu::write(ini, L"Tactics", L"SingleAlways", c.singleAlways);
        ModkitMenu::write(ini, L"Tactics", L"ArtRange", c.artRange);
        ModkitMenu::write(ini, L"Tactics", L"BuffEvery", c.buffEverySeconds);
        ModkitMenu::write(ini, L"Tactics", L"Retreat", c.retreat);
        ModkitMenu::write(ini, L"Tactics", L"RetreatPercent", c.retreatPercent);
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

namespace
{
    // The art is over (done, refused or given up): the player's slot again, a little time before the next.
    void finishArt(DWORD now)
    {
        g_nextArtAt = now + jitter(1200, 2200);
        if (g_restoreSlot >= 0 && g_restoreSlot < kSlots && g_restoreSlot != g_slot)
        {
            tap(slotKey(g_restoreSlot));
        }
        g_phase = Phase::Idle;
        CombatState::measuringUntil = now;
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
    switch (g_phase)
    {
    case Phase::Selecting:
    {
        if (elapsed < static_cast<int>(kSelectMs))
        {
            return;
        }
        // The engine must hold the art selected: the cast order carries the engine's art, so a key not taken yet
        // would cast the one before (and credit its effect to this one).
        cEngine* engine = FrameHooks::engine();
        const uint16_t engineArt = engine ? at<uint16_t>(engine, kEngineArt) : 0xFFFF;
        if (engineArt != static_cast<uint16_t>(g_art))
        {
            if (!g_retapped && elapsed >= 350)
            {
                tap(slotKey(g_slot));
                g_retapped = true;
            }
            if (elapsed < static_cast<int>(kSelectMaxMs))
            {
                return;
            }
            LOG("Tactics: art {:x} not selected in the engine (it holds {:x}), not cast", g_art, engineArt);
            learned(g_art).readyAt = now + 5000;
            finishArt(now);
            return;
        }
        if (!sendArt(g_castTarget))
        {
            learned(g_art).readyAt = now + 5000;
            finishArt(now);
            return;
        }
        g_castAt = now;
        g_doneAt = 0;
        g_phase = Phase::Confirming;
        g_phaseAt = now;
        return;
    }
    case Phase::Confirming:
    {
        Learned& l = learned(g_art);
        if (CombatState::action(heroCreature()) == CombatState::CastingArt)
        {
            l.taken(now);
            l.lastCast = now;
            g_phase = Phase::Casting;
            g_phaseAt = now;
            return;
        }
        if (elapsed < static_cast<int>(kConfirmMs) ||
            (elapsed < static_cast<int>(kConfirmWalkMs) &&
             CombatState::action(heroCreature()) == CombatState::Walking))
        {
            return;
        }
        // The hero never started it. With the target dead or gone it may have been only that: no lesson.
        cObjectManager* objects = cObjectManager::instance();
        const cCreature* target = objects && g_castTarget ? objects->creature(g_castTarget) : nullptr;
        if (g_castTarget && (!target || target->health <= 0))
        {
            l.readyAt = now + 500;
            LOG("Tactics: art {:x} not cast, its target is gone", g_art);
            finishArt(now);
            return;
        }
        // Otherwise the game refused the art, which still recharges.
        const DWORD since = l.lastEnd ? now - l.lastEnd : 0;
        l.refused(now);
        g_refusedAt = now;
        DWORD ready = l.lastEnd ? l.lastEnd + l.recharge() : now + 3000;
        if (static_cast<int>(ready - (now + 1500)) < 0)
        {
            ready = now + 1500;
        }
        l.readyAt = ready;
        LOG("Tactics: art {:x} refused by the game ({} ms after its last cast ended; recharge now taken as {} ms)",
            g_art, since, l.recharge());
        finishArt(now);
        return;
    }
    case Phase::Casting:
    {
        // Under way while the hero's action says so; its last blow's damage shows a moment after.
        const bool going = CombatState::action(heroCreature()) == CombatState::CastingArt;
        if (going && elapsed < static_cast<int>(kCastMaxMs))
        {
            g_doneAt = 0;
            return;
        }
        if (!g_doneAt)
        {
            g_doneAt = now;
            return;
        }
        if (static_cast<int>(now - g_doneAt) < static_cast<int>(kSettleMs))
        {
            return;
        }
        Learned& l = learned(g_art);
        l.lastEnd = g_doneAt;
        const bool seen = measure(now);
        l.readyAt = g_doneAt + l.recharge();
        if (!seen && l.heal + l.area + l.single > 0)
        {
            // Taken, yet nothing seen from an art known to do something: the target left, or died first.
            LOG("Tactics: art {:x} took {} ms, nothing seen this time", g_art, g_doneAt - g_phaseAt);
        }
        finishArt(now);
        return;
    }
    default:
        return;
    }
}

bool Tactics::busy(DWORD)
{
    // From the slot key until the art is done: any other order would cut it short.
    return g_phase != Phase::Idle;
}

void Tactics::reset()
{
    if (g_phase != Phase::Idle)
    {
        finishArt(GetTickCount());
    }
}

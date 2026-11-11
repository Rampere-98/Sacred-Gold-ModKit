#include "game/tactics.h"
#include "game/hero_profile.h"
#include "game/agent_cursor.h"
#include "game/gear.h"
#include "config.h"
#include "config/tactics.h"
#include "game/combat_learn.h"
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
#include <cstring>
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
    // The game's art tables: moveInfo (ENG 004D1AF0, 0x78-byte entries) and spellInfo (ENG 004D2310, 0x94-byte
    // entries), cdecl (id) -> the art's entry or null.
    using ArtInfo = const uint8_t*(__cdecl*)(uint32_t id);
    uintptr_t g_moveInfo = 0;
    uintptr_t g_spellInfo = 0;

    const Sig::Entry kSigs[] = {
        {"usePotion", "8B 0D ?? ?? ?? ?? 83 EC 08 53 55 56 57 E8 ?? ?? ?? ?? 8B F8 85 FF 0F 84 ?? ?? ?? ?? 8B 47 0C", 0,
            Sig::Take::Match, &g_usePotion},
        {"artValue", "83 EC 08 8B 41 10 8B 54 24 0C 83 F8 09 53 89 4C 24 08", 0, Sig::Take::Match, &g_artValue},
        {"players", "A1 ?? ?? ?? ?? 85 C0 75 ?? 56 68 08 30 02 00", 1, Sig::Take::Abs32, &g_players},
        {"moveInfo", "66 8B 54 24 04 B9 01 00 00 00 B8 ?? ?? ?? ?? 66 39 10 74 ?? 83 C0 78 41", 0, Sig::Take::Match,
            &g_moveInfo},
        {"spellInfo", "66 8B 54 24 04 B9 01 00 00 00 B8 ?? ?? ?? ?? 66 39 10 74 ?? 05 94 00 00 00 41", 0,
            Sig::Take::Match, &g_spellInfo},
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
    // empty, 2 a spell, 3 a weapon art; the engine then holds kind 0 or 1) and the art at +4; the selected slot of each group at +0xD58 (group 1, +0xD5A, the arts).
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

    // How the game aims an art (a spell's byte +0x3C, a weapon art's +0x4C): 2 = at the hero himself (the slot key
    // casts it at the hero's own position, ENG 00548D56), 1 = at a target, others at a place. Weapon arts of the
    // Seraphim: 1, but 3FA/3FB 5; spells: 0F 3, 10 5, 11 14 15 16 2 (also the heal 7), 0D 12 13 1.
    bool selfCast(uint16_t slotKind, uint32_t art)
    {
        const uintptr_t info = slotKind == 2 ? g_spellInfo : slotKind == 3 ? g_moveInfo : 0;
        const uint8_t* entry = info && art < 0x10000 ? reinterpret_cast<ArtInfo>(info)(art) : nullptr;
        return entry && entry[slotKind == 2 ? 0x3C : 0x4C] == 2;
    }

    // Whether an art may be cast on horseback, as the game's order handler decides (Manolito's block U4,
    // 2026-11-10, ENG 00530036): a weapon art's flags at its record +0x74, a spell's at +0x90; flag 2 wants a bow in
    // hand (ENG 0054A2A0 thiscall(creature), u8), otherwise flag 1 forbids it. Combos never (the web: «combos
    // imposibles montado»). Most spells, Hard Hit's variants, the buffs and the bow arts pass; the melee moves do not.
    bool castMounted(uint16_t slotKind, uint32_t art, const cCreature* hero)
    {
        if ((slotKind != 2 && slotKind != 3) || !g_spellInfo || !g_moveInfo)
        {
            return false;   // combos never; the tables unknown: not cast mounted
        }
        // The slot's id to the art's own, as the order handler does (Manolito, U4: ENG 00552370 thiscall(creature,
        // id, 0), ret 8; its first bytes checked, ENG only so far), then either table may allow it.
        static constexpr uint8_t kResolveSig[] = {0x83, 0xEC, 0x08, 0x8B, 0x41, 0x10, 0x8B, 0x54, 0x24, 0x0C, 0x83, 0xF8, 0x09};
        static constexpr uint8_t kHasBowSig[] = {0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x87, 0xD4, 0x01, 0x00, 0x00, 0x85, 0xC0, 0x74, 0x3D};
        static const bool s_known =
            std::memcmp(reinterpret_cast<const void*>(0x552370), kResolveSig, sizeof(kResolveSig)) == 0 &&
            std::memcmp(reinterpret_cast<const void*>(0x54A2A0), kHasBowSig, sizeof(kHasBowSig)) == 0;
        if (!s_known)
        {
            static bool s_told = false;
            if (!s_told)
            {
                s_told = true;
                LOG("Tactics: the mounted art check's exe functions are not where expected: no arts on horseback");
            }
            return false;
        }
        using Resolve = uint32_t(__fastcall*)(const void* creature, void*, uint32_t id, uint8_t zero);
        const uint32_t id = reinterpret_cast<Resolve>(0x552370)(hero, nullptr, art, 0);
        if (id >= 0x10000)
        {
            return false;
        }
        using HasBow = uint8_t(__fastcall*)(const void* creature, void*);
        const auto allowed = [&](const uint8_t* entry, size_t at) {
            if (!entry)
            {
                return false;
            }
            const uint8_t flags = entry[at];
            if ((flags & 2) && !reinterpret_cast<HasBow>(0x54A2A0)(hero, nullptr))
            {
                return false;
            }
            return !(flags & 1);
        };
        return allowed(reinterpret_cast<ArtInfo>(g_moveInfo)(id), 0x74) ||
               allowed(reinterpret_cast<ArtInfo>(g_spellInfo)(id), 0x90);
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
        int unselected = 0;                             // slot keys in a row the game did not take (a locked slot)
        int refusals = 0;                               // casts in a row the game refused
        int tries = 0;                                  // casts taken and measured, whatever they showed
        bool self = false;                              // the game casts it at the hero himself (selfCast)
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
            refusals = 0;
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
            // Nothing seen while a heal would have shown: a buff. Cast at the hero himself it can be nothing else (no
            // strike at a target), so two casts are enough.
            return none >= (self ? 2 : 4) ? Art::Buff : Art::Auto;   // Auto: not known yet
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
        if (ini.empty())
        {
            return;
        }
        wchar_t key[24];
        swprintf_s(key, L"Art%X", art);
        // Not known any more (taken for a heal once, then it healed nothing): the key goes, else the next start brings
        // the mistake back.
        WritePrivateProfileStringW(L"Tactics.Learned", key, kind == Art::Auto ? nullptr : Config::artKey(kind),
                                   ini.c_str());
    }

    // --- The fight, as seen ------------------------------------------------------------------------------------------

    // Enemies' maximum health seen lately: what an ordinary enemy has here.
    std::vector<int> g_seenMaxima;
    std::unordered_map<uint32_t, int> g_lastHealth;     // each enemy's health at the last look
    // The plain attack's damage per second at the target (while no art is cast), a running estimate.
    float g_dps = 0.0f;
    float g_dpsSeconds = 0.0f;
    DWORD g_lastThink = 0;

    int g_drainTicks = 0;           // seconds in a row the hero lost health with no enemy about
    int g_prevHealth = 0;
    DWORD g_drainSampleAt = 0;      // when the health was last sampled for it (once a second, whatever the look rate)
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
    constexpr DWORD kCastMaxMs = 15000;     // an art lasting longer is not waited for (combos: kComboMaxMs)
    constexpr DWORD kSettleMs = 200;        // after the art is done, its last blow's damage shows

    // The hero, also when mounted (the rider casts).
    cCreature* heroCreature()
    {
        cObjectManager* objects = cObjectManager::instance();
        return objects ? objects->hero() : nullptr;
    }

    // The arts the hero has learned (seen live and in the save, 2026-11-08): the hero's stats at cCreature +0x3A8
    // keep a vector (begin +0x102, end +0x106 of the stats) of 22-byte entries: a kind dword (1 a spell, 2 a weapon
    // art), the id word at +4, the level word at +6 and, at +0x12, the seconds the art still recharges (a float, 0
    // when ready; the game's check before a cast, ENG 00580DF0). The recharge depends on the hero's build, so it is
    // read rather than guessed. A slot's kind word maps to it: 3 a weapon art, 2 a spell (ENG 0055FCC0).
    constexpr size_t kLearnedBegin = 0x3A8 + 0x102;
    constexpr size_t kLearnedEnd = 0x3A8 + 0x106;
    constexpr size_t kLearnedSize = 22;
    constexpr size_t kLearnedLeft = 0x12;

    struct HeroArt
    {
        bool found = false;     // the hero has learned it
        uint16_t level = 0;
        float left = 0.0f;      // seconds of recharge still to go
        float total = 0.0f;     // its whole recharge: the longer, the stronger the art (kept for the hard enemies)
    };

    // Combos (slot kind 4, ids 0x200-0x204, the combo master's five; ENG 004D2B30): their recharge is kept in 38-byte
    // records, one per combo, at the pointer at stats +0x10E: a float total at +0x1A and the part recharged so far at
    // +0x22, ready once it reaches the total (ENG 00581A20 / 00581A60).
    constexpr size_t kCombos = 0x3A8 + 0x10E;
    constexpr size_t kComboSize = 38;
    constexpr uint32_t kFirstCombo = 0x200, kLastCombo = 0x203;   // four records (the fifth is not one)
    // A combo under way (cCreature +0x2B4, nonzero ~0.2 s after the order is taken, also while the hero still walks
    // up to the target; back to 0 when it ends or is cut short by another order; seen live by Manolito, 2026-11-09).
    constexpr size_t kComboRunning = 0x2B4;
    // ... but +0x2B4 clears as the last piece goes out: the combo ends when +0x401 clears too (the hero standing,
    // ENG 00542C91). A combo of four with a walk first: ~19 s.
    constexpr size_t kComboPiece = 0x401;
    constexpr DWORD kComboMaxMs = 30000;

    // The hero's entry for the art in a slot; found false when the list cannot be read or does not have the art.
    // The art being cast is a combo and the game runs it (walking to the target counts: its pieces follow).
    bool comboRunning()
    {
        const cCreature* hero = heroCreature();
        return hero && g_art >= kFirstCombo && g_art <= kLastCombo &&
               (at<uint8_t>(hero, kComboRunning) != 0 || at<uint8_t>(hero, kComboPiece) != 0);
    }

    HeroArt heroArt(uint16_t slotKind, uint32_t art)
    {
        HeroArt r;
        const uint32_t kind = slotKind == 3 ? 2 : slotKind == 2 ? 1 : 0;
        const cCreature* hero = heroCreature();
        if (slotKind == 4 && hero && art >= kFirstCombo && art <= kLastCombo)
        {
            const uintptr_t combos = at<uintptr_t>(hero, kCombos);
            if (combos)
            {
                const auto* p = reinterpret_cast<const uint8_t*>(combos + (art - kFirstCombo) * kComboSize);
                const float total = at<float>(p, 0x1A), done = at<float>(p, 0x22);
                // Its arts in the words before (all zero: an empty combo, nothing to cast; seen live, 2026-11-08).
                bool any = false;
                for (size_t o = 2; o < 0x1A; o += 2)
                {
                    any = any || at<uint16_t>(p, o) != 0;
                }
                if (at<uint16_t>(p, 0) == art && any && total > 0.0f && total < 1000.0f)
                {
                    r.found = true;
                    r.left = done < total ? total - done : 0.0f;
                    r.total = total;
                }
            }
            return r;
        }
        if (!kind || !hero)
        {
            return r;
        }
        const uintptr_t begin = at<uintptr_t>(hero, kLearnedBegin);
        const uintptr_t end = at<uintptr_t>(hero, kLearnedEnd);
        if (!begin || end <= begin || (end - begin) % kLearnedSize || (end - begin) / kLearnedSize > 256)
        {
            return r;
        }
        for (uintptr_t e = begin; e < end; e += kLearnedSize)
        {
            const auto* p = reinterpret_cast<const uint8_t*>(e);
            if (at<uint32_t>(p, 0) == kind && at<uint16_t>(p, 4) == static_cast<uint16_t>(art))
            {
                r.found = true;
                r.level = at<uint16_t>(p, 6);
                r.left = at<float>(p, kLearnedLeft);
                r.total = at<float>(p, 0x0A);
                return r;
            }
        }
        return r;
    }

    // Whether the hero's art list could be read at all (else the recharge is guessed as before).
    bool heroArtsReadable()
    {
        const cCreature* hero = heroCreature();
        if (!hero)
        {
            return false;
        }
        const uintptr_t begin = at<uintptr_t>(hero, kLearnedBegin);
        const uintptr_t end = at<uintptr_t>(hero, kLearnedEnd);
        return begin && end > begin && (end - begin) % kLearnedSize == 0 && (end - begin) / kLearnedSize <= 256;
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
        // Riding, the controlled creature is the horse (Manolito AD/AG): the art is the rider's, as a click on a
        // creature goes to the rider (the world mouse handler's orders).
        if (cCreature* hero = objects->hero();
            hero && *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + 0x1EC) != 0)
        {
            self = hero;
        }
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
        CombatState::measuringUntil = s.now + kSelectMaxMs + kConfirmWalkMs +
                                      (art >= kFirstCombo && art <= kLastCombo ? kComboMaxMs : kCastMaxMs);
        // An art the game casts at the hero acts there, wherever the target is.
        g_castClose = !target || learned(art).self;
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
        ++l.tries;
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
        const int healThreshold = std::max(8, g_maxHealth * 6 / 100);
        const bool healed = gain >= healThreshold;
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
        else if (l.self && (hurtTarget || hurtOthers > 0))
        {
            // Cast at the hero himself yet it hurt: it strikes around him.
            ++l.area;
            seen = "hurt around the hero";
        }
        else if (hurtTarget)
        {
            ++l.single;
            seen = "hurt the target";
        }
        else if (potionMeanwhile)
        {
            // A potion's healing hides a heal's: nothing can be told.
            seen = "nothing told (a potion was drunk meanwhile)";
        }
        else if (g_castClose && g_maxHealth - g_healthAtCast < healThreshold)
        {
            // Too little health missing for a heal to show: nothing seen says nothing of a buff either.
            seen = "did nothing seen, at full health";
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

    // What the game's own data says an art does (docs: the art records' aiming and names, read by Manolito,
    // 2026-11-09): a cast at a lone enemy cannot tell an area art from a single one, nor a buff that hurt nearby.
    // What every class's arts do (Manolito's table, docs/AGENT_ADDRESSES.md «Clases del héroe»: the game's aiming
    // byte and names; ids are unique across classes). Summons count as buffs (cast once a hard fight starts); form
    // changes and the teleport are left to the player.
    Art knownUse(uint32_t art)
    {
        switch (art)
        {
        // Area
        case 0x42D: case 0x3FC: case 0xF:                               // seraphim: multi-hit, whirling hit, celestial light
        case 0x421:                                                     // gladiator: multi-hit
        case 0x17: case 0x1A: case 0xB: case 0x23: case 0x10: case 0x1B: case 0x4:  // battle mage
        case 0x3ED: case 0x3EC: case 0x27: case 0x29: case 0x26: case 0x425:        // wood elf
        case 0x429: case 0x419: case 0x41A: case 0x41B: case 0x41C: case 0x41D:     // dark elf: sudden fury, traps
        case 0x431: case 0x40E:                                         // vampiress: multi-hit (both forms)
        case 0x40B: case 0x40C: case 0x40D:                             // vampiress: bats
        case 0x436: case 0x43D: case 0x43E: case 0x43F: case 0x38: case 0x39:      // dwarf
        case 0x44D: case 0x44E: case 0x44F: case 0x3C: case 0x3E: case 0x41: case 0x40: case 0x42: case 0x43: // daemon
            return Art::Area;
        // Buffs and summons
        case 0x11: case 0x14: case 0x15: case 0x16:                     // seraphim
        case 0x3F6: case 0x3F7: case 0x3F8:                             // gladiator: awe, heroic courage, dagger stare
        case 0x2: case 0x18: case 0x1C: case 0x1E: case 0x1F: case 0x20:            // battle mage
        case 0x25: case 0x3:                                            // wood elf: quick as a flash, companion
        case 0x415: case 0x416: case 0x417: case 0x418:                 // dark elf
        case 0x402: case 0x403: case 0x404: case 0x405: case 0x40A:     // vampiress: wolves, time control, dead
        case 0x43A: case 0x441: case 0x3A: case 0x3B:                   // dwarf
        case 0x3F:                                                      // daemon: infernal power
            return Art::Buff;
        // Heals
        case 0x7:                                                       // battle mage: spiritual healing
        case 0x2A:                                                      // wood elf: recuperation
            return Art::Heal;
        // One target
        case 0x42E: case 0x42F: case 0x430: case 0x3F9: case 0x41E: case 0x3FA: case 0x3FB:
        case 0xD: case 0x12: case 0x13:                                 // seraphim
        case 0x423: case 0x422: case 0x424: case 0x3F2: case 0x3F4: case 0x3EB: case 0x3F5:  // gladiator
        case 0x5: case 0x22: case 0x1D: case 0x21: case 0x19:           // battle mage
        case 0x3EE: case 0x3F0: case 0x3EF: case 0x28: case 0x426: case 0x427: case 0x428:   // wood elf
        case 0x42A: case 0x42B: case 0x42C: case 0x412: case 0x413: case 0x414:              // dark elf
        case 0x400: case 0x401: case 0x406: case 0x407: case 0x408: case 0x409:
        case 0x432: case 0x433: case 0x434: case 0x40F: case 0x410: case 0x411:              // vampiress
        case 0x437: case 0x438: case 0x43B: case 0x43C:                 // dwarf
        case 0x443: case 0x444:                                         // daemon
            return Art::Single;
        // Form changes, teleport: the player's call
        // Dwarf's Greed (0x37): more gold from the one it is cast on, no harm; cast every 2 s at an enemy behind a
        // fence, 170 times (seen live, 2026-11-09). Not the agent's.
        case 0x37:
        case 0x1: case 0x24: case 0x3FE: case 0x3FF:
        case 0x446: case 0x449: case 0x44A: case 0x44B: case 0x44C:
            return Art::Off;
        default:
            return Art::Auto;
        }
    }

    // A combo by its pieces (stats +0x10E: 38-byte records, u32 arts at +2): an area one with two area pieces or more.
    Art comboUse(uint32_t combo)
    {
        const cCreature* hero = heroCreature();
        if (!hero || combo < kFirstCombo || combo > kLastCombo)
        {
            return Art::Auto;
        }
        const auto* stats = reinterpret_cast<const uint8_t*>(hero) + 0x3A8;
        const auto* records = *reinterpret_cast<const uint8_t* const*>(stats + 0x10E);
        if (!records)
        {
            return Art::Auto;
        }
        const uint8_t* r = records + (combo - kFirstCombo) * 38;
        int area = 0;
        for (int i = 0; i < 4; ++i)
        {
            area += knownUse(*reinterpret_cast<const uint32_t*>(r + 2 + 4 * i)) == Art::Area;
        }
        return area >= 2 ? Art::Area : Art::Auto;
    }

    Art slotUse(int slot, uint32_t art)
    {
        const Art set = Config::tactics.slots[slot];
        if (set != Art::Auto)
        {
            return set;
        }
        if (const Art known = art >= kFirstCombo && art <= kLastCombo ? comboUse(art) : knownUse(art); known != Art::Auto)
        {
            return known;
        }
        return learned(art).kind();
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
        // Set again below each look: a stale "recharging" would have Concentration drunk (mounted, between arts).
        g_artsRecharging = false;
        const uint8_t* player = playerData();
        if (!player || (g_nextArtAt && static_cast<int>(s.now - g_nextArtAt) < 0))
        {
            return;
        }
        // The player casts an art himself: nothing of ours now (it would cut his short and pass for ours).
        if (CombatState::casting(heroCreature()))
        {
            return;
        }
        // Mounted, the arts go to the rider (sendArt); only those the game allows on horseback.
        cEngine* engine = FrameHooks::engine();
        cCreature* hero = heroCreature();
        const bool riding = hero && *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + 0x1EC) != 0;
        // Half off (the control left on the horse, the hero on foot): AutoAttack mends it first.
        if (!engine || !hero || (engine->controlled != hero->id && !riding))
        {
            return;
        }
        // Mounted, only the arts the game allows there (the player, 2026-11-10: «usa habilidades bloqueadas a caballo
        // saltándose el bloqueo»: the slot keys go past the game's own check). Mounted by the hero's mount field, not
        // the horse on his spot (just off, it stays there; the reviewer, 2026-11-12).
        const bool mounted = riding;
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
        // Hard pressed: low on health, many at the hero, or those striking it would take its health in a few seconds.
        const bool danger = s.health * 100 <= s.maxHealth * 45 || (s.hurt && close >= 3) ||
                            (s.hurt && CombatLearn::secondsToDie() < 4.0f);
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
            strong = (typical > 0 && target->maxHealth * 10 >= typical * 18) || seconds > 8.0f ||
                     CombatLearn::relativeDanger(target->id) >= 1.8f;
            easy = (seconds > 0.0f && seconds < 3.0f) || target->health * 10 < target->maxHealth * 2;
        }
        // The densest group within the art range: an area art goes at its middle enemy, wherever the target is (the
        // most enemies caught in one cast).
        uint32_t cluster = 0;
        int clusterSize = 0;
        {
            const float r = s.range * 0.3f;
            const float reach = c.artRange > 0 ? static_cast<float>(c.artRange) : 1e9f;
            for (int i = 0; i < s.count; ++i)
            {
                const float hx = s.foes[i].x - s.hx, hy = s.foes[i].y - s.hy;
                if (s.foes[i].health <= 0 || hx * hx + hy * hy > reach * reach)
                {
                    continue;
                }
                int n = 0;
                for (int j = 0; j < s.count; ++j)
                {
                    const float dx = s.foes[j].x - s.foes[i].x, dy = s.foes[j].y - s.foes[i].y;
                    if (s.foes[j].health > 0 && dx * dx + dy * dy <= r * r)
                    {
                        ++n;
                    }
                }
                if (n > clusterSize)
                {
                    clusterSize = n;
                    cluster = s.foes[i].id;
                }
            }
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
        float pickTotal = 0.0f;
        const char* why = "";
        bool wantedButRecharging = false, gameRecharging = false;
        const bool readable = heroArtsReadable();
        // What each slot holds and whether it may be cast, logged when the slots change (the player, 2026-11-11: the
        // Vampiress «tiene equipadas habilidades que sí se pueden usar a caballo … no las usa ni a pie», and no Tactics
        // line said why).
        {
            static uint64_t s_seen = 0;
            uint64_t sig = (mounted ? 1 : 0) + 2 * *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + 0x10);
            for (int slot = 0; slot < kSlots; ++slot)
            {
                const SlotArt sa = slotArt(player, slot);
                sig = sig * 1000003u + sa.kind * 65599u + sa.art;
            }
            if (sig != s_seen)
            {
                s_seen = sig;
                std::string list;
                for (int slot = 0; slot < kSlots; ++slot)
                {
                    const SlotArt sa = slotArt(player, slot);
                    if (!sa.kind && !sa.art)
                    {
                        continue;
                    }
                    const bool listed = sa.kind == 2 || sa.kind == 3 || sa.kind == 4;
                    const HeroArt ha = readable ? heroArt(sa.kind, sa.art) : HeroArt{};
                    list += std::format(" | {}: kind {} art {:x} learned {} mounted-ok {} use {}", slot + 1, sa.kind,
                                        sa.art, !readable || !listed || ha.found, castMounted(sa.kind, sa.art, hero),
                                        static_cast<int>(slotUse(slot, sa.art)));
                }
                LOG("Tactics: slots{} (creature type {}){}", mounted ? " (mounted)" : "",
                    *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + 0x10),
                    list.empty() ? " empty" : list);
            }
        }
        for (int slot = 0; slot < kSlots; ++slot)
        {
            const SlotArt sa = slotArt(player, slot);
            if (!sa.kind || !sa.art || (mounted && !castMounted(sa.kind, sa.art, hero)))
            {
                continue;
            }
            // An art the hero has not learned (or an empty combo) is never cast: the game would refuse it.
            const bool listed = sa.kind == 2 || sa.kind == 3 || sa.kind == 4;
            const HeroArt ha = readable ? heroArt(sa.kind, sa.art) : HeroArt{};
            if (readable && listed && !ha.found)
            {
                continue;
            }
            learned(sa.art).self = selfCast(sa.kind, sa.art);
            Art use = slotUse(slot, sa.art);
            // The Vampiress's turn into a vampire (0x3FE) is her fighting form (the player, 2026-11-11: she «tiene
            // equipadas habilidades que sí se pueden usar a caballo … no las usa ni a pie», and 0x3FE/0x3FF were all
            // her slots held): cast as a buff at a hard fight's start, only from the knight's form (creature type
            // hero+0x10 6; the vampire's taken as 7, Manolito's guess, not seen live yet) and at most every 10 min,
            // so a wrong guess cannot flip her back and forth. Turning back (0x3FF) stays the player's.
            if (sa.art == 0x3FE && Config::tactics.slots[slot] == Art::Auto)
            {
                const uint32_t type = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + 0x10);
                const DWORD last = learned(sa.art).lastCast;
                use = type == 6 && (!last || static_cast<int>(s.now - last) > 600000) ? Art::Buff : Art::Off;
            }
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
                if (cluster && (clusterSize >= 3 || (danger && clusterSize >= 2)))
                {
                    rank = 8;
                    // Cast at the hero himself, it strikes around him: worth it only with the group close.
                    if (learned(sa.art).self && close < 2)
                    {
                        rank = 0;
                        break;
                    }
                    at_ = cluster;
                    reason = clusterSize > around ? "a denser group elsewhere" : "several enemies together";
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
                // Before the blows, as a good player does: once a fight is on its way (two near, or more about).
                if ((close >= 2 || s.count >= 3 || strong || danger) && s.threat &&
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
                    // Hurt enough for a heal to show: an art that showed nothing yet is worth a few tries now, else a
                    // heal never cast while hurt is never learned.
                    const bool showsHeal = missing >= std::max(8, s.maxHealth * 6 / 100);
                    const bool unknown = l.heal + l.area + l.single == 0 && l.tries < 4;
                    rank = (l.tries == 0 && !danger) || (showsHeal && unknown) ? 7 : 2;
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
            // The game's own recharge when it can be read; the waits this side keeps (an art not selected, a target
            // gone, the guess when the list cannot be read) on top.
            if (ha.found && ha.left > 0.0f)
            {
                wantedButRecharging = gameRecharging = true;
                continue;
            }
            if ((learned(sa.art).readyAt && static_cast<int>(s.now - learned(sa.art).readyAt) < 0))
            {
                wantedButRecharging = true;
                continue;
            }
            // Two of the same use: against a strong enemy the stronger one (the longer recharge: a combo, a high
            // art), else the quick one, keeping the strong ones ready for when they count.
            const bool better = rank == pickRank && ha.found && (strong || danger ? ha.total > pickTotal
                                                                                  : ha.total < pickTotal);
            if (rank > pickRank || better)
            {
                pick = slot;
                pickRank = rank;
                pickArt = sa.art;
                pickTarget = at_;
                pickTotal = ha.total;
                why = reason;
            }
        }
        // Concentration only for recharges the game showed (a cast refused lately), not for the waits this side keeps.
        g_artsRecharging = wantedButRecharging && pick < 0 && (danger || close >= 3 || strong) &&
                           (gameRecharging || CombatState::within(g_refusedAt, s.now, 20000));
        if (pick >= 0)
        {
            startArt(pick, pickArt, pickTarget, s, why);
        }
    }

    // --- Consumables ---------------------------------------------------------------------------------------------

    bool potionReady(Potion type, DWORD now)
    {
        return !g_potionReady[type] || static_cast<int>(now - g_potionReady[type]) >= 0;
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
            if (s.foes[i].id == s.target && ((typical > 0 && s.foes[i].maxHealth * 10 >= typical * 18) ||
                                             CombatLearn::relativeDanger(s.target) >= 1.8f))
            {
                strong = true;
            }
        }
        // Hard pressed: low on health, many at the hero, or those striking it would take its health in a few seconds.
        const bool danger = s.health * 100 <= s.maxHealth * 45 || (s.hurt && close >= 3) ||
                            (s.hurt && CombatLearn::secondsToDie() < 4.0f);
        const bool bigFight = s.count >= 4 || strong;

        // Poison: health draining with no enemy about, sampled once a second (the looks come at any rate).
        if (s.count > 0)
        {
            g_drainTicks = 0;
            g_prevHealth = 0;
        }
        else if (!g_drainSampleAt || s.now - g_drainSampleAt >= 1000)
        {
            if (g_prevHealth && s.health < g_prevHealth)
            {
                ++g_drainTicks;
            }
            else if (s.health > g_prevHealth)
            {
                g_drainTicks = 0;
            }
            g_prevHealth = s.health;
            g_drainSampleAt = s.now;
        }

        if (c.antidote && g_drainTicks >= 3 && potionReady(Antidote, s.now))
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
        if (c.healHirelings && potionReady(Healing, s.now) && s.hirelingCount > 0)
        {
            // Hirelings: the creatures the auto attack saw follow the hero. Before they are badly hurt (they die fast
            // alone) and before Mentor: a hireling lost costs more than a slower fight.
            cObjectManager* objects = cObjectManager::instance();
            bool hurtHireling = false;
            for (int i = 0; objects && i < s.hirelingCount; ++i)
            {
                const cCreature* h = objects->creature(s.hirelings[i]);
                // Badly hurt only (35 %), and the hero's own kept: the analyst, 2026-11-10, counted 12 potions given
                // to hirelings against 7 bought, the hero then with none (stepping back at 26 of 124).
                // The last potions too once the hero is well (the player, 2026-11-11: A4 let a mercenary die, «tiene
                // que proteger a sus compañeros»; the rule above had kept every potion from 17:17 on, the hero rarely
                // holding 3): at 80 % health or more the hero gives one even when it is the last, and below 20 % the
                // hireling gets it at 60 %.
                const int left = Gear::healingLeft();
                const bool low = h && h->maxHealth > 0 && h->health > 0 && h->health * 100 < h->maxHealth * 35;
                const bool dying = low && h->health * 100 < h->maxHealth * 20;
                if (low && left >= 1 &&
                    ((left >= 3 && s.health * 2 >= s.maxHealth) || s.health * 10 >= s.maxHealth * 8 ||
                     (dying && s.health * 10 >= s.maxHealth * 6)))
                {
                    hurtHireling = true;
                }
            }
            if (hurtHireling)
            {
                tryPotion(Healing, true, s.now, 8000, "a healing potion to the hirelings", "a hireling is hurt");
                return;
            }
        }
        if (c.mentor && bigFight && s.threat && potionReady(Mentor, s.now))
        {
            tryPotion(Mentor, false, s.now, 120000, "Mentor", "a big fight");
            return;
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
            // A slot the hero has not unlocked yet (more come with the levels) is never taken: tried again ever later.
            Learned& l = learned(g_art);
            const int misses = std::min(++l.unselected, 6);
            const DWORD wait = std::min<DWORD>(5000u << (misses - 1), 120000);
            l.readyAt = now + wait;
            LOG("Tactics: art {:x} not selected in the engine (it holds {:x}), not cast; next try in {} s", g_art,
                engineArt, wait / 1000);
            finishArt(now);
            return;
        }
        learned(g_art).unselected = 0;
        // The game's cursor onto the target first (AgentCursor), as the right click of a player's would be.
        if (!AgentCursor::reach({g_castTarget ? g_castTarget : (heroCreature() ? heroCreature()->id : 0u), 0, 0}))
        {
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
        if (CombatState::casting(heroCreature()) || comboRunning())
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
        // Otherwise the game refused the art. With its recharge read from the game, the reason is another (the
        // weapon, the place): tried again later and later, not taken for a recharge (nor a cause for Concentration).
        const int refusals = ++l.refusals;
        const uint8_t* player = playerData();
        const SlotArt sa = player ? slotArt(player, g_slot) : SlotArt{};
        const HeroArt ha = sa.art == g_art && heroArtsReadable() ? heroArt(sa.kind, g_art) : HeroArt{};
        if (ha.found)
        {
            const DWORD wait = ha.left > 0.0f ? static_cast<DWORD>(ha.left * 1000.0f) + 200
                                              : std::min<DWORD>(3000u << std::min(refusals - 1, 6), 120000);
            l.readyAt = now + wait;
            if (ha.left > 0.0f)
            {
                g_refusedAt = now;
            }
            if (refusals <= 3)
            {
                LOG("Tactics: art {:x} refused by the game ({}), next try in {} s", g_art,
                    ha.left > 0.0f ? "recharging" : "not for its recharge", wait / 1000);
            }
            finishArt(now);
            return;
        }
        const DWORD since = l.lastEnd ? now - l.lastEnd : 0;
        l.refused(now);
        if (refusals <= 2)
        {
            g_refusedAt = now;
        }
        DWORD ready = l.lastEnd ? l.lastEnd + l.recharge() : now + 3000;
        if (static_cast<int>(ready - (now + 1500)) < 0)
        {
            ready = now + 1500;
        }
        if (refusals > 2)
        {
            // Refused again and again: something else than its recharge; seldom tried.
            const DWORD later = now + std::min<DWORD>(3000u << std::min(refusals - 1, 6), 120000);
            if (static_cast<int>(later - ready) > 0)
            {
                ready = later;
            }
        }
        l.readyAt = ready;
        if (refusals <= 3)
        {
            LOG("Tactics: art {:x} refused by the game ({} ms after its last cast ended; recharge now taken as {} ms)",
                g_art, since, l.recharge());
        }
        finishArt(now);
        return;
    }
    case Phase::Casting:
    {
        // Under way while the hero's action says so; its last blow's damage shows a moment after.
        bool going = CombatState::casting(heroCreature()) || comboRunning();
        const bool combo = g_art >= kFirstCombo && g_art <= kLastCombo;
        // A combo whose target died: its pieces left would go at the corpse. Not waited for any more; the auto
        // attack's next order (another enemy, the loot) cuts it.
        if (combo && going && g_castTarget)
        {
            cObjectManager* objects = cObjectManager::instance();
            cObject* object = objects ? objects->object(g_castTarget) : nullptr;
            const cCreature* target = object ? object->asCreature() : nullptr;
            if (!target || target->health <= 0)
            {
                if (!g_doneAt)
                {
                    LOG("Tactics: combo {:x}'s target {:x} died after {} ms: the rest not waited for", g_art,
                        g_castTarget, elapsed);
                }
                going = false;
            }
        }
        if (going && elapsed < static_cast<int>(combo ? kComboMaxMs : kCastMaxMs))
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

bool Tactics::vampireForm(unsigned long)
{
    // Manolito, block AH: the vampire form is bit 0x2000 of +0x14 (and the creature type +0x10 7), both set only by
    // setForm 0x557040.
    const cCreature* hero = heroCreature();
    return hero && (*reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + 0x14) & 0x2000) != 0;
}

int Tactics::artsBarredMounted()
{
    const uint8_t* player = playerData();
    const cCreature* hero = heroCreature();
    if (!g_ready || !player || !hero)
    {
        return 0;
    }
    const bool readable = heroArtsReadable();
    int barred = 0;
    for (int slot = 0; slot < kSlots; ++slot)
    {
        const SlotArt sa = slotArt(player, slot);
        if (!sa.kind || !sa.art)
        {
            continue;
        }
        const bool listed = sa.kind == 2 || sa.kind == 3 || sa.kind == 4;
        if (readable && listed && !heroArt(sa.kind, sa.art).found)
        {
            continue;   // not learned: no loss either way
        }
        if (!castMounted(sa.kind, sa.art, hero))
        {
            ++barred;
        }
    }
    return barred;
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
        const DWORD now = GetTickCount();
        if (g_phase == Phase::Casting)
        {
            // Cut short while it went out: it recharges all the same.
            Learned& l = learned(g_art);
            l.lastEnd = now;
            l.readyAt = now + l.recharge();
        }
        finishArt(now);
    }
}

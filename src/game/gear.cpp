#include "mind.h"
#include "hero_profile.h"
#include "game/gear.h"
#include "game/map_memory.h"
#include "game/modkit_menu.h"
#include "input/inject.h"
#include "log.h"
#include "sacred/object.h"
#include "sacred/ui.h"
#include "sig.h"

#include <imgui.h>
#include <algorithm>
#include <vector>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace Sacred;

// Addresses and layouts: docs/ITEMS_RE.md (ENG build; found by their bytes).
namespace
{
    struct Settings
    {
        bool equip = true;
        bool sell = false;
        bool buy = false;
        int keepGoldPercent = 50;   // buying spends at most the rest
        bool trips = true;      // to a merchant when the backpack is full
        bool runes = true;
        bool scrolls = true;
        bool weaponSlots = true;    // fill the weapon slots and fight with the best one against each enemy    // read the scrolls and books of the backpack (the encyclopedia: monsters, the undead)      // read the runes in the backpack (learn the art, or a level more)
    };
    Settings g_settings;

    uintptr_t g_inventoryPtr = 0, g_typeMgrPtr = 0;     // the globals holding cInventory* and the type manager
    uintptr_t g_getEntry = 0, g_findFree = 0, g_putItem = 0, g_removeItem = 0, g_typeSize = 0, g_getInfo = 0,
              g_getValue = 0, g_equip = 0, g_getEquipped = 0, g_canUse = 0, g_slotAccepts = 0, g_price = 0,
              g_addGoldCall = 0, g_useItem = 0, g_mouseClear = 0;
    uintptr_t g_addGold = 0;
    const Sig::Entry kSigs[] = {
        {"cInventory", "8B 35 ?? ?? ?? ?? 85 F6 74 1A 8B CE C7 05 ?? ?? ?? ?? 00 00 00 00 E8 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 83 C4 04 5E C3 90 90 90 90 90 90 90 90 90 8B 44 24 04 66 3D 20 00", 2,
            Sig::Take::Abs32, &g_inventoryPtr},
        {"typeMgr", "8B 0D ?? ?? ?? ?? 55 57 89 5C 24 28", 2, Sig::Take::Abs32, &g_typeMgrPtr},
        {"getEntry", "8B 44 24 04 66 3D 20 00 73 1B 25 FF FF 00 00", 0, Sig::Take::Match, &g_getEntry},
        {"findFree", "8A 44 24 10 53 55 56 57 8B F9 84 C0", 0, Sig::Take::Match, &g_findFree},
        {"putItem", "64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 83 EC 78 53 8B", 0,
            Sig::Take::Match, &g_putItem},
        {"removeItem", "83 EC 08 53 55 56 57 8B F9 33 D2 33 C0", 0, Sig::Take::Match, &g_removeItem},
        {"typeSize", "8B 44 24 04 53 56 ?? ?? ?? ?? 00 57 7D 36", 0, Sig::Take::Match, &g_typeSize},
        {"getInfo", "66 8B 44 24 04 56 66 3D FF FF 0F 85", 0, Sig::Take::Match, &g_getInfo},
        {"getValue", "55 56 8B F1 6A FF E8 ?? ?? ?? ?? 8B E8", 0, Sig::Take::Match, &g_getValue},
        {"equip", "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 ?? ?? ?? ?? 00 00 53 55 8B ?? ?? ?? ?? 00 00 33 DB 56 66 83 FD 12",
            0, Sig::Take::Match, &g_equip},
        {"getEquipped", "8B 44 24 04 56 ?? ?? ?? ?? 00 8D ?? ?? ?? ?? 00 00", 0, Sig::Take::Match, &g_getEquipped},
        {"canUseItem", "8B 44 24 04 56 57 6A 00 68 ?? ?? ?? ?? 8B F9 8B 0D ?? ?? ?? ?? 68 ?? ?? ?? ?? 6A 00 50 E8 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 8B F0 83 C4 14 85 F6 0F 84", 0,
            Sig::Take::Match, &g_canUse},
        {"slotAccepts", "53 56 8B 74 24 10 32 DB 81 FE 00 16 00 00", 0, Sig::Take::Match, &g_slotAccepts},
        {"price", "8B 44 24 04 8B 0D ?? ?? ?? ?? 56 57", 0, Sig::Take::Match, &g_price},
        // addGold (ENG 0057FD80) has a twin of the same bytes; it is taken from a call to it (the horse's healing).
        {"addGoldCall", "E8 ?? ?? ?? ?? 8B 56 0C 8B CB 52 E8 ?? ?? ?? ??", 0, Sig::Take::Match, &g_addGoldCall},
        // cCreature::useItem(id) (ENG 0054AA80): what a right click in the backpack does, also playing alone (it sends
        // the item the use event; a rune then learns its art or raises it, and goes from the backpack once used).
        {"useItem", "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 48 53 8B 5C 24 5C", 0,
            Sig::Take::Match, &g_useItem},
    };
    bool g_ready = false;

    using GetEntry = uint8_t*(__fastcall*)(void* inv, void*, int idx, int idx2);
    using FindFree = uint8_t(__fastcall*)(uint8_t* entry, void*, int w, int h, uint16_t* cell, int zero);
    using PutItem = uint8_t(__fastcall*)(uint8_t* entry, void*, int cell, uint32_t id, int w, int h, int net);
    using RemoveItem = uint8_t(__fastcall*)(uint8_t* entry, void*, uint32_t id, int net);
    using TypeSize = void(__fastcall*)(void* typeMgr, void*, uint32_t type, int* w, int* h);
    using GetInfo = uint8_t*(__fastcall*)(cObject* item, void*, int idx);
    using GetValue = uint32_t(__fastcall*)(cObject* item, void*);
    using Equip = uint32_t(__fastcall*)(cCreature* c, void*, int slot, uint32_t id, int net);
    using GetEquipped = uint32_t(__fastcall*)(cCreature* c, void*, int slot);
    using CanUse = uint8_t(__fastcall*)(cCreature* c, void*, uint32_t id, int classOnly);
    using SlotAccepts = uint8_t(__fastcall*)(void* typeMgr, void*, int slot, uint32_t type);
    using Price = uint32_t(__fastcall*)(void* inv, void*, uint32_t id, int buy);
    using AddGold = void(__fastcall*)(void* stats, void*, int delta);
    using UseItem = uint8_t(__fastcall*)(cCreature* c, void*, uint32_t id);
    using MouseClear = void(__fastcall*)(void* mouse, void*);

    // Runes: cItemUpgrade (vtable ENG 00891D90), types 0x14FF..0x1696. Each tried once: one of another class, or a
    // requirement not met, stays in the backpack and is not tried again.
    constexpr uintptr_t kRuneVtable = 0x891D90;
    // Runes tried, with the hero's level then: one refused (a level requirement) is tried again once the hero has
    // gone up a level (the player, 2026-11-09: runes are to be used as soon as they can be).
    std::unordered_map<uint32_t, int> g_runesTried;

    template <class F>
    F fn(uintptr_t a)
    {
        return reinterpret_cast<F>(a);
    }

    constexpr size_t kStats = 0x3A8;
    constexpr size_t kGold = kStats + 0x46;
    constexpr size_t kItemType = 0x10;
    constexpr size_t kItemFlags = 0x14;
    constexpr uint32_t kQuestItem = 0x800;
    constexpr int kSlots = 18;          // 0..17; 18 is the mount
    constexpr int kFirstStockPage = 0x14, kStockPages = 3;
    constexpr size_t kTrade = 0x23000;  // cInventory: the merchant traded with, 0 for none

    void* inventory() { return g_inventoryPtr ? *reinterpret_cast<void**>(g_inventoryPtr) : nullptr; }
    void* typeMgr() { return g_typeMgrPtr ? *reinterpret_cast<void**>(g_typeMgrPtr) : nullptr; }

    uint8_t* entryOf(int idx)
    {
        void* inv = inventory();
        return inv ? fn<GetEntry>(g_getEntry)(inv, nullptr, idx & 0xFFFF, idx & 0xFFFF) : nullptr;
    }

    // The backpack grid: cells of 12 bytes from +0x18, width/height at +0x115C/+0x115E.
    template <class F>
    void eachItem(uint8_t* entry, F&& f)
    {
        const int w = *reinterpret_cast<uint16_t*>(entry + 0x115C), h = *reinterpret_cast<uint16_t*>(entry + 0x115E);
        if (w <= 0 || h <= 0 || w * h > 0x100)
        {
            return;
        }
        for (int i = 0; i < w * h; ++i)
        {
            const uint8_t* cell = entry + 0x18 + 12 * i;
            const uint32_t id = *reinterpret_cast<const uint32_t*>(cell);
            // Only the item's top-left cell holds its id (+4 bit 0 marks every occupied cell, that one too: seen live).
            if (id)
            {
                f(id, i, static_cast<int>(cell[5]), static_cast<int>(cell[6]));
            }
        }
    }

    struct Worth
    {
        float primary = 0.0f;   // weapon damage (mean of min and max, all kinds) or armor (all kinds)
        float mods = 0.0f;      // its bonus modifiers' strengths, summed
        float score = 0.0f;     // primary + mods: what pieces are compared by
        float value = 0.0f;     // the game's value: the tie-breaker
        int set = -1;           // the set it belongs to (index in the game's set table), -1 none
        bool weapon = false;
    };

    // The game's sets (bin\sets.bin, loaded to typeMgr +0x563244 .. +0x563248): records of 0x70 bytes, the pieces'
    // info ids (the item's info +0x00) at +0x00..+0x24, 0 ending. Seen live: 0x54E in set 6, Lilith's wings in set 7.
    // (The weapon-info record's +0x23 is 0 on set pieces too: not the set id.) Read once.
    int setOf(uint32_t infoId)
    {
        static std::unordered_map<uint32_t, int> sets;
        static bool read = false;
        if (!read)
        {
            const uint8_t* types = static_cast<const uint8_t*>(typeMgr());
            const uint8_t* begin = types ? *reinterpret_cast<const uint8_t* const*>(types + 0x563244) : nullptr;
            const uint8_t* end = types ? *reinterpret_cast<const uint8_t* const*>(types + 0x563248) : nullptr;
            if (begin && end > begin && (end - begin) % 0x70 == 0 && (end - begin) / 0x70 < 1000)
            {
                for (int s = 0; s < (end - begin) / 0x70; ++s)
                {
                    const uint32_t* ids = reinterpret_cast<const uint32_t*>(begin + s * 0x70);
                    for (int i = 0; i < 10 && ids[i] && ids[i] != 0xCCCCCCCC; ++i)
                    {
                        sets.emplace(ids[i], s);
                    }
                }
                read = true;
                LOG("Gear: {} sets, {} set pieces", (end - begin) / 0x70, sets.size());
            }
        }
        const auto it = sets.find(infoId);
        return it == sets.end() ? -1 : it->second;
    }

    Worth worth(uint32_t id)
    {
        Worth w;
        cObjectManager* objects = cObjectManager::instance();
        cObject* item = objects ? objects->object(id) : nullptr;
        if (!item)
        {
            return w;
        }
        if (const uint8_t* info = fn<GetInfo>(g_getInfo)(item, nullptr, -1))
        {
            // The bonus modifiers (seen live): 5 x (u16 id, u16 param) at +0x4A, their strengths u16[5] at +0x6A. What
            // each id does is not known yet: every point counts alike.
            const auto* mods = reinterpret_cast<const uint16_t*>(info + 0x4A);
            const auto* strength = reinterpret_cast<const uint16_t*>(info + 0x6A);
            for (int m = 0; m < 5; ++m)
            {
                if (mods[2 * m])
                {
                    w.mods += strength[m];
                }
            }
            w.set = setOf(*reinterpret_cast<const uint32_t*>(info));
            const auto* lo = reinterpret_cast<const uint16_t*>(info + 0x1C);
            const auto* hi = reinterpret_cast<const uint16_t*>(info + 0x24);
            const auto* armor = reinterpret_cast<const int16_t*>(info + 0x32);
            float damage = 0.0f, defense = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                damage += (lo[k] + hi[k]) * 0.5f;
                defense += std::max<int16_t>(armor[k], 0);
            }
            w.weapon = damage > 0.0f;
            w.primary = w.weapon ? damage : defense;
            w.score = w.primary + w.mods;
        }
        w.value = static_cast<float>(fn<GetValue>(g_getValue)(item, nullptr));
        return w;
    }

    int gradeOf(uint32_t id, bool equipment)
    {
        cObjectManager* objects = cObjectManager::instance();
        cObject* item = objects ? objects->object(id) : nullptr;
        const uint8_t* info = item && g_getInfo ? fn<GetInfo>(g_getInfo)(item, nullptr, -1) : nullptr;
        if (!info)
        {
            return -1;
        }
        const auto* lo = reinterpret_cast<const uint16_t*>(info + 0x1C);
        const auto* hi = reinterpret_cast<const uint16_t*>(info + 0x24);
        const auto* armor = reinterpret_cast<const int16_t*>(info + 0x32);
        bool gear = false;
        for (int k = 0; k < 4; ++k)
        {
            gear = gear || lo[k] || hi[k] || armor[k] > 0;
        }
        if (!gear && !equipment)
        {
            return -1;
        }
        if (setOf(*reinterpret_cast<const uint32_t*>(info)) >= 0)
        {
            return 4;
        }
        const int nibble = info[6] & 0xF;
        return nibble <= 4 ? 0 : nibble <= 0xB ? 1 : nibble <= 0xE ? 2 : 3;
    }

    // Pieces of each set the hero wears (by set index).
    std::unordered_map<int, int> setsWorn(cCreature* hero)
    {
        std::unordered_map<int, int> worn;
        for (int slot = 0; slot < kSlots; ++slot)
        {
            if (const uint32_t id = fn<GetEquipped>(g_getEquipped)(hero, nullptr, slot))
            {
                if (const int set = worth(id).set; set >= 0)
                {
                    ++worn[set];
                }
            }
        }
        return worn;
    }

    // A set piece's score with what wearing it adds to its set: the set's bonuses come at piece counts, so a piece
    // joining pieces worn counts a tenth more for each of them (up to half again).
    float scoreWith(const Worth& w, const std::unordered_map<int, int>& worn, bool wornNow)
    {
        if (w.set < 0)
        {
            return w.score;
        }
        const auto it = worn.find(w.set);
        const int others = (it == worn.end() ? 0 : it->second) - (wornNow ? 1 : 0);
        return w.score * (1.0f + std::min(0.5f, 0.1f * std::max(others, 0)));
    }

    // `candidate` clearly better than `current` (0: an empty slot): by score (damage or armor plus its bonuses, set
    // pieces counting more with others of their set worn), over `margin` (a change for less only makes the gear
    // swing back and forth), the game's value breaking ties. A piece of a set the hero wears two or more of is kept
    // unless the candidate is of the same set.
    bool better(const Worth& candidate, uint32_t current, float margin, const std::unordered_map<int, int>& worn)
    {
        if (!current)
        {
            return true;
        }
        const Worth now = worth(current);
        if (candidate.weapon != now.weapon)
        {
            return false;       // a shield for a weapon or the other way round: the player's choice
        }
        if (now.set >= 0 && candidate.set != now.set)
        {
            const auto it = worn.find(now.set);
            if (it != worn.end() && it->second >= 2)
            {
                return false;
            }
        }
        const float mine = scoreWith(candidate, worn, false), theirs = scoreWith(now, worn, true);
        if (mine > theirs * (1.0f + margin) + 0.5f)
        {
            return true;
        }
        const bool same = mine >= theirs * (1.0f - margin * 0.5f);
        return same && candidate.value > now.value * (1.0f + 2.0f * margin) + 1.0f;
    }

    uint32_t typeOf(uint32_t id)
    {
        cObjectManager* objects = cObjectManager::instance();
        cObject* o = objects ? objects->object(id) : nullptr;
        return o ? *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(o) + kItemType) : 0;
    }

    uint32_t flagsOf(uint32_t id)
    {
        cObjectManager* objects = cObjectManager::instance();
        cObject* o = objects ? objects->object(id) : nullptr;
        return o ? *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(o) + kItemFlags) : 0;
    }

    // The slot `id` would best go to, and what is there now; -1 if it is better nowhere. Of the rings and amulets,
    // the weakest worn.
    int bestSlot(cCreature* hero, uint32_t id, float margin, uint32_t* replaced = nullptr, bool* fits = nullptr)
    {
        const uint32_t type = typeOf(id);
        void* types = typeMgr();
        if (!type || !types)
        {
            return -1;
        }
        const Worth w = worth(id);
        const std::unordered_map<int, int> worn = setsWorn(hero);
        int best = -1;
        float bestGain = 0.0f;
        for (int slot = 0; slot < kSlots; ++slot)
        {
            if (!fn<SlotAccepts>(g_slotAccepts)(types, nullptr, slot, type))
            {
                continue;
            }
            if (fits)
            {
                *fits = true;
            }
            const uint32_t current = fn<GetEquipped>(g_getEquipped)(hero, nullptr, slot);
            // The weapon hands (12, 13) go with the taskbar's weapon sets: an empty one is the player's choice.
            if (!current && (slot == 12 || slot == 13))
            {
                continue;
            }
            if (!better(w, current, margin, worn))
            {
                continue;
            }
            const Worth now = current ? worth(current) : Worth{};
            const float gain = current ? (scoreWith(w, worn, false) - scoreWith(now, worn, true)) +
                                             (w.value - now.value) * 0.01f
                                       : 1e9f;
            if (best < 0 || gain > bestGain)
            {
                best = slot;
                bestGain = gain;
                if (replaced)
                {
                    *replaced = current;
                }
            }
        }
        return best;
    }

    bool sizeOf(uint32_t id, int& w, int& h)
    {
        void* types = typeMgr();
        const uint32_t type = typeOf(id);
        if (!types || !type)
        {
            return false;
        }
        w = h = 0;
        fn<TypeSize>(g_typeSize)(types, nullptr, type, &w, &h);
        return w > 0 && h > 0;
    }

    // One piece put on: out of the backpack, onto the hero, what it wore into the backpack. False if nothing to do.
    bool equipOne(cCreature* hero, uint8_t* pack)
    {
        bool done = false;
        eachItem(pack, [&](uint32_t id, int cell, int w, int h) {
            if (done || (flagsOf(id) & kQuestItem) || !fn<CanUse>(g_canUse)(hero, nullptr, id, 0))
            {
                return;
            }
            uint32_t old = 0;
            const int slot = bestSlot(hero, id, 0.05f, &old);
            if (slot < 0)
            {
                return;
            }
            int ow = 0, oh = 0;
            if (old && !sizeOf(old, ow, oh))
            {
                return;
            }
            fn<RemoveItem>(g_removeItem)(pack, nullptr, id, 1);
            uint16_t free = 0;
            if (old && !fn<FindFree>(g_findFree)(pack, nullptr, ow, oh, &free, 0))
            {
                fn<PutItem>(g_putItem)(pack, nullptr, cell, id, w, h, 1);     // no room for the old one: as it was
                return;
            }
            const uint32_t previous = fn<Equip>(g_equip)(hero, nullptr, slot, id, 1);
            if (old && previous)
            {
                fn<PutItem>(g_putItem)(pack, nullptr, free, previous, ow, oh, 1);
            }
            LOG("Gear: put on {:x} (type {:x}) in slot {}{}", id, typeOf(id), slot, old ? ", the old one to the backpack" : "");
            done = true;
        });
        return done;
    }

    std::atomic<uint32_t> g_socketBase{0};     // the item kept for the smith while a socketing is wanted
    bool socketable(uint32_t id);

    // At a merchant: one item sold (worse than the worn one for every slot it fits, or not for this class).
    bool sellOne(cCreature* hero, uint8_t* pack)
    {
        void* inv = inventory();
        bool done = false;
        eachItem(pack, [&](uint32_t id, int cell, int w, int h) {
            if (done || (flagsOf(id) & kQuestItem))
            {
                return;
            }
            // Not the item on its way to the smith (the live verifier, 2026-11-10: «589 has 1 free sockets … off to
            // a smith», then «sold 589» at the merchant on the way).
            // Nor the gems and runes it is to take (the code reviewer).
            if (id == g_socketBase.load() || (g_socketBase.load() && socketable(id)))
            {
                return;
            }
            // Runes are never sold (the player, 2026-11-09): another class's rune is traded at the combo master for one
            // of the hero's own, worth far more than the 2 gold a merchant gives (seen live: sold 0x1501, 0x1527, 0x1694
            // for 2 gold). By their class only: the torches 0x1600/0x1601 lie in the runes' type range.
            if (cObjectManager* objects = cObjectManager::instance())
            {
                const auto* o = reinterpret_cast<const uint8_t*>(objects->object(id));
                if (o && *reinterpret_cast<const uintptr_t*>(o) == kRuneVtable)
                {
                    return;
                }
            }
            bool fits = false;
            const bool classOk = fn<CanUse>(g_canUse)(hero, nullptr, id, 1) != 0;
            const int slot = classOk ? bestSlot(hero, id, 0.0f, nullptr, &fits) : -1;
            if (classOk && (!fits || slot >= 0))
            {
                return;     // potions, runes, gems...; or an upgrade (perhaps once the hero is stronger)
            }
            if (const int set = worth(id).set; set >= 0 && setsWorn(hero).count(set))
            {
                return;     // a piece of a set the hero wears: kept for the set
            }
            for (int page = 0; page < kStockPages; ++page)
            {
                uint8_t* stock = entryOf(kFirstStockPage + page);
                uint16_t free = 0;
                if (!stock || !fn<FindFree>(g_findFree)(stock, nullptr, w, h, &free, 0))
                {
                    continue;
                }
                const uint32_t price = fn<Price>(g_price)(inv, nullptr, id, 0);
                fn<RemoveItem>(g_removeItem)(pack, nullptr, id, 1);
                fn<PutItem>(g_putItem)(stock, nullptr, free, id, w, h, 1);
                fn<AddGold>(g_addGold)(reinterpret_cast<uint8_t*>(hero) + kStats, nullptr, static_cast<int>(price));
                LOG("Gear: sold {:x} (type {:x}) for {} gold{}", id, typeOf(id), price, classOk ? "" : " (not for this class)");
                done = true;
                return;
            }
        });
        (void)hero;
        return done;
    }

    // Healing potions (ITEM_FUNCTIONS.md: 0x1433 «Schwacher Heiltrank», 0x1419, 0x1414): how many the backpack holds,
    // and at a merchant one bought while under 5 (the analyst, 2026-11-10: out of them mid-fight at 59 of 158).
    bool healingType(uint32_t type)
    {
        return type == 0x1433 || type == 0x1419 || type == 0x1414;
    }
    int g_healing = -1;     // in the backpack at the last look (-1: not counted yet)
    // Healing potions kept, by the class's mind (8 for the melee classes, 5 for the Mage: docs/Mentalidades.md).
    int wantHealing()
    {
        return Mind::now().potionStock;
    }

    int countHealing(uint8_t* pack)
    {
        int n = 0;
        eachItem(pack, [&](uint32_t id, int, int, int) { n += healingType(typeOf(id)) ? 1 : 0; });
        return n;
    }

    bool buyPotion(cCreature* hero, uint8_t* pack)
    {
        if (countHealing(pack) >= wantHealing())
        {
            return false;
        }
        void* inv = inventory();
        const uint32_t gold = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + kGold);
        // The gold at the visit's start (no potion bought for a minute: a new visit): each potion at most a quarter of
        // it, and the potions together at most 60 % of it (the analyst, 2026-11-10: a quarter of the gold left took
        // one potion a visit).
        static uint32_t s_visitGold = 0;
        static DWORD s_lastBuy = 0;
        const DWORD now = GetTickCount();
        if (!s_lastBuy || now - s_lastBuy > 60000 || gold > s_visitGold)
        {
            s_visitGold = gold;
        }
        if (gold < s_visitGold * 2 / 5)
        {
            return false;
        }
        bool done = false;
        // The cheapest healing potion on sale first (more of them for the gold; the analyst, 2026-11-10: 3-5 a visit
        // at 585 each did not last the melee classes).
        uint32_t cheapest = 0xFFFFFFFFu;
        for (int page = 0; page < kStockPages; ++page)
        {
            if (uint8_t* stock = entryOf(kFirstStockPage + page))
            {
                eachItem(stock, [&](uint32_t id, int, int, int) {
                    if (healingType(typeOf(id)))
                    {
                        cheapest = std::min(cheapest, fn<Price>(g_price)(inv, nullptr, id, 1));
                    }
                });
            }
        }
        for (int page = 0; page < kStockPages && !done; ++page)
        {
            uint8_t* stock = entryOf(kFirstStockPage + page);
            if (!stock)
            {
                continue;
            }
            eachItem(stock, [&](uint32_t id, int, int w, int h) {
                if (done || !healingType(typeOf(id)))
                {
                    return;
                }
                const uint32_t price = fn<Price>(g_price)(inv, nullptr, id, 1);
                uint16_t free = 0;
                // Potions before the gold kept back: at most a quarter of the visit's gold on one, 40 % of it kept.
                if (price > cheapest || price > s_visitGold / 4 || gold - price < s_visitGold * 2 / 5 ||
                    !fn<FindFree>(g_findFree)(pack, nullptr, w, h, &free, 0))
                {
                    return;
                }
                cObject* item = cObjectManager::instance()->object(id);
                fn<RemoveItem>(g_removeItem)(stock, nullptr, id, 1);
                fn<AddGold>(g_addGold)(reinterpret_cast<uint8_t*>(hero) + kStats, nullptr, -static_cast<int>(price));
                if (uint8_t* info = item ? fn<GetInfo>(g_getInfo)(item, nullptr, -1) : nullptr)
                {
                    *reinterpret_cast<uint32_t*>(info + 4) &= ~0x200000u;
                }
                fn<PutItem>(g_putItem)(pack, nullptr, free, id, w, h, 1);
                LOG("Gear: bought healing potion {:x} (type {:x}) for {} gold (of {}); potions: have {}, want {}", id,
                    typeOf(id), price, gold, countHealing(pack), wantHealing());
                done = true;
                s_lastBuy = now;
            });
        }
        return done;
    }

    // At a merchant: one item bought that would be clearly better than what the hero wears, within the gold to spend.
    bool buyOne(cCreature* hero, uint8_t* pack)
    {
        void* inv = inventory();
        const uint32_t gold = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + kGold);
        const uint32_t budget = static_cast<uint32_t>(gold * static_cast<uint64_t>(100 - g_settings.keepGoldPercent) / 100);
        // The cheapest upgrade seen over budget, told once per merchant (what it would buy with more gold).
        static uint32_t toldFor = 0;
        uint32_t cheapest = 0, cheapestPrice = 0;
        for (int page = 0; page < kStockPages; ++page)
        {
            uint8_t* stock = entryOf(kFirstStockPage + page);
            if (!stock)
            {
                continue;
            }
            bool done = false;
            eachItem(stock, [&](uint32_t id, int, int w, int h) {
                if (done || !fn<CanUse>(g_canUse)(hero, nullptr, id, 0) || bestSlot(hero, id, 0.15f) < 0)
                {
                    return;
                }
                const uint32_t price = fn<Price>(g_price)(inv, nullptr, id, 1);
                uint16_t free = 0;
                if (price > budget)
                {
                    if (!cheapest || price < cheapestPrice)
                    {
                        cheapest = id;
                        cheapestPrice = price;
                    }
                    return;
                }
                if (!fn<FindFree>(g_findFree)(pack, nullptr, w, h, &free, 0))
                {
                    return;
                }
                // As the shop's buy (ENG 006D9840) does it.
                cObject* item = cObjectManager::instance()->object(id);
                fn<RemoveItem>(g_removeItem)(stock, nullptr, id, 1);
                fn<AddGold>(g_addGold)(reinterpret_cast<uint8_t*>(hero) + kStats, nullptr, -static_cast<int>(price));
                if (uint8_t* info = item ? fn<GetInfo>(g_getInfo)(item, nullptr, -1) : nullptr)
                {
                    *reinterpret_cast<uint32_t*>(info + 4) &= ~0x200000u;
                }
                fn<PutItem>(g_putItem)(pack, nullptr, free, id, w, h, 1);
                LOG("Gear: bought {:x} (type {:x}) for {} gold (of {})", id, typeOf(id), price, gold);
                done = true;
            });
            if (done)
            {
                return true;
            }
        }
        const uint32_t merchant = *reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(inv) + kTrade);
        if (cheapest && toldFor != merchant)
        {
            toldFor = merchant;
            LOG("Gear: the cheapest upgrade here, {:x} (type {:x}), costs {} gold; {} to spend of {}", cheapest,
                typeOf(cheapest), cheapestPrice, budget, gold);
        }
        return false;
    }

    bool readRune(cCreature* hero, uint8_t* pack)
    {
        const int heroLevel = *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(hero) + kStats + 0x56);
        cObjectManager* objects = cObjectManager::instance();
        uint32_t rune = 0;
        eachItem(pack, [&](uint32_t id, int, int, int) {
            cObject* item = rune || !objects ? nullptr : objects->object(id);
            const auto* o = reinterpret_cast<const uint8_t*>(item);
            const uint32_t type = o ? *reinterpret_cast<const uint32_t*>(o + kItemType) & 0xFFFF : 0;
            const auto tried = g_runesTried.find(id);
            // By its class (the type range alone holds the torches 0x1600/0x1601 too, cWeapon3D: ITEM_FUNCTIONS).
            if (o && *reinterpret_cast<const uintptr_t*>(o) == kRuneVtable && type >= 0x14FF && type <= 0x1696 &&
                (tried == g_runesTried.end() || tried->second < heroLevel))
            {
                rune = id;
            }
        });
        if (!rune)
        {
            return false;
        }
        g_runesTried[rune] = heroLevel;
        const bool used = fn<UseItem>(g_useItem)(hero, nullptr, rune) != 0;
        LOG("Gear: rune {:x} (type {:x}) read: {}", rune, typeOf(rune), used ? "done" : "refused (class or requirement)");
        return true;
    }

    // The scrolls in the backpack (cScroll3D, by RTTI): read once each with the use that potions take (Manolito,
    // 2026-11-09: useItem 0x54AA80 accepts their category 0x10; most are books for the encyclopedia, of monsters and
    // the undead among them, which the player wants the agent to know). One a call, while calm.
    std::unordered_set<uint32_t> g_scrollsTried;

    bool isScroll(const cObject* item)
    {
        struct TypeDescriptor
        {
            const void* vtable;
            void* spare;
            char name[1];
        };
        struct Locator
        {
            uint32_t signature, offset, cdOffset;
            const TypeDescriptor* type;
        };
        const auto* locator = reinterpret_cast<const Locator* const*>(*reinterpret_cast<const void* const*>(item))[-1];
        return locator && locator->type && std::strcmp(locator->type->name, ".?AVcScroll3D@@") == 0;
    }

    bool readScroll(cCreature* hero, uint8_t* pack)
    {
        cObjectManager* objects = cObjectManager::instance();
        uint32_t scroll = 0;
        eachItem(pack, [&](uint32_t id, int, int, int) {
            cObject* item = scroll || !objects || g_scrollsTried.count(id) ? nullptr : objects->object(id);
            if (item && isScroll(item))
            {
                scroll = id;
            }
        });
        if (!scroll)
        {
            return false;
        }
        g_scrollsTried.insert(scroll);
        const bool used = fn<UseItem>(g_useItem)(hero, nullptr, scroll) != 0;
        LOG("Gear: scroll {:x} (type {:x}) read: {}", scroll, typeOf(scroll), used ? "done" : "refused");
        return true;
    }

    bool g_calm = false;
    std::atomic<bool> g_working{false};

    // On the window's thread, where the game's own windows do the same.
    // A merchant trip (the backpack full, [Gear] Trips): the agent walks the hero there and opens the shop; meanwhile
    // selling is on whatever the menu says, and once nothing more is sold or bought the shop is closed.
    std::atomic<bool> g_full{false};
    std::atomic<bool> g_tripShop{false};    // the shop open now is the trip's
    std::atomic<bool> g_tripDone{false};    // the trip's shop has nothing more: to be closed
    int g_idleWorks = 0;
    std::atomic<DWORD> g_noTripUntil{0};    // full with nothing to sell: no trip again for a while

    // Full: no room left for a 2x2 piece (a helmet, gloves; armor needs more).
    bool packFull(uint8_t* pack)
    {
        uint16_t free = 0;
        return !fn<FindFree>(g_findFree)(pack, nullptr, 2, 2, &free, 0);
    }

    // An item left in the cursor's hand (seen live, 2026-11-09: a healing potion picked up while the inventory was
    // open stayed there): into the backpack once it has been held 3 s (a drag of the player's is quicker). The hand:
    // cMouse at [0xCDDB5C], +0x68 the held entry (+0 u16 kind, 1 an item; +4 its id), Manolito's catalogue «Objeto en el
    // cursor». Put into the backpack first, then the hand emptied (cMouse::clear keeps the item).
    void stashHeld(uint8_t* pack)
    {
        static uint32_t s_heldId = 0;
        static DWORD s_heldSince = 0;
        uint8_t* mouse = g_mouseClear ? *reinterpret_cast<uint8_t**>(0xCDDB5C) : nullptr;
        const uint8_t* held = mouse ? *reinterpret_cast<uint8_t* const*>(mouse + 0x68) : nullptr;
        // +0 is a u16 kind (1 an item); the u16 after it is something else (0x19 seen live with a glaive held).
        const uint32_t id = held && *reinterpret_cast<const uint16_t*>(held) == 1 ? *reinterpret_cast<const uint32_t*>(held + 4) : 0;
        const DWORD now = GetTickCount();
        if (!id || id != s_heldId)
        {
            s_heldId = id;
            s_heldSince = now;
            return;
        }
        if (now - s_heldSince < 3000 || !cObjectManager::instance()->object(id))
        {
            return;
        }
        int w = 0, h = 0;
        uint16_t free = 0;
        if (!sizeOf(id, w, h) || !fn<FindFree>(g_findFree)(pack, nullptr, w, h, &free, 0))
        {
            static uint32_t s_told = 0;
            if (s_told != id)
            {
                s_told = id;
                LOG("Gear: {:x} (type {:x}) held on the cursor: no room in the backpack", id, typeOf(id));
            }
            return;
        }
        fn<PutItem>(g_putItem)(pack, nullptr, free, id, w, h, 1);
        fn<MouseClear>(g_mouseClear)(mouse, nullptr);
        LOG("Gear: {:x} (type {:x}) held on the cursor {} s: put into the backpack", id, typeOf(id),
            (now - s_heldSince) / 1000);
        s_heldId = 0;
    }

    // --- The weapon slots (the player's wish, 2026-11-10: each slot a build, the agent fights with the best one) ------
    // Manolito, AGENT_ADDRESSES «Ranuras de armas (bloque H)»: in the hero's inventory entry (the backpack's), 5 slots x
    // 2 hands x 12 bytes at +0xC18 (hand 0 = equipment 12: shield, bow, second weapon; hand 1 = 13: the main weapon),
    // the active slot u16 at +0xD58, the slots open by level (1: 1, 2-7: 2, 8-15: 3, 16-29: 4, 30+: 5). The taskbar
    // copies equipment 12/13 into the active slot every frame. selectWeaponSlot (the keys 1-5) thiscall(taskbar, slot,
    // 1); setSlotHand thiscall(entry, slot, hand, id, net). Weapon damage by kind (physical, fire, magic, poison):
    // info +0x1C.. min, +0x24.. max; the hero's multiplier of kind k = st+0x96+4k + st+0xA6+4k; an enemy's resistance
    // = st+0x66+4k x st+0xD6+4k; through it gets dealt^2 / (dealt + resistance) (the manual's formula).
    uintptr_t g_selectSlot = 0, g_setSlotHand = 0;
    using SelectSlot = void(__fastcall*)(void* taskbar, void*, uint32_t slot, uint32_t flag);
    using SetSlotHand = uint8_t(__fastcall*)(uint8_t* entry, void*, uint32_t slot, uint32_t hand, uint32_t id, uint32_t net);
    constexpr size_t kSlotHands = 0xC18, kActiveSlot = 0xD58;
    constexpr size_t kHeroLevel = 0x3FE;
    bool g_slotsReady = false;

    int slotsOpen(const cCreature* hero)
    {
        const int level = *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(hero) + kHeroLevel);
        return level >= 30 ? 5 : level >= 16 ? 4 : level >= 8 ? 3 : level >= 2 ? 2 : 1;
    }

    uint32_t slotItem(const uint8_t* entry, int slot, int hand)
    {
        return *reinterpret_cast<const uint32_t*>(entry + kSlotHands + slot * 0x18 + hand * 0xC);
    }

    // A weapon's mean damage of each kind (physical, fire, magic, poison); false if it is no weapon.
    bool damageOf(uint32_t id, float out[4])
    {
        cObjectManager* objects = cObjectManager::instance();
        cObject* item = objects && id ? objects->object(id) : nullptr;
        const uint8_t* info = item ? fn<GetInfo>(g_getInfo)(item, nullptr, -1) : nullptr;
        if (!info)
        {
            return false;
        }
        const auto* lo = reinterpret_cast<const uint16_t*>(info + 0x1C);
        const auto* hi = reinterpret_cast<const uint16_t*>(info + 0x24);
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k)
        {
            out[k] = (lo[k] + hi[k]) * 0.5f;
            sum += out[k];
        }
        return sum > 0.0f;
    }

    // The kind a weapon deals most of (0 physical, 1 fire, 2 magic, 3 poison); -1 none.
    int mainKind(uint32_t id)
    {
        float d[4] = {};
        if (!damageOf(id, d))
        {
            return -1;
        }
        int best = 0;
        for (int k = 1; k < 4; ++k)
        {
            best = d[k] > d[best] ? k : best;
        }
        return best;
    }

    const char* kindName(int k)
    {
        static const char* const names[] = {"physical", "fire", "magic", "poison"};
        return k >= 0 && k < 4 ? names[k] : "none";
    }

    float resistanceOf(const cCreature* c, int k)
    {
        const uint8_t* st = reinterpret_cast<const uint8_t*>(c) + kStats;
        const float r = *reinterpret_cast<const float*>(st + 0x66 + 4 * k) * *reinterpret_cast<const float*>(st + 0xD6 + 4 * k);
        return r > 0.0f && r < 100000.0f ? r : 0.0f;
    }

    // A slot's expected damage against `target` (null: plain, no resistances), with the hero's multipliers.
    // Each weapon's panel damage over its bare estimate, learned while it is the active one (the skills' bonuses for
    // its kind, the bow's for the Wood Elf): the others are judged by their estimate times theirs (or the mean of
    // those known). Without it the estimate ran 1.5 times the panel and the slots swapped every 5-10 s (the analyst,
    // 2026-11-10: 11 switches in 8 minutes, the active slot 0.67 of itself idle).
    // Still the ping-pong (the analyst, 2026-11-10 17:14: the Dwarf 11 switches in 4 min, the active slot 0.5-0.64 of
    // itself idle): the panel lags a switch, so a ratio learned then was the other weapon's. Now learned only once
    // the panel has settled (3 s after a switch), and every slot, the active one too, judged the same way (its
    // estimate times its ratio), so none looks better only for being idle.
    std::unordered_map<uint32_t, float> g_panelRatio;
    DWORD g_slotSwitchAt = 0;

    // A slot's damage from its weapons alone (multipliers by kind, through the target's resistances).
    float estimateDamage(const cCreature* hero, const uint8_t* entry, int slot, const cCreature* target)
    {
        const uint8_t* st = reinterpret_cast<const uint8_t*>(hero) + kStats;
        float total = 0.0f;
        for (int hand = 0; hand < 2; ++hand)
        {
            float d[4] = {};
            if (!damageOf(slotItem(entry, slot, hand), d))
            {
                continue;
            }
            for (int k = 0; k < 4; ++k)
            {
                float mult = *reinterpret_cast<const float*>(st + 0x96 + 4 * k) +
                             *reinterpret_cast<const float*>(st + 0xA6 + 4 * k);
                if (!(mult > 0.1f && mult < 20.0f))
                {
                    mult = 1.0f;
                }
                // What gets through a resistance (the official manual's formula; Manolito: resistDamage 0x43EEE0):
                // dealt^2 / (dealt + resistance), kind by kind.
                const float dealt = d[k] * mult;
                const float r = target ? resistanceOf(target, k) : 0.0f;
                total += dealt > 0.0f ? dealt * dealt / (dealt + r) : 0.0f;
            }
        }
        return total;
    }

    // What each weapon really did on the panel, plain (no resistances), learned while it was the active one and the
    // panel had settled (the analyst, 2026-11-10 17:46: with a ratio to the estimate the idle slot still came out
    // 1.5-2 times itself active, every slot switched at each enemy). Every slot is judged from it, the active one too:
    // the panel's plain damage times what gets through this target's resistances (by the estimate's kinds); one never
    // held yet by its estimate times the mean ratio of those known (it is tried once, then known).
    std::unordered_map<uint32_t, float> g_panelPlain;

    float slotDamage(const cCreature* hero, const uint8_t* entry, int slot, const cCreature* target)
    {
        const uint8_t* st = reinterpret_cast<const uint8_t*>(hero) + kStats;
        const bool active = slot == *reinterpret_cast<const uint16_t*>(entry + kActiveSlot);
        const uint32_t weapon = slotItem(entry, slot, 0);
        const float plainEstimate = estimateDamage(hero, entry, slot, nullptr);
        const float estimate = target ? estimateDamage(hero, entry, slot, target) : plainEstimate;
        const float through = plainEstimate > 0.5f ? std::clamp(estimate / plainEstimate, 0.0f, 1.0f) : 1.0f;
        if (active)
        {
            // The hero's own weapon damage (st+0x86 min, st+0x76 max, the panel's «Arma»), its multipliers in it.
            float panel = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                const float lo = *reinterpret_cast<const float*>(st + 0x86 + 4 * k);
                const float hi = *reinterpret_cast<const float*>(st + 0x76 + 4 * k);
                panel += lo >= 0.0f && hi >= 0.0f && hi < 100000.0f ? (lo + hi) * 0.5f : 0.0f;
            }
            if (weapon && panel > 0.0f && GetTickCount() - g_slotSwitchAt > 3000)
            {
                auto [it, fresh] = g_panelPlain.try_emplace(weapon, panel);
                if (!fresh)
                {
                    it->second = it->second * 0.8f + panel * 0.2f;
                }
                if (plainEstimate > 1.0f)
                {
                    g_panelRatio[weapon] = std::clamp(panel / plainEstimate, 0.1f, 10.0f);
                }
            }
        }
        if (const auto it = g_panelPlain.find(weapon); it != g_panelPlain.end())
        {
            return it->second * through;
        }
        float ratio = 0.0f;
        for (const auto& [id, r] : g_panelRatio)
        {
            ratio += r;
        }
        ratio = g_panelRatio.empty() ? 1.0f : ratio / static_cast<float>(g_panelRatio.size());
        return estimate * ratio;
    }

    // Out of a fight: an empty open slot (not the active one) gets a weapon from the backpack, one of a kind the slots
    // do not have yet first (a fire sword beside the plain one), else the strongest. Main hand only (no shields, bows or
    // a second weapon: those need the game's own drop checks). One a call.
    bool fillWeaponSlot(cCreature* hero, uint8_t* pack)
    {
        if (!g_slotsReady)
        {
            return false;
        }
        const int open = slotsOpen(hero);
        const int active = *reinterpret_cast<const uint16_t*>(pack + kActiveSlot);
        int empty = -1;
        bool have[4] = {};
        for (int slot = 0; slot < open; ++slot)
        {
            const uint32_t a = slotItem(pack, slot, 0), b = slotItem(pack, slot, 1);
            if (!a && !b && slot != active && empty < 0)
            {
                empty = slot;
            }
            for (uint32_t id : {a, b})
            {
                if (const int k = mainKind(id); k >= 0)
                {
                    have[k] = true;
                }
            }
        }
        if (empty < 0)
        {
            return false;
        }
        void* types = typeMgr();
        uint32_t best = 0;
        float bestScore = 0.0f;
        eachItem(pack, [&](uint32_t id, int, int, int) {
            const uint32_t type = typeOf(id);
            if ((flagsOf(id) & kQuestItem) || type == 0x1600 || type == 0x1601 || !types ||
                !fn<SlotAccepts>(g_slotAccepts)(types, nullptr, 13, type) || !fn<CanUse>(g_canUse)(hero, nullptr, id, 0))
            {
                return;
            }
            const int kind = mainKind(id);
            if (kind < 0)
            {
                return;
            }
            const float score = worth(id).score * (have[kind] ? 1.0f : 3.0f);   // a new kind first
            if (!best || score > bestScore)
            {
                best = id;
                bestScore = score;
            }
        });
        if (!best)
        {
            return false;
        }
        fn<RemoveItem>(g_removeItem)(pack, nullptr, best, 1);
        fn<SetSlotHand>(g_setSlotHand)(pack, nullptr, static_cast<uint32_t>(empty), 1, best, 1);
        if (slotItem(pack, empty, 1) != best)
        {
            // Not taken: back into the backpack, and the slots left alone from now on.
            int w = 0, h = 0;
            uint16_t free = 0;
            if (sizeOf(best, w, h) && fn<FindFree>(g_findFree)(pack, nullptr, w, h, &free, 0))
            {
                fn<PutItem>(g_putItem)(pack, nullptr, free, best, w, h, 1);
            }
            LOG("Gear: weapon {:x} not taken by weapon slot {}: the slots left alone", best, empty + 1);
            g_slotsReady = false;
            return false;
        }
        LOG("Gear: weapon {:x} ({}) put in weapon slot {} of {}", best, kindName(mainKind(best)), empty + 1, open);
        return true;
    }

    std::atomic<uint32_t> g_switchTo{0xFFFFFFFFu};

    void switchSlot(void*)
    {
        const uint32_t slot = g_switchTo.exchange(0xFFFFFFFFu);
        cUI_Manager* ui = cUI_Manager::instance();
        void* taskbar = ui ? static_cast<void*>(ui->taskbar()) : nullptr;
        if (slot < 5 && taskbar && g_selectSlot)
        {
            fn<SelectSlot>(g_selectSlot)(taskbar, nullptr, slot, 1);
        }
    }

    // Runes of other classes in the backpack (the player, 2026-11-10: «las runas son importantes para
    // intercambiar/usar»): three of them and 500 gold make one of the hero's own at a combo master (AGENT_ADDRESSES
    // «Comerciante de runas»: 2 -> any class for 100, 3 -> own class for 500, 4 -> chosen for 2000).
    std::atomic<int> g_foreignRunes{0};

    int countForeignRunes(cCreature* hero, uint8_t* pack)
    {
        cObjectManager* objects = cObjectManager::instance();
        int n = 0;
        eachItem(pack, [&](uint32_t id, int, int, int) {
            const auto* o = objects ? reinterpret_cast<const uint8_t*>(objects->object(id)) : nullptr;
            if (o && *reinterpret_cast<const uintptr_t*>(o) == kRuneVtable && !(flagsOf(id) & kQuestItem) &&
                fn<CanUse>(g_canUse)(hero, nullptr, id, 1) == 0)
            {
                ++n;
            }
        });
        return n;
    }

    // The swap at a combo master's window (Manolito, AGENT_ADDRESSES «Canje por código, sin ratón (bloque S)»): the
    // window cUI_Master is UI window 14 ([[0x17EEBBC] + 0xB8], open while +0x10 & 1); its tab sheet at +0x154 (pages
    // vector +0x78, entries 0x18 with the page at +0x14, the active page u16 +0x84; 1 = the runes' page,
    // cUI_MasterLotto vtable 0x896834), setActivePage 0x729260 thiscall(tab, u16). The page's 4 input slots at
    // +0x24C + i*0x114 start with a drag descriptor {u16 1, u16 0x1C, u32 item id, 0, 0}; refresh 0x6D3FD0 (the price
    // to +0x7B4); the button: pay 0x57FD80(-price) (the game's own gold change, its anti-cheat copies right), doSwap
    // 0x6D4520 (3 runes -> one of the hero's class), the new rune's id at +0x798 (+0x794..+0x7A7 its descriptor), into
    // the backpack with findFree/putItem. show(0) of the page (vtable +0x24) gives back whatever is still in the slots.
    constexpr uintptr_t kUiManager = 0x17EEBBC, kMasterVtable = 0x896798, kLottoVtable = 0x896834;
    constexpr uintptr_t kSetActivePage = 0x729260, kLottoRefresh = 0x6D3FD0, kDoSwap = 0x6D4520;
    constexpr size_t kSlot0 = 0x24C, kSlotSize = 0x114, kResult = 0x794, kPrice = 0x7B4;
    using SetActivePage = void(__fastcall*)(uint8_t* tab, void*, uint16_t page);
    using UiCall = void(__fastcall*)(uint8_t* ui, void*);
    using UiShow = void(__fastcall*)(uint8_t* ui, void*, int show);
    int g_swapsDone = 0;

    uint8_t* masterWindow()
    {
        const uint8_t* ui = *reinterpret_cast<const uint8_t* const*>(kUiManager);
        uint8_t* master = ui ? *reinterpret_cast<uint8_t* const*>(ui + 0xB8) : nullptr;
        if (!master || *reinterpret_cast<const uintptr_t*>(master) != kMasterVtable ||
            !(*reinterpret_cast<const uint32_t*>(master + 0x10) & 1))
        {
            return nullptr;
        }
        return master;
    }

    // One step of the swap a frame; true while the master's window holds the agent.
    bool swapRunes(cCreature* hero, uint8_t* pack)
    {
        static DWORD s_openAt = 0;
        static bool s_told = false;
        uint8_t* master = masterWindow();
        if (!master)
        {
            s_openAt = 0;
            s_told = false;
            return false;
        }
        const DWORD now = GetTickCount();
        if (!s_openAt)
        {
            s_openAt = now;
        }
        uint8_t* tab = master + 0x154;
        const auto* pages = *reinterpret_cast<uint8_t* const*>(tab + 0x78);
        const auto* pagesEnd = *reinterpret_cast<uint8_t* const*>(tab + 0x7C);
        uint8_t* lotto = pages && pagesEnd - pages >= 2 * 0x18 ? *reinterpret_cast<uint8_t* const*>(pages + 0x18 + 0x14) : nullptr;
        if (!lotto || *reinterpret_cast<const uintptr_t*>(lotto) != kLottoVtable)
        {
            if (!s_told)
            {
                s_told = true;
                LOG("Gear: combo master's window open, but no runes' page found (pages {})", (pagesEnd - pages) / 0x18);
            }
            return now - s_openAt < 10000;
        }
        if (*reinterpret_cast<const uint16_t*>(tab + 0x84) != 1)
        {
            LOG("Gear: combo master's window open: to the runes' page");
            fn<SetActivePage>(kSetActivePage)(tab, nullptr, 1);
            return true;
        }
        // The swap's rune: into the backpack.
        if (const uint32_t made = *reinterpret_cast<const uint32_t*>(lotto + kResult + 4))
        {
            int w = 0, h = 0;
            uint16_t free = 0;
            if (sizeOf(made, w, h) && fn<FindFree>(g_findFree)(pack, nullptr, w, h, &free, 0))
            {
                fn<PutItem>(g_putItem)(pack, nullptr, free, made, w, h, 1);
                std::memset(lotto + kResult, 0, 0x14);
                *reinterpret_cast<uint32_t*>(lotto + kPrice) = 0;
                fn<UiCall>(kLottoRefresh)(lotto, nullptr);
                ++g_swapsDone;
                LOG("Gear: rune swap done: new rune {:x} (type {:x}) in the backpack ({} swaps)", made, typeOf(made),
                    g_swapsDone);
                g_runesTried.erase(made);
            }
            else
            {
                LOG("Gear: the swap's rune {:x}: no room in the backpack; given back on closing", made);
                fn<UiShow>((*reinterpret_cast<uintptr_t* const*>(lotto))[0x24 / 4])(lotto, nullptr, 0);
            }
            return true;
        }
        int filled = 0;
        for (int i = 0; i < 4; ++i)
        {
            filled += *reinterpret_cast<const uint32_t*>(lotto + kSlot0 + i * kSlotSize + 4) ? 1 : 0;
        }
        const uint32_t gold = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + kGold);
        const uint32_t price3 = *reinterpret_cast<const uint32_t*>(0xAD4B2C + 3 * 4);
        if (filled < 3 && countForeignRunes(hero, pack) + filled >= 3 && gold >= price3 + 300)
        {
            // Another rune of another class into the next free slot.
            cObjectManager* objects = cObjectManager::instance();
            uint32_t rune = 0;
            eachItem(pack, [&](uint32_t id, int, int, int) {
                const auto* o = rune || !objects ? nullptr : reinterpret_cast<const uint8_t*>(objects->object(id));
                if (o && *reinterpret_cast<const uintptr_t*>(o) == kRuneVtable && !(flagsOf(id) & kQuestItem) &&
                    fn<CanUse>(g_canUse)(hero, nullptr, id, 1) == 0)
                {
                    rune = id;
                }
            });
            for (int i = 0; rune && i < 4; ++i)
            {
                uint8_t* slot = lotto + kSlot0 + i * kSlotSize;
                if (*reinterpret_cast<const uint32_t*>(slot + 4))
                {
                    continue;
                }
                fn<RemoveItem>(g_removeItem)(pack, nullptr, rune, 1);
                std::memset(slot, 0, 0x14);
                *reinterpret_cast<uint16_t*>(slot) = 1;
                *reinterpret_cast<uint16_t*>(slot + 2) = 0x1C;
                *reinterpret_cast<uint32_t*>(slot + 4) = rune;
                fn<UiCall>(kLottoRefresh)(lotto, nullptr);
                LOG("Gear: rune {:x} (type {:x}, another class) into the swap's slot {}", rune, typeOf(rune), i + 1);
                return true;
            }
        }
        if (filled == 3)
        {
            const uint32_t price = *reinterpret_cast<const uint32_t*>(0xAD4B2C + filled * 4);
            if (gold >= price && price && price < 100000)
            {
                fn<AddGold>(g_addGold)(reinterpret_cast<uint8_t*>(hero) + kStats, nullptr, -static_cast<int>(price));
                fn<UiCall>(kDoSwap)(lotto, nullptr);
                fn<UiCall>(kLottoRefresh)(lotto, nullptr);
                LOG("Gear: 3 runes of other classes swapped for {} gold (one of the hero's class)", price);
                return true;
            }
        }
        // Nothing more to swap: whatever is in the slots back to the backpack, the window closed.
        LOG("Gear: rune swaps over ({} done, {} runes of other classes left): closing the master's window", g_swapsDone,
            countForeignRunes(hero, pack));
        fn<UiShow>((*reinterpret_cast<uintptr_t* const*>(lotto))[0x24 / 4])(lotto, nullptr, 0);
        fn<UiShow>((*reinterpret_cast<uintptr_t* const*>(master))[0x24 / 4])(master, nullptr, 0);
        g_foreignRunes = countForeignRunes(hero, pack);
        return true;
    }

    // Combos at a combo master (the tutorial's step 15; the player, 2026-11-10: «tienen que saber hacer todo el
    // tutorial perfectamente»). Manolito, block V1: the master's tab page 0 is cUI_MasterCombo (vtable 0x8967E4); its 4
    // art slots hold the art's id (dword) at +0x2CC + i*0x188 and its level (u16) at +0x2D0 + i*0x188, and each counts
    // only when it matches the template of the dropped descriptor (+0x778, u16 kind 4; +0x780 u16 ids[4], +0x788 u8
    // levels[4]); learn 0x6D22A0 thiscall(page, u32 the local player's id), ret 4: it checks the gold, finds the free
    // combo, stores it (stats +0x10E), pays with the game's own addGold and empties the slots. Of the hero's learned
    // arts (stats +0x102..+0x106, 22 bytes: u32 kind 1 spell / 2 weapon art, u16 id at +4, u16 level at +6) the 2-3 of
    // one kind with the highest levels, not mixed (mixing unconfirmed). One combo for now (the tutorial's lesson).
    constexpr uintptr_t kComboPageVtable = 0x8967E4, kLearnCombo = 0x6D22A0;
    using LearnCombo = void(__fastcall*)(uint8_t* page, void*, uint32_t player);
    std::atomic<bool> g_wantCombo{false};
    DWORD g_comboTriedAt = 0;

    int comboCount(const uint8_t* st)
    {
        const uint8_t* combos = *reinterpret_cast<uint8_t* const*>(st + 0x10E);
        int n = 0;
        for (int i = 0; combos && i < 4; ++i)
        {
            const uint8_t* r = combos + i * 38;
            const uint16_t id = *reinterpret_cast<const uint16_t*>(r);
            n += id >= 0x200 && id < 0x204 && *reinterpret_cast<const uint32_t*>(r + 2) ? 1 : 0;
        }
        return n;
    }

    // The arts for the next combo: up to 3 of the kind with the most, highest levels first; 0 when under 2.
    int comboArts(const uint8_t* st, uint32_t ids[4], uint16_t levels[4])
    {
        struct Art
        {
            uint32_t kind;
            uint16_t id, level;
        };
        std::vector<Art> arts;
        const uint8_t* begin = *reinterpret_cast<uint8_t* const*>(st + 0x102);
        const uint8_t* end = *reinterpret_cast<uint8_t* const*>(st + 0x106);
        for (const uint8_t* a = begin; begin && end > begin && end - begin < 22 * 64 && a + 22 <= end; a += 22)
        {
            const uint32_t kind = *reinterpret_cast<const uint32_t*>(a);
            const uint16_t id = *reinterpret_cast<const uint16_t*>(a + 4), level = *reinterpret_cast<const uint16_t*>(a + 6);
            if ((kind == 1 || kind == 2) && id && level)
            {
                arts.push_back({kind, id, level});
            }
        }
        const auto count = [&](uint32_t k) {
            return std::count_if(arts.begin(), arts.end(), [&](const Art& a) { return a.kind == k; });
        };
        const uint32_t kind = count(2) >= count(1) ? 2 : 1;
        std::erase_if(arts, [&](const Art& a) { return a.kind != kind; });
        std::sort(arts.begin(), arts.end(), [](const Art& a, const Art& b) { return a.level > b.level; });
        const int n = static_cast<int>(std::min<size_t>(arts.size(), 3));
        for (int i = 0; i < 4; ++i)
        {
            ids[i] = i < n ? arts[i].id : 0;
            levels[i] = i < n ? arts[i].level : 0;
        }
        return n >= 2 ? n : 0;
    }

    // One step a frame at the master's window; true while it holds the agent.
    bool makeCombo(cCreature* hero)
    {
        static int s_steps = 0;
        uint8_t* master = masterWindow();
        if (!master || !g_wantCombo)
        {
            s_steps = 0;
            return false;
        }
        const uint8_t* st = reinterpret_cast<const uint8_t*>(hero) + kStats;
        uint8_t* tab = master + 0x154;
        const auto* pages = *reinterpret_cast<uint8_t* const*>(tab + 0x78);
        const auto* pagesEnd = *reinterpret_cast<uint8_t* const*>(tab + 0x7C);
        uint8_t* page = pages && pagesEnd - pages >= 0x18 ? *reinterpret_cast<uint8_t* const*>(pages + 0x14) : nullptr;
        const auto giveUp = [&](const char* why) {
            LOG("Gear: combo: {}; left 30 min", why);
            g_wantCombo = false;
            g_comboTriedAt = GetTickCount();
            return true;
        };
        if (!page || *reinterpret_cast<const uintptr_t*>(page) != kComboPageVtable)
        {
            return giveUp("no combos' page in the master's window");
        }
        if (++s_steps > 4)
        {
            return giveUp("not learned after 4 steps");
        }
        if (*reinterpret_cast<const uint16_t*>(tab + 0x84) != 0)
        {
            LOG("Gear: combo master's window open: to the combos' page");
            fn<SetActivePage>(kSetActivePage)(tab, nullptr, 0);
            return true;
        }
        uint32_t ids[4];
        uint16_t levels[4];
        const int n = comboArts(st, ids, levels);
        if (!n)
        {
            return giveUp("fewer than 2 arts of one kind");
        }
        const int before = comboCount(st);
        std::memset(page + 0x778, 0, 0x14);
        *reinterpret_cast<uint16_t*>(page + 0x778) = 4;
        for (int i = 0; i < 4; ++i)
        {
            *reinterpret_cast<uint32_t*>(page + 0x2CC + i * 0x188) = ids[i];
            *reinterpret_cast<uint16_t*>(page + 0x2D0 + i * 0x188) = levels[i];
            *reinterpret_cast<uint16_t*>(page + 0x780 + i * 2) = static_cast<uint16_t>(ids[i]);
            *(page + 0x788 + i) = static_cast<uint8_t>(std::min<uint16_t>(levels[i], 255));
        }
        const uint8_t* local = *reinterpret_cast<const uint8_t* const*>(0x182EBE8);
        const uint32_t player = local ? *reinterpret_cast<const uint32_t*>(local + 0x14) : 0;
        const uint32_t gold = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + kGold);
        fn<LearnCombo>(kLearnCombo)(page, nullptr, player);
        const int after = comboCount(st);
        const uint32_t left = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + kGold);
        if (after > before)
        {
            LOG("Gear: combo learned at the master: arts {:x} {:x} {:x} (levels {} {} {}) for {} gold, {} combos now",
                ids[0], ids[1], ids[2], levels[0], levels[1], levels[2], gold - left, after);
            g_wantCombo = false;
            g_comboTriedAt = GetTickCount();
            return true;
        }
        LOG("Gear: combo not learned (try {}, {} arts, {} gold)", s_steps, n, gold);
        for (int i = 0; i < 4; ++i)
        {
            *reinterpret_cast<uint32_t*>(page + 0x2CC + i * 0x188) = 0;
            *reinterpret_cast<uint16_t*>(page + 0x2D0 + i * 0x188) = 0;
        }
        std::memset(page + 0x778, 0, 0x14);
        return true;
    }

    // Socketing at a smith (the tutorial's step 16; the player: «tienen que saber hacer todo el tutorial
    // perfectamente»). Manolito, block V2: the smith's window cUI_Blacksmith (vtable 0x896288) is UI window 3
    // ([[0x17EEBBC] + 0x8C], open while +0x10 & 1), opened by a use click on the smith as a merchant's. Its base item
    // goes into its own inventory [bs + 0x154] (putItem, then refresh 0x6A2470 thiscall(bs, id), ret 4); the free
    // sockets come to bs + 0x15C; what goes in, into 4 slots at bs + 0x4C4 + i*0x10C as a drag descriptor {u16 1,
    // u16 0x1C, u32 id, 0, 0} (out of the backpack first); forge 0x6A2660 thiscall(bs), no arguments: it prices,
    // pays with the game's addGold and sets each one. An item's sockets: 0x5BE9C0 thiscall(item) counts the
    // non-zero bytes at item + 0x124..+0x12B; those used are the vector item + 0x100..+0x104 (0x82 bytes each); the
    // item from the object table through __RTDynamicCast 0x84A961(obj, 0, 0x8EB648, 0x94EBC0, 0). What fits:
    // 0x428780 thiscall([0xAAB5E4], type), ret 4, al 1. Backpack items only (the worn ones would have to come off).
    constexpr uintptr_t kSmithVtable = 0x896288, kSmithRefresh = 0x6A2470, kForge = 0x6A2660;
    constexpr uintptr_t kSocketCount = 0x5BE9C0, kSocketable = 0x428780, kDynamicCast = 0x84A961;
    using SmithRefresh = void(__fastcall*)(uint8_t* bs, void*, uint32_t id);
    using SocketCount = int(__fastcall*)(const uint8_t* item, void*);
    using Socketable = uint8_t(__fastcall*)(void* types, void*, uint32_t type);
    using DynamicCast = void*(__cdecl*)(void* object, long delta, void* from, void* to, int reference);
    std::atomic<bool> g_wantSocket{false};
    DWORD g_socketTriedAt = 0;

    const uint8_t* itemBase(uint32_t id)
    {
        cObjectManager* objects = cObjectManager::instance();
        cObject* o = objects ? objects->object(id) : nullptr;
        return o ? static_cast<const uint8_t*>(fn<DynamicCast>(kDynamicCast)(o, 0, reinterpret_cast<void*>(0x8EB648),
                                                                             reinterpret_cast<void*>(0x94EBC0), 0))
                 : nullptr;
    }

    int freeSockets(uint32_t id)
    {
        const uint8_t* item = itemBase(id);
        if (!item)
        {
            return 0;
        }
        const int sockets = fn<SocketCount>(kSocketCount)(item, nullptr);
        const auto* begin = *reinterpret_cast<const uint8_t* const*>(item + 0x100);
        const auto* end = *reinterpret_cast<const uint8_t* const*>(item + 0x104);
        const int used = begin && end >= begin ? static_cast<int>((end - begin) / 0x82) : 0;
        return sockets > 0 && sockets <= 8 ? std::max(0, sockets - used) : 0;
    }

    bool socketable(uint32_t id)
    {
        cObjectManager* objects = cObjectManager::instance();
        const auto* o = objects ? reinterpret_cast<const uint8_t*>(objects->object(id)) : nullptr;
        const uint32_t type = typeOf(id);
        void* types = *reinterpret_cast<void* const*>(0xAAB5E4);
        return o && type && types && *reinterpret_cast<const uintptr_t*>(o) != kRuneVtable && !(flagsOf(id) & kQuestItem) &&
               fn<Socketable>(kSocketable)(types, nullptr, type) != 0;
    }

    // The base (a backpack item with free sockets, one the hero would wear first) and what goes in; 0 when none.
    uint32_t socketPlan(cCreature* hero, uint8_t* pack, std::vector<uint32_t>& gems)
    {
        uint32_t base = 0;
        bool baseWorn = false;
        gems.clear();
        eachItem(pack, [&](uint32_t id, int, int, int) {
            if (flagsOf(id) & kQuestItem)
            {
                return;
            }
            if (socketable(id))
            {
                gems.push_back(id);
                return;
            }
            if (freeSockets(id) > 0)
            {
                const bool worn = bestSlot(hero, id, 0.0f) >= 0;
                if (!base || (worn && !baseWorn))
                {
                    base = id;
                    baseWorn = worn;
                }
            }
        });
        return gems.empty() ? 0 : base;
    }

    uint8_t* smithWindow()
    {
        const uint8_t* ui = *reinterpret_cast<const uint8_t* const*>(kUiManager);
        uint8_t* bs = ui ? *reinterpret_cast<uint8_t* const*>(ui + 0x8C) : nullptr;
        if (!bs || *reinterpret_cast<const uintptr_t*>(bs) != kSmithVtable || !(*reinterpret_cast<const uint32_t*>(bs + 0x10) & 1))
        {
            return nullptr;
        }
        return bs;
    }

    // One step a frame at the smith's window; true while it holds the agent.
    bool socketAtSmith(cCreature* hero, uint8_t* pack)
    {
        static uint32_t s_base = 0;
        static int s_steps = 0;
        uint8_t* bs = smithWindow();
        if (!bs || !g_wantSocket)
        {
            s_base = 0;
            s_steps = 0;
            return false;
        }
        const auto done = [&](const char* why) {
            LOG("Gear: socketing: {}", why);
            // The base left on the smith's table back into the backpack (the code reviewer: an early end left it).
            if (s_base)
            {
                int bw = 0, bh = 0;
                uint16_t cell = 0;
                uint8_t* table = *reinterpret_cast<uint8_t* const*>(bs + 0x154);
                if (table && sizeOf(s_base, bw, bh) && fn<FindFree>(g_findFree)(pack, nullptr, bw, bh, &cell, 0))
                {
                    fn<RemoveItem>(g_removeItem)(table, nullptr, s_base, 1);
                    fn<PutItem>(g_putItem)(pack, nullptr, cell, s_base, bw, bh, 1);
                }
                s_base = 0;
            }
            g_wantSocket = false;
            g_socketTriedAt = GetTickCount();
            return true;
        };
        uint8_t* inv = *reinterpret_cast<uint8_t* const*>(bs + 0x154);
        if (!inv)
        {
            return done("the smith's window has no item place");
        }
        if (++s_steps > 12)
        {
            return done("given up after 12 steps");
        }
        if (!s_base)
        {
            std::vector<uint32_t> gems;
            const uint32_t base = socketPlan(hero, pack, gems);
            int w = 0, h = 0;
            uint16_t cell = 0;
            if (!base || !sizeOf(base, w, h) || !fn<FindFree>(g_findFree)(inv, nullptr, w, h, &cell, 0))
            {
                return done("nothing to socket here");
            }
            fn<RemoveItem>(g_removeItem)(pack, nullptr, base, 1);
            fn<PutItem>(g_putItem)(inv, nullptr, cell, base, w, h, 1);
            fn<SmithRefresh>(kSmithRefresh)(bs, nullptr, base);
            s_base = base;
            LOG("Gear: socketing: {:x} (type {:x}, {} free sockets) onto the smith's table", base, typeOf(base),
                freeSockets(base));
            return true;
        }
        const int before = freeSockets(s_base);
        std::vector<uint32_t> gems;
        eachItem(pack, [&](uint32_t id, int, int, int) {
            if (socketable(id))
            {
                gems.push_back(id);
            }
        });
        int placed = 0;
        for (int i = 0; i < 4 && i < before; ++i)
        {
            uint8_t* slot = bs + 0x4C4 + i * 0x10C;
            if (*reinterpret_cast<const uint32_t*>(slot + 4))
            {
                ++placed;
                continue;
            }
            if (gems.empty())
            {
                break;
            }
            const uint32_t gem = gems.back();
            gems.pop_back();
            fn<RemoveItem>(g_removeItem)(pack, nullptr, gem, 1);
            std::memset(slot, 0, 0x14);
            *reinterpret_cast<uint16_t*>(slot) = 1;
            *reinterpret_cast<uint16_t*>(slot + 2) = 0x1C;
            *reinterpret_cast<uint32_t*>(slot + 4) = gem;
            ++placed;
            LOG("Gear: socketing: {:x} (type {:x}) into socket {}", gem, typeOf(gem), i + 1);
        }
        const uint32_t gold = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + kGold);
        if (placed)
        {
            fn<UiCall>(kForge)(bs, nullptr);
        }
        const int after = freeSockets(s_base);
        const uint32_t left = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + kGold);
        // The item back into the backpack, whatever came of it; what is still in the slots is given back on closing.
        int w = 0, h = 0;
        uint16_t free = 0;
        if (sizeOf(s_base, w, h) && fn<FindFree>(g_findFree)(pack, nullptr, w, h, &free, 0))
        {
            fn<RemoveItem>(g_removeItem)(inv, nullptr, s_base, 1);
            fn<PutItem>(g_putItem)(pack, nullptr, free, s_base, w, h, 1);
            fn<SmithRefresh>(kSmithRefresh)(bs, nullptr, 0);
        }
        LOG("Gear: socketing {:x}: {} placed, free sockets {} -> {}, {} gold paid", s_base, placed, before, after,
            gold - left);
        s_base = 0;
        fn<UiShow>((*reinterpret_cast<uintptr_t* const*>(bs))[0x24 / 4])(bs, nullptr, 0);
        return done(after < before ? "done" : "nothing set");
    }

    // A horse from a stable (the tutorial's step 10). Manolito, block V3b: cUI_Horse (vtable 0x896528) is UI window 17
    // ([[0x17EEBBC] + 0xC4], open while +0x10 & 1), opened by a use click on the stable man; it offers one horse, its
    // id at +0x154, the price at +0x15C (the riding skill's discount in it). Buying as its button does: bind 0x55C640
    // thiscall(hero, horse id, 1), ret 8 (hero +0x570 the horse, the horse's +0x570 the rider), then 0x5498F0
    // thiscall(horse, 0, 1), and the price paid with the game's own addGold.
    constexpr uintptr_t kHorseVtable = 0x896528, kBindHorse = 0x55C640, kHorseSettle = 0x5498F0;
    using BindHorse = void(__fastcall*)(void* creature, void*, uint32_t id, int flag);
    using HorseSettle = void(__fastcall*)(void* creature, void*, int n, int flag);
    std::atomic<bool> g_wantHorse{false};
    std::atomic<bool> g_horseUpgrade{false};    // a stable near has a clearly better horse than the hero's
    DWORD g_horseTriedAt = 0;

    bool buyHorse(cCreature* hero)
    {
        const uint8_t* ui = *reinterpret_cast<const uint8_t* const*>(kUiManager);
        uint8_t* w = ui ? *reinterpret_cast<uint8_t* const*>(ui + 0xC4) : nullptr;
        if (!w || *reinterpret_cast<const uintptr_t*>(w) != kHorseVtable || !(*reinterpret_cast<const uint32_t*>(w + 0x10) & 1))
        {
            return false;
        }
        const auto close = [&](const char* why) {
            LOG("Gear: horse: {}", why);
            g_wantHorse = false;
            g_horseTriedAt = GetTickCount();
            // Its vtable +0x24 is an empty notice (Manolito): the windows' own close 0x7325C0 thiscall(window, bits
            // to clear from +0x10), ret 4, as the big map closes itself.
            using ClearFlags = void(__fastcall*)(uint8_t* window, void*, uint32_t bits);
            fn<ClearFlags>(0x7325C0)(w, nullptr, 1);
            // show(0) leaves this window open (the live verifier, 2026-11-10: flags 5 five minutes on): Escape, as a
            // player closes it, only while it is still open (Escape with none open would bring up the game menu).
            if (*reinterpret_cast<const uint32_t*>(w + 0x10) & 1)
            {
                Inject::key(VK_ESCAPE, true);
                Inject::key(VK_ESCAPE, false);
            }
            return true;
        };
        if (!g_wantHorse)
        {
            return false;
        }
        // The offer is filled a moment after the window opens (the live verifier, 2026-11-10: «no horse offered»
        // on the first frame): up to 3 s for it.
        static DWORD s_openAt = 0, s_lastSeen = 0;
        const DWORD tick = GetTickCount();
        if (!s_openAt || tick - s_lastSeen > 1000)
        {
            s_openAt = tick;
        }
        s_lastSeen = tick;
        // The window opens empty (the live verifier, 2026-11-10: +0x154 still 0 minutes on); the game fills it on a
        // click on the horse. Manolito, V3b live: the horse for sale is bound to the stable man both ways (man +0x570
        // the horse, the horse's +0x570 the man; the man's id at window +0x158): offered here with 0x6BEB90
        // thiscall(window, horse id), ret 4, which also sets the price.
        static DWORD s_offeredAt = 0;
        if (!*reinterpret_cast<const uint32_t*>(w + 0x154) && s_offeredAt < s_openAt)
        {
            s_offeredAt = tick;
            cObjectManager* om = cObjectManager::instance();
            const uint32_t manId = *reinterpret_cast<const uint32_t*>(w + 0x158);
            const auto* man = om && manId ? reinterpret_cast<const uint8_t*>(om->object(manId)) : nullptr;
            const uint32_t forSale = man ? *reinterpret_cast<const uint32_t*>(man + 0x570) : 0;
            const auto* offered = om && forSale ? reinterpret_cast<const uint8_t*>(om->creature(forSale)) : nullptr;
            if (offered && *reinterpret_cast<const uint32_t*>(offered + 0x570) == manId)
            {
                using Offer = void(__fastcall*)(uint8_t* window, void*, uint32_t id);
                fn<Offer>(0x6BEB90)(w, nullptr, forSale);
                LOG("Gear: horse: the stable man {:x}'s horse {:x} offered", manId, forSale);
                return true;
            }
        }
        const uint32_t horseId = *reinterpret_cast<const uint32_t*>(w + 0x154);
        const uint32_t price = *reinterpret_cast<const uint32_t*>(w + 0x15C);
        const uint32_t gold = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + kGold);
        cObjectManager* objects = cObjectManager::instance();
        cCreature* horse = objects && horseId ? objects->creature(horseId) : nullptr;
        if (!horse)
        {
            if (tick - s_openAt < 3000)
            {
                return true;
            }
            const auto* d = reinterpret_cast<const uint32_t*>(w + 0x150);
            LOG("Gear: horse: window +0x150..+0x170: {:x} {:x} {:x} {:x} {:x} {:x} {:x} {:x} {:x}", d[0], d[1], d[2], d[3],
                d[4], d[5], d[6], d[7], d[8]);
            return close("no horse offered");
        }
        // The hero's own horse kept unless this one is clearly better (the player, 2026-11-10: «cada caballo tiene
        // sus estadísticas… que el agente hiciera comparaciones antes de cambiar de caballo»): two levels or more.
        const uint32_t ownId = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + 0x570);
        if (const cCreature* own = ownId && objects ? objects->creature(ownId) : nullptr)
        {
            const int ownWorth = Gear::horseWorth(own), newWorth = Gear::horseWorth(horse);
            LOG("Gear: horse {:x} for sale (worth {}) against the hero's {:x} (worth {})", horseId, newWorth, ownId, ownWorth);
            if (newWorth < ownWorth + 2000)
            {
                return close("not clearly better than the hero's own");
            }
        }
        // At most half the gold (potions come first).
        if (!price || price > 100000 || price * 2 > gold)
        {
            return close(price ? "the horse costs more than half the gold: not now" : "no price");
        }
        fn<BindHorse>(kBindHorse)(hero, nullptr, horseId, 1);
        fn<HorseSettle>(kHorseSettle)(horse, nullptr, 0, 1);
        fn<AddGold>(g_addGold)(reinterpret_cast<uint8_t*>(hero) + kStats, nullptr, -static_cast<int>(price));
        const uint32_t own = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + 0x570);
        LOG("Gear: horse {:x} bought for {} gold (the hero's horse now {:x})", horseId, price, own);
        return close("bought");
    }

    void work(void*)
    {
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        void* inv = inventory();
        uint8_t* pack = hero ? entryOf(static_cast<int>(hero->id)) : nullptr;
        if (hero && inv && pack && hero->health > 0)
        {
            stashHeld(pack);
            {
                // A combo wanted: none yet, 2 arts of one kind for it, gold for it and for potions, half an hour
                // since the last try.
                const uint8_t* st = reinterpret_cast<const uint8_t*>(hero) + kStats;
                uint32_t ids[4];
                uint16_t levels[4];
                const uint32_t gold = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + kGold);
                const bool want = comboCount(st) == 0 && comboArts(st, ids, levels) &&
                                  gold >= (g_wantCombo ? 2000u : 3000u) &&
                                  (!g_comboTriedAt || GetTickCount() - g_comboTriedAt > 1800000);
                if (want != g_wantCombo.exchange(want) && want)
                {
                    LOG("Gear: a combo to learn (none known, {} gold): off to a combo master", gold);
                }
                // A horse wanted: none yet, gold enough to spare half on it.
                // Never for the Dwarf: he does not ride (Builds.md; the player, 2026-11-10: no riding animation).
                const bool horse = HeroProfile::current().cls != HeroProfile::Class::Dwarf &&
                                   (*reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + 0x570) == 0 ||
                                    g_horseUpgrade) &&
                                   gold >= 2000 && (!g_horseTriedAt || GetTickCount() - g_horseTriedAt > 1800000);
                if (horse != g_wantHorse.exchange(horse) && horse)
                {
                    LOG("Gear: {} and {} gold: off to a stable", g_horseUpgrade ? "a better horse there" : "no horse", gold);
                }
                // Something to socket: a backpack item with a free socket and a gem for it, gold for the smith.
                static DWORD s_socketLookAt = 0;
                if (!smithWindow() && GetTickCount() - s_socketLookAt > 10000)
                {
                    s_socketLookAt = GetTickCount();
                    std::vector<uint32_t> gems;
                    const uint32_t base = socketPlan(hero, pack, gems);
                    const bool socket = base && gold >= 1500 &&
                                        (!g_socketTriedAt || GetTickCount() - g_socketTriedAt > 1800000);
                    g_socketBase = socket ? base : 0u;
                    if (socket != g_wantSocket.exchange(socket) && socket)
                    {
                        LOG("Gear: {:x} has {} free sockets and {} things fit them: off to a smith", base,
                            freeSockets(base), gems.size());
                    }
                }
            }
            if (g_canUse)
            {
                const int foreign = countForeignRunes(hero, pack);
                if (g_foreignRunes.exchange(foreign) != foreign && foreign)
                {
                    LOG("Gear: {} runes of other classes in the backpack (3 and 500 gold make one of the hero's own "
                        "at a combo master)", foreign);
                }
            }
            const bool shop = *reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(inv) + kTrade) != 0;
            const bool trip = shop && g_tripShop;
            bool acted = true;
            if (buyHorse(hero))
            {
            }
            else if (g_wantCombo && makeCombo(hero))
            {
            }
            else if (g_wantSocket && socketAtSmith(hero, pack))
            {
            }
            else if (g_settings.runes && g_canUse && swapRunes(hero, pack))
            {
            }
            else if (shop && (g_settings.sell || trip) && sellOne(hero, pack))
            {
            }
            else if (shop && (g_settings.buy || trip) && buyPotion(hero, pack))
            {
            }
            else if (shop && (g_settings.buy || trip) && buyOne(hero, pack))
            {
            }
            else if ((g_calm || shop) && (g_settings.equip || trip) && equipOne(hero, pack))
            {
            }
            else if (g_calm && !shop && g_settings.runes && g_useItem && readRune(hero, pack))
            {
            }
            else if (g_calm && !shop && g_settings.scrolls && g_useItem && readScroll(hero, pack))
            {
            }
            else if (g_calm && !shop && g_settings.weaponSlots && fillWeaponSlot(hero, pack))
            {
            }
            else
            {
                acted = false;
            }
            g_full = packFull(pack);
            g_healing = countHealing(pack);
            g_idleWorks = acted ? 0 : g_idleWorks + 1;
            if (trip && g_idleWorks >= 2)
            {
                LOG("Gear: merchant trip done (backpack {})", g_full ? "still full: nothing more to sell" : "has room");
                g_tripDone = true;
                if (g_full || g_healing <= 2)   // no potion sold there either (or gold short): not straight back
                {
                    g_noTripUntil = GetTickCount() + 600000;
                }
            }
            if (!shop)
            {
                g_tripShop = false;
            }
        }
        g_working = false;
    }

    std::wstring iniPath()
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe;
        return path.substr(0, path.find_last_of(L"\\/") + 1) + L"SacredBild.ini";
    }

    void drawMenu()
    {
        if (!g_ready)
        {
            ImGui::TextDisabled("No disponible en esta versión del juego.");
            return;
        }
        ImGui::Checkbox("Equipar mejoras de la mochila (fuera de combate)", &g_settings.equip);
        ImGui::Checkbox("Vender lo que no usará (con un mercader abierto)", &g_settings.sell);
        ImGui::SetItemTooltip("Lo peor que lo equipado en todo hueco donde cabe, o de otra clase. Nunca objetos de misión ni "
                              "los que no son equipo (pociones, runas, gemas).");
        ImGui::Checkbox("Comprar mejoras (con un mercader abierto)", &g_settings.buy);
        ImGui::SliderInt("Oro que guarda (%)", &g_settings.keepGoldPercent, 0, 95);
        ImGui::Checkbox("Leer las runas de la mochila", &g_settings.runes);
        ImGui::Checkbox("Leer los pergaminos y libros (enciclopedia)", &g_settings.scrolls);
        ImGui::SetItemTooltip("Fuera de combate, como el clic derecho: aprende el arte o le sube un nivel. Las de otra "
                              "clase se quedan en la mochila.");
        ImGui::Checkbox("Usar las ranuras de armas", &g_settings.weaponSlots);
        ImGui::SetItemTooltip("Llena las ranuras abiertas con armas de la mochila (elementos distintos primero) y, al "
                              "elegir enemigo, cambia a la ranura que más daño le hace según sus resistencias.");
        ImGui::Checkbox("Ir al mercader con la mochila llena", &g_settings.trips);
        ImGui::SetItemTooltip("Con el agente activo: va al mercader más cercano que conoce, vende lo que no usará, se "
                              "equipa lo mejor y cierra la tienda; luego sigue jugando.");
    }

    void saveMenu(const std::wstring& ini)
    {
        ModkitMenu::write(ini, L"Gear", L"Equip", g_settings.equip);
        ModkitMenu::write(ini, L"Gear", L"Sell", g_settings.sell);
        ModkitMenu::write(ini, L"Gear", L"Buy", g_settings.buy);
        ModkitMenu::write(ini, L"Gear", L"KeepGoldPercent", g_settings.keepGoldPercent);
        ModkitMenu::write(ini, L"Gear", L"Trips", g_settings.trips);
        ModkitMenu::write(ini, L"Gear", L"Runes", g_settings.runes);
        ModkitMenu::write(ini, L"Gear", L"Scrolls", g_settings.scrolls);
        ModkitMenu::write(ini, L"Gear", L"WeaponSlots", g_settings.weaponSlots);
    }

    void install()
    {
        const std::wstring ini = iniPath();
        g_settings.equip = GetPrivateProfileIntW(L"Gear", L"Equip", 1, ini.c_str()) != 0;
        g_settings.sell = GetPrivateProfileIntW(L"Gear", L"Sell", 0, ini.c_str()) != 0;
        g_settings.buy = GetPrivateProfileIntW(L"Gear", L"Buy", 0, ini.c_str()) != 0;
        g_settings.trips = GetPrivateProfileIntW(L"Gear", L"Trips", 1, ini.c_str()) != 0;
        g_settings.runes = GetPrivateProfileIntW(L"Gear", L"Runes", 1, ini.c_str()) != 0;
        g_settings.scrolls = GetPrivateProfileIntW(L"Gear", L"Scrolls", 1, ini.c_str()) != 0;
        g_settings.weaponSlots = GetPrivateProfileIntW(L"Gear", L"WeaponSlots", 1, ini.c_str()) != 0;
        g_settings.keepGoldPercent =
            std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Gear", L"KeepGoldPercent", 50, ini.c_str())), 0, 95);
        const size_t missing = Sig::resolve(kSigs);
        // cMouse::clear (ENG 006556F0, Manolito): empties the cursor's hand without destroying what it held. Apart:
        // without it only the held item's stashing is off.
        const Sig::Entry kMouseSig[] = {
            {"cMouse::clear", "83 EC 14 53 57 8B F9 33 DB 8B 47 68", 0, Sig::Take::Match, &g_mouseClear}};
        Sig::resolve(kMouseSig);
        // The weapon slots apart: without them only the slots are left alone.
        const Sig::Entry kSlotSigs[] = {
            {"selectWeaponSlot", "64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 83 EC 50 53 55 56 8B F1", 0,
                Sig::Take::Match, &g_selectSlot},
            {"setSlotHand", "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 74 8A", 0,
                Sig::Take::Match, &g_setSlotHand},
        };
        g_slotsReady = Sig::resolve(kSlotSigs) == 0;
        LOG("Gear: weapon slots {} (select {:x}, setSlotHand {:x})", g_slotsReady ? "ready" : "not available",
            g_selectSlot, g_setSlotHand);
        if (g_addGoldCall && *reinterpret_cast<const uint8_t*>(g_addGoldCall) == 0xE8)
        {
            g_addGold = g_addGoldCall + 5 + *reinterpret_cast<const int32_t*>(g_addGoldCall + 1);
        }
        g_ready = missing == 0 && g_addGold;
        LOG("Gear: {} (equip {}, sell {}, buy {})", g_ready ? "ready" : "not available (signatures)", g_settings.equip,
            g_settings.sell, g_settings.buy);
        ModkitMenu::add({"Equipo y comercio", &drawMenu, &saveMenu});
    }
}

void Gear::onFrame(DWORD now, bool calm)
{
    static std::once_flag once;
    std::call_once(once, install);
    // A shop opened: the merchant goes on the agent's map, where the hero stands (beside him).
    static uint32_t lastMerchant = 0;
    if (void* inv = g_ready ? inventory() : nullptr)
    {
        const uint32_t merchant = *reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(inv) + kTrade);
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        if (merchant && merchant != lastMerchant && hero)
        {
            MapMemory::mark(MapMemory::Mark::Merchant, *(reinterpret_cast<const uint8_t*>(hero) + 0x24), hero->x, hero->y);
        }
        lastMerchant = merchant;
    }
    static DWORD nextAt = 0;
    if (!g_ready || !(g_settings.equip || g_settings.sell || g_settings.buy || g_settings.trips || g_settings.runes ||
                      g_settings.scrolls) ||
        (nextAt && static_cast<int>(now - nextAt) < 0) || g_working)
    {
        return;
    }
    nextAt = now + 1500;
    g_calm = calm;
    g_working = true;
    Inject::call(&work, nullptr);
}

int Gear::grade(uint32_t id, bool equipment)
{
    return g_ready ? gradeOf(id, equipment) : -1;
}

bool Gear::packFull()
{
    return g_ready && g_full;
}

int Gear::foreignRunes()
{
    return g_ready ? g_foreignRunes.load() : 0;
}

void Gear::setHorseUpgrade(bool better)
{
    g_horseUpgrade = better;
}

bool Gear::wantsHorse()
{
    return g_ready && g_wantHorse;
}

bool Gear::wantsSocket()
{
    return g_ready && g_wantSocket;
}

bool Gear::wantsCombo()
{
    return g_ready && g_wantCombo;
}

bool Gear::wantsRuneSwap()
{
    const cCreature* hero = cObjectManager::instance() ? cObjectManager::instance()->hero() : nullptr;
    const uint32_t gold = hero ? *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + kGold) : 0;
    // 500 for the swap and as much kept for potions.
    return g_ready && g_settings.runes && g_foreignRunes >= 3 && gold >= 1000;
}

bool Gear::wantsShop()
{
    const DWORD quiet = g_noTripUntil;
    // A full backpack, or no healing potion left with gold to buy some (and buying on).
    const cCreature* hero = cObjectManager::instance() ? cObjectManager::instance()->hero() : nullptr;
    const uint32_t gold = hero ? *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + kGold) : 0;
    // Before they run out (the analyst, 2026-11-10 18:17: 9 of 137 fights below 20 % of health once the last
    // potion was gone, 11-15 enemies at level 4): at 2 left.
    const bool potions = g_settings.buy && g_healing >= 0 && g_healing <= 2 && gold >= 200;
    return g_ready && g_settings.trips && (g_full || potions) && (!quiet || static_cast<int>(GetTickCount() - quiet) >= 0);
}

void Gear::tripOpened()
{
    g_tripShop = true;
    g_tripDone = false;
    g_idleWorks = 0;
}

bool Gear::tripDone()
{
    if (!g_tripDone)
    {
        return false;
    }
    g_tripDone = false;
    g_tripShop = false;
    return true;
}

bool Gear::shopOpen()
{
    void* inv = g_ready ? inventory() : nullptr;
    return inv && *reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(inv) + kTrade) != 0;
}

float Gear::expectedHit(uint32_t targetId)
{
    if (!g_ready)
    {
        return -1.0f;
    }
    cObjectManager* objects = cObjectManager::instance();
    cCreature* hero = objects ? objects->hero() : nullptr;
    cCreature* target = objects ? objects->creature(targetId) : nullptr;
    const uint8_t* pack = hero ? entryOf(static_cast<int>(hero->id)) : nullptr;
    if (!hero || !target || !pack)
    {
        return -1.0f;
    }
    const int active = *reinterpret_cast<const uint16_t*>(pack + kActiveSlot);
    if (active < 0 || active >= 5)
    {
        return -1.0f;
    }
    const float d = slotDamage(hero, pack, active, target);
    if (!(d > 0.0f))
    {
        return -1.0f;
    }
    // The chance to hit: attack / (attack + defence) (SacredWiki «Chance to Hit», archived, via the web researcher;
    // 842 against 158 hits 84.2 %), the attack st+0x5A and the defence st+0x5E (f32, the panel's).
    const float attack = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(hero) + kStats + 0x5A);
    const float defence = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(target) + kStats + 0x5E);
    const float chance = attack > 0.0f && attack < 1e7f && defence >= 0.0f && defence < 1e7f
                             ? attack / (attack + defence) : 1.0f;
    return d * chance;
}

bool Gear::chooseWeaponFor(uint32_t targetId)
{
    if (!g_ready || !g_slotsReady || !g_settings.weaponSlots)
    {
        return false;
    }
    cObjectManager* objects = cObjectManager::instance();
    cCreature* hero = objects ? objects->hero() : nullptr;
    cCreature* target = objects ? objects->creature(targetId) : nullptr;
    uint8_t* pack = hero ? entryOf(static_cast<int>(hero->id)) : nullptr;
    if (!hero || !target || !pack)
    {
        return false;
    }
    const int open = slotsOpen(hero);
    const int active = *reinterpret_cast<const uint16_t*>(pack + kActiveSlot);
    if (active < 0 || active >= 5)
    {
        return false;
    }
    const float current = slotDamage(hero, pack, active, target);
    int best = active;
    float bestDamage = current;
    std::string each;
    for (int slot = 0; slot < open; ++slot)
    {
        const float d = slot == active ? 0.0f : slotDamage(hero, pack, slot, target);
        each += std::to_string(slot + 1) + "=" + std::to_string(static_cast<int>(slot == active ? current : d)) + " ";
        if (d > bestDamage)
        {
            best = slot;
            bestDamage = d;
        }
    }
    // Now and then what each slot was worth (the analyst's measure of the choice).
    static DWORD s_toldAt = 0;
    if (open > 1 && (!s_toldAt || GetTickCount() - s_toldAt > 120000))
    {
        s_toldAt = GetTickCount();
        LOG("Gear: weapon slots against {:x}: {}(active {})", targetId, each, active + 1);
    }
    // Only for a clear gain (15 %), and not within 20 s of the last switch: no switching back and forth.
    if (best == active || bestDamage < current * 1.15f + 0.5f || GetTickCount() - g_slotSwitchAt < 20000)
    {
        return false;
    }
    g_slotSwitchAt = GetTickCount();
    LOG("Gear: weapon slot {} -> {} against {:x} (expected damage {:.0f} -> {:.0f}; its resistances {:.0f}/{:.0f}/{:.0f}/{:.0f})",
        active + 1, best + 1, targetId, current, bestDamage, resistanceOf(target, 0), resistanceOf(target, 1),
        resistanceOf(target, 2), resistanceOf(target, 3));
    g_switchTo = static_cast<uint32_t>(best);
    Inject::call(&switchSlot, nullptr);
    return true;
}

int Gear::healingLeft()
{
    return g_healing;
}

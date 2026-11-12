#include "mind.h"
#include "hero_profile.h"
#include "game/auto_attack.h"
#include "game/combat_learn.h"
#include "game/gear.h"
#include "game/auto_start.h"
#include "game/agent_save.h"
#include "game/teleports.h"
#include "game/agent_cursor.h"
#include "game/agent_overlay.h"
#include "game/hero_build.h"
#include "game/lore.h"
#include "game/click_walk.h"
#include "game/map_memory.h"
#include "game/whereabouts.h"
#include "game/findings.h"
#include "config/auto_attack.h"
#include "config/controller.h"
#include "config/tactics.h"
#include "game/aim_assist.h"
#include "game/combat_state.h"
#include "game/controller.h"
#include "game/focus.h"
#include "game/frame_hooks.h"
#include "game/modkit_menu.h"
#include "game/tactics.h"
#include "game/granny_async.h"
#include "game/ui_nav.h"
#include "input/inject.h"
#include "log.h"
#include "sacred/engine.h"
#include "sacred/object.h"
#include "sacred/ui.h"

#include <imgui.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <thread>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <vector>

using namespace Sacred;

namespace
{
    // The order a left click on a creature or object gives (the world mouse handler, ENG 006172C0, at 00617935):
    // cOrder's layout with type 7, the picked id at +0x18 and the clicked world point after it. The handler hands
    // it straight to the controlled creature's order handler (vtable +0x18), not through cEngine::sendOrder, which
    // would snap +0x18/+0x1C as a walk target. On an enemy the hero walks up to it and attacks it.
    struct UseOrder
    {
        uintptr_t vtable = Addr::cOrder_vtable;
        uint32_t type = 7;
        uint8_t _08[0x18 - 0x08] = {};
        uint32_t target = 0;
        int32_t x = 0, y = 0;
        uint8_t _24[0x44 - 0x24] = {};
    };
    static_assert(sizeof(UseOrder) == sizeof(cOrder));
    static_assert(offsetof(UseOrder, target) == 0x18 && offsetof(UseOrder, x) == 0x1C && offsetof(UseOrder, y) == 0x20);

    // cCreature's order handler, thiscall (order*), callee pops.
    using OrderHandler = void(__fastcall*)(cCreature* self, void* edx, UseOrder* order);

    // MSVC's RTTI behind every vtable (vtable[-1], the complete object locator): the object's own class.
    struct TypeDescriptor
    {
        const void* vtable;
        void* spare;
        char name[1];
    };
    struct CompleteObjectLocator
    {
        uint32_t signature, offset, cdOffset;
        const TypeDescriptor* type;
        const void* hierarchy;
    };

    enum class Loot : uint8_t
    {
        None,
        Item,       // cItemBase: lying on the ground, a click picks it up
        Container,  // cChest3D: chests, coffers and the like, a click opens them
        Fixed,      // a fixed thing of the map the cursor shows the hand on (id with bit 31): magic hiding places,
                    // wells, gold (not runes and signposts, see fixedUsable); a click uses it. No object behind it, only its place in the pick list.
    };

    // The pick list's ids with bit 31 set are the map's fixed things the cursor can use (the world mouse handler,
    // ENG 00617939, gives them the same order 7 as objects, with the world point under the cursor).
    constexpr uint32_t kFixedId = 0x80000000u;
    bool isFixed(uint32_t id)
    {
        return (id & kFixedId) != 0;
    }

    // The map's fixed things (ENG 0053B713: the low 16 bits of the id index it): records of 0x58 bytes, a kind at +4
    // and a script name at +0x18. Seen in a whole map (2026-11-07): the usable ones, kind 0x00010000, are VERSTECK
    // (magic hiding places), BRUNNEN (wells) and F_Gold; the rest, kind 0.25f, are scripts and texts: F_Stein (runes),
    // Wegweiser (signposts), setvar triggers, Powerup. Only the usable ones are taken: a text is not loot.
    constexpr uintptr_t kFixedBegin = 0xAB45B8, kFixedEnd = 0xAB45BC;
    constexpr size_t kFixedSize = 0x58, kFixedKind = 0x04, kFixedName = 0x18;
    constexpr uint32_t kFixedUsable = 0x00010000;

    size_t fixedCount()
    {
        const auto* begin = *reinterpret_cast<const uint8_t* const*>(kFixedBegin);
        const auto* end = *reinterpret_cast<const uint8_t* const*>(kFixedEnd);
        return begin && end >= begin ? static_cast<size_t>(end - begin) / kFixedSize : 0;
    }

    const uint8_t* fixedRecord(uint32_t id)
    {
        const size_t index = id & 0xFFFF;
        return index < fixedCount() ? *reinterpret_cast<const uint8_t* const*>(kFixedBegin) + index * kFixedSize
                                    : nullptr;
    }

    // Where the fixed thing stands (world): its tile (+0, +2), where the game sends a creature using it (ENG 0053B784).
    constexpr double kFixedTile = 53.66563;
    bool fixedPosition(uint32_t id, int32_t& x, int32_t& y)
    {
        const uint8_t* record = fixedRecord(id);
        if (!record)
        {
            return false;
        }
        x = static_cast<int32_t>((*reinterpret_cast<const uint16_t*>(record) + 0.5) * kFixedTile);
        y = static_cast<int32_t>((*reinterpret_cast<const uint16_t*>(record + 2) + 0.5) * kFixedTile);
        return true;
    }

    // What the bookkeeping below keys loot by: an object's id, a fixed thing's tile (the game drops a used hiding
    // place from the table, which moves the later ones down: their ids change).
    uint32_t lootKey(uint32_t id)
    {
        int32_t x = 0, y = 0;
        if (!isFixed(id) || !fixedPosition(id, x, y))
        {
            return id;
        }
        const uint32_t tx = static_cast<uint32_t>(x / kFixedTile) & 0x7FFF, ty = static_cast<uint32_t>(y / kFixedTile) & 0xFFFF;
        return kFixedId | (tx << 16) | ty;
    }

    // Stones with the hand cursor (F_Stein): runes, plaques and gravestones (seen live, 2026-11-09: a click opens
    // the gravestone's dialog, "Abrir" / "Salir"; "Abrir" gives a second one with the outcome, "Aceptar": empty, an
    // item, or enemies). Each is tried once: the dialogs are answered with their first answer (Enter), and a stone
    // with only text closes the same way.
    bool fixedStone(const uint8_t* record)
    {
        return record && std::memcmp(record + kFixedName, "F_Stein", 7) == 0;
    }

    bool fixedStone(uint32_t id)
    {
        return isFixed(id) && fixedStone(fixedRecord(id));
    }

    bool fixedUsable(uint32_t id)
    {
        const uint8_t* record = fixedRecord(id);
        if (!record)
        {
            return false;
        }
        const bool usable =
            *reinterpret_cast<const uint32_t*>(record + kFixedKind) == kFixedUsable || fixedStone(record);
        static std::unordered_set<uint32_t> logged;
        if (logged.insert(id).second && logged.size() < 512)
        {
            char name[0x40 + 1] = {};
            std::memcpy(name, record + kFixedName, 0x40);
            LOG("AutoAttack: fixed thing {} ({:x}) is {}", name, id, usable ? "taken" : "left alone");
        }
        return usable;
    }

    // The pick category the world pick ranks by (cObject::Vtable::pickCategory): Alt puts 2 first, the things lying
    // on the ground that Alt shows the names of.
    constexpr uint16_t kPickItem = 2;

    // The object's flags (cObject +0x14). Bit 0 marks fixtures the game still ranks as ground items: signposts and
    // plaques (cSpecial3D like gold and quest items, whose click only opens their text). Seen: signpost 04000001;
    // weapons, rings, gold 04000000, gold a monster dropped 04100000.
    constexpr size_t kObjectFlags = 0x14;
    constexpr uint32_t kFixtureFlag = 0x1;

    // What the object is: a cChest3D (its own class, by RTTI; it derives from cItemBase) is a container; anything
    // else the game ranks as a ground item is an item, whatever its class (gold included), but fixtures.
    Loot lootOf(cObject* object)
    {
        const void* vtable = object->vtable;
        const auto* locator = reinterpret_cast<const CompleteObjectLocator* const*>(vtable)[-1];
        const char* name = locator && locator->type ? locator->type->name : "?";
        const uint16_t category = object->vtable->pickCategory(object, nullptr, 1, 0);
        Loot loot = Loot::None;
        if (std::strcmp(name, ".?AVcChest3D@@") == 0)
        {
            loot = Loot::Container;
        }
        else if (category == kPickItem)
        {
            const uint32_t flags = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(object) +
                                                                     kObjectFlags);
            // A quest item (0x800, Gear's kQuestItem) is taken though it carries the fixture bit too (the player,
            // 2026-11-12: A5 walked to a shining TYPE_OBJECT_QI_WATERSKIN, flags 04000801, on its quest's goal and
            // never picked it up).
            loot = (flags & kFixtureFlag) && !(flags & 0x800) ? Loot::None : Loot::Item;
        }
        // Each class and category once, to see in the log what is taken and what is not.
        static std::unordered_set<uint64_t> logged;
        if (logged.insert(reinterpret_cast<uintptr_t>(vtable) | (static_cast<uint64_t>(category) << 32)).second)
        {
            LOG("AutoAttack: class {} category {} (id {:x}) is {}", name, category, object->id,
                loot == Loot::Item ? "an item" : loot == Loot::Container ? "a container" : "left alone");
        }
        return loot;
    }

    // Equipment by its class (RTTI): weapons, armour, rings, amulets. Only these get the grade filter: gold, potions,
    // scrolls, runes and upgrades are always taken (the player, 2026-11-09: «se salta hasta el oro»).
    bool isEquipment(const cObject* object)
    {
        const auto* locator = reinterpret_cast<const CompleteObjectLocator* const*>(object->vtable)[-1];
        const char* name = locator && locator->type ? locator->type->name : "";
        return std::strcmp(name, ".?AVcWeapon3D@@") == 0 || std::strcmp(name, ".?AVcArmor3D@@") == 0 ||
               std::strcmp(name, ".?AVcRing3D@@") == 0 || std::strcmp(name, ".?AVcShield3D@@") == 0 ||
               std::strstr(name, "Amulet") != nullptr;
    }

    // Loot the hero failed at: opened containers, items it stood at without picking them up (a full inventory) or
    // could not reach. Tried again later, a few times, as the hero may have been busy or the way blocked.
    struct LootFailure
    {
        DWORD retryAt = 0;
        int count = 0;
    };
    std::unordered_map<uint32_t, LootFailure> g_lootFailures;
    // Not reached (behind a fence, across a river): tried again a minute later (then two), up to kLootTries in all.
    // Still there with the hero at it (a pick up cut short by a blow, a nudge lost): again after kLootSoonMs. Seen
    // live (2026-11-09): a weapon left for good after one such try, the backpack with room.
    constexpr int kLootTries = 4;
    constexpr DWORD kLootRetryMs = 60000;
    constexpr DWORD kLootSoonMs = 4000;

    bool lootBlocked(uint32_t id, DWORD now)
    {
        id = lootKey(id);
        const auto it = g_lootFailures.find(id);
        return it != g_lootFailures.end() &&
               (it->second.count >= kLootTries || static_cast<int>(now - it->second.retryAt) < 0);
    }

    // Loot the hero stood at and that stayed: an opened container, an item that did not fit. Never gone for again; nor loot
    // not reached four times.
    std::unordered_set<uint32_t> g_lootDone;

    void lootFailed(uint32_t id, DWORD now, bool soon = false)
    {
        id = lootKey(id);
        if (g_lootFailures.size() > 1024)
        {
            g_lootFailures.clear();
        }
        LootFailure& f = g_lootFailures[id];
        ++f.count;
        // Three times as long each time, left for good at the fourth (the analyst, 2026-11-10: the same loot «not
        // reached» again and again in a few minutes).
        DWORD wait = kLootRetryMs;
        for (int i = 1; i < f.count && wait < 1800000; ++i)
        {
            wait *= 3;
        }
        f.retryAt = now + (soon ? kLootSoonMs : wait);
        if (!soon && f.count >= 4)
        {
            g_lootDone.insert(id);
        }
    }

    // The hero gets no nearer to the loot for this long: it cannot reach it (behind a fence, in a house).
    constexpr DWORD kLootStuckMs = 6000;
    // How long it may stand at the loot before we leave it.
    constexpr DWORD kLootAtMs = 3000;
    // A loot order is given once (giving it again restarts the pick up, so gold farther than a second away was
    // never taken); again only after this long without the hero getting nearer.
    constexpr DWORD kLootNudgeMs = 1200;
    // Loot gone from the ground is being picked up: no new order for this long, which would cut the hero's
    // stooping short and cancel the pick up.
    constexpr DWORD kPickupMs = 1300;
    constexpr float kLootAtDistance = 2.5f * 53.66563f;   // world units, 2.5 tiles

    bool g_on = false;
    bool g_keyWasDown = false;
    uint32_t g_target = 0;          // the enemy we attack or the loot we go for, 0 for none
    bool g_looting = false;         // g_target is loot
    // Loot and fights weigh alike (the user, 2026-11-09): on each look, loot nearer to the hero than the chosen enemy
    // is taken first, enemies about or not. Loot taken that way is one piece, then
    // the fight again (g_collectOne), a hit meanwhile not ending it.
    bool g_lootInstead = false;
    bool g_lootInsteadAll = false;  // ... all of it (nobody attacks), not one piece and back to the fight
    bool g_collectOne = false;
    DWORD g_lootAtSince = 0;        // since when the hero stands at it, 0 while it does not
    bool g_lootStill = false;       // lootDone gave up on it with the hero at it and the thing still there
    bool g_lootStale = false;       // ... or its id now names another fixed thing (the table shifted): nothing marked
    float g_lootBest = 0.0f;        // the nearest the hero got to it, world units
    DWORD g_lootBestAt = 0;         // when it got that near
    DWORD g_sentAt = 0;             // when the last order went out
    DWORD g_quietUntil = 0;         // the player's own clicks: no auto attack before this tick
    DWORD g_pickupUntil = 0;        // a pick up under way: no new order before this tick

    // The physical state: SacredBild's GetAsyncKeyState is not the game's patched import, so injected input and
    // the controller never show here.
    bool physicallyDown(int vk)
    {
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }

    // A mouse button held: the player gives their own orders. Moving the mouse does not count, so the cursor stays
    // free for the inventory and other windows while the hero fights.
    bool playerClicks()
    {
        return physicallyDown(VK_LBUTTON) || physicallyDown(VK_RBUTTON) || physicallyDown(VK_MBUTTON);
    }

    bool heroAlive()
    {
        int health = 0, maximum = 0;
        return AimAssist::heroHealth(health, maximum) && health > 0;
    }

    // The hero casts a combat art or a spell (the player's own, when Tactics is not busy): any order now, an attack,
    // a walk or a pick up, cuts it short.
    bool heroCasting()
    {
        cObjectManager* objects = cObjectManager::instance();
        return objects && CombatState::casting(objects->hero());
    }

    // All the loot seen is taken before the hero goes on, on a trip too (the player's wish, 2026-11-09: none left
    // behind but when it does not fit). Never further than kLootMaxTiles (a fixed thing's place read wrong put one
    // 3000 tiles off). g_questTravel: the hero is on a trip.
    bool g_questTravel = false;
    constexpr double kLootMaxTiles = 60.0;
    // On a trip (a quest's goal, a merchant) the focus is the trip (the player, 2026-11-10: «si pretende hacer quest el
    // foco no es batallar necesariamente»): only loot at hand is taken on the way.
    constexpr double kTripLootTiles = 8.0;

    // What the agent is about and why (the player, 2026-11-10: «en cada cosa que hace tiene que saber el motivo de por
    // qué lo hace; si pretende farmear, el foco no es ir a la quest»). Set by the branch that drives the hero, logged on
    // each change with its reason; the branches that would pull the hero elsewhere (loot, seeking enemies, quests while
    // farming) read it.
    enum class Intent
    {
        Explore,
        Farm,
        Quest,
        Shop,
        Recover,
        Escape,
    };
    Intent g_intent = Intent::Explore;
    std::string g_intentWhy;
    DWORD g_intentSince = 0;

    const char* intentName(Intent i)
    {
        switch (i)
        {
        case Intent::Farm: return "farm";
        case Intent::Quest: return "quest";
        case Intent::Shop: return "shop";
        case Intent::Recover: return "recover";
        case Intent::Escape: return "escape";
        default: return "explore";
        }
    }

    // The agent's last decisions, for its mind map (the player, 2026-11-10: «cómo el agente está pensando»).
    std::vector<std::string> g_decisions;

    void remember(const std::string& what)
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char at[16];
        std::snprintf(at, sizeof at, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
        g_decisions.push_back(at + what);
        if (g_decisions.size() > 8)
        {
            g_decisions.erase(g_decisions.begin());
        }
    }

    void setIntent(Intent i, const std::string& why)
    {
        if (i == g_intent && why == g_intentWhy)
        {
            return;
        }
        // Logged on each change of intent; a new reason for the same one at most every 30 s (the log stays readable).
        static DWORD s_toldAt = 0;
        const DWORD tick = GetTickCount();
        if (i != g_intent)
        {
            g_intentSince = tick;
        }
        if (i != g_intent || tick - s_toldAt > 30000)
        {
            s_toldAt = tick;
            LOG("Intent: {} ({})", intentName(i), why);
            remember(std::string("intención ") + intentName(i) + ": " + why);
        }
        g_intent = i;
        g_intentWhy = why;
    }

    bool lootNear(int32_t x, int32_t y)
    {
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        if (!hero)
        {
            return false;
        }
        const double tiles = std::max(std::abs(static_cast<double>(x - hero->x)), std::abs(static_cast<double>(y - hero->y))) /
                             kFixedTile;
        // All of it before going on; on a trip only what lies at hand (the player's newer wish: the focus is the trip).
        return tiles <= (g_questTravel ? kTripLootTiles : kLootMaxTiles);
    }

    // Doors (AGENT_ADDRESSES «Puertas y entradas»: TYPE_OBJECT_DOOR_* 0x1612..0x1697 and TYPE_OBJECT_S_DOOR 0x1465,
    // cObject3D; the village's open with a click). Only shut ones a key or a click opens (their trigger, below): a door is
    // used, as the player's click on it does, when the hero is stuck, the nearest one not tried for 2 minutes.
    std::unordered_map<uint32_t, DWORD> g_doorTried;
    // After a click on a door, the hero is let walk to it and use it (a diagnosis of the logs, 2026-11-10: the Daemon
    // 14 min in a merchant's house, a Seraphim 10 min in a cave: each door click overridden 0.13-3.6 s later by the
    // roam's, the quest's or the shop's own walk, the door never reached).
    DWORD g_doorHoldUntil = 0;
    DWORD g_craftHoldUntil = 0;     // by a craftsman until then: the roam and the stray dialog's Enter wait
    float g_doorRange = 12.0f * 53.66563f;

    bool acceptDoor(uint32_t id, void*)
    {
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        cObject* o = objects && !isFixed(id) ? objects->object(id) : nullptr;
        if (!o || !hero || o->asCreature())
        {
            return false;
        }
        const uint16_t type = *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(o) + 0x10);
        // A lever (LEVER01 0x17CD) too: used as a door is, it opens its own trigger, and the script then frees the
        // door it holds (Manolito, block K); one already pulled (its trigger open) is passed by.
        const bool lever = type == 0x17CD;
        if (!((type >= 0x1612 && type <= 0x1697) || type == 0x1465 || lever))
        {
            return false;
        }
        // Noted for the routes (MapMemory::noteDoor): opens or not, by its trigger's state below.
        const int doorLayer = *(reinterpret_cast<const uint8_t*>(hero) + 0x24);
        const int doorX = static_cast<int>(o->x / 53.66563), doorY = static_cast<int>(o->y / 53.66563);  // kTileSize, defined further on
        bool shutForGood = false;
        // Its state, in its trigger record (Manolito, block K, AGENT_ADDRESSES «Puertas y palancas»): door+0x34 the
        // trigger's index, the records [world+0x268] (end [world+0x26C]) 0x10 each, world = [[0xAD5C40]]; +0xA u16
        // bit 0 open, 0x8000 locked by a script or lever; +0xC u32 the key it asks (the game looks in the backpack).
        // Open ones and those a script holds shut are passed by.
        if (const uintptr_t holder = *reinterpret_cast<const uintptr_t*>(0xAD5C40))
        {
            if (const uintptr_t world = *reinterpret_cast<const uintptr_t*>(holder))
            {
                const uintptr_t begin = *reinterpret_cast<const uintptr_t*>(world + 0x268);
                const uintptr_t end = *reinterpret_cast<const uintptr_t*>(world + 0x26C);
                const uint32_t index = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(o) + 0x34);
                const uintptr_t trig = begin + static_cast<uintptr_t>(index) * 0x10;
                if (begin && end > begin && index < 0x100000 && trig + 0x10 <= end)
                {
                    const uint16_t state = *reinterpret_cast<const uint16_t*>(trig + 0xA);
                    const uint32_t key = *reinterpret_cast<const uint32_t*>(trig + 0xC);
                    // 0x4000 is no lock: «just switched», gone in a second (Manolito).
                    // A key asked counts as shut for the routes: the backpack may not hold it.
                    shutForGood = !(state & 1) && ((state & 0x8000) || key);
                    if (!lever)
                    {
                        MapMemory::noteDoor(doorLayer, doorX, doorY, !shutForGood);
                    }
                    if ((state & 1) || (!lever && (state & 0x8000) && !key))
                    {
                        return false;
                    }
                }
            }
        }
        const auto it = g_doorTried.find(id);
        if (it != g_doorTried.end() && GetTickCount() - it->second < 120000)
        {
            // Tried and still shut a few seconds on: no way through for the routes either.
            if (!lever && GetTickCount() - it->second > 5000)
            {
                MapMemory::noteDoor(doorLayer, doorX, doorY, false);
            }
            return false;
        }
        const float dx = static_cast<float>(o->x - hero->x), dy = static_cast<float>(o->y - hero->y);
        return dx * dx + dy * dy <= g_doorRange * g_doorRange;
    }

    bool acceptLoot(uint32_t id, void*)
    {
        if (lootBlocked(id, GetTickCount()) || g_lootDone.count(lootKey(id)))
        {
            return false;
        }
        if (isFixed(id))
        {
            int32_t x = 0, y = 0;
            return fixedPosition(id, x, y) && lootNear(x, y) && fixedUsable(id);
        }
        cObjectManager* objects = cObjectManager::instance();
        cObject* object = objects ? objects->object(id) : nullptr;
        if (object && !lootNear(object->x, object->y))
        {
            return false;
        }
        const Loot loot = object ? lootOf(object) : Loot::None;
        // A container already opened (object flags +0x14 & 0x4000, seen live 2026-11-09: 0x94000001 shut, 0x9400C001
        // open; also after a load): nothing in it any more.
        if (loot == Loot::Container && (*reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(object) + 0x14) & 0x4000))
        {
            return false;
        }
        // Equipment below the grade asked for is left lying (the player's wish, 2026-11-09: no junk in the backpack);
        // set pieces as asked; potions, runes, gold and the rest always.
        // The grade filter only with gold enough (the player's wish, 2026-11-09): low grades are worth gold at a
        // merchant, and early on the basic gear is what the hero puts on. Enough: so much per hero level. The hero's
        // level: u16 at +0x3A8 + 0x56; the purse: u32 at +0x3A8 + 0x46 (Gear's kGold).
        const cCreature* hero = objects ? objects->hero() : nullptr;
        const uint8_t* heroBytes = reinterpret_cast<const uint8_t*>(hero);
        const int heroLevel = hero ? *reinterpret_cast<const uint16_t*>(heroBytes + 0x3A8 + 0x56) : 0;
        const uint32_t heroGold = hero ? *reinterpret_cast<const uint32_t*>(heroBytes + 0x3A8 + 0x46) : 0;
        const bool goldEnough = hero && static_cast<uint64_t>(heroGold) >=
                                            static_cast<uint64_t>(Config::autoAttack.rarityGoldPerLevel) * std::max(heroLevel, 1);
        const bool droppedGold = object && (*reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(object) + 0x14) & 0x00100000);
        if (loot == Loot::Item && goldEnough && !droppedGold && isEquipment(object) &&
            (Config::autoAttack.minRarity > 0 || !Config::autoAttack.keepSets))
        {
            const int grade = Gear::grade(id, true);
            const bool keep = grade < 0 || (grade == 4 ? Config::autoAttack.keepSets || Config::autoAttack.minRarity == 0
                                                       : grade >= Config::autoAttack.minRarity);
            if (!keep)
            {
                static std::unordered_set<uint32_t> s_told;
                if (s_told.size() > 512)
                {
                    s_told.clear();
                }
                if (s_told.insert(id).second)
                {
                    static const char* kNames[] = {"white", "blue", "yellow", "gold", "set"};
                    LOG("AutoAttack: item {:x} ({}) below the grade asked for, left lying ({} gold, level {})", id,
                        kNames[grade], heroGold, heroLevel);
                }
                return false;
            }
        }
        // The backpack full: items are left lying (none fits; seen live, 2026-11-09: each tried again and again on the
        // way to the merchant), but dropped gold (flags +0x14 & 0x00100000), which goes to the purse.
        if (loot == Loot::Item && Gear::packFull() &&
            !(*reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(object) + 0x14) & 0x00100000))
        {
            return false;
        }
        return loot == Loot::Item || loot == Loot::Container;
    }

    bool isCurrent(uint32_t id, void*)
    {
        return id == g_target;
    }

    // Magic hiding places (VERSTECK) have no picture and are not in the pick list: the cursor finds them by the tile
    // under it, shows the hand and their name. Gone for by their record's tile, within LootRange / 40 tiles (about
    // the screen pixels LootRange stands for) on either axis.
    bool findHidingPlace(uint32_t& out)
    {
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        const size_t count = fixedCount();
        if (!hero || !count)
        {
            return false;
        }
        const double reach = Config::autoAttack.lootRange / 40.0 * kFixedTile;
        double best = 1e30;
        for (size_t i = 0; i < count; ++i)
        {
            const uint32_t id = kFixedId | static_cast<uint32_t>(i);
            const uint8_t* record = fixedRecord(id);
            if (std::memcmp(record + kFixedName, "VERSTECK", 9) != 0)
            {
                continue;
            }
            int32_t x = 0, y = 0;
            fixedPosition(id, x, y);
            const double dx = x - hero->x, dy = y - hero->y;
            if (std::abs(dx) > reach || std::abs(dy) > reach || dx * dx + dy * dy >= best || !lootNear(x, y) ||
                lootBlocked(id, GetTickCount()) || g_lootDone.count(lootKey(id)))
            {
                continue;
            }
            best = dx * dx + dy * dy;
            out = id;
        }
        return best < 1e30;
    }

    // The loot on the agent's radar (what the collect goes for, done ones left out): items and containers in the
    // world pick (the screen and a margin around), the usable fixed things (hiding places, wells, gold) within 60
    // tiles. Between beginRadar and endRadar.
    bool radarPick(uint32_t id, void*)
    {
        if (g_lootDone.count(lootKey(id)) || isFixed(id))
        {
            return false;
        }
        cObjectManager* objects = cObjectManager::instance();
        cObject* object = objects ? objects->object(id) : nullptr;
        const Loot loot = object ? lootOf(object) : Loot::None;
        if (loot == Loot::Item || loot == Loot::Container)
        {
            MapMemory::addBlip(loot == Loot::Item ? MapMemory::Blip::Item : MapMemory::Blip::Chest, object->x, object->y);
        }
        return false;   // never chosen: only seen
    }

    void radarLoot()
    {
        AimAssist::Target unused;
        AimAssist::findObject(4000.0f, &radarPick, nullptr, unused);
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        const size_t count = fixedCount();
        const double reach = 60 * kFixedTile;
        for (size_t i = 0; hero && i < count; ++i)
        {
            const uint32_t id = kFixedId | static_cast<uint32_t>(i);
            const uint8_t* record = fixedRecord(id);
            int32_t x = 0, y = 0;
            if (!record || *reinterpret_cast<const uint32_t*>(record + kFixedKind) != kFixedUsable ||
                !fixedPosition(id, x, y) || std::abs(x - hero->x) > reach || std::abs(y - hero->y) > reach ||
                g_lootDone.count(lootKey(id)))
            {
                continue;
            }
            MapMemory::addBlip(MapMemory::Blip::Hiding, x, y);
        }
    }

    // The nearest loot: on the screen (items, containers, wells) or a hiding place.
    bool findLoot(uint32_t& out)
    {
        AimAssist::Target t;
        if (AimAssist::findObject(static_cast<float>(Config::autoAttack.lootRange), &acceptLoot, nullptr, t))
        {
            out = t.id;
            return true;
        }
        return findHidingPlace(out);
    }

    // Whether the hero is done with the loot it goes for: it gives up after a while, or after standing at it (an
    // opened container stays, and so does an item when the inventory is full).
    int32_t g_fixedX = 0, g_fixedY = 0;  // where the fixed thing gone for stands (world), see attack

    bool lootDone(uint32_t id, DWORD now)
    {
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        cObject* loot = objects && !isFixed(id) ? objects->object(id) : nullptr;
        if (!hero || (!loot && !isFixed(id)))
        {
            return true;
        }
        // A fixed thing's table shifts when the game removes one of its entries (a used stone): the id then names
        // another record (the review of the findings, 2026-11-09: «not reached» at 111 to 4366 tiles, F_Stein_0011
        // e8 -> e7 in 22 s). Its place read again: moved off, it is left at once, and nothing is marked for it.
        if (!loot)
        {
            int32_t fx = 0, fy = 0;
            if (!fixedPosition(id, fx, fy) || std::abs(fx - g_fixedX) > 3 * 53.66563f || std::abs(fy - g_fixedY) > 3 * 53.66563f)
            {
                LOG("AutoAttack: fixed thing {:x} is another one now (its table shifted), left", id);
                g_lootStale = true;
                return true;
            }
        }
        // Any dialog open (a quest talk on the way, a stone's text): the loot's clocks stand still (the review of the
        // findings: «not reached» after 12 s of a quest talk, the hero standing).
        if (UiNav::npcDialogOpen())
        {
            g_lootBestAt = now;
            if (g_lootAtSince)
            {
                g_lootAtSince = now;
            }
            return false;
        }
                const int32_t lx = loot ? loot->x : g_fixedX, ly = loot ? loot->y : g_fixedY;
        const float dx = static_cast<float>(lx - hero->x), dy = static_cast<float>(ly - hero->y);
        const float distance = std::sqrt(dx * dx + dy * dy);
        // Progress: half a tile nearer than ever before.
        if (!g_lootBestAt || distance < g_lootBest - 0.5f * 53.66563f)
        {
            g_lootBest = distance;
            g_lootBestAt = now;
        }
        if (distance > kLootAtDistance)
        {
            g_lootAtSince = 0;
            if (static_cast<int>(now - g_lootBestAt) > static_cast<int>(kLootStuckMs))
            {
                LOG("AutoAttack: loot {:x} not reached, left", id);
            Findings::note("loot", hero->x, hero->y, "loot {:x} not reached ({:.1f} tiles left)", id, distance / 53.66563f);
                return true;
            }
            return false;
        }
        if (!g_lootAtSince)
        {
            g_lootAtSince = now;
        }
        // A container the hero opened (flags +0x14 & 0x4000): done once its lid is up (a moment for the spill), its
        // contents on the ground for the next collect.
        if (loot && lootOf(loot) == Loot::Container &&
            (*reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(loot) + 0x14) & 0x4000) &&
            now - g_lootAtSince > 800)
        {
            LOG("AutoAttack: container {:x} opened", id);
            return true;
        }
        // A well stays where it is once used (it heals, or nothing with full health): done once the hero stood at it
        // after the use order (seen live, 2026-11-09: the abbey's well «still there» again and again).
        if (!loot && static_cast<int>(now - g_lootAtSince) > 1500 && static_cast<int>(g_sentAt - g_lootAtSince) >= 0)
        {
            const uint8_t* record = fixedRecord(id);
            if (record && std::memcmp(record + kFixedName, "BRUNNEN", 7) == 0)
            {
                LOG("AutoAttack: well {:x} used", id);
                return true;
            }
        }
        if (static_cast<int>(now - g_lootAtSince) > static_cast<int>(kLootAtMs))
        {
            LOG("AutoAttack: loot {:x} still there with the hero at it, again in {} s", id, kLootSoonMs / 1000);
            Findings::note("loot", hero->x, hero->y, "loot {:x} still there with the hero at it ({:.1f} tiles)", id,
                           distance / 53.66563f);
            g_lootStill = true;
            return true;
        }
        return false;
    }

    // The game's path finders search 64x64 tiles around the walker (ClickWalk): loot further out on a tile axis
    // (a well across a river, 40 tiles up a 32:9 screen) is walked to first, with a walk order, which ClickWalk
    // routes in legs; once within reach, the next order (lootDone's nudge when the hero stops) uses it.
    constexpr double kTileSize = 53.66563;
    constexpr int32_t kUseReach = static_cast<int32_t>(24 * kTileSize);

    // How far the hero walks from one point to another, in world units (the player, 2026-11-11: «que paren de medir
    // cercanía con la distancia en línea recta, … la distancia de la ruta entera»): the route's length over what the
    // map knows (roads, doors that open, walls and doors held shut), once worked out on its own thread; until then, or
    // with no route (too far apart for one search), the straight line.
    double wayLength(int32_t fromX, int32_t fromY, int32_t toX, int32_t toY)
    {
        const double straight = std::hypot(static_cast<double>(toX - fromX), static_cast<double>(toY - fromY));
        cObjectManager* objects = cObjectManager::instance();
        const cCreature* hero = objects ? objects->hero() : nullptr;
        const int layer = hero ? *(reinterpret_cast<const uint8_t*>(hero) + 0x24) : 0;
        const float tiles = MapMemory::routeTiles(layer, static_cast<int>(fromX / kTileSize),
                                                  static_cast<int>(fromY / kTileSize), static_cast<int>(toX / kTileSize),
                                                  static_cast<int>(toY / kTileSize));
        return tiles >= 0.0f ? std::max(straight, tiles * kTileSize) : straight;
    }

    int32_t g_walkX = 0, g_walkY = 0;
    DWORD g_roamAt = 0;     // the last roaming step (0: not roaming)

    // On the window's thread, as the player's clicks (HeroMove).
    void walkThere(void*)
    {
        cEngine* engine = FrameHooks::engine();
        cObjectManager* objects = cObjectManager::instance();
        cCreature* self = engine && objects ? objects->creature(engine->controlled) : nullptr;
        // Riding, to the hero all the same: his handler passes a move on to the horse, which swallows direct orders
        // (Manolito, phase 2, 2026-11-12: 0x52E590; the horse with a rider, +0x200 & 4, takes none).
        if (cCreature* hero = objects ? objects->hero() : nullptr;
            hero && *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + 0x1EC))
        {
            self = hero;
        }
        if (!self)
        {
            return;
        }
        cOrder order;
        order.type = cOrder::move;
        order.mode = cOrder::walk;
        order.x = g_walkX;
        order.y = g_walkY;
        order.layer = reinterpret_cast<const uint8_t*>(self)[0x24];     // the walker's layer (Manolito, block AF)
        // The path-finding walk to the spot a click gives (follow 0). follow 1 is hold-to-walk: it heads for the
        // cursor and ends within a tenth of a second unless the left button is held (seen live: walking, then 2,
        // then standing, not a pixel moved).
        order.follow = 0;
        engine->sendOrder(self, order);
    }

    // The agent's walk order, once the game's cursor is on the spot (AgentCursor; at once with the option off).
    void fireWalk(void*)
    {
        Inject::call(&walkThere, nullptr);
    }
    void sendWalk()
    {
        AgentCursor::act({0, g_walkX, g_walkY}, &fireWalk, nullptr);
    }

    // True if `self` is too far from (x, y) for the use order: a walk order went out instead.
    bool walkFirst(cCreature* self, int32_t x, int32_t y)
    {
        if (std::abs(x - self->x) <= kUseReach && std::abs(y - self->y) <= kUseReach)
        {
            return false;
        }
        // A spot far off the loot range is a stale position (a fixed thing's table shifts when the game removes one
        // of its entries): no walk across the world for it; the stalled pick up leaves it.
        if (std::abs(x - self->x) > 60 * kTileSize || std::abs(y - self->y) > 60 * kTileSize)
        {
            return true;
        }
        // Once per destination: the walk order goes out again at each nudge (about every 1.2 s) of a long walk.
        static int32_t loggedX = 0, loggedY = 0;
        if (x != loggedX || y != loggedY)
        {
            loggedX = x;
            loggedY = y;
            LOG("AutoAttack: loot at {},{} is far ({:.0f},{:.0f} tiles), walking there first", x, y,
                (x - self->x) / kTileSize, (y - self->y) / kTileSize);
        }
        g_walkX = x;
        g_walkY = y;
        sendWalk();
        return true;
    }

    // The A key's «collect» (Manolito's block U3, 2026-11-10; the player: «al apretar A el personaje recogía sin salir
    // del caballo»): a cEvent 0x113 with the player's id at +0xC to the hero's receiveEvent puts it in its pick up mode
    // (+0xFC = 0xC), which walks to the nearest loot and takes it, mounted or not.
    void sendCollect(void*)
    {
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        const uint8_t* player = *reinterpret_cast<const uint8_t* const*>(0x182EBE8);
        if (!hero || !player)
        {
            return;
        }
        alignas(8) uint8_t ev[0x44] = {};
        *reinterpret_cast<uintptr_t*>(ev) = 0x89095C;
        *reinterpret_cast<uint32_t*>(ev + 4) = 0x113;
        *reinterpret_cast<uint32_t*>(ev + 0xC) = *reinterpret_cast<const uint32_t*>(player + 0x14);
        using Handle = void(__fastcall*)(void* self, void*, void* ev);
        const auto* vt = *reinterpret_cast<const uintptr_t* const*>(hero);
        reinterpret_cast<Handle>(vt[6])(hero, nullptr, ev);
    }

    // Orders the creature the mouse commands (the hero, or its mount) to attack, pick up or open `id`, as a click
    // on it does.
    bool attack(uint32_t id);
    bool g_cursorFiring = false;    // attack() called back by AgentCursor, the cursor on the target
    void fireAttack(void* id)
    {
        g_cursorFiring = true;
        attack(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(id)));
        g_cursorFiring = false;
    }
    // The use order (a left click on `id`), once the game's cursor is on it; at once with the option off.
    bool clickOn(cCreature* self, UseOrder& order)
    {
        if (!g_cursorFiring && AgentCursor::active())
        {
            AgentCursor::act({order.target, order.x, order.y}, &fireAttack,
                             reinterpret_cast<void*>(static_cast<uintptr_t>(order.target)));
            return true;
        }
        const auto handler = reinterpret_cast<const OrderHandler*>(self->vtable)[6];
        handler(self, nullptr, &order);
        return true;
    }

    bool attack(uint32_t id)
    {
        cEngine* engine = FrameHooks::engine();
        cObjectManager* objects = cObjectManager::instance();
        if (!engine || !objects)
        {
            return false;
        }
        cCreature* self = objects->creature(engine->controlled);
        cCreature* rider = objects->hero();
        // Riding: the hero's own mount field (Manolito AD/AG), not «controlled is not the hero» (half off counts so too).
        const bool riding = rider && *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(rider) + 0x1EC) != 0;
        if (self && isFixed(id))
        {
            // A well, a hiding place: used on foot (the horse uses nothing; the reviewer, 2026-11-12).
            if (riding)
            {
                AutoAttack::footFor(15000);
                return true;
            }
            if (rider)
            {
                self = rider;
            }
            // As a click on it: the order 7 with its id and where it stands.
            int32_t x = 0, y = 0;
            if (!fixedPosition(id, x, y))
            {
                return false;
            }
            // Beyond the loot range: a stale id (its table shifted), not a thing to walk to.
            if (std::abs(x - self->x) > 60 * kTileSize || std::abs(y - self->y) > 60 * kTileSize)
            {
                LOG("AutoAttack: fixed thing {:x} reads {:.0f} tiles off: a stale id, not gone for", id,
                    std::max(std::abs(x - self->x), std::abs(y - self->y)) / kTileSize);
                return false;
            }
            g_fixedX = x;
            g_fixedY = y;
            if (walkFirst(self, x, y))
            {
                return true;
            }
            UseOrder order;
            order.target = id;
            order.x = x;
            order.y = y;
            return clickOn(self, order);
        }
        cObject* target = objects->object(id);
        if (!self || !target)
        {
            return false;
        }
        // Mounted, a click on a creature goes to the rider, not the mount (seen in the orders the world mouse
        // handler gives): the horse would only walk up to it.
        if (rider && rider != self && target->asCreature())
        {
            self = rider;
        }
        // Mounted, loot on the ground is taken as the A key takes it, without getting off (once a second at most);
        // a door, a chest or anything else is used on foot (the reviewer: the A key opens none of them).
        else if (riding && !target->asCreature() && lootOf(target) != Loot::Item)
        {
            AutoAttack::footFor(15000);
            return true;
        }
        else if (riding && !target->asCreature())
        {
            static DWORD s_collectAt = 0;
            const DWORD now = GetTickCount();
            if (now - s_collectAt > 1000)
            {
                s_collectAt = now;
                LOG("AutoAttack: mounted: collecting {:x} as the A key does", id);
                Inject::call(&sendCollect, nullptr);
            }
            return true;
        }
        if (!target->asCreature() && walkFirst(self, target->x, target->y))
        {
            return true;
        }
        UseOrder order;
        order.target = id;
        order.x = target->x;
        order.y = target->y;
        return clickOn(self, order);
    }

    // Natural pace: a look at the situation every so often, a moment before acting on a new aim, orders repeated at
    // slightly uneven intervals.
    constexpr DWORD kThinkMinMs = 180, kThinkMaxMs = 340;
    constexpr DWORD kIdleThinkMinMs = 100, kIdleThinkMaxMs = 180;   // no enemy aimed at: a new one is seen sooner
    constexpr DWORD kReactMinMs = 220, kReactMaxMs = 480;
    constexpr DWORD kReactUrgentMinMs = 90, kReactUrgentMaxMs = 220;
    // Hurt this recently: the hero is under attack.
    constexpr DWORD kHurtMs = 4000;
    constexpr int kMaxFoes = 64;
    // Enemies attacking the hero are fought this far away (times Range): archers and casters shoot from afar.
    constexpr float kEngagedReach = 4.0f;

    DWORD g_thinkAt = 0;            // the next look at the situation
    uint32_t g_pending = 0;         // the new aim not acted on yet, 0 for none
    DWORD g_actAt = 0;              // ... acted on from this tick
    int g_repeatMs = 1000;          // this target's repeat interval: Repeat with some spread
    int g_lastHealth = 0;
    DWORD g_hurtAt = 0;             // when the hero last lost health, 0 for never
    std::unordered_map<uint32_t, float> g_lastDistance;   // each enemy's distance at the last look, pixels

    DWORD jitter(DWORD lo, DWORD hi)
    {
        static uint32_t state = GetTickCount() | 1;
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return lo + state % (hi - lo + 1);
    }

    // A creature's aim (seen live, 2026-11-07): attacking, +0x104 holds its target's id and +0x108/+0x10C where the
    // target stands; walking or idle, +0x104/+0x108 hold the point it goes to (world coordinates, far above any id).
    // The id counts only when the position after it is its object's, give or take its last steps.
    constexpr size_t kAimId = 0x104, kAimX = 0x108, kAimY = 0x10C;
    constexpr int32_t kAimSlack = 400;     // world units, about 7 tiles

    // Hostile creatures near the hero on the surface (40 tiles), from the world pass each second: the explorer keeps
    // clear of those not after it yet (Manolito, block N: a creature fixes on the hero within 600 u, 11.2 tiles, and
    // a pack (+0x24D its leader) comes as one; gives up beyond 1500 u, 27.9 tiles).
    struct AggroFoe
    {
        uint32_t id;
        int32_t x, y;
        bool onHero;    // +0x251 the hero, +0x1F4 bit 4 (an enemy fixed), not a summon's master (bit 0x100)
    };
    std::vector<AggroFoe> g_aggroFoes;
    std::atomic<DWORD> g_attackAt{0};    // the last attack order (read by Gear on its own thread)
    int32_t g_aggroHeroX = 0, g_aggroHeroY = 0;    // where the hero stood when g_aggroFoes was read
    constexpr double kAggroTiles = 13.0;

    // A creature with aggro on the hero (Manolito, block N): its AI target +0x251 the hero (or what it rides), an enemy
    // fixed (+0x1F4 bit 4), and no master (bit 0x100: then +0x251 is the master). Set from 11.2 tiles on, before the
    // first blow; dropped beyond 28 tiles (the pursuit's end). The player, 2026-11-10: «saber cuando un enemigo le
    // tiene aggro, para saber a quién atacar o de quién huir».
    bool aggroOnHero(const cObject* creature, uint32_t heroId, uint32_t selfId)
    {
        if (!creature)
        {
            return false;
        }
        const auto* base = reinterpret_cast<const uint8_t*>(creature);
        const int32_t target = *reinterpret_cast<const int32_t*>(base + 0x251);
        const uint32_t ai = *reinterpret_cast<const uint32_t*>(base + 0x1F4);
        return target > 0 && (ai & 4) && !(ai & 0x100) &&
               (static_cast<uint32_t>(target) == heroId || static_cast<uint32_t>(target) == selfId);
    }

    uint32_t aimOf(cObjectManager* objects, cObject* creature)
    {
        const auto* base = reinterpret_cast<const uint8_t*>(creature);
        const uint32_t id = *reinterpret_cast<const uint32_t*>(base + kAimId);
        cObject* target = id && id != creature->id ? objects->object(id) : nullptr;
        if (!target)
        {
            return 0;
        }
        const int32_t x = *reinterpret_cast<const int32_t*>(base + kAimX);
        const int32_t y = *reinterpret_cast<const int32_t*>(base + kAimY);
        return std::abs(x - target->x) <= kAimSlack && std::abs(y - target->y) <= kAimSlack ? id : 0;
    }

    // Enemies the player attacked themself (the hero's aim after a click of theirs): fought to the end.
    std::unordered_set<uint32_t> g_playerTargets;

    // After a click of the player's: the enemy they set the hero on is theirs to the end. Dead or gone ones are
    // dropped.
    void notePlayerTarget()
    {
        cObjectManager* objects = cObjectManager::instance();
        cEngine* engine = FrameHooks::engine();
        cCreature* hero = objects ? objects->hero() : nullptr;
        if (!objects || !engine || !hero)
        {
            return;
        }
        cObject* controlled = objects->object(engine->controlled);
        const uint32_t aim = controlled ? aimOf(objects, controlled) : 0;
        cCreature* foe = aim ? objects->creature(aim) : nullptr;
        if (foe && foe->health > 0 && hero->isEnemy(foe) && g_playerTargets.insert(aim).second)
        {
            LOG("AutoAttack: the player attacks {:x}", aim);
        }
        for (auto it = g_playerTargets.begin(); it != g_playerTargets.end();)
        {
            cCreature* c = objects->creature(*it);
            it = c && c->health > 0 ? std::next(it) : g_playerTargets.erase(it);
        }
    }

    struct Choice
    {
        uint32_t id = 0;
        bool urgent = false;    // under threat: acted on sooner
    };

    // --- Who follows the hero: hirelings ---------------------------------------------------------------------------

    // A creature that is no enemy, stays about the hero for a while and kept up while the hero went some way: a
    // hireling (people in town and animals stay where they are; the horse waits where the hero got off).
    struct Follower
    {
        DWORD since = 0, seenAt = 0;
        int32_t heroX = 0, heroY = 0;   // where the hero stood when it was first seen
        int32_t ownX = 0, ownY = 0;     // ... and where it stood itself (townsfolk stay, hirelings go along)
        bool follows = false;
    };
    std::unordered_map<uint32_t, Follower> g_followers;
    std::vector<uint32_t> g_hirelings;
    constexpr DWORD kFollowMs = 15000;
    constexpr int32_t kFollowWay = static_cast<int32_t>(6 * kTileSize);

    void noteHirelings(DWORD now, const cCreature* hero, float range)
    {
        AimAssist::Foe friends[32];
        const int n = AimAssist::friends(range, friends, 32);
        cObjectManager* objects = cObjectManager::instance();
        for (int i = 0; i < n; ++i)
        {
            const cObject* o = objects ? objects->object(friends[i].id) : nullptr;
            if (!o)
            {
                continue;
            }
            Follower& f = g_followers[friends[i].id];
            if (!f.since || now - f.seenAt > 10000)
            {
                const bool follows = f.follows;
                f = Follower{now, now, hero->x, hero->y, o->x, o->y, follows};
            }
            f.seenAt = now;
            // The hero went a way, and it went the same way along (townsfolk and guards walk their own ways).
            const int64_t hdx = static_cast<int64_t>(hero->x) - f.heroX, hdy = static_cast<int64_t>(hero->y) - f.heroY;
            const int64_t odx = static_cast<int64_t>(o->x) - f.ownX, ody = static_cast<int64_t>(o->y) - f.ownY;
            const bool heroWent = std::abs(hdx) > kFollowWay || std::abs(hdy) > kFollowWay;
            const bool alongWith = std::abs(odx - hdx) < kFollowWay * 2 / 3 && std::abs(ody - hdy) < kFollowWay * 2 / 3;
            if (!f.follows && now - f.since > kFollowMs && heroWent && alongWith)
            {
                f.follows = true;
                LOG("AutoAttack: {:x} follows the hero: a hireling", friends[i].id);
            }
        }
        // The game's own list of companions (Manolito, AGENT_ADDRESSES «Mercenarios y acompañantes»): a std::vector
        // of 9-byte records at hero+0x39C..+0x3A0, +0 the creature's id, +8 non-zero for a hired mercenary (0: one
        // the script makes follow, e.g. an escorted man). Known at once, without waiting to see it follow.
        // When the list reads well (an empty one too), it alone says who the companions are: the guess from who walks
        // along took Julius, the arena master, for one (seen live, 2026-11-09).
        {
            const auto* heroBytes = reinterpret_cast<const uint8_t*>(hero);
            const auto begin = *reinterpret_cast<const uint8_t* const*>(heroBytes + 0x39C);
            const auto end = *reinterpret_cast<const uint8_t* const*>(heroBytes + 0x3A0);
            const bool listKnown = (!begin && !end) ||
                                   (begin && end >= begin && !((end - begin) % 9) && (end - begin) / 9 <= 8);
            if (listKnown)
            {
                std::vector<uint32_t> listed;
                for (const uint8_t* r = begin; r && r < end; r += 9)
                {
                    const uint32_t id = *reinterpret_cast<const uint32_t*>(r);
                    const cCreature* c = objects ? objects->creature(id) : nullptr;
                    if (!c || c->health <= 0)
                    {
                        // A companion lost (the player, 2026-11-11: A4 let a mercenary die): told once.
                        static std::unordered_set<uint32_t> s_lost;
                        if (c && std::find(g_hirelings.begin(), g_hirelings.end(), id) != g_hirelings.end() && s_lost.insert(id).second)
                        {
                            LOG("AutoAttack: companion {:x} died ({} of {} health)", id, c->health, c->maxHealth);
                        }
                        continue;
                    }
                    listed.push_back(id);
                    static std::unordered_set<uint32_t> s_told;
                    if (s_told.insert(id).second)
                    {
                        LOG("AutoAttack: {:x} is in the hero's companions ({}): looked after", id,
                            r[8] ? "a hired mercenary" : "following by script");
                    }
                }
                g_hirelings = std::move(listed);
                return;
            }
        }
        g_hirelings.clear();
        for (auto it = g_followers.begin(); it != g_followers.end();)
        {
            if (now - it->second.seenAt > 30000)
            {
                it = g_followers.erase(it);
                continue;
            }
            if (it->second.follows)
            {
                g_hirelings.push_back(it->first);
            }
            ++it;
        }
    }

    bool isHireling(uint32_t id)
    {
        return std::find(g_hirelings.begin(), g_hirelings.end(), id) != g_hirelings.end();
    }

    // --- The target: kept, given up when out of reach ---------------------------------------------------------------

    DWORD g_targetSince = 0;        // when the current target was taken
    float g_targetBest = 0.0f;      // its nearest distance since, pixels (0: none yet)
    DWORD g_targetBestAt = 0;       // ... reached then
    DWORD g_lastLookAt = 0;         // the last look at the situation
    DWORD g_retreatStartAt = 0;     // when the retreat began
    bool g_retreatDoomed = false;   // the retreat is from a hopeless fight (not for health or potions)
    int g_retreatExtended = 0;      // times the retreat went on with little health and no potion
    std::unordered_map<uint32_t, DWORD> g_unreachable;      // enemies given up on, until then
    std::unordered_map<uint32_t, float> g_unreachableAt;    // ... and how far they were then (pixels)
    std::unordered_map<uint32_t, int> g_unreachableTimes;   // ... how many times so far (the wait grows each time)
    std::unordered_map<uint32_t, int> g_farAimLooks;        // looks in a row an enemy aimed at the hero from afar
    uint32_t g_lastShooter = 0;                             // the last one taken for the shooter (logged once)
    int g_unseenHits = 0;                                   // hits in a row with no enemy to answer
    DWORD g_meleeAt = 0;                                    // the last look with an enemy in the melee
    DWORD g_unseenHitAt = 0;
    int32_t g_unseenSpotX = 0, g_unseenSpotY = 0;   // where the hero last walked off such hits ...
    DWORD g_unseenSpotAt = 0;                       // ... and when
    std::unordered_map<uint32_t, std::pair<int32_t, int32_t>> g_lastWorld;   // each enemy's world position, last look
    constexpr DWORD kStuckMs = 4000;        // no nearer to the target in this long, not fighting: out of reach
    constexpr DWORD kUnreachableMs = 20000;

    DWORD g_lastTargetAt = 0;       // the last enemy taken for target
    DWORD g_lastHurtAt = 0;         // the target's health last went down (a blow that told)
    DWORD g_lootCount = 0;          // loot taken for good, counted (AgentStuck: progress)
    void targetTaken(DWORD now, uint32_t id)
    {
        // The same one taken again moments after a dodge or a look away (the player, 2026-11-11: the Vampiress after
        // archers across a river, «target 5d4» every 2 s between the dodges): the time without getting nearer goes on.
        static uint32_t s_lastId = 0;
        const bool again = id == s_lastId && g_lastTargetAt && now - g_lastTargetAt < 6000;
        s_lastId = id;
        g_lastTargetAt = now;
        if (again)
        {
            return;
        }
        g_targetSince = now;
        g_targetBest = 0.0f;
        g_targetBestAt = now;
    }

    // --- Stepping back -------------------------------------------------------------------------------------------

    DWORD g_retreatUntil = 0;       // stepping back until then at the latest (0: not stepping back)
    // Hopeless fights stepped back from (A3, the Battle Mage at level 4, 2026-11-12: away from groups of 5 at 418 %
    // of his health again and again, a level 4-7 region his farm and quest ground for hours, never a level up).
    std::deque<DWORD> g_hopelessAt;
    bool g_collecting = false;      // collecting a quest item or loot (declared early: the position watch reads it)
    DWORD g_questNoWayUntil = 0;    // a quest with no way there: the quests left alone until then
    // Where those hopeless fights were (tiles): a quest whose goal lies within 30 tiles of one waits 30 min (the
    // analyst, 2026-11-12: A3 stepped back from 7 enemies at 27 % to win, then walked on to its quest 24 tiles from
    // the archers' camp and died 16 s later).
    struct HopelessSpot { int tx, ty; DWORD at; };
    std::vector<HopelessSpot> g_hopelessSpots;
    bool nearHopelessSpot(int tx, int ty, DWORD now)
    {
        std::erase_if(g_hopelessSpots, [now](const HopelessSpot& h) { return now - h.at > 1800000; });
        for (const HopelessSpot& h : g_hopelessSpots)
        {
            if ((tx - h.tx) * (tx - h.tx) + (ty - h.ty) * (ty - h.ty) < 900)
            {
                return true;
            }
        }
        return false;
    }

    // Two hopeless fights in 10 min: outmatched where it is; farmed and quested only where the enemies are no
    // higher than the hero, for as long as two stay within the 10 min.
    bool outmatched(DWORD now)
    {
        while (!g_hopelessAt.empty() && now - g_hopelessAt.front() > 600000)
        {
            g_hopelessAt.pop_front();
        }
        return g_hopelessAt.size() >= 2;
    }
    DWORD g_retreatWalkAt = 0;      // the last walk order away
    DWORD g_noRetreatUntil = 0;     // after a long retreat, fight again for a while first
    constexpr DWORD kRetreatMaxMs = 12000, kRetreatPauseMs = 8000;
    constexpr double kRetreatWay = 8 * kTileSize;
    // The hero's last positions (one every 2 s while it moves, the last 2 minutes): the way back for a retreat.
    std::deque<std::pair<int32_t, int32_t>> g_fleeTrail;

    // The game's tutorial (the player, 2026-11-10: «tienen que saber hacer todo el tutorial perfectamente»; «beneficia
    // a los agentes que llevan buena conducta con el tutorial»). Manolito, block T/V3: the script variable «Tutorial»
    // (the save's script variables, a vector [0xAACF80+0x7550..+0x7554] of 0x24-byte records, name char[0x20], u32
    // value at +0x20) holds the step; 0x0D is over (or skipped). Each step taken is logged and remembered with how
    // long it took; one standing over 3 minutes is logged once with what it asks for, to be seen and taught.
    void watchTutorial(DWORD now)
    {
        static DWORD s_at = 0, s_stepAt = 0;
        static const uint8_t* s_var = nullptr;
        static uint32_t s_step = 0xFFFFFFFFu;
        static bool s_stuckLogged = false;
        static int s_taken = 0;
        if (now - s_at < 2000)
        {
            return;
        }
        s_at = now;
        const auto begin = *reinterpret_cast<const uint8_t* const*>(0xAACF80 + 0x7550);
        const auto end = *reinterpret_cast<const uint8_t* const*>(0xAACF80 + 0x7554);
        if (!begin || end <= begin || (end - begin) / 0x24 > 20000)
        {
            s_var = nullptr;
            return;
        }
        if (!s_var || s_var < begin || s_var >= end || std::strncmp(reinterpret_cast<const char*>(s_var), "Tutorial", 0x20) != 0)
        {
            s_var = nullptr;
            for (const uint8_t* r = begin; r + 0x24 <= end; r += 0x24)
            {
                if (std::strncmp(reinterpret_cast<const char*>(r), "Tutorial", 0x20) == 0)
                {
                    s_var = r;
                    break;
                }
            }
            if (!s_var)
            {
                return;
            }
        }
        static const char* const kAsks[] = {
            "no empezado", "bienvenida: abrir el cofre y hablar con el PNJ", "recoger objetos: abrir el cofre",
            "equipo: equipar un objeto de la mochila", "combate: atacar", "experiencia: repartir los puntos",
            "compañeros: aceptar al compañero", "brújula: seguirla", "misiones: hablar con el PNJ de «?!»",
            "mercaderes: comprar o vender", "caballo: comprar o montar un caballo", "historia: seguir la misión principal",
            "primeros pasos: abrir el cofre y hablar con el PNJ", "terminado", "recompensas: cobrar en el «!»",
            "maestro de combos: crear un combo", "herrero: engarzar", "escaleras: subirlas", "paso 18"};
        // Seen live 2026-11-10 (8 heroes): the value is the tutorial on screen, back to 0 once it is closed (2 at
        // the first chest, 16 by the smith), not a running count: each one shown is a lesson, counted once.
        static uint32_t s_seen = 0;     // bit per tutorial shown this session
        const uint32_t step = *reinterpret_cast<const uint32_t*>(s_var + 0x20);
        const char* ask = step < std::size(kAsks) ? kAsks[step] : "paso desconocido";
        if (step != s_step)
        {
            if (step && step < 32 && !(s_seen & (1u << step)))
            {
                s_seen |= 1u << step;
                ++s_taken;
                LOG("AutoAttack: tutorial {} shown ({}), {} of 17 seen this session", step, ask, s_taken);
                remember(std::format("tutorial {}: {}", step, ask));
            }
            else if (!step && s_step && s_step != 0xFFFFFFFFu)
            {
                LOG("AutoAttack: tutorial {} closed after {} s", s_step, (now - s_stepAt) / 1000);
            }
            s_step = step;
            s_stepAt = now;
            s_stuckLogged = false;
        }
        else if (!s_stuckLogged && step != 0 && now - s_stepAt > 20000)
        {
            s_stuckLogged = true;
            LOG("AutoAttack: tutorial {} ({}) open for {} s", step, ask, (now - s_stepAt) / 1000);
        }
    }

    // Riding (the tutorial's step 10). Manolito, block V3/V3b: mounting is an event to the rider, hero->vt[6](&ev),
    // a 0x44-byte cEvent (vtable 0x89095C) with +4 0x209 to mount, 0x20A to get off, +0x14 the horse's id (hero
    // +0x570); destroyed by 0x416B30 thiscall(&ev). Mounted: creature +0x200 & 0x10. Mounted blows are split by the
    // riding skill (down to a third): on for the long walks with no enemy within 20 tiles, off once one comes within
    // 12; at most a change every 8 s.
    // Free horses about (side 4, no owner at +0x570; Manolito, block H2: wild horses, taken by walking up and the ride
    // event), within 25 tiles on the surface, with their worth (Gear::horseWorth).
    struct WildHorse
    {
        uint32_t id;
        int32_t x, y;
        int worth;
    };
    std::vector<WildHorse> g_wildHorses;
    uint32_t g_rideCode = 0, g_rideHorse = 0;
    // A walk stuck on horseback (the analyst, 2026-11-11: A5, mounted all the time, the most «no nearer» of the eight,
    // in a wood; a horse does not pass where a man on foot does): off for a minute, on foot, before the tile is left.
    DWORD g_onFootUntil = 0;

    void sendRide(void*)
    {
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        if (!hero || !g_rideHorse || !objects->creature(g_rideHorse))
        {
            return;
        }
        alignas(8) uint8_t ev[0x44] = {};
        *reinterpret_cast<uintptr_t*>(ev) = 0x89095C;
        *reinterpret_cast<uint32_t*>(ev + 4) = g_rideCode;
        *reinterpret_cast<uint32_t*>(ev + 0x14) = g_rideHorse;
        using Handle = void(__fastcall*)(void* self, void*, void* ev);
        const auto* vt = *reinterpret_cast<const uintptr_t* const*>(hero);
        reinterpret_cast<Handle>(vt[6])(hero, nullptr, ev);
        // The off event leaves the control on the horse (Manolito, block AG): given back in the same call, as the game's
        // input handler does on the next mouse event, so no frame sees the hero half off.
        if (g_rideCode == 0x20A)
        {
            if (cEngine* engine = FrameHooks::engine())
            {
                engine->controlled = hero->id;
            }
        }
        // Not destroyed with 0x416B30: its return is not documented (the code reviewer), and the zeroed event on the
        // stack holds nothing to free.
    }

    // The horseshoe's call (Manolito, block Y, 2026-11-11; the player: «el caballo se puede llamar siempre que él
    // quiera que automáticamente él se acerca a ti»): a cEvent 0x116 to the hero's vt[6], +0x10 the horse's id, as the
    // horseshoe's left click sends with the horse far. The hero's handler (0x530D68) uses hero +0x570 and does nothing
    // underground, in an interior or with the horse dead; the horse walks up (or is set beside its owner).
    void sendCall(void*)
    {
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        if (!hero || !g_rideHorse)     // the horse may lie beyond the loaded ground: the handler goes by hero +0x570
        {
            return;
        }
        alignas(8) uint8_t ev[0x44] = {};
        *reinterpret_cast<uintptr_t*>(ev) = 0x89095C;
        *reinterpret_cast<uint32_t*>(ev + 4) = 0x116;
        *reinterpret_cast<uint32_t*>(ev + 0x10) = g_rideHorse;
        using Handle = void(__fastcall*)(void* self, void*, void* ev);
        const auto* vt = *reinterpret_cast<const uintptr_t* const*>(hero);
        reinterpret_cast<Handle>(vt[6])(hero, nullptr, ev);
    }

    // The call, at most every 30 s, only where the game's handler is where block Y has it.
    void callHorse(DWORD now, uint32_t horse, double tiles)
    {
        static DWORD s_calledAt = 0;
        static const bool s_known = [] {
            static constexpr uint8_t kSig[] = {0x83, 0xBD, 0x70, 0x05, 0x00, 0x00, 0x10, 0x0F};
            return std::memcmp(reinterpret_cast<const void*>(0x530D68), kSig, sizeof(kSig)) == 0;
        }();
        if (!s_known || (s_calledAt && now - s_calledAt < 30000))
        {
            return;
        }
        s_calledAt = now;
        if (tiles > 0)
        {
            LOG("AutoAttack: horse {:x} {:.0f} tiles off: called (the horseshoe)", horse, tiles);
        }
        else
        {
            LOG("AutoAttack: horse {:x} beyond the loaded ground: called (the horseshoe)", horse);
        }
        remember("llama al caballo");
        g_rideHorse = horse;
        Inject::call(&sendCall, nullptr);
    }

    // Half off the horse (Manolito, block AG, 2026-11-12, read live in A6): the hero on foot (+0x1EC 0) but its
    // riding bits (+0x200 & 0x14) still set and cEngine::controlled still the horse, so the camera and the mouse
    // follow the horse. The game itself mends it only on a mouse event (0x6173E7 in the input handler 0x6172C0); the
    // agent makes none. Mended as that does, on the game's thread: the off event with the horse's id (hero +0x570, the
    // handler 0x53677C does nothing with id 0), then controlled back to the hero.
    void mendHalfOff(void*)
    {
        cEngine* engine = FrameHooks::engine();
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        if (!engine || !hero)
        {
            return;
        }
        const auto* h = reinterpret_cast<const uint8_t*>(hero);
        const uint32_t horse = *reinterpret_cast<const uint32_t*>(h + 0x570);
        if (horse && (*reinterpret_cast<const uint32_t*>(h + 0x200) & 0x14) && objects->creature(horse))
        {
            alignas(8) uint8_t ev[0x44] = {};
            *reinterpret_cast<uintptr_t*>(ev) = 0x89095C;
            *reinterpret_cast<uint32_t*>(ev + 4) = 0x20A;
            *reinterpret_cast<uint32_t*>(ev + 0x14) = horse;
            using Handle = void(__fastcall*)(void* self, void*, void* ev);
            const auto* vt = *reinterpret_cast<const uintptr_t* const*>(hero);
            reinterpret_cast<Handle>(vt[6])(hero, nullptr, ev);
        }
        engine->controlled = hero->id;
    }

    void ride(DWORD now, const cCreature* self)
    {
        // The hero, not the controlled creature (Manolito, block AD, 2026-11-11: mounted, engine->controlled is the
        // horse, whose +0x570 is its rider, so «horse 1» was the hero's own id and A3 stood still for hours).
        if (cObjectManager* om = cObjectManager::instance())
        {
            if (const cCreature* hero = om->hero())
            {
                self = hero;
            }
        }
        static DWORD s_at = 0;
        // Walking up to the horse: since when (0: not), and when last told to walk.
        static DWORD s_approachSince = 0, s_approachStep = 0, s_restUntil = 0;
        uint32_t horse = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(self) + 0x570);
        // A wild horse ridden unbound (+0x570 0): the one under the hero (+0x1EC), so it can be got off (the reviewer).
        if (!horse)
        {
            horse = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(self) + 0x1EC);
        }
        // Half off (the camera on the horse, the hero on foot beside it): mended, once in 5 s at most.
        if (const cEngine* engine = FrameHooks::engine();
            engine && engine->controlled != self->id &&
            !*reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(self) + 0x1EC))
        {
            static DWORD s_mendAt = 0;
            static int s_mends = 0;
            if (now - s_mendAt > 5000)
            {
                s_mendAt = now;
                if (++s_mends <= 20)
                {
                    LOG("AutoAttack: half off the horse (controlled {:x}, the hero {:x} on foot): control back to the hero "
                        "(try {})", engine->controlled, self->id, s_mends);
                }
                Inject::call(&mendHalfOff, nullptr);
            }
            return;
        }
        // A wild horse taken (the player, 2026-11-10: «existen caballos salvajes, son útiles cuando no tienes
        // caballo»; Manolito, H2: walk up to it and send the ride event): one within 25 tiles when the hero has none,
        // or one two levels better than his, on foot, no enemy within 15 tiles.
        static uint32_t s_wildId = 0;
        static DWORD s_wildSince = 0, s_wildStep = 0, s_wildSentAt = 0;
        static std::unordered_map<uint32_t, DWORD> s_wildFailed;
        if (HeroProfile::current().cls != HeroProfile::Class::Dwarf && !*(reinterpret_cast<const uint8_t*>(self) + 0x24) &&
            !g_retreatUntil)
        {
            cObjectManager* om = cObjectManager::instance();
            const cCreature* own = horse && om ? om->creature(horse) : nullptr;
            if (s_wildSentAt)
            {
                // Taken: the hero's own now, or at least ridden (the hero where it stands; the code reviewer: the ride
                // event may mount a free horse without binding it, unconfirmed).
                const cCreature* wild = om ? om->creature(s_wildId) : nullptr;
                const bool riding = wild && std::abs(wild->x - self->x) <= 3 && std::abs(wild->y - self->y) <= 3;
                if (horse == s_wildId || riding)
                {
                    LOG("AutoAttack: wild horse {:x} taken ({})", s_wildId, horse == s_wildId ? "the hero's now" : "ridden, not bound");
                    s_wildId = 0;
                    s_wildSentAt = 0;
                }
                else if (now - s_wildSentAt > 3000)
                {
                    LOG("AutoAttack: wild horse {:x} not taken (riding skill?): left 10 min", s_wildId);
                    s_wildFailed[s_wildId] = now;
                    s_wildId = 0;
                    s_wildSentAt = 0;
                }
                return;
            }
            const bool onFoot = !own || std::abs(own->x - self->x) > 3 || std::abs(own->y - self->y) > 3;
            double nearestFoe = 1e9;
            for (const AggroFoe& f : g_aggroFoes)
            {
                nearestFoe = std::min(nearestFoe, std::hypot(static_cast<double>(f.x - self->x), static_cast<double>(f.y - self->y)));
            }
            const int bar = own ? Gear::horseWorth(own) + 2000 : 1;
            const WildHorse* best = nullptr;
            for (const WildHorse& w : g_wildHorses)
            {
                const auto failed = s_wildFailed.find(w.id);
                if (w.worth >= bar && (failed == s_wildFailed.end() || now - failed->second > 600000) &&
                    (!best || w.worth > best->worth))
                {
                    best = &w;
                }
            }
            if (best && onFoot && nearestFoe > 15.0 * kTileSize)
            {
                const cCreature* wild = om ? om->creature(best->id) : nullptr;
                const double off = wild ? std::hypot(static_cast<double>(wild->x - self->x), static_cast<double>(wild->y - self->y)) : 1e9;
                if (best->id != s_wildId)
                {
                    s_wildId = best->id;
                    s_wildSince = now;
                    LOG("AutoAttack: wild horse {:x} (worth {}, the hero's {}) {:.1f} tiles off: going to take it", best->id,
                        best->worth, own ? Gear::horseWorth(own) : 0, off / kTileSize);
                }
                if (!wild || now - s_wildSince > 20000)
                {
                    LOG("AutoAttack: wild horse {:x} not reached in 20 s: left 10 min", s_wildId);
                    s_wildFailed[s_wildId] = now;
                    s_wildId = 0;
                    return;
                }
                if (off > 2.0 * kTileSize)
                {
                    g_doorHoldUntil = now + 1500;
                    if (now - s_wildStep > 1000)
                    {
                        s_wildStep = now;
                        g_walkX = wild->x;
                        g_walkY = wild->y;
                        sendWalk();
                    }
                    return;
                }
                LOG("AutoAttack: mounting wild horse {:x}", s_wildId);
                g_rideCode = 0x209;
                g_rideHorse = s_wildId;
                Inject::call(&sendRide, nullptr);
                s_wildSentAt = now;
                return;
            }
            s_wildId = 0;
        }
        // The Dwarf does not ride (Builds.md; the player, 2026-11-10: «el enano no tiene animación montado»).
        if (!horse || *(reinterpret_cast<const uint8_t*>(self) + 0x24) ||
            HeroProfile::current().cls == HeroProfile::Class::Dwarf)
        {
            s_approachSince = 0;
            return;
        }
        // The 8 s between changes holds the mounting only: getting off for an enemy near is never put off (the live
        // verifier, 2026-11-10: off at 7 and 4 tiles, the gate had held it).
        const bool onFoot = g_onFootUntil && static_cast<int>(now - g_onFootUntil) < 0;
        const bool gated = now - s_at < 8000 || static_cast<int>(now - s_restUntil) < 0 || onFoot;
        // Mounted: the horse stands exactly where the rider does (seen live 2026-11-10, the player's Gladiator on its
        // horse: same position each second; +0x200 & 0x10, the guess, stayed clear and the agent kept mounting).
        cObjectManager* om = cObjectManager::instance();
        const cCreature* steed = om ? om->creature(horse) : nullptr;
        // Not while stepping back from a fight (the code reviewer: a walk to the horse must not undo a retreat).
        if (!steed && !g_retreatUntil && (ClickWalk::routing() || g_questTravel) && now - s_at > 8000)
        {
            // Left so far behind that it is not on the loaded ground: called all the same, on a long walk with no
            // enemy within 15 tiles.
            bool calm = true;
            for (const AggroFoe& f : g_aggroFoes)
            {
                calm = calm && std::hypot(static_cast<double>(f.x - self->x), static_cast<double>(f.y - self->y)) > 15.0 * kTileSize;
            }
            if (calm)
            {
                callHorse(now, horse, 0.0);
            }
        }
        if (!steed || g_retreatUntil)
        {
            s_approachSince = 0;
            return;
        }
        // The hero's own mount field (+0x1EC, the horse's id while riding; HeroProfile and AgentSave read it), not the
        // horse standing where the hero does: just off, the horse stays on the hero's spot, and the off event sent
        // again each 1.5 s put the hero back up (A6 2026-11-12 00:08-00:27, 30 «getting off» in a row, standing still).
        const bool mounted = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(self) + 0x1EC) != 0;
        // Ride events sent with the hero still on foot after them (no riding skill?): after 3, a long rest.
        static int s_unmounted = 0;
        if (mounted)
        {
            s_unmounted = 0;
        }
        else if (s_unmounted >= 3)
        {
            LOG("AutoAttack: 3 mounts of horse {:x} left the hero on foot: on foot for 10 min", horse);
            s_unmounted = 0;
            s_restUntil = now + 600000;
            return;
        }
        double nearest = 1e9;
        for (const AggroFoe& f : g_aggroFoes)
        {
            nearest = std::min(nearest, std::hypot(static_cast<double>(f.x - self->x), static_cast<double>(f.y - self->y)));
        }
        const bool longWalk = ClickWalk::routing() || g_questTravel;
        int close = 0;
        for (const AggroFoe& f : g_aggroFoes)
        {
            close += std::hypot(static_cast<double>(f.x - self->x), static_cast<double>(f.y - self->y)) < 8.0 * kTileSize;
        }
        const bool hardOnFoot = close >= 3 && Config::tactics.arts && Tactics::artsBarredMounted() > 0;
        // The Vampiress rides only in her knight's form (the player, 2026-11-12).
        const bool vampire = Tactics::vampireForm(now);
        uint32_t code = 0;
        // Up again once the nearest enemy is 5 tiles off: the saddle is the better place to fight from (the player,
        // 2026-11-11), so only an enemy at hand puts the walk to the horse off.
        if (!mounted && longWalk && nearest > 5.0 * kTileSize && !hardOnFoot && !vampire && (s_approachSince || !gated))
        {
            code = 0x209;
        }
        else if (mounted && vampire && now - s_at > 1500)
        {
            LOG("AutoAttack: the Vampiress in her vampire form: off the horse");
            code = 0x20A;
        }
        else if (mounted && onFoot && now - s_at > 1500)
        {
            LOG("AutoAttack: on foot a while (a walk that got no nearer, a quest creature to speak to, or a save): off the horse");
            code = 0x20A;
        }
        // Every class fights mounted (Mind: dismountTiles 0), the off branch kept for a class given a distance.
        else if (mounted && nearest < Mind::now().dismountTiles * kTileSize && now - s_at > 1500)
        {
            code = 0x20A;
        }
        // But a hard fight (three or more within 8 tiles) is fought on foot when arts in the slots cannot be cast up
        // there (the player, 2026-11-11: «no todas las habilidades pueden ser usadas a caballo»): an easy one the
        // saddle's quicker blows end anyway.
        else if (mounted && hardOnFoot && now - s_at > 1500)
        {
            LOG("AutoAttack: {} enemies near and arts barred on horseback: fighting on foot", close);
            code = 0x20A;
        }
        if (!code)
        {
            s_approachSince = 0;
            return;
        }
        if (code == 0x209)
        {
            // Up as a player gets up: the hero walks to the horse first and mounts beside it (the player, 2026-11-10:
            // the event put the hero onto the horse wherever it stood, «se teleporta encima del caballo»; Manolito's
            // block U2: a click on one's own horse is no order, the game's pick skips it, so a walk to it).
            const double off = std::hypot(static_cast<double>(steed->x - self->x), static_cast<double>(steed->y - self->y));
            if (off > 2.0 * kTileSize)
            {
                // More than 10 tiles back: left behind, on foot (the Daemon walked 24 tiles back to it after each
                // fight, the player: «no está teniendo mucha eficiencia»).
                if (off > 10.0 * kTileSize)
                {
                    // Far: called as the horseshoe calls it (on the surface, the hero's handler checks the rest), and
                    // mounted once it has come up; never walked back for.
                    callHorse(now, horse, off / kTileSize);
                    s_at = now;     // not walked back to: it comes
                    s_approachSince = 0;
                    return;
                }
                if (!s_approachSince)
                {
                    s_approachSince = now;
                    LOG("AutoAttack: walking to horse {:x} ({:.1f} tiles) to mount it", horse, off / kTileSize);
                }
                else if (now - s_approachSince > 20000)
                {
                    LOG("AutoAttack: horse {:x} not reached in 20 s ({:.1f} tiles): on foot", horse, off / kTileSize);
                    s_restUntil = now + 60000;  // a minute before the next try
                    s_approachSince = 0;
                    return;
                }
                g_doorHoldUntil = now + 1500;   // the roam's travel holds its orders meanwhile
                if (now - s_approachStep > 1000)
                {
                    s_approachStep = now;
                    g_walkX = steed->x;
                    g_walkY = steed->y;
                    sendWalk();
                }
                return;
            }
            s_approachSince = 0;
            ++s_unmounted;
        }
        s_at = now;
        LOG("AutoAttack: {} horse {:x} ({:.0f} tiles to the nearest enemy)", code == 0x209 ? "mounting" : "getting off", horse,
            nearest < 1e8 ? nearest / kTileSize : 99.0);
        g_rideCode = code;
        g_rideHorse = horse;
        Inject::call(&sendRide, nullptr);
    }

    void noteTrail(DWORD now, int32_t x, int32_t y, int layer)
    {
        static DWORD s_at = 0;
        static int s_layer = -1;
        if (layer != s_layer)
        {
            s_layer = layer;
            g_fleeTrail.clear();    // another floor: the old way back does not lead from here
        }
        if (now - s_at < 2000)
        {
            return;
        }
        s_at = now;
        if (!g_fleeTrail.empty() && std::hypot(static_cast<double>(x - g_fleeTrail.back().first),
                                           static_cast<double>(y - g_fleeTrail.back().second)) < 2 * kTileSize)
        {
            return;
        }
        g_fleeTrail.emplace_back(x, y);
        if (g_fleeTrail.size() > 60)
        {
            g_fleeTrail.pop_front();
        }
    }

    // The hero's trail (the player's wish, 2026-11-09: the agent knows where it is and how it got there): a point for
    // each tile walked, so a hero shut in backs out the way it came. Seen live: the Vampiress walked into her start
    // house after a fixture, the tile test blind in there, and roamed against its walls for 20 minutes.
    struct Crumb
    {
        int32_t x, y;
        int layer;
    };
    std::vector<Crumb> g_trail;
    bool g_backing = false;     // walking back along the trail (its points are not noted meanwhile)

    void noteTrail(const cCreature* self, int layer)
    {
        if (g_backing)
        {
            return;
        }
        if (!g_trail.empty())
        {
            const Crumb& last = g_trail.back();
            const double d = std::hypot(static_cast<double>(self->x - last.x), static_cast<double>(self->y - last.y));
            if (last.layer != layer || d > 40.0 * kTileSize)
            {
                g_trail.clear();    // a portal, a load: the way back is not this one
            }
            else if (d < kTileSize)
            {
                return;
            }
        }
        g_trail.push_back({self->x, self->y, layer});
        if (g_trail.size() > 400)
        {
            g_trail.erase(g_trail.begin(), g_trail.begin() + 100);
        }
    }

    // Low on health, no healing potion left, enemies close: walk away from them (the order a click on the ground
    // gives, to a point 8 tiles off on the far side of the hero), again every second while it lasts. Ends once health
    // is back up, a potion turns up, no enemy is close any more, or after 12 s (then the hero fights again for 8 s).
    bool retreat(DWORD now, const cCreature* self, int health, int maximum, const AimAssist::Foe* foes, int count,
                 float hx, float hy, float range, cObjectManager* objects)
    {
        const Config::Tactics& c = Config::tactics;
        double cx = 0.0, cy = 0.0;
        int close = 0;
        bool anyNear = false;   // within Range: a retreat ends only once all are further (no back and forth)
        // Those with aggro on the hero are the ones that follow (Manolito, block N): away from them, and over once none
        // is near; those without, standing by, are left out (the old way, all of them, when none has aggro).
        const cCreature* heroCreature = objects->hero();
        const uint32_t heroId = heroCreature ? heroCreature->id : self->id;
        bool anyAggro = false;
        for (int i = 0; i < count; ++i)
        {
            anyAggro = anyAggro || aggroOnHero(objects->object(foes[i].id), heroId, self->id);
        }
        for (int i = 0; i < count; ++i)
        {
            const float dx = foes[i].x - hx, dy = foes[i].y - hy;
            cObject* o = objects->object(foes[i].id);
            if (anyAggro && !aggroOnHero(o, heroId, self->id))
            {
                continue;
            }
            anyNear = anyNear || dx * dx + dy * dy < range * range;
            if (o && dx * dx + dy * dy < (range * 0.45f) * (range * 0.45f))
            {
                cx += o->x;
                cy += o->y;
                ++close;
            }
        }
        // Low on health, or below 40 % with those striking about to take the rest in moments.
        // Or the forecast gives the fight under 50 % with health below 60 % (the analyst, 2026-11-10: out of healing
        // potions at 59 of 158, into another fight of three at 20 % to win).
        const CombatLearn::Forecast& fc = CombatLearn::forecast();
        // Without a healing potion, a big fight forecast lost is left early (the analyst, 2026-11-10 21:03: the
        // Gladiator, Dark Elf and Wood Elf below 20 % of health in 12-13 % of their fights, into 11-16 enemies with
        // the potions gone): at 85 % of health with five or more about.
        // Each class by its own mind (Mind, docs/Mentalidades.md): the frail casters leave sooner; the player's own
        // retreat percent, once changed in the menu from its 25, rules over the class's.
        const Mind::Values& mind = Mind::now();
        // Careful after dying twice from the same save: away 15 points sooner.
        const int retreatPct = (c.retreatPercent != 25 ? c.retreatPercent : mind.retreatPercent) +
                               (AgentSave::deathsSinceSave() >= 2 ? 15 : 0);
        const bool dry = Gear::healingLeft() == 0 && fc.foes >= 5;
        const bool lost = fc.foes > 0 && fc.win < mind.lostWin &&
                          health * 100 <= maximum * (dry ? mind.lostHealthDryPct : mind.lostHealthPct);
        const bool low = health * 100 <= maximum * retreatPct ||
                         (health * 100 <= maximum * 40 && CombatLearn::secondsToDie() < 2.5f) || lost;
        const bool noPotions = CombatState::noPotionsAt.load() != 0;
        // A group the fight with is foreseen hopeless is left at once, potions or not, health full or not (the
        // player, 2026-11-11: «la vampiresa siempre tiene la costumbre de morir en el mismo lugar»: into 8 at 18 % to
        // win, the potions drunk one by one, then dead; twice at the same spot).
        // Also one or two far above the hero (A3, the Battle Mage at level 4, 2026-11-12: 9 deaths in 31 min, each a
        // minute after loading, into one enemy foreseen at 254 % of his health lost; the «careful» after two deaths
        // only moved the retreat by 15 points): more than one and a half lives' worth, or more than one once careful.
        const bool careful = AgentSave::deathsSinceSave() >= 2;
        // On the loss corrected by what fights like it really cost (the player, 2026-11-12: the Daemon fled one thin
        // goblin «foreseeing defeat»: 1000 % raw, but her forecasts ran eleven times too grim, 47 % to win), and never
        // from one enemy alone the hero's blows put down in a few seconds.
        const bool quickOne = fc.foes == 1 && fc.seconds > 0.0f && fc.seconds < 6.0f;
        const bool doomed = !quickOne &&
                            ((fc.foes >= 4 && fc.win < 0.25f && fc.expected > 1.0f) ||
                             (fc.foes > 0 && fc.expected > 1.5f && fc.win < 0.75f) ||
                             (careful && fc.foes > 0 && fc.expected > 1.0f && fc.win < 0.85f));
        // Not from a fight about won (the player, 2026-11-10: «huyendo de un enemigo que le falta poco para morir pero
        // se deja matar por huir»): running, the blows come in the back all the same. Kept fighting when the
        // forecast puts all down well before the hero's health would go, or when only one is close and nearly down.
        int lastBreath = 0;
        for (int i = 0; i < count; ++i)
        {
            const float dx = foes[i].x - hx, dy = foes[i].y - hy;
            if (dx * dx + dy * dy < (range * 0.45f) * (range * 0.45f) && foes[i].maxHealth > 0 &&
                foes[i].health * 4 <= foes[i].maxHealth)
            {
                ++lastBreath;
            }
        }
        const float toDie = CombatLearn::secondsToDie();
        const bool aboutWon = (fc.foes > 0 && fc.win >= 0.6f && fc.seconds > 0.0f && fc.seconds < toDie * 0.7f) ||
                              (close == 1 && lastBreath == 1 && toDie > 2.0f);
        if (aboutWon)
        {
            if (g_retreatUntil)
            {
                LOG("AutoAttack: stepping back over: the fight is about won ({:.1f} s to win, {:.1f} s of health)",
                    fc.seconds, toDie);
                g_retreatUntil = 0;
            }
            return false;
        }
        if (g_retreatUntil)
        {
            // Really got away (the player, 2026-11-10: «saber cuándo realmente ha huido»): none has the hero as its
            // aim any more, and none within its aggro reach (Manolito, block N: they give up beyond 1500 u). Then it
            // rests out of their reach a while (health comes back without enemies near) before playing on.
            bool chased = false;
            double nearest = 1e9;
            for (const AggroFoe& f : g_aggroFoes)
            {
                const double d = std::hypot(static_cast<double>(f.x - self->x), static_cast<double>(f.y - self->y));
                nearest = std::min(nearest, d);
                chased = chased || f.onHero;
            }
            // Only on the surface: g_aggroFoes is filled there alone, so below ground it would always read as got away
            // (the code reviewer, 2026-11-10).
            if (!*(reinterpret_cast<const uint8_t*>(self) + 0x24) && !chased && nearest > 700.0 && g_retreatWalkAt &&
                now - g_retreatWalkAt > 1500)
            {
                LOG("AutoAttack: got away: no enemy after the hero any more ({:.0f} tiles from the nearest), resting at "
                    "{} of {} health", nearest < 1e8 ? nearest / kTileSize : 99.0, health, maximum);
                remember(std::format("escapó: nadie le persigue ({} de {} de vida)", health, maximum));
                g_retreatUntil = 0;
                g_noRetreatUntil = now + kRetreatPauseMs;
                if (health * 100 < maximum * 70)
                {
                    g_quietUntil = now + 15000;
                }
                return false;
            }
            const bool recovered =
                health * 100 >= maximum * std::min((c.retreatPercent != 25 ? c.retreatPercent : Mind::now().retreatPercent) + 20, 90);
            bool tooLong = static_cast<int>(now - g_retreatUntil) >= 0;
            // Not back into the fight with little health and no potion (the live verifier, 2026-11-11: 5 of 13
            // deaths within 15 s of «too long, fighting again», one at 30 of 189): the retreat goes on, three times
            // at most, before it does turn to fight.
            if (tooLong && noPotions && anyNear && health * 100 < maximum * 40 && g_retreatExtended < 3)
            {
                ++g_retreatExtended;
                g_retreatUntil = now + kRetreatMaxMs;
                LOG("AutoAttack: stepping back on: {} of {} health and no potion ({} of 3)", health, maximum, g_retreatExtended);
                tooLong = false;
            }
            else if ((recovered && !g_retreatDoomed) || (!noPotions && !g_retreatDoomed) || !anyNear || tooLong)
            {
                g_retreatExtended = 0;
            }
            // Not on the first look that sees fewer (the live verifier, 2026-11-11: 24 of 33 such retreats over in
            // under 3 s, the group still there, only out of sight): 8 s away at least.
            if (g_retreatDoomed && !doomed && !tooLong && anyNear && g_retreatStartAt && now - g_retreatStartAt > 8000)
            {
                LOG("AutoAttack: stepping back over (the fight no longer foreseen hopeless)");
                g_retreatUntil = 0;
                g_retreatDoomed = false;
                return false;
            }
            if ((recovered && !g_retreatDoomed) || (!noPotions && !g_retreatDoomed) || !anyNear || tooLong)
            {
                LOG("AutoAttack: stepping back over ({})", recovered    ? "health back up"
                                                           : !noPotions ? "a potion turned up"
                                                           : !anyNear   ? "no enemy near"
                                                                        : "too long, fighting again");
                g_retreatUntil = 0;
                if (tooLong)
                {
                    g_noRetreatUntil = now + kRetreatPauseMs;
                }
                return false;
            }
        }
        else if (!c.retreat || (!low && !doomed) || (!noPotions && !doomed) || !close ||
                 (g_noRetreatUntil && static_cast<int>(now - g_noRetreatUntil) < 0))
        {
            return false;
        }
        else
        {
            g_retreatUntil = now + kRetreatMaxMs;
            g_retreatWalkAt = 0;
            g_retreatExtended = 0;
            g_retreatDoomed = doomed && !(low && noPotions);
            g_retreatStartAt = now;
            if (g_retreatDoomed)
            {
                g_hopelessAt.push_back(now);
                g_hopelessSpots.push_back({static_cast<int>(self->x / kTileSize), static_cast<int>(self->y / kTileSize), now});
                LOG("AutoAttack: a hopeless fight ({} enemies, {:.0f}% to win): stepping back at {} of {} health{}",
                    fc.foes, fc.win * 100.0f, health, maximum,
                    outmatched(now) ? "; outmatched here: farming and quests only where the enemies are not above the hero"
                                    : "");
            }
            setIntent(Intent::Recover, "low on health, no healing potion or a fight foreseen lost: stepping back");
            LOG("AutoAttack: stepping back at {} of {} health, no healing potion, {} enemies close", health, maximum,
                close);
            // Down below a quarter by them: those close are not gone back to for 3 min, fought only if they come
            // into the melee (the analyst, 2026-11-10: an Orc Warrior of level 5 brought the Seraphim to 5-15 of 158
            // five times in 8 min, the hero healed and went back to it each time).
            if (health * 4 < maximum || g_retreatDoomed)
            {
                for (int i = 0; i < count; ++i)
                {
                    const float dx = foes[i].x - hx, dy = foes[i].y - hy;
                    if (dx * dx + dy * dy < range * range)
                    {
                        g_unreachable[foes[i].id] = now + 180000;
                        g_unreachableAt[foes[i].id] = 1e9f;
                        LOG("AutoAttack: {:x} too strong for now (health {} of {}): not gone back to for 3 min",
                            foes[i].id, health, maximum);
                    }
                }
            }
        }
        if (close && (!g_retreatWalkAt || now - g_retreatWalkAt >= 1000))
        {
            // Where to: back the way the hero came (the player: «muriendo por no saber huir»; straight away from the
            // enemies ran it into walls and unknown ground, caught there): of the hero's last positions, the one
            // farthest from the enemies that lies away from them, at least 8 tiles off; else straight away as before.
            const double ex = cx / close, ey = cy / close;
            double bestScore = 0.0;
            int32_t bx = 0, by = 0;
            int bestCrossed = 0;
            const int layer = *(reinterpret_cast<const uint8_t*>(self) + 0x24);
            // Where to run (the player, 2026-11-10: «la idea de saber el aggro es identificar por donde huir o por donde
            // no pasar»): the way back and 16 ways around, each weighed by how far it takes the hero from those after
            // it, less what it costs to pass by the hostiles not after it yet (each within its aggro reach of the way
            // would join: heavy), on walkable ground only.
            std::vector<std::pair<int32_t, int32_t>> options(g_fleeTrail.begin(), g_fleeTrail.end());
            for (int k = 0; k < 16; ++k)
            {
                const double a = k * 3.14159265 / 8.0;
                for (const double tiles : {10.0, 16.0})
                {
                    options.emplace_back(static_cast<int32_t>(self->x + std::cos(a) * tiles * kTileSize),
                                         static_cast<int32_t>(self->y + std::sin(a) * tiles * kTileSize));
                }
            }
            const double heroFromFoes = std::hypot(self->x - ex, self->y - ey);
            for (const auto& [px, py] : options)
            {
                const double fromFoes = std::hypot(px - ex, py - ey), fromHero = std::hypot(px - self->x, py - self->y);
                if (fromHero < kRetreatWay || fromFoes < heroFromFoes + 4 * kTileSize ||
                    ClickWalk::walkable(layer, static_cast<int>(px / kTileSize), static_cast<int>(py / kTileSize)) == 0)
                {
                    continue;
                }
                int crossed = 0;
                for (const AggroFoe& f : g_aggroFoes)
                {
                    if (f.onHero)
                    {
                        continue;
                    }
                    // Its distance to the way (the segment hero -> point).
                    const double vx = px - self->x, vy = py - self->y, wx = f.x - self->x, wy = f.y - self->y;
                    const double t = std::clamp((vx * wx + vy * wy) / std::max(vx * vx + vy * vy, 1.0), 0.0, 1.0);
                    if (std::hypot(wx - t * vx, wy - t * vy) < kAggroTiles * kTileSize)
                    {
                        ++crossed;
                    }
                }
                const double score = fromFoes - 0.3 * fromHero - crossed * 20.0 * kTileSize + 1.0 * kTileSize;
                if (score > bestScore)
                {
                    bestScore = score;
                    bx = px;
                    by = py;
                    bestCrossed = crossed;
                }
            }
            if (bestScore > 0.0)
            {
                g_walkX = bx;
                g_walkY = by;
            }
            else
            {
                const double ax = self->x - ex, ay = self->y - ey;
                const double len = std::sqrt(ax * ax + ay * ay);
                const double ux = len > 1.0 ? ax / len : 1.0, uy = len > 1.0 ? ay / len : 0.0;
                g_walkX = static_cast<int32_t>(self->x + ux * kRetreatWay);
                g_walkY = static_cast<int32_t>(self->y + uy * kRetreatWay);
            }
            if (!g_retreatWalkAt)
            {
                LOG("AutoAttack: stepping back {} ({:.0f} tiles off, {} hostile not after it on that way)",
                    bestScore > 0.0 ? "the safest way (aggro weighed)" : "straight away",
                    std::hypot(g_walkX - self->x, g_walkY - self->y) / kTileSize, bestCrossed);
            }
            sendWalk();
            g_retreatWalkAt = now;
        }
        return true;
    }

    // Which enemy to fight. Those attacking the hero, what it rides or a hireling, and those the player attacked
    // (engaged), and with Seek any other within Range; of them, weighing the situation: those close, coming at the
    // hero (by their own steps, in the world) or shooting at it from afar first, the badly wounded finished off, the
    // current one kept for a moment at least and then unless another is clearly better; one the hero gets no nearer
    // to is given up for a while. Loot is not the auto attack's: the player collects with A.
    // Arrows coming at the hero (the player, 2026-11-10: «el agente no identifica proyectiles, es bueno que lo haga
    // para conseguir esquivarlos»). Manolito, block U: cArrow3D (vtable 0x891AB4) lives in the object table, +0x1E4
    // bit 1 in flight, +0x204 its target, +0x208 the shooter; they fly straight. Seen live 2026-11-10: the world
    // position is the usual one at +0x1C/+0x20; the direction +0x1F4/+0x1F8 is in the game's axes, turned 45 degrees
    // from the world's (world = (dx + dy, dy - dx)), about 16 tiles a second. One whose line passes within a tile of
    // the hero, ahead of it within 15 tiles, is side-stepped: 2.5 tiles across its line, the side away from it first,
    // on walkable ground; once per arrow, at most every 1.2 s.
    // Punished when an arrow hits (the player, 2026-11-11: «castiga al agente cuando sea acertado por un proyectil»):
    // each arrow on its way at the hero is followed; one gone from the air within 5 tiles of it (16 tiles a second, a look every 0.2-0.3 s) while its health fell
    // is a hit, and each hit makes the dodge keener for the rest of the session: seen farther off, a wider line taken
    // for one coming at it, a longer step aside, a shorter wait between two.
    struct DodgeLearn
    {
        double reach = 15.0, line = 1.0, step = 2.5;    // tiles
        DWORD wait = 1200;                              // ms between two side-steps
        int hits = 0, dodged = 0;
    };
    DodgeLearn g_dodge;

    // Who shot at the hero lately, by its object index (an arrow's shooter at +0x208): the player, 2026-11-11: «el
    // agente ignora a los que le atacan de lejos, y son los que más molestan» (their aim is not always read).
    std::unordered_map<uint32_t, DWORD> g_shotBy;

    bool shotAtHero(uint32_t id, DWORD now)
    {
        const auto it = g_shotBy.find(id & 0xFFFF);
        return it != g_shotBy.end() && now - it->second < 8000;
    }

    void noteArrowHits(DWORD now, const cCreature* self, const uint8_t* const* table, size_t entries)
    {
        struct Seen
        {
            double t;   // tiles ahead along its line, last look
            DWORD at;
        };
        static std::unordered_map<uint32_t, Seen> s_coming;
        static int s_health = 0;
        int health = 0, maximum = 0;
        AimAssist::heroHealth(health, maximum);
        const bool fell = s_health && health < s_health;
        s_health = health;
        for (auto it = s_coming.begin(); it != s_coming.end();)
        {
            const uint8_t* o = it->first < entries ? table[it->first] : nullptr;
            const bool flying = o && *reinterpret_cast<const uint32_t*>(o) == 0x891AB4 &&
                                (*reinterpret_cast<const uint32_t*>(o + 0x1E4) & 1);
            if (flying && now - it->second.at < 3000)
            {
                ++it;
                continue;
            }
            if (!flying && fell && it->second.t < 5.0 && now - it->second.at < 600)   // 16 tiles a second: up to 5 between two looks
            {
                ++g_dodge.hits;
                g_dodge.reach = std::min(25.0, g_dodge.reach + 1.5);
                g_dodge.line = std::min(1.8, g_dodge.line + 0.1);
                g_dodge.step = std::min(4.0, g_dodge.step + 0.2);
                g_dodge.wait = std::max<DWORD>(500, g_dodge.wait - 100);
                LOG("AutoAttack: hit by arrow {:x} (hit {}, {} dodged): the dodge keener now (seen at {:.1f} tiles, line "
                    "{:.1f}, step {:.1f}, every {} ms)",
                    it->first, g_dodge.hits, g_dodge.dodged, g_dodge.reach, g_dodge.line, g_dodge.step, g_dodge.wait);
                Findings::note("arrow", self->x, self->y, "hit by an arrow (hit {})", g_dodge.hits);
            }
            it = s_coming.erase(it);
        }
        // The arrows on their way at the hero now.
        for (size_t i = 1; i < entries; ++i)
        {
            const uint8_t* o = table[i];
            if (!o || *reinterpret_cast<const uint32_t*>(o) != 0x891AB4 || !(*reinterpret_cast<const uint32_t*>(o + 0x1E4) & 1))
            {
                continue;
            }
            const float gx = *reinterpret_cast<const float*>(o + 0x1F4), gy = *reinterpret_cast<const float*>(o + 0x1F8);
            double dx = gx + gy, dy = gy - gx;
            const double len = std::hypot(dx, dy);
            if (len < 0.1)
            {
                continue;
            }
            dx /= len;
            dy /= len;
            const double wx = self->x - *reinterpret_cast<const int32_t*>(o + 0x1C);
            const double wy = self->y - *reinterpret_cast<const int32_t*>(o + 0x20);
            const double t = wx * dx + wy * dy;
            if (t > -1.0 * kTileSize && t < 25.0 * kTileSize && std::hypot(wx - t * dx, wy - t * dy) < 1.5 * kTileSize)
            {
                s_coming[static_cast<uint32_t>(i)] = {std::abs(t) / kTileSize, now};
                if (const uint32_t shooter = *reinterpret_cast<const uint32_t*>(o + 0x208) & 0xFFFF)
                {
                    g_shotBy[shooter] = now;
                }
            }
        }
    }

    bool dodgeArrow(DWORD now, const cCreature* self, uint32_t heroId)
    {
        static DWORD s_at = 0;
        static uint32_t s_lastArrow = 0;
        // Never through an art going out (the player, 2026-11-11: «esquivar los proyectiles está cancelando las
        // artes»): the main attack is an art too, cast by the game from the click, not by Tactics, so Tactics::busy did
        // not tell; the step would cut it short and its recharge lost. The art's blows first, the arrow taken.
        if (g_retreatUntil || CombatState::casting(self))
        {
            return false;
        }
        // Not a dodge every half second (the live log, 2026-11-11: 71 dodges in a fight against archers, the hero
        // hardly striking, dead): 1.5 s at least between two, none with an enemy in the melee (its blows first), and
        // none on the way to the one shooting (it is the answer).
        if (now - s_at < std::max<DWORD>(g_dodge.wait, 1500) || (g_meleeAt && now - g_meleeAt < 1500) ||
            (g_target && shotAtHero(g_target, now)))
        {
            return false;
        }
        const auto manager = *reinterpret_cast<const uint8_t* const*>(0xAD5C40);
        if (!manager)
        {
            return false;
        }
        const auto table = *reinterpret_cast<const uint8_t* const* const*>(manager + 4);
        const auto tableEnd = *reinterpret_cast<const uint8_t* const* const*>(manager + 8);
        if (!table || tableEnd <= table || tableEnd - table >= 0x40000)
        {
            return false;
        }
        const auto* sides = reinterpret_cast<const uint8_t*>(0x8EB548);
        const size_t entries = static_cast<size_t>(tableEnd - table);
        double bestT = 1e9, offX = 0.0, offY = 0.0, dirX = 0.0, dirY = 0.0;
        uint32_t arrow = 0;
        for (size_t i = 1; i < entries; ++i)
        {
            const uint8_t* o = table[i];
            if (!o || *reinterpret_cast<const uint32_t*>(o) != 0x891AB4 || !(*reinterpret_cast<const uint32_t*>(o + 0x1E4) & 1))
            {
                continue;
            }
            const uint32_t target = *reinterpret_cast<const uint32_t*>(o + 0x204);
            const uint32_t shooter = *reinterpret_cast<const uint32_t*>(o + 0x208) & 0xFFFF;
            bool hostile = target == heroId || target == self->id;
            if (!hostile && shooter && shooter < entries && table[shooter])
            {
                const uint32_t side = *reinterpret_cast<const uint32_t*>(table[shooter] + 0x1F0);
                hostile = side < 16 && side != 1 && sides[16 + side] == 0;
            }
            if (!hostile)
            {
                continue;
            }
            const float gx = *reinterpret_cast<const float*>(o + 0x1F4), gy = *reinterpret_cast<const float*>(o + 0x1F8);
            double dx = gx + gy, dy = gy - gx;
            const double len = std::hypot(dx, dy);
            if (len < 0.1)
            {
                continue;
            }
            dx /= len;
            dy /= len;
            const double wx = self->x - *reinterpret_cast<const int32_t*>(o + 0x1C);
            const double wy = self->y - *reinterpret_cast<const int32_t*>(o + 0x20);
            const double t = wx * dx + wy * dy;     // how far ahead along its line the hero stands
            if (t <= 0.0 || t > g_dodge.reach * kTileSize)
            {
                continue;
            }
            const double ox = wx - t * dx, oy = wy - t * dy;
            if (std::hypot(ox, oy) < g_dodge.line * kTileSize && t < bestT)
            {
                bestT = t;
                offX = ox;
                offY = oy;
                dirX = dx;
                dirY = dy;
                arrow = static_cast<uint32_t>(i);
            }
        }
        if (!arrow || arrow == s_lastArrow)
        {
            return false;
        }
        const int layer = *(reinterpret_cast<const uint8_t*>(self) + 0x24);
        // Across its line: the side the hero already leans to first.
        double nx = -dirY, ny = dirX;
        if (offX * nx + offY * ny < 0.0)
        {
            nx = -nx;
            ny = -ny;
        }
        for (const double sign : {1.0, -1.0})
        {
            const int32_t px = static_cast<int32_t>(self->x + sign * nx * g_dodge.step * kTileSize);
            const int32_t py = static_cast<int32_t>(self->y + sign * ny * g_dodge.step * kTileSize);
            if (ClickWalk::walkable(layer, static_cast<int>(px / kTileSize), static_cast<int>(py / kTileSize)) == 0)
            {
                continue;
            }
            s_at = now;
            s_lastArrow = arrow;
            ++g_dodge.dodged;
            LOG("AutoAttack: arrow {:x} coming ({:.1f} tiles off), side-stepping {} ({} dodged, {} hits)", arrow,
                bestT / kTileSize, sign > 0 ? "away from its line" : "the other way", g_dodge.dodged, g_dodge.hits);
            g_walkX = px;
            g_walkY = py;
            sendWalk();
            return true;
        }
        return false;
    }

    bool decide(DWORD now, Choice& out)
    {
        float hx, hy;
        if (!AimAssist::hero(hx, hy))
        {
            return false;
        }
        int health = 0, maximum = 0;
        if (AimAssist::heroHealth(health, maximum))
        {
            if (g_lastHealth && health < g_lastHealth)
            {
                g_hurtAt = now;
            }
            g_lastHealth = health;
        }
        const bool hurt = g_hurtAt && static_cast<int>(now - g_hurtAt) < static_cast<int>(kHurtMs);

        cObjectManager* objects = cObjectManager::instance();
        cEngine* engine = FrameHooks::engine();
        cCreature* heroObject = objects ? objects->hero() : nullptr;
        cCreature* self = objects && engine ? objects->creature(engine->controlled) : nullptr;
        if (!objects || !engine || !heroObject || !self)
        {
            return false;
        }
        const uint32_t heroId = heroObject->id;
        const uint32_t selfId = engine->controlled;
        // The map memory: fights count as ground seen too (layer at the creature's +0x24).
        {
            const int layer = *(reinterpret_cast<const uint8_t*>(self) + 0x24);
            noteTrail(self, layer);
            MapMemory::visit(layer, self->x, self->y);
            MapMemory::wantGround(layer, self->x, self->y, hx, hy);
            MapMemory::save(now, 60000);
            // The reach known in the fights too (at a new game the first fight came before any fill).
            Whereabouts::update(layer, static_cast<int>(self->x / kTileSize), static_cast<int>(self->y / kTileSize));
        }

        const float range = static_cast<float>(Config::autoAttack.range);
        const float melee = range * 0.45f;
        // Shut in (a yard behind its fence): an enemy on no tile the hero can walk to is out of reach, whatever its
        // aggro. Seen live (2026-11-10): the Dark Elf and the Wood Elf at their start chased the enemies beyond the
        // fence by turns, 4 s each, for up to 3 minutes; their aggro made them strike, so the focus kept switching.
        // A fence, water or a wall on the straight line from the hero to `o` (the walkable test every half tile).
        const auto lineBlocked = [&](const cObject* o) {
            const int layer = *(reinterpret_cast<const uint8_t*>(self) + 0x24);
            const double sx = self->x / kTileSize, sy = self->y / kTileSize;
            const double ex = o->x / kTileSize, ey = o->y / kTileSize;
            const int steps = static_cast<int>(std::ceil(std::max(std::abs(ex - sx), std::abs(ey - sy)) * 2.0));
            for (int k = 2; k < steps - 1; ++k)
            {
                const double t = static_cast<double>(k) / steps;
                if (ClickWalk::walkable(layer, static_cast<int>(sx + (ex - sx) * t), static_cast<int>(sy + (ey - sy) * t)) == 0)
                {
                    return true;
                }
            }
            return false;
        };
        const auto walledOff = [&](const cObject* o, float d) {
            // Next to the hero, nothing between. Within the melee reach too a fence counts (the player, 2026-11-11:
            // the Seraphim's spear reaching a goblin over a fence, «acaba nunca pegándole»).
            if (!o || d < 1.5f * static_cast<float>(kTileSize))
            {
                return false;
            }
            if (!Whereabouts::area().shutIn)
            {
                if (d < melee)
                {
                    return lineBlocked(o);
                }
                // Across water or a cliff (the player, 2026-11-11: «no detecta colisión ni casilla no pisable», the
                // Vampiress at archers over a river): no walk there, or one far longer than the line to it.
                const float way = MapMemory::routeTiles(*(reinterpret_cast<const uint8_t*>(self) + 0x24), static_cast<int>(self->x / kTileSize),
                                                        static_cast<int>(self->y / kTileSize),
                                                        static_cast<int>(o->x / kTileSize), static_cast<int>(o->y / kTileSize));
                const float line = d / static_cast<float>(kTileSize);
                if (way == -2.0f || (way >= 0.0f && way > line * 3.0f + 12.0f))
                {
                    return true;
                }
                // Seen only over walkable ground (the player, 2026-11-11: «que la visión del agente se anulara al
                // intentar pasar por una casilla no pisable»): a fence, water or a wall on the straight line hides it,
                // unless a short way round is known.
                return lineBlocked(o) && !(way >= 0.0f && way <= line * 1.5f + 5.0f);
            }
            if (d < melee)
            {
                return false;
            }
            const int tx = static_cast<int>(o->x / kTileSize), ty = static_cast<int>(o->y / kTileSize);
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    if (Whereabouts::reachable(tx + dx, ty + dy) != 0)
                    {
                        return false;   // reached, or unknown: kept
                    }
                }
            }
            return true;
        };
        noteHirelings(now, heroObject, range * 2.0f);
        AimAssist::Foe foes[kMaxFoes];
        const int count = AimAssist::enemies(range * kEngagedReach, foes, kMaxFoes);
        for (auto it = g_unreachable.begin(); it != g_unreachable.end();)
        {
            if (static_cast<int>(now - it->second) >= 0)
            {
                g_unreachableAt.erase(it->first);
                it = g_unreachable.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // The current target: no nearer in a while without a blow, it is out of reach (across water, up a wall).
        float currentDistance = -1.0f;
        int currentHealth = -1;
        bool inMelee = false;
        for (int i = 0; i < count; ++i)
        {
            const float dx = foes[i].x - hx, dy = foes[i].y - hy;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (foes[i].id == g_target)
            {
                currentDistance = d;
                currentHealth = foes[i].health;
            }
            if (d < melee)
            {
                inMelee = true;
            }
        }
        // No looks for a while (an art went on, the game paused): the time without getting nearer starts again.
        if (now - g_lastLookAt > 1500)
        {
            g_targetBestAt = now;
        }
        g_lastLookAt = now;
        if (g_target && !g_looting && currentDistance >= 0.0f)
        {
            // Striking or casting is progress only if it tells (the player, 2026-11-11: the Seraphim casting her arts
            // at one behind a fence, «did nothing seen» over and over, never left): its health must go down.
            static uint32_t s_healthOf = 0;
            static int s_healthBest = 0;
            if (s_healthOf != g_target || currentHealth > s_healthBest)
            {
                s_healthOf = g_target;
                s_healthBest = currentHealth;
            }
            const bool hurtIt = currentHealth >= 0 && currentHealth < s_healthBest;
            if (hurtIt)
            {
                s_healthBest = currentHealth;
                g_lastHurtAt = now;
            }
            const bool striking = CombatState::action(self) == CombatState::Fighting || CombatState::casting(self);
            // Striking over a fence (a spear's reach) is no progress: only its health going down is.
            const cObject* targetObject = objects && self ? objects->object(g_target) : nullptr;
            const bool fencedOff = targetObject && currentDistance >= 1.5f * static_cast<float>(kTileSize) &&
                                   lineBlocked(targetObject);
            if (g_targetBest <= 0.0f || currentDistance < g_targetBest * 0.9f || currentDistance < range * 0.3f ||
                hurtIt || (striking && currentDistance < melee && !fencedOff))
            {
                g_targetBest = g_targetBest > 0.0f ? std::min(g_targetBest, currentDistance) : currentDistance;
                g_targetBestAt = now;
            }
            else if (now - g_targetBestAt > kStuckMs)
            {
                // Each time again the same one, three times as long (20 s, 1 min, 3 min, then 10 min). Seen live
                // (2026-11-10): an enemy across the water taken again every 20 s for three hours, the 4 s chase
                // each time starting the stuck guard's watch again, so the hero never moved on.
                const int times = ++g_unreachableTimes[g_target];
                DWORD wait = kUnreachableMs;
                for (int t = 1; t < times && wait < 600000; ++t)
                {
                    wait *= 3;
                }
                wait = std::min<DWORD>(wait, 600000);
                LOG("AutoAttack: no nearer to {:x} in {} s ({:.0f} px away): out of reach, left for {} s (time {})",
                    g_target, kStuckMs / 1000, currentDistance, wait / 1000, times);
                if (times <= 2)
                {
                    Findings::note("unreachable", self->x, self->y, "enemy {:x} no nearer in {} s at {:.0f} px",
                                   g_target, kStuckMs / 1000, currentDistance);
                }
                g_unreachable[g_target] = now + wait;
                g_unreachableAt[g_target] = currentDistance;
                g_playerTargets.erase(g_target);
                g_target = 0;
            }
        }

        std::unordered_map<uint32_t, float> distances;
        std::vector<CombatLearn::Foe> learnFoes;
        std::unordered_map<uint32_t, std::pair<int32_t, int32_t>> world;
        std::unordered_map<uint32_t, int> farAim;
        bool threat = hurt;
        float enemyCost = 0.0f;
        uint32_t enemy = 0;
        bool currentCandidate = false;      // the enemy fought now is still there to fight
        bool enemyEngaged = false, currentEngaged = false;
        float currentLeft = 1.0f;           // the current one's health left (a share of its maximum)
        // What an ordinary enemy's maximum health is here: one well above hits harder (a strong one).
        int typical = 0;
        {
            std::vector<int> maxima;
            for (int i = 0; i < count; ++i)
            {
                if (foes[i].maxHealth > 0)
                {
                    maxima.push_back(foes[i].maxHealth);
                }
            }
            if (!maxima.empty())
            {
                std::nth_element(maxima.begin(), maxima.begin() + maxima.size() / 2, maxima.end());
                typical = maxima[maxima.size() / 2];
            }
        }
        // How many have aggro on the hero, logged when it changes (after the loop, as it leaves this scope).
        int aggroCount = 0;
        struct AggroTell
        {
            const int& n;
            ~AggroTell()
            {
                static int s_told = 0;
                if (n != s_told)
                {
                    if (n > s_told)
                    {
                        LOG("AutoAttack: {} enemies with aggro on the hero", n);
                    }
                    s_told = n;
                }
            }
        } aggroTell{aggroCount};
        for (int i = 0; i < count; ++i)
        {
            const AimAssist::Foe& f = foes[i];
            const float dx = f.x - hx, dy = f.y - hy;
            const float d = std::sqrt(dx * dx + dy * dy);
            distances[f.id] = d;
            cObject* foeObject = objects->object(f.id);
            if (foeObject)
            {
                world[f.id] = {foeObject->x, foeObject->y};
            }
            const uint32_t aim = foeObject ? aimOf(objects, foeObject) : 0;
            // Striking or aiming at the hero, or with aggro on it (coming, a blow or two off): a threat either way.
            const bool aggro = aggroOnHero(foeObject, heroId, selfId);
            const bool shooting = shotAtHero(f.id, GetTickCount());     // its arrows at the hero lately
            const bool atHero = (aim && (aim == heroId || aim == selfId)) || aggro || shooting;
            aggroCount += aggro ? 1 : 0;
            const bool atHireling = aim && !atHero && isHireling(aim);
            const bool engagedFoe = g_playerTargets.count(f.id) || atHero || atHireling;
            const bool current = f.id == g_target;
            {
                const auto looks = g_farAimLooks.find(f.id);
                // Within its aggro reach (600 world units, a margin on top) it joins the fight in moments: counted by
                // the forecast (the player, 2026-11-10: «no consigue identificar cuántos enemigos tiene alrededor»;
                // 8 around, the forecast took the 2 striking: 104 % foreseen, 201 % lost).
                const bool soon = foeObject && std::hypot(static_cast<double>(foeObject->x - self->x),
                                                          static_cast<double>(foeObject->y - self->y)) < 750.0;
                learnFoes.push_back({f.id, f.health, f.maxHealth, atHero, atHireling,
                                     looks != g_farAimLooks.end() && looks->second >= 3, soon});
            }
            // Given up on: only if it comes clearly nearer than it was then. Seen live (2026-11-09): one 122 px off
            // behind a graveyard's fence, within the melee look, was taken again at once, for minutes on end.
            if (const auto gone = g_unreachableAt.find(f.id);
                g_unreachable.count(f.id) && d > std::min(melee, gone != g_unreachableAt.end() ? gone->second * 0.6f : melee))
            {
                continue;
            }
            // Careful after dying again and again from the same save (the live verifier, 2026-11-11: A1 and A3 loaded
            // the same game a dozen times, the potions drunk and dead at the same spot each time): only those not
            // above the hero's level are sought; a stronger one only once it strikes the hero in the melee.
            if (AgentSave::deathsSinceSave() >= 2 && foeObject && foeObject->asCreature() && heroObject && !current &&
                !(atHero && d <= melee) &&
                *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(foeObject) + 0x3FE) >
                    *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(heroObject) + 0x3FE))
            {
                continue;
            }
            if (walledOff(foeObject, d))
            {
                continue;
            }
            // Engaged ones within the look's reach (kEngagedReach Range): an archer shooting from afar is gone after
            // too. Seeking fights, any other enemy within Range as well. The current one while it is engaged or
            // within 1.5 Range.
            // On the way to the merchant with the backpack full, or on a trip to a quest: only those that attack (no
            // fight sought). Focus (the player's wish, 2026-11-10): a trip decided on is gone through to the end;
            // only danger (one that attacks) stops it on the way.
            // Farming is seeking the enemies (the intent); elsewhere only as the setting says, never on a trip.
        const bool seek = (Config::autoAttack.seek || g_intent == Intent::Farm) && !Gear::wantsShop() && !g_questTravel;
            if (current ? !engagedFoe && d > range * 1.5f : !engagedFoe && (!seek || d > range))
            {
                continue;
            }
            // Aiming at the hero from beyond the melee look after look: it shoots (archer, caster).
            int farLooks = 0;
            if (atHero && d > melee)
            {
                const auto it = g_farAimLooks.find(f.id);
                farLooks = (it != g_farAimLooks.end() ? it->second : 0) + 1;
                farAim[f.id] = farLooks;
            }
            // Coming at the hero by its own steps (in the world: the hero's own walk says nothing about it).
            bool closing = false;
            const auto last = g_lastWorld.find(f.id);
            if (foeObject && last != g_lastWorld.end() && d >= melee)
            {
                const double mx = foeObject->x - last->second.first, my = foeObject->y - last->second.second;
                const double tx = self->x - foeObject->x, ty = self->y - foeObject->y;
                const double tl = std::sqrt(tx * tx + ty * ty);
                closing = tl > 1.0 && (mx * tx + my * ty) / tl > 0.5 * kTileSize;
            }
            if (d < melee || closing)
            {
                threat = true;
            }
            float cost = d;
            if (closing)
            {
                cost *= 0.7f;       // the one coming at the hero first
            }
            if (farLooks >= 3 && !inMelee)
            {
                cost *= 0.6f;       // shooting at the hero from afar, nobody in the melee: go for it
            }
            if (shooting && d > melee)
            {
                // Its arrows seen at the hero: the shooter before those about, unless one in the melee hits hard
                // (the strong-foe pull below); a player goes for the archer that keeps hitting.
                cost *= inMelee ? 0.5f : 0.35f;
            }
            // The threat first: who strikes the hero (the strong ones before the weak), then who strikes the
            // hireling (it dies fast alone); a weak one standing by waits.
            // Strong: well above the ordinary health here, or learned to hit far harder than those around.
            const float hardness = CombatLearn::relativeDanger(f.id);
            const bool strongFoe = (typical > 0 && f.maxHealth * 2 >= typical * 3) || hardness >= 1.6f;
            if (atHero && d < melee)
            {
                cost *= strongFoe ? 0.45f : 0.6f;
            }
            if (atHireling)
            {
                cost *= strongFoe ? 0.6f : 0.75f;
            }
            if ((atHero || atHireling || current) && f.maxHealth > 0)
            {
                // Among those in the fight, the less health left, the sooner it is down (from x0.7 at its last breath
                // to x1 unhurt): one focused down instead of blows spread. A weak one standing by gets no such pull.
                const float left = std::clamp(static_cast<float>(f.health) / static_cast<float>(f.maxHealth), 0.0f, 1.0f);
                cost *= 0.7f + 0.3f * left;
            }
            const bool striking = atHero || atHireling;
            if (striking)
            {
                // Focus fire (the player, 2026-11-10: «parece que deja los más peligrosos para el final»): of those
                // striking, the one that takes the most of the hero's health for the blows it still needs goes down
                // first: how hard it hits (learned, against an ordinary one) weighed above its health left (against an
                // ordinary one's), so a hard hitter is not left for last because it has more health.
                const float healthShare =
                    typical > 0 ? std::clamp(static_cast<float>(f.health) / static_cast<float>(typical), 0.15f, 4.0f) : 1.0f;
                const float priority = std::clamp(std::pow(hardness, 1.5f) / std::sqrt(healthShare), 0.3f, 6.0f);
                cost /= priority;
            }
            // The forecast's kill order (the least health lost: the highest blows per second needed to kill it, a
            // summoner weighed with what it brings): its first one pulled ahead among several.
            if (CombatLearn::forecast().foes > 1 && CombatLearn::killRank(f.id) == 0)
            {
                cost *= 0.6f;
            }
            if (current)
            {
                // Stay on it: a player finishes the enemy they fight, unless another is far nearer; a new one at least
                // a moment (no back and forth).
                cost *= 0.4f;
                currentCandidate = true;
                currentEngaged = striking;
                currentLeft = f.maxHealth > 0 ? static_cast<float>(f.health) / static_cast<float>(f.maxHealth) : 1.0f;
            }
            // One that strikes always before one that does not, whatever the distance.
            if (!enemy || (striking && !enemyEngaged) || (striking == enemyEngaged && cost < enemyCost))
            {
                enemy = f.id;
                enemyCost = cost;
                enemyEngaged = striking;
            }
        }
        // Hurt with nobody in the melee and no enemy seen striking (an archer's or caster's aim is not always read):
        // the nearest enemy within the look's reach is taken for the shooter and gone for. Seen live (2026-11-09):
        // the hero stood taking arrows until it died.
        if (inMelee)
        {
            g_meleeAt = now;
        }
        // Only with nobody in the melee for 3 s (a blow from one just killed is no shot from afar) and within twice
        // the Range (the nearest at any distance was a guess: the review of the findings, 2026-11-09).
        if (hurt && !inMelee && !enemyEngaged && (!g_meleeAt || now - g_meleeAt > 3000))
        {
            float best = 0.0f;
            uint32_t shooter = 0;
            for (int i = 0; i < count; ++i)
            {
                const auto d = distances.find(foes[i].id);
                if (d == distances.end() || d->second > range * 2.0f || (g_unreachable.count(foes[i].id) && d->second > melee) ||
                    walledOff(objects->object(foes[i].id), d->second))
                {
                    continue;
                }
                if (!shooter || d->second < best)
                {
                    shooter = foes[i].id;
                    best = d->second;
                }
            }
            if (shooter)
            {
                if (shooter != g_target && shooter != g_lastShooter)
                {
                    LOG("AutoAttack: hurt with nobody close: {:x} at {:.0f} px taken for the shooter", shooter, best);
                    Findings::note("shooter", self->x, self->y, "hurt with nobody close: {:x} at {:.0f} px taken for the shooter",
                                   shooter, best);
                }
                g_lastShooter = shooter;
                enemy = shooter;
                enemyEngaged = true;
                threat = true;
            }
        }
        // Hurt again and again with no enemy to answer (none seen, or all out of reach): never stand there. Seen live
        // (2026-11-09): hit every 3.5 s from somewhere out of reach, the hero went from the loot to the shooter and
        // back until it died. Off 10 tiles away from the enemies seen (anywhere when none), the agent quiet meanwhile.
        // With no enemy seen too (seen live, 2026-11-10, three Daemon starts): drained to death by the chest at her
        // arrival, by something the look does not list. A lone hit (her arrival's) is not enough: two within 12 s.
        if (hurt && !inMelee && !enemyEngaged && g_hurtAt == now)
        {
            g_unseenHits = static_cast<int>(now - g_unseenHitAt) < 12000 ? g_unseenHits + 1 : 1;
            g_unseenHitAt = now;
            if (g_unseenHits >= 2)
            {
                double cx = 0.0, cy = 0.0;
                int seen = 0;
                for (int i = 0; i < count; ++i)
                {
                    if (const cObject* o = objects->object(foes[i].id))
                    {
                        cx += o->x;
                        cy += o->y;
                        ++seen;
                    }
                }
                double ax = seen ? self->x - cx / seen : std::cos(now * 0.001), ay = seen ? self->y - cy / seen : std::sin(now * 0.001);
                const double len = std::sqrt(ax * ax + ay * ay);
                ax = len > 1.0 ? ax / len : 1.0;
                ay = len > 1.0 ? ay / len : 0.0;
                // Hurt there again within 10 minutes (the analyst, 2026-11-10: three times on the same two tiles, the
                // goals bringing the hero back): farther off each time, 10, 25, then 40 tiles.
                static int32_t s_spotX = 0, s_spotY = 0;
                static DWORD s_spotAt = 0;
                static int s_spotTimes = 0;
                const bool again = s_spotAt && now - s_spotAt < 600000 && std::abs(self->x - s_spotX) < 8 * kTileSize &&
                                   std::abs(self->y - s_spotY) < 8 * kTileSize;
                s_spotTimes = again ? s_spotTimes + 1 : 1;
                s_spotX = self->x;
                s_spotY = self->y;
                s_spotAt = now;
                g_unseenSpotX = self->x;
                g_unseenSpotY = self->y;
                g_unseenSpotAt = now;
                const int tiles = s_spotTimes >= 3 ? 40 : s_spotTimes == 2 ? 25 : 10;
                g_walkX = static_cast<int32_t>(self->x + ax * tiles * kTileSize);
                g_walkY = static_cast<int32_t>(self->y + ay * tiles * kTileSize);
                sendWalk();
                g_quietUntil = now + 4000 + 1500 * (tiles / 10);
                g_unseenHits = 0;
                LOG("AutoAttack: hurt again with no enemy to answer ({} seen): off {} tiles away (time {} here)", seen,
                    tiles, s_spotTimes);
                Findings::note("unseen-hit", self->x, self->y, "hurt twice in 12 s with no enemy to answer ({} seen, health {} of {})",
                               seen, health, maximum);
                return false;
            }
        }
        // Focus (the player's wish, 2026-11-10): the enemy taken is fought to the end (down, gone, or out of reach);
        // only danger changes it: another one strikes the hero (or a hireling) while this one does not.
        if (currentCandidate && g_target && enemy != g_target)
        {
            if (enemyEngaged && !currentEngaged)
            {
                LOG("AutoAttack: focus: {:x} left for {:x}, which strikes (danger)", g_target, enemy);
            }
            else if (enemyEngaged && currentEngaged && currentLeft > 0.3f)
            {
                // Both strike: the other came out well ahead though the current one is kept at 0.4 of its cost (it
                // hits far harder for the health it has left); the current one is kept when nearly down.
                LOG("AutoAttack: focus: {:x} left for {:x}, far more dangerous ({:.0f} % of its health left on {:x})",
                    g_target, enemy, currentLeft * 100.0f, g_target);
            }
            else
            {
                enemy = g_target;
            }
        }
        CombatLearn::observe({now, heroObject, self, learnFoes.data(), static_cast<int>(learnFoes.size()),
                              g_hirelings.data(), static_cast<int>(g_hirelings.size())});
        g_lastDistance = std::move(distances);
        g_lastWorld = std::move(world);
        g_farAimLooks = std::move(farAim);

        bool chosen = false;
        g_lootInstead = false;
        // Loot before a fight only when the fight is a new decision: one under way is finished first (focus).
        if (enemy && enemy != g_target && Config::autoAttack.pickup && Config::autoAttack.lootInFight)
        {
            AimAssist::Target loot;
            const auto it = distances.find(enemy);
            if (it != distances.end() &&
                AimAssist::findObject(static_cast<float>(Config::autoAttack.lootRange), &acceptLoot, nullptr, loot))
            {
                // Nobody strikes the hero and nothing hurts it: all the loot about first, the fight sought after it
                // (the player's wish, 2026-11-09: never a fight picked while loot lies there). Attacked: loot first
                // with no danger of dying (60 % of health or more, not being hurt), one piece at a time; in danger
                // only loot nearer than the enemy.
                const float lootDistance = std::hypot(loot.x - hx, loot.y - hy);
                const bool calm = !enemyEngaged && !hurt;
                const bool safe = maximum > 0 && health * 10 >= maximum * 6 && !hurt;
                // Mounted, loot is taken slowly (the player, 2026-11-11): left on the ground while enemies strike, all of
                // it gathered once the fight is over («mapea los objetos del suelo para recoger después de batallar»).
                const bool saddle = HeroProfile::current().mounted;
                if (calm || (!saddle && (safe || lootDistance < it->second)))
                {
                    LOG("AutoAttack: loot {:x} first ({:.0f} px; enemy {:x} at {:.0f} px; {})", loot.id, lootDistance,
                        enemy, it->second, calm ? "nobody attacks" : safe ? "no danger" : "nearer");
                    g_lootInstead = true;
                    g_lootInsteadAll = calm;
                    enemy = 0;
                }
            }
        }
        if (enemy)
        {
            out = {enemy, threat};
            chosen = true;
        }
        // The radar on the agent's map: the enemies seen, the one fought, the hirelings, and where it heads.
        MapMemory::beginRadar();
        for (const auto& [id, at] : g_lastWorld)
        {
            MapMemory::addBlip(id == enemy ? MapMemory::Blip::Target : MapMemory::Blip::Enemy, at.first, at.second);
        }
        for (uint32_t id : g_hirelings)
        {
            if (cObject* h = objects->object(id))
            {
                MapMemory::addBlip(MapMemory::Blip::Hireling, h->x, h->y);
            }
        }
        radarLoot();
        MapMemory::endRadar();
        {
            const auto it = chosen ? g_lastWorld.find(enemy) : g_lastWorld.end();
            if (it != g_lastWorld.end())
            {
                MapMemory::setRoute(true, it->second.first, it->second.second);
            }
            else
            {
                MapMemory::setRoute(g_roamAt != 0, g_walkX, g_walkY);
            }
        }

        // Combat arts and consumables, on the same look: at the enemy the hero fights now.
        Tactics::Situation situation;
        situation.now = now;
        situation.health = health;
        situation.maxHealth = maximum;
        situation.hurt = hurt;
        situation.threat = threat;
        situation.target = g_target && !g_looting && g_lastDistance.count(g_target) ? g_target : (chosen ? out.id : 0);
        situation.foes = foes;
        situation.count = count;
        situation.hx = hx;
        situation.hy = hy;
        // The arrows seen every frame, through arts and retreats too (who shoots, and the hits for the dodge's
        // learning), the dodge itself only when free.
        if (const auto m = *reinterpret_cast<const uint8_t* const*>(0xAD5C40))
        {
            const auto tb = *reinterpret_cast<const uint8_t* const* const*>(m + 4);
            const auto te = *reinterpret_cast<const uint8_t* const* const*>(m + 8);
            if (tb && te > tb && te - tb < 0x40000)
            {
                noteArrowHits(now, self, tb, static_cast<size_t>(te - tb));
            }
        }
        // Low on health with no potion: step back instead (unless an art is under way); no arts meanwhile.
        if (!Tactics::busy(now) && retreat(now, self, health, maximum, foes, count, hx, hy, range, objects))
        {
            return false;
        }
        if (!Tactics::busy(now) && dodgeArrow(now, self, objects->hero() ? objects->hero()->id : self->id))
        {
            return false;
        }
        situation.range = range;
        situation.hirelings = g_hirelings.data();
        situation.hirelingCount = static_cast<int>(g_hirelings.size());
        Tactics::think(situation);
        return chosen;
    }

    // Seeking fights with none around: the hero goes looking, as a player farming would. After a few quiet seconds (and
    // with most of its health back) it walks on in one direction a step (2 tiles) at a time, turning when it gets no further, and
    // back toward where it began once it strays too far from there. Any enemy or loot seen ends it.
    constexpr DWORD kRoamQuietMs = 4000;
    constexpr DWORD kRoamStepMs = 500;       // at least this between steps
    constexpr DWORD kRoamStepMaxMs = 4000;   // at most this on one step
    constexpr double kRoamLeash = 60 * kTileSize;
    DWORD g_quietSince = 0;
    double g_roamAngle = 0.0;
    int32_t g_roamFromX = 0, g_roamFromY = 0;   // the hero at the last walk order
    int32_t g_homeX = 0, g_homeY = 0;
    bool g_homeSet = false;
    int g_roamStuck = 0;    // walk orders in a row that got the hero no further
    int g_roamLayer = -1;
    int g_roamSteps = 0;    // steps since the way was chosen
    constexpr size_t kLayer = 0x24;     // a creature's layer: the surface, a dungeon, an interior

    // The quest journal (seen live, 2026-11-09): a std::vector of 0x174-byte records at [0xAAD3A4]..[0xAAD3A8]
    // (the quest manager 0xAACF80 +0x424): +0 kind (3 main, 4 side), +4 state (1-2 under way, 100 done, 101
    // failed), +0x14 the target object's id (a creature to speak to, or -1), +0x18/+0x1C the target's tile, +0x20
    // its radius in world units (a zone: inside it the hero is there), +0x24/+0x28 title and objective text keys.
    //
    // The quest book (the player's wish, 2026-11-09: the agent knows the quests under way, those not done, those to
    // take, and goes for the nearest objective): every quest under way, every creature offering a quest ("?!") or
    // waiting for a report ("!") the hero can take now, ordered by distance. The agent goes for the nearest it can
    // reach; one it found no way to is left a while (travel's block) and the next one taken.
    enum class GoalKind { Journal, Offer, Report };
    struct QuestGoal
    {
        GoalKind kind;
        bool main = false;
        int32_t x = 0, y = 0;       // world
        double radius = 0.0;
        uint32_t who = 0;           // creature to speak to (or -1)
        uint32_t titleKey = 0, objectiveKey = 0;
        int32_t number = 0;         // the quest's number (record +0x08), journal goals only
        int tier = 0;               // 2 a deadline running, 1 before it is lost, 0 normal, -1 last
        int32_t minutesLeft = -1;   // game minutes to its deadline, -1 none
        double distance = 0.0;
        bool blocked = false;
    };

    // The quest's number (Manolito's block X, 2026-11-10): DQ_<n> is n, R<rr>NQ<qq> is 100 + 50*rr + qq.
    constexpr int32_t rq(int32_t region, int32_t quest)
    {
        return 100 + 50 * region + quest;
    }
    // From the quest reader's table (docs/QUESTS.md, 2026-11-10): 1 = do before it is lost or because it opens
    // something (the Book of Wisdom: rune swaps and the combo master), -1 = leave for last (the Riddle in the Desert,
    // Pure Water before the Whiskey Supplier, Moca not to be fought...).
    int questPriority(int32_t number)
    {
        static constexpr int32_t kFirst[] = {15009, 15015, rq(2, 4), rq(3, 1), rq(5, 1), rq(5, 2),
                                             rq(7, 3),  rq(12, 2), rq(22, 1), rq(24, 11)};
        static constexpr int32_t kLast[] = {10259, 10260, rq(6, 2), rq(13, 1), rq(22, 3)};
        if (std::find(std::begin(kFirst), std::end(kFirst), number) != std::end(kFirst))
        {
            return 1;
        }
        if (std::find(std::begin(kLast), std::end(kLast), number) != std::end(kLast))
        {
            return -1;
        }
        return 0;
    }

    // The game minutes left on a quest's deadline, -1 if none (Manolito's block W2, 2026-11-10): the script's
    // timers at [0xAA4540] +0x2C..+0x30, 0x4C bytes each (+0 key = the quest's number, +4 length and +8 start in
    // game minutes); the clock's date at +0x18 (u32 year, u16 month, day, hour, minute). Timers of 6 h or less are
    // a scene's pauses, not deadlines. 24 game minutes go by in a real one.
    int32_t deadlineLeft(int32_t number)
    {
        const uint8_t* clk = *reinterpret_cast<const uint8_t* const*>(0xAA4540);
        if (!clk || number <= 0)
        {
            return -1;
        }
        const auto begin = *reinterpret_cast<const uint8_t* const*>(clk + 0x2C);
        const auto end = *reinterpret_cast<const uint8_t* const*>(clk + 0x30);
        constexpr size_t kTimer = 0x4C;
        if (!begin || end <= begin || (end - begin) % kTimer || (end - begin) / kTimer > 256)
        {
            return -1;
        }
        const auto u16 = [&](size_t at) { return static_cast<int64_t>(*reinterpret_cast<const uint16_t*>(clk + at)); };
        const int64_t nowMin =
            u16(0x22) +
            60 * (u16(0x20) + 24 * (u16(0x1E) + 30 * (u16(0x1C) + 12 * static_cast<int64_t>(
                                                                         *reinterpret_cast<const uint32_t*>(clk + 0x18)))));
        int64_t best = -1;
        for (const uint8_t* t = begin; t < end; t += kTimer)
        {
            const int32_t key = *reinterpret_cast<const int32_t*>(t);
            const int64_t length = *reinterpret_cast<const int32_t*>(t + 4), start = *reinterpret_cast<const int32_t*>(t + 8);
            if (key != number || length <= 360)
            {
                continue;
            }
            const int64_t left = start + length - nowMin;
            if (left > 0 && (best < 0 || left < best))
            {
                best = left;
            }
        }
        return best > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int32_t>(best);
    }
    std::vector<QuestGoal> g_questBook;     // rebuilt by questTarget, nearest first
    int g_questsDone = 0, g_questsFailed = 0;
    DWORD g_questsDoneAt = 0;           // when the count of quests done last changed
    DWORD g_questPauseUntil = 0;        // quests left a while for farming (a long spell of neither fights nor quests)
    // The yellow "?!" / "!" signs of the last pass (creatures the hero can speak to now), on the hero's layer.
    struct Sign
    {
        GoalKind kind;
        uint32_t id;
        int32_t x, y;
    };
    std::vector<Sign> g_signs;

    constexpr double kSignRange = 150.0;                    // tiles: farther quest signs are not goals
    bool goalBlocked(int32_t x, int32_t y, DWORD now);     // travel gave up on it for now
    bool talkedLately(uint32_t id, DWORD now);              // spoken to a short while ago
    void forgetTalks();                                     // a quest step done: anyone may be spoken to again

    bool questTarget(int32_t heroX, int32_t heroY, int32_t& x, int32_t& y, double& radius, uint32_t& who)
    {
        const DWORD now = GetTickCount();
        g_questBook.clear();
        const auto begin = *reinterpret_cast<const uint8_t* const*>(0xAAD3A4);
        const auto end = *reinterpret_cast<const uint8_t* const*>(0xAAD3A8);
        constexpr size_t kRecord = 0x174;
        if (begin && end > begin && !((end - begin) % kRecord) && (end - begin) / kRecord <= 512)
        {
            int done = 0, failed = 0;
            for (const uint8_t* r = begin; r < end; r += kRecord)
            {
                const int32_t kind = *reinterpret_cast<const int32_t*>(r), state = *reinterpret_cast<const int32_t*>(r + 4);
                done += state == 100;
                failed += state == 101;
                if (state != 1 && state != 2)
                {
                    continue;
                }
                const int32_t tx = *reinterpret_cast<const int32_t*>(r + 0x18), ty = *reinterpret_cast<const int32_t*>(r + 0x1C);
                if (tx <= 0 || ty <= 0)
                {
                    continue;
                }
                QuestGoal g{GoalKind::Journal};
                g.main = kind == 3;
                g.x = static_cast<int32_t>((tx + 0.5) * kTileSize);
                g.y = static_cast<int32_t>((ty + 0.5) * kTileSize);
                g.radius = std::max(2.0 * kTileSize, static_cast<double>(*reinterpret_cast<const int32_t*>(r + 0x20)));
                // +0x10/+0x14: a fixed destination's tile, -1 at +0x10 when +0x14 is the creature to speak to
                // (Manolito, 2026-11-09: the arena fight's "14a" was the tile's y, 330, not a creature).
                g.who = *reinterpret_cast<const int32_t*>(r + 0x10) == -1 ? *reinterpret_cast<const uint32_t*>(r + 0x14)
                                                                         : 0xFFFFFFFFu;
                g.titleKey = *reinterpret_cast<const uint32_t*>(r + 0x24);
                g.objectiveKey = *reinterpret_cast<const uint32_t*>(r + 0x28);
                g.number = *reinterpret_cast<const int32_t*>(r + 0x08);
                g.minutesLeft = deadlineLeft(g.number);
                g.tier = g.minutesLeft > 0 ? 2 : questPriority(g.number);
                g_questBook.push_back(g);
            }
            // A step done or failed (the journal moved on): who was spoken to lately may have the next one at once.
            // Seen live (2026-11-09): the arena won, Julius' next quest waited out the 2 min of «spoken to lately»
            // with the hero roaming the yard.
            static bool s_journalRead = false;  // this run read the journal before (a failure is a change since)
            const bool readBefore = s_journalRead;
            s_journalRead = true;
            if ((done != g_questsDone || failed != g_questsFailed) && (g_questsDone || g_questsFailed || done || failed))
            {
                LOG("AutoAttack: the journal moved on ({} done, {} failed): anyone may be spoken to again", done, failed);
                // A quest done (the count up, not the reset of a new game): the copy's game saved (the player,
                // 2026-11-11: «el momento de guardar partida tiene que ser en el momento que se supera una mision»).
                if (done == g_questsDone + 1)   // one more; a jump is the journal first read after a load
                {
                    AgentSave::questDone();
                }
                // A quest failed: a death (the player, 2026-11-11: «el fracaso cuenta como muerte»). Only one more
                // than this run read before, not the first read of a run nor right after a load.
                if (readBefore && failed == g_questsFailed + 1 && !AgentSave::worldChanging())
                {
                    LOG("AutoAttack: a quest failed ({} failed now): as a death, the last save loaded", failed);
                    remember("falló una misión: cuenta como muerte");
                    AgentSave::questFailed();
                }
                Findings::note("quest", heroX, heroY, "the journal moved on: {} done, {} failed", done, failed);
                forgetTalks();
            }
            // When a quest was last done: set at each read of the journal it was every few seconds, so neither the
            // stuck report (AgentStuck: «no quest done») nor the 8 min with no fight nor quest ever came.
            if (done > g_questsDone && readBefore)
            {
                g_questsDoneAt = GetTickCount();
            }
            g_questsDone = done;
            g_questsFailed = failed;
        }
        for (const Sign& sign : g_signs)
        {
            // A creature the journal already leads to is not a goal of its own.
            const bool known = std::any_of(g_questBook.begin(), g_questBook.end(), [&](const QuestGoal& g) {
                return g.kind == GoalKind::Journal && g.who == sign.id;
            });
            // Only signs near enough to walk to: the scan sees the whole world's creatures, and a sign thousands of
            // tiles off (seen live: 2474 and 2924 tiles, from inside the closed arena) is no goal for now.
            const double off = std::hypot(static_cast<double>(sign.x - heroX), static_cast<double>(sign.y - heroY));
            if (known || talkedLately(sign.id, now) || off > kSignRange * kTileSize)
            {
                continue;
            }
            QuestGoal g{sign.kind};
            g.x = sign.x;
            g.y = sign.y;
            g.radius = 2.0 * kTileSize;
            g.who = sign.id;
            g_questBook.push_back(g);
        }
        for (QuestGoal& g : g_questBook)
        {
            g.distance = wayLength(heroX, heroY, g.x, g.y);
            g.blocked = goalBlocked(g.x, g.y, now);
        }
        std::stable_sort(g_questBook.begin(), g_questBook.end(), [](const QuestGoal& a, const QuestGoal& b) {
            if (a.blocked != b.blocked)
            {
                return !a.blocked;
            }
            // The quests first by their weight (QUESTS.md and the deadlines), then the nearest: one with a
            // deadline running, the soonest first; one that is lost if left; the rest; those for last.
            if (a.tier != b.tier)
            {
                return a.tier > b.tier;
            }
            if (a.tier == 2 && a.minutesLeft != b.minutesLeft)
            {
                return a.minutesLeft < b.minutesLeft;
            }
            return a.distance < b.distance;
        });
        if (g_questBook.empty() || g_questBook.front().blocked)
        {
            return false;
        }
        const QuestGoal& g = g_questBook.front();
        x = g.x;
        y = g.y;
        radius = g.radius;
        who = g.who;
        if (g.kind == GoalKind::Journal)
        {
            Lore::readQuest(g.titleKey, g.objectiveKey);     // the quest's title and objective read: its kind
        }
        static uint32_t told = 0;
        const uint32_t pick = g.who ^ static_cast<uint32_t>(g.x) ^ (static_cast<uint32_t>(g.y) << 7);
        if (pick != told)
        {
            told = pick;
            LOG("AutoAttack: quest book: {} goals ({} done, {} failed), going for: {} {} (weight {}, {} game min left) "
                "at {:.0f} tiles",
                g_questBook.size(), g_questsDone, g_questsFailed,
                g.kind == GoalKind::Journal ? (g.main ? "main quest" : "side quest")
                                            : g.kind == GoalKind::Offer ? "a quest offered" : "a report",
                g.number, g.tier, g.minutesLeft, g.distance / kTileSize);
        }
        return true;
    }

    void drawQuestBook()
    {
        ImGui::Text("Misiones hechas: %d, fallidas: %d", g_questsDone, g_questsFailed);
        if (g_questBook.empty())
        {
            ImGui::TextDisabled("Ninguna misión en curso ni ofrecida a la vista.");
            return;
        }
        ImGui::TextDisabled("Por peso (plazo, antes de perderse, normal, al final) y cercanía; la primera es la que sigue el agente.");
        int n = 0;
        for (const QuestGoal& g : g_questBook)
        {
            std::string line = std::to_string(++n) + ". ";
            if (g.kind == GoalKind::Journal)
            {
                const std::string title = Lore::textUtf8(g.titleKey), objective = Lore::textUtf8(g.objectiveKey);
                line += std::string(g.main ? "[principal] " : "[secundaria] ") + (title.empty() ? "?" : title);
                if (!objective.empty())
                {
                    line += ": " + objective;
                }
            }
            else
            {
                line += g.kind == GoalKind::Offer ? "[ofrece misión] alguien con ?!" : "[informar] alguien con !";
            }
            line += " (" + std::to_string(static_cast<int>(g.distance / kTileSize)) + " casillas)";
            if (g.blocked)
            {
                line += " - sin camino por ahora";
            }
            ImGui::TextWrapped("%s", line.c_str());
        }
    }

    // The quest signs for the agent's map, once a second: every creature the game marks with an overhead icon (object
    // flags +0x14 & 0x80000), its icon as npcIconId (ENG 00499E90) picks it: crafts first (creature +0x200: 0x1000
    // smith, 0x2000 merchant, 0x4000 combo master, 0x4000000 stable; none of them a quest sign), else the dialog record
    // +0x245 (u32 index, seen live 2026-11-09; 0 none) of [0xAACF80 +0x755C .. +0x7560), 0x50 bytes, icon at +0x48:
    // 0 "?!" (dialogs "auftrag*", quest offered), 1 "!" ("ziel*", report), 4/5 the others ("?"); +10 greyed (taken
    // first for «not yet for the hero»; by what was seen live, the side quests: to be confirmed by Manolito). Plus the journal's targets under way, the one the agent goes for in yellow.
    DWORD g_questMarksAt = 0;
    // The merchants in the world (creature +0x200 & 0x2000), from the same pass: where a full backpack goes.
    struct Merchant
    {
        uint32_t id;
        uint8_t layer;
        int32_t x, y;
    };
    std::vector<Merchant> g_merchants;
    // The combo masters (creature +0x200 & 0x4000): where runes of other classes are swapped for the hero's own.
    std::vector<Merchant> g_masters;
    // The smiths (+0x200 & 0x1000): where sockets are filled.
    std::vector<Merchant> g_smiths;
    // The stable men (+0x200 & 0x4000000): where a horse is bought.
    std::vector<Merchant> g_stables;

    // Farming zones, from the same pass (the object list holds the whole world): living hostile creatures on the
    // surface counted per sector (64x64 tiles), the sector's level range from its block ([sector +0x17C] +0xD3 min,
    // +0xD5 max; sector id u16 at world +0x284 + 2 * ((ty >> 6) * 128 + (tx >> 6)), sector [world +0x240][id]; seen
    // live 2026-11-09). The best: many enemies, of the hero's level, not too far.
    struct FarmCount
    {
        int count = 0;
        int64_t sumX = 0, sumY = 0;     // tiles, for where they gather
    };
    std::unordered_map<int, FarmCount> g_farmBuilding;

    // Chests not yet opened anywhere on the surface (the object list holds the whole world; opened: flags +0x14 &
    // 0x4000), the nearest kept as a place to go: no need to explore blind for them.
    uint32_t g_chestVtable = 0;
    uint32_t g_chestId = 0, g_chestBuilding = 0;
    double g_chestBest = 0.0;
    int32_t g_chestX = 0, g_chestY = 0;
    DWORD g_chestSince = 0;            // since when the hero heads for this one
    uint32_t g_chestHeading = 0;

    bool isChest(const uint8_t* o)
    {
        const uint32_t vtable = *reinterpret_cast<const uint32_t*>(o);
        if (g_chestVtable)
        {
            return vtable == g_chestVtable;
        }
        const auto* locator = reinterpret_cast<const CompleteObjectLocator* const*>(vtable)[-1];
        if (locator && locator->type && std::strcmp(locator->type->name, ".?AVcChest3D@@") == 0)
        {
            g_chestVtable = vtable;
            return true;
        }
        return false;
    }
    bool g_farmKnown = false;
    int32_t g_farmX = 0, g_farmY = 0;   // world: the chosen zone's middle (where its enemies gather)
    int g_farmSector = -1;

    bool sectorLevels(const uint8_t* world, int tx, int ty, int& lo, int& hi)
    {
        if (!world || tx < 0 || ty < 0 || tx >= 128 * 64 || ty >= 128 * 64)
        {
            return false;
        }
        const uint16_t id = *reinterpret_cast<const uint16_t*>(world + 0x284 + 2 * ((ty >> 6) * 128 + (tx >> 6)));
        const auto table = *reinterpret_cast<const uint8_t* const* const*>(world + 0x240);
        const auto tableEnd = *reinterpret_cast<const uint8_t* const* const*>(world + 0x244);
        if (!table || id >= tableEnd - table || !table[id])
        {
            return false;
        }
        const uint8_t* block = *reinterpret_cast<const uint8_t* const*>(table[id] + 0x17C);
        if (!block)
        {
            return false;
        }
        lo = *reinterpret_cast<const uint16_t*>(block + 0xD3);
        hi = *reinterpret_cast<const uint16_t*>(block + 0xD5);
        return hi > 0;
    }

    // The region of a tile's sector (Manolito, AGENT_ADDRESSES «Dónde está el héroe»: [sector+0x17C] +0xD7, the
    // level range beside it at +0xD3/+0xD5), or -1. Regions span many sectors; the game names them by script only.
    int sectorRegion(const uint8_t* world, int tx, int ty)
    {
        if (!world || tx < 0 || ty < 0 || tx >= 128 * 64 || ty >= 128 * 64)
        {
            return -1;
        }
        const uint16_t id = *reinterpret_cast<const uint16_t*>(world + 0x284 + 2 * ((ty >> 6) * 128 + (tx >> 6)));
        const auto table = *reinterpret_cast<const uint8_t* const* const*>(world + 0x240);
        const auto tableEnd = *reinterpret_cast<const uint8_t* const* const*>(world + 0x244);
        if (!table || id >= tableEnd - table || !table[id])
        {
            return -1;
        }
        const uint8_t* block = *reinterpret_cast<const uint8_t* const*>(table[id] + 0x17C);
        return block ? *(block + 0xD7) : -1;
    }

    // The zone the hero is in (the player's wish, 2026-11-09: the agent knows which zone it is in, what it can do
    // there, and whether staying is worth it; with nothing to do there it looks for another). A zone is a region of
    // sectors; what it holds is counted from what the once-a-second scan sees: quest goals (journal and signs),
    // living enemies of a fitting level, chests not yet opened.
    struct ZoneBrief
    {
        bool valid = false;
        int region = -1, lo = 0, hi = 0, heroLevel = 0;
        int quests = 0, enemies = 0, chests = 0;
        bool worth = false;
        // With nothing here: the next zone's goal (a quest sign beyond the book's 150 tiles), if one is known.
        bool next = false;
        int nextRegion = -1;
        int32_t nextX = 0, nextY = 0;
        double nextDistance = 0.0;
        GoalKind nextKind = GoalKind::Offer;
    };
    ZoneBrief g_zone;

    void judgeZone(const uint8_t* world, int heroLevel, int32_t heroX, int32_t heroY, DWORD now)
    {
        ZoneBrief z;
        const int htx = static_cast<int>(heroX / kTileSize), hty = static_cast<int>(heroY / kTileSize);
        z.region = sectorRegion(world, htx, hty);
        if (z.region < 0)
        {
            g_zone = z;
            return;
        }
        z.valid = true;
        z.heroLevel = heroLevel;
        sectorLevels(world, htx, hty, z.lo, z.hi);
        const auto inZone = [&](int32_t x, int32_t y) {
            return sectorRegion(world, static_cast<int>(x / kTileSize), static_cast<int>(y / kTileSize)) == z.region;
        };
        for (const QuestGoal& g : g_questBook)
        {
            z.quests += !g.blocked && inZone(g.x, g.y);
        }
        for (const auto& [key, f] : g_farmBuilding)
        {
            const int n = std::max(f.count, 1);
            const int cx = static_cast<int>(f.sumX / n), cy = static_cast<int>(f.sumY / n);
            int lo = 0, hi = 0;
            // Not far below the hero either: a level 11 Dwarf «worth staying» in a 1-4 region, 269 enemies there
            // giving next to nothing (the analyst, 2026-11-12: 54 xp/h).
            if (sectorRegion(world, cx, cy) == z.region && sectorLevels(world, cx, cy, lo, hi) && lo <= heroLevel + 5 &&
                hi + 4 >= heroLevel)
            {
                z.enemies += f.count;
            }
        }
        z.chests = g_chestBuilding && inZone(g_chestX, g_chestY) ? 1 : 0;
        z.worth = z.quests > 0 || z.enemies >= 3 || z.chests > 0;
        if (!z.worth)
        {
            // The nearest quest sign anywhere (the book keeps only those within 150 tiles), not spoken to lately and
            // not given up on by travel: the next zone to go to.
            for (const Sign& sign : g_signs)
            {
                const double d = std::hypot(static_cast<double>(sign.x - heroX), static_cast<double>(sign.y - heroY));
                if (talkedLately(sign.id, now) || goalBlocked(sign.x, sign.y, now) || (z.next && d >= z.nextDistance))
                {
                    continue;
                }
                z.next = true;
                z.nextX = sign.x;
                z.nextY = sign.y;
                z.nextDistance = d;
                z.nextKind = sign.kind;
                z.nextRegion = sectorRegion(world, static_cast<int>(sign.x / kTileSize), static_cast<int>(sign.y / kTileSize));
            }
        }
        static int toldRegion = -2, toldWorth = -1, toldNext = -2;
        if (z.region != toldRegion || static_cast<int>(z.worth) != toldWorth || (!z.worth && z.nextRegion != toldNext))
        {
            toldRegion = z.region;
            toldWorth = z.worth;
            toldNext = z.nextRegion;
            if (z.worth)
            {
                LOG("Zone: region {} (levels {}-{}, hero {}): {} quest goals, {} enemies of its level, {} chests: worth "
                    "staying",
                    z.region, z.lo, z.hi, heroLevel, z.quests, z.enemies, z.chests);
            }
            else if (z.next)
            {
                LOG("Zone: region {} (levels {}-{}, hero {}): nothing to do here; next: {} in region {} at {:.0f} tiles",
                    z.region, z.lo, z.hi, heroLevel, z.nextKind == GoalKind::Report ? "a report" : "a quest offered",
                    z.nextRegion, z.nextDistance / kTileSize);
            }
            else
            {
                LOG("Zone: region {} (levels {}-{}, hero {}): nothing to do here and no other zone's goal known: "
                    "farming or exploring",
                    z.region, z.lo, z.hi, heroLevel);
            }
        }
        g_zone = z;
    }

    void drawZone()
    {
        const ZoneBrief& z = g_zone;
        if (!z.valid)
        {
            ImGui::TextDisabled("Zona aún sin leer.");
            return;
        }
        ImGui::Text("Región %d, niveles %d-%d (héroe %d)", z.region, z.lo, z.hi, z.heroLevel);
        ImGui::Text("Misiones aquí: %d   Enemigos de su nivel: %d   Cofres sin abrir: %d", z.quests, z.enemies, z.chests);
        if (z.worth)
        {
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Vale la pena quedarse.");
        }
        else if (z.next)
        {
            ImGui::TextWrapped("Nada que hacer aquí: siguiente zona, región %d (%s, %.0f casillas).", z.nextRegion,
                               z.nextKind == GoalKind::Report ? "informar" : "misión ofrecida", z.nextDistance / kTileSize);
        }
        else
        {
            ImGui::TextWrapped("Nada que hacer aquí ni otra zona con misión conocida: cultivar o explorar.");
        }
    }

    // Where the hero died lately (tiles): the farming zone it was in and where it fell, shunned 30 min (the live
    // verifier, 2026-11-11: A3 died 8 times running in the zone at 3223,2664 «levels 1-4», loaded and went back to it).
    bool nearDeathSpot(int tx, int ty)
    {
        return AgentSave::nearDeathSpot(tx, ty);
    }

    void chooseFarm(const uint8_t* world, int heroLevel, int32_t heroX, int32_t heroY)
    {
        float best = 0.0f;
        int bestKey = -1;
        int32_t bestX = 0, bestY = 0;
        int bestLo = 0, bestHi = 0, bestCount = 0;
        for (const auto& [key, f] : g_farmBuilding)
        {
            if (f.count < 3)
            {
                continue;
            }
            const int cx = static_cast<int>(f.sumX / f.count), cy = static_cast<int>(f.sumY / f.count);
            int lo = 0, hi = 0;
            if (!sectorLevels(world, cx, cy, lo, hi))
            {
                continue;
            }
            // Not a zone well above the hero's level, by the class's mind (the class strategist, 2026-11-10: the
            // Gladiator at level 1 sent to a zone of 19-32, the Daemon to one of 45-80).
            if (lo > heroLevel + Mind::now().farmLevelSlack)
            {
                continue;
            }
            if (nearDeathSpot(cx, cy))
            {
                continue;
            }
            // Careful (2+ deaths from the same save): only ground whose enemies the hero tops or matches (the analyst,
            // 2026-11-11: the Mage, level 3 with 102 health, sent to a zone of levels 4-7 and dead 15 times running).
            if (AgentSave::deathsSinceSave() >= 2 && hi > heroLevel + 1)
            {
                continue;
            }
            if (outmatched(GetTickCount()) && hi > heroLevel)
            {
                continue;
            }
            // Of its level: full worth; below it less and less (little experience); well above it, risky.
            float fit = 1.0f;
            if (heroLevel > hi)
            {
                // Over 4 levels below: next to no experience, a last resort only (A7, level 11, in a 1-4 zone).
                fit = hi + 4 < heroLevel ? 0.03f : std::max(0.15f, static_cast<float>(hi) / static_cast<float>(heroLevel));
            }
            else if (lo > heroLevel + 5)
            {
                fit = 0.25f;
            }
            double tiles = std::hypot(cx - heroX / kTileSize, cy - heroY / kTileSize);
            if (tiles < 300.0)
            {
                // The walk there, not the straight line.
                tiles = wayLength(heroX, heroY, static_cast<int32_t>((cx + 0.5) * kTileSize),
                                  static_cast<int32_t>((cy + 0.5) * kTileSize)) / kTileSize;
            }
            float score = static_cast<float>(f.count) * fit / static_cast<float>(1.0 + tiles / 150.0);
            if (key == g_farmSector)
            {
                score *= 1.5f;      // the zone chosen stays unless another is clearly better
            }
            if (score > best)
            {
                best = score;
                bestKey = key;
                bestX = static_cast<int32_t>((cx + 0.5) * kTileSize);
                bestY = static_cast<int32_t>((cy + 0.5) * kTileSize);
                bestLo = lo;
                bestHi = hi;
                bestCount = f.count;
            }
        }
        g_farmKnown = bestKey >= 0;
        if (g_farmKnown)
        {
            if (bestKey != g_farmSector)
            {
                LOG("AutoAttack: farming zone at tile {},{}: {} enemies, levels {}-{} (hero {}), {:.0f} tiles away",
                    static_cast<int>(bestX / kTileSize), static_cast<int>(bestY / kTileSize), bestCount, bestLo, bestHi,
                    heroLevel, std::hypot(static_cast<double>(bestX - heroX), static_cast<double>(bestY - heroY)) / kTileSize);
                g_farmSector = bestKey;
            }
            g_farmX = bestX;
            g_farmY = bestY;
        }
    }


    // The door or way in the agent heads for (an interior, a quest's house), for the overlay; 0 at none.
    int32_t g_doorX = 0, g_doorY = 0;
    DWORD g_doorAt = 0;

    void noteDoor(int tx, int ty)
    {
        g_doorX = static_cast<int32_t>((tx + 0.5) * kTileSize);
        g_doorY = static_cast<int32_t>((ty + 0.5) * kTileSize);
        g_doorAt = GetTickCount();
    }

    void scanQuestMarks(DWORD now, int32_t heroX, int32_t heroY, int heroLevel, uint8_t heroLayer)
    {
        if (g_questMarksAt && now - g_questMarksAt < 1000)
        {
            return;
        }
        g_questMarksAt = now;
        g_signs.clear();
        std::vector<MapMemory::QuestMark> marks;
        g_merchants.clear();
        g_masters.clear();
        g_smiths.clear();
        g_stables.clear();
        g_wildHorses.clear();
        g_farmBuilding.clear();
        g_chestBuilding = 0;
        g_chestBest = 0.0;
        g_aggroFoes.clear();
        g_aggroHeroX = heroX;
        g_aggroHeroY = heroY;
        const auto manager = *reinterpret_cast<const uint8_t* const*>(0xAD5C40);
        const auto dialogs = *reinterpret_cast<const uint8_t* const*>(0xAACF80 + 0x755C);
        const auto dialogsEnd = *reinterpret_cast<const uint8_t* const*>(0xAACF80 + 0x7560);
        if (manager && dialogs && dialogsEnd > dialogs)
        {
            const size_t dialogCount = static_cast<size_t>(dialogsEnd - dialogs) / 0x50;
            const auto table = *reinterpret_cast<const uint8_t* const* const*>(manager + 4);
            const auto tableEnd = *reinterpret_cast<const uint8_t* const* const*>(manager + 8);
            if (table && tableEnd > table && tableEnd - table < 0x40000)
            {
                for (const uint8_t* const* it = table; it < tableEnd; ++it)
                {
                    const uint8_t* o = *it;
                    // A living hostile creature on the surface, for the farming zones and the aggro rings: hostile
                    // to the hero's side (1) by the game's own live side matrix (Manolito, 2026-11-10: u8
                    // [0x8EB548 + a*16 + b], 0 hostile; by default 0, 3 civilians, 4 horses, 6 animals such as
                    // rabbits and 7 soldiers are not).
                    if (o && *reinterpret_cast<const uint32_t*>(o) >= 0x880000 && *reinterpret_cast<const uint32_t*>(o) < 0x8A0000 &&
                        (*reinterpret_cast<const uint32_t*>(o + 0xC) & 0xFFFF) == static_cast<uint32_t>(it - table) &&
                        !*(o + 0x24) && reinterpret_cast<cObject*>(const_cast<uint8_t*>(o))->asCreature() &&
                        *reinterpret_cast<const int32_t*>(o + 0x4D8) > 0)
                    {
                        const uint32_t side = *reinterpret_cast<const uint32_t*>(o + 0x1F0);
                        const auto* sides = reinterpret_cast<const uint8_t*>(0x8EB548);
                        if (side == 4 && heroLayer == 0 && !*reinterpret_cast<const uint32_t*>(o + 0x570))
                        {
                            const int32_t hx = *reinterpret_cast<const int32_t*>(o + 0x1C), hy = *reinterpret_cast<const int32_t*>(o + 0x20);
                            if (std::hypot(static_cast<double>(hx - heroX), static_cast<double>(hy - heroY)) < 25.0 * kTileSize)
                            {
                                g_wildHorses.push_back({*reinterpret_cast<const uint32_t*>(o + 0xC), hx, hy, Gear::horseWorth(o)});
                            }
                        }
                        if (side < 16 && side != 1 && sides[16 + side] == 0)
                        {
                            const int tx = static_cast<int>(*reinterpret_cast<const int32_t*>(o + 0x1C) / kTileSize);
                            const int ty = static_cast<int>(*reinterpret_cast<const int32_t*>(o + 0x20) / kTileSize);
                            const int32_t fx = *reinterpret_cast<const int32_t*>(o + 0x1C);
                            const int32_t fy = *reinterpret_cast<const int32_t*>(o + 0x20);
                            if (heroLayer == 0 && std::hypot(static_cast<double>(fx - heroX), static_cast<double>(fy - heroY)) <
                                                      40.0 * kTileSize)
                            {
                                const int32_t target = *reinterpret_cast<const int32_t*>(o + 0x251);
                                const uint32_t ai = *reinterpret_cast<const uint32_t*>(o + 0x1F4);
                                g_aggroFoes.push_back({static_cast<uint32_t>(it - table), fx, fy, target >= 1 && target <= 0x10 && (ai & 4) && !(ai & 0x100)});
                            }
                            FarmCount& f = g_farmBuilding[(tx >> 6) * 1024 + (ty >> 6)];
                            ++f.count;
                            f.sumX += tx;
                            f.sumY += ty;
                        }
                    }
                    if (o && *reinterpret_cast<const uint32_t*>(o) >= 0x880000 && *reinterpret_cast<const uint32_t*>(o) < 0x8A0000 &&
                        (*reinterpret_cast<const uint32_t*>(o + 0xC) & 0xFFFF) == static_cast<uint32_t>(it - table) &&
                        !*(o + 0x24) && isChest(o) && !(*reinterpret_cast<const uint32_t*>(o + 0x14) & 0x4000))
                    {
                        const uint32_t id = *reinterpret_cast<const uint32_t*>(o + 0xC);
                        double d = std::hypot(static_cast<double>(*reinterpret_cast<const int32_t*>(o + 0x1C) - heroX),
                                              static_cast<double>(*reinterpret_cast<const int32_t*>(o + 0x20) - heroY));
                        if (d < 150.0 * kTileSize && !lootBlocked(id, now) && !g_lootDone.count(lootKey(id)))
                        {
                            d = wayLength(heroX, heroY, *reinterpret_cast<const int32_t*>(o + 0x1C),
                                          *reinterpret_cast<const int32_t*>(o + 0x20));    // the walk
                        }
                        if (!lootBlocked(id, now) && !g_lootDone.count(lootKey(id)) && (!g_chestBuilding || d < g_chestBest))
                        {
                            g_chestBuilding = id;
                            g_chestBest = d;
                            g_chestX = *reinterpret_cast<const int32_t*>(o + 0x1C);
                            g_chestY = *reinterpret_cast<const int32_t*>(o + 0x20);
                        }
                    }
                    if (o && (*reinterpret_cast<const uint32_t*>(o + 0x14) & 0x80000) &&
                        (*reinterpret_cast<const uint32_t*>(o + 0x200) & 0x2000))
                    {
                        g_merchants.push_back({*reinterpret_cast<const uint32_t*>(o + 0xC), *(o + 0x24),
                                               *reinterpret_cast<const int32_t*>(o + 0x1C),
                                               *reinterpret_cast<const int32_t*>(o + 0x20)});
                    }
                    if (o && (*reinterpret_cast<const uint32_t*>(o + 0x14) & 0x80000) &&
                        (*reinterpret_cast<const uint32_t*>(o + 0x200) & 0x4000))
                    {
                        g_masters.push_back({*reinterpret_cast<const uint32_t*>(o + 0xC), *(o + 0x24),
                                             *reinterpret_cast<const int32_t*>(o + 0x1C),
                                             *reinterpret_cast<const int32_t*>(o + 0x20)});
                    }
                    if (o && (*reinterpret_cast<const uint32_t*>(o + 0x14) & 0x80000) &&
                        (*reinterpret_cast<const uint32_t*>(o + 0x200) & 0x1000))
                    {
                        g_smiths.push_back({*reinterpret_cast<const uint32_t*>(o + 0xC), *(o + 0x24),
                                            *reinterpret_cast<const int32_t*>(o + 0x1C),
                                            *reinterpret_cast<const int32_t*>(o + 0x20)});
                    }
                    if (o && (*reinterpret_cast<const uint32_t*>(o + 0x14) & 0x80000) &&
                        (*reinterpret_cast<const uint32_t*>(o + 0x200) & 0x4000000))
                    {
                        g_stables.push_back({*reinterpret_cast<const uint32_t*>(o + 0xC), *(o + 0x24),
                                             *reinterpret_cast<const int32_t*>(o + 0x1C),
                                             *reinterpret_cast<const int32_t*>(o + 0x20)});
                    }
                    if (!o || !(*reinterpret_cast<const uint32_t*>(o + 0x14) & 0x80000) ||
                        (*reinterpret_cast<const uint32_t*>(o + 0x200) & (0x1000 | 0x2000 | 0x4000 | 0x4000000)))
                    {
                        continue;
                    }
                    const uint32_t dialog = *reinterpret_cast<const uint32_t*>(o + 0x245);
                    if (!dialog || dialog >= dialogCount)
                    {
                        continue;
                    }
                    const uint32_t icon = *reinterpret_cast<const uint32_t*>(dialogs + dialog * 0x50 + 0x48);
                    MapMemory::Glyph glyph;
                    switch (icon % 10)
                    {
                    case 0: glyph = MapMemory::Glyph::Offer; break;
                    case 1: glyph = MapMemory::Glyph::Report; break;
                    case 4:
                    case 5: glyph = MapMemory::Glyph::Other; break;
                    default: continue;
                    }
                    if (icon >= 20)
                    {
                        continue;
                    }
                    marks.push_back({glyph, icon < 10, *(o + 0x24), *reinterpret_cast<const int32_t*>(o + 0x1C),
                                     *reinterpret_cast<const int32_t*>(o + 0x20)});
                    // A quest to take or a report to make, yellow or grey: the yellow ones are the main quest's (seen live
                    // 2026-11-09: the only yellow sign was the Vampiress's main quest giver), the grey ones the side quests
                    // (up to 7 in a town, never spoken to: the player saw no side quest taken).
                    if (*(o + 0x24) == heroLayer &&
                        (glyph == MapMemory::Glyph::Offer || glyph == MapMemory::Glyph::Report))
                    {
                        g_signs.push_back({glyph == MapMemory::Glyph::Offer ? GoalKind::Offer : GoalKind::Report,
                                           *reinterpret_cast<const uint32_t*>(o + 0xC), *reinterpret_cast<const int32_t*>(o + 0x1C),
                                           *reinterpret_cast<const int32_t*>(o + 0x20)});
                    }
                }
            }
        }
        // The journal: every quest under way, its target a ring; the agent's own (questTarget's pick) in yellow.
        int32_t gx = 0, gy = 0;
        double radius = 0.0;
        uint32_t who = 0;
        const bool goal = questTarget(heroX, heroY, gx, gy, radius, who);
        const auto begin = *reinterpret_cast<const uint8_t* const*>(0xAAD3A4);
        const auto end = *reinterpret_cast<const uint8_t* const*>(0xAAD3A8);
        constexpr size_t kRecord = 0x174;
        if (begin && end > begin && !((end - begin) % kRecord) && (end - begin) / kRecord <= 512)
        {
            for (const uint8_t* r = begin; r < end; r += kRecord)
            {
                const int32_t state = *reinterpret_cast<const int32_t*>(r + 4);
                const int32_t tx = *reinterpret_cast<const int32_t*>(r + 0x18), ty = *reinterpret_cast<const int32_t*>(r + 0x1C);
                if ((state != 1 && state != 2) || tx <= 0 || ty <= 0)
                {
                    continue;
                }
                const int32_t wx = static_cast<int32_t>((tx + 0.5) * kTileSize), wy = static_cast<int32_t>((ty + 0.5) * kTileSize);
                marks.push_back({MapMemory::Glyph::Target, goal && wx == gx && wy == gy, 0, wx, wy});
            }
        }
        // Told when what it knows changes: signs within 60 tiles by kind, and the journal's targets.
        {
            int close[3] = {}, yellow = 0, targets = 0;
            for (const MapMemory::QuestMark& q : marks)
            {
                if (q.glyph == MapMemory::Glyph::Target)
                {
                    ++targets;
                }
                else if (std::hypot(static_cast<double>(q.x - heroX), static_cast<double>(q.y - heroY)) < 60 * kTileSize)
                {
                    ++close[static_cast<int>(q.glyph)];
                    yellow += q.yellow;
                }
            }
            static int told = -1;
            const int seen = close[0] | close[1] << 8 | close[2] << 16 | yellow << 24 | targets << 28;
            if (seen != told)
            {
                told = seen;
                LOG("AutoAttack: quest signs within 60 tiles: {} ?!, {} !, {} ? ({} yellow); {} quests under way; {} "
                    "merchants known",
                    close[0], close[1], close[2], yellow, targets, g_merchants.size());
            }
        }
        chooseFarm(manager ? *reinterpret_cast<const uint8_t* const*>(manager) : nullptr, heroLevel, heroX, heroY);
        g_chestId = g_chestBest < 150.0 * kTileSize ? g_chestBuilding : 0;
        judgeZone(manager ? *reinterpret_cast<const uint8_t* const*>(manager) : nullptr, heroLevel, heroX, heroY, now);
        MapMemory::setQuestMarks(std::move(marks));
        // A stable within 60 tiles selling a horse clearly better than the hero's (two levels or more): Gear goes to
        // buy it (the player, 2026-11-10: compare before changing horses).
        {
            bool better = false;
            cObjectManager* om = cObjectManager::instance();
            const cCreature* hero = om ? om->hero() : nullptr;
            const uint32_t ownId = hero ? *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + 0x570) : 0;
            const cCreature* own = ownId ? om->creature(ownId) : nullptr;
            if (own)
            {
                const int ownWorth = Gear::horseWorth(own);
                for (const Merchant& m : g_stables)
                {
                    const auto* man = reinterpret_cast<const uint8_t*>(om->object(m.id));
                    const uint32_t sale = man ? *reinterpret_cast<const uint32_t*>(man + 0x570) : 0;
                    const cCreature* h = sale ? om->creature(sale) : nullptr;
                    if (h && std::hypot(static_cast<double>(m.x - heroX), static_cast<double>(m.y - heroY)) < 60.0 * kTileSize &&
                        wayLength(heroX, heroY, m.x, m.y) < 60.0 * kTileSize && Gear::horseWorth(h) >= ownWorth + 2000)
                    {
                        better = true;
                    }
                }
            }
            Gear::setHorseUpgrade(better);
        }
    }

    DWORD g_questAskAt = 0;
    // The merchant trip (Gear::wantsShop): the merchant gone to, when the walk and the click were last asked for.
    uint32_t g_shopId = 0;
    DWORD g_shopAskAt = 0, g_shopClickAt = 0;
    int g_shopTries = 0;
    bool g_shopTold = false;
    std::unordered_map<uint32_t, DWORD> g_shopFailedAt;     // merchants that would not open: left 5 min
    int32_t g_questX = 0, g_questY = 0;

    // Speaking to the quest's creature (the quest giver, or whom to report to): a click on it, as the player's, then
    // its dialog answered with the first answer (Enter: accept, go on, report) until it closes. Not again for a
    // while after (the journal moves on to the next target once the script took it).
    // A creature's side (+0x1F0, seen live 2026-11-09): 1 hero, 3 civilians, 4 horses, 7 soldiers are spoken to.
    constexpr size_t kSide = 0x1F0;
    constexpr DWORD kTalkAgainMs = 120000;
    // Creatures that would not speak, and how often: each time four times as long before the next try (2, 8, 32 min,
    // then left for good). Seen live (2026-11-10, the analyst): a hireling for hire taken for the quest creature, 41
    // tries in 24 minutes.
    std::unordered_map<uint32_t, int> g_talkFails;
    uint32_t g_talkId = 0;
    DWORD g_talkAt = 0;
    int g_talkTries = 0;
    std::unordered_map<uint32_t, DWORD> g_talkedAt;

    bool talkable(cObject* object)
    {
        if (!object || !object->asCreature())
        {
            return false;
        }
        const uint32_t side = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(object) + kSide);
        return side == 1 || side == 3 || side == 4 || side == 7;
    }

    // A long walk (to a quest, a farming zone, a merchant) watched for progress: ClickWalk can stay on its route with
    // the hero shaking on one spot (seen live 2026-11-09, 194 tiles to a quest). No 2 tiles nearer in 20 s: the route
    // dropped and a step aside (8 tiles, a random way) before trying again; the third time the goal is left 3 min.
    struct Travel
    {
        int32_t x = 0, y = 0;
        double best = 0.0;
        DWORD bestAt = 0, lastAt = 0, detourUntil = 0;
        int fails = 0;
        int32_t movedX = 0, movedY = 0;     // where the hero last was 3 tiles on along a route
        DWORD aroundSince = 0;              // since when moving along a route with no nearer for it (0: not)
    };
    Travel g_travel;

    // A way round (a gate, a bridge, a river's bend: the player, 2026-11-11, «el agente continúa con problemas con
    // las puertas»): along a route the straight line may grow before it shrinks. A hero 3 tiles on from where it was,
    // following ClickWalk's route, is making progress, for up to 2 min with the goal no nearer.
    bool goingAround(int32_t& movedX, int32_t& movedY, DWORD& since, const cCreature* self, DWORD now)
    {
        if (!self || !ClickWalk::routing())
        {
            return false;
        }
        if (std::hypot(static_cast<double>(self->x - movedX), static_cast<double>(self->y - movedY)) < 3.0 * kTileSize)
        {
            return false;
        }
        movedX = self->x;
        movedY = self->y;
        if (!since)
        {
            since = now;
        }
        return now - since < 120000;
    }
    std::unordered_map<uint64_t, DWORD> g_travelBlocked;

    bool goalBlocked(int32_t x, int32_t y, DWORD now)
    {
        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(x / kTileSize)) << 32) |
                             static_cast<uint32_t>(y / kTileSize);
        const auto it = g_travelBlocked.find(key);
        return it != g_travelBlocked.end() && static_cast<int>(now - it->second) < 0;
    }

    // A walk to a near tile that gets no nearer (seen live: 5 min at 24 tiles from a tile the fill reached but the
    // game's path finder does not): true once 20 s pass without a tile of progress; that tile is then left 2 min.
    bool approachStalled(int32_t wx, int32_t wy, double distance, DWORD now)
    {
        static uint64_t s_key = 0;
        static double s_best = 0.0;
        static DWORD s_since = 0;
        static std::unordered_map<uint64_t, DWORD> s_left;
        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(wx)) << 32) | static_cast<uint32_t>(wy);
        // Each time again no nearer, left longer (the analyst, 2026-11-11: 333 «no nearer» in 3 h over the 8 copies,
        // ~1.9 h lost, the same tiles again and again, 3174,2794 39 times in one copy): 2, 10, 30 min, then an hour.
        static std::unordered_map<uint64_t, int> s_times;
        static std::unordered_set<uint64_t> s_offFor;     // tiles tried again on foot already
        // Kept on disk beside the game (SacredBild-parked.txt: key, times), so a restart does not try them all again
        // (the analyst, 2026-11-11: the count of «no nearer» went up with each reinstall).
        static std::wstring s_file;
        if (s_file.empty())
        {
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            s_file = exe;
            s_file = s_file.substr(0, s_file.find_last_of(L"\\/") + 1) + L"SacredBild-parked.txt";
            if (FILE* f = _wfopen(s_file.c_str(), L"rb"))
            {
                unsigned long long k = 0;
                int n = 0;
                while (std::fscanf(f, "%llu %d", &k, &n) == 2)
                {
                    s_times[k] = std::min(n, 4);
                    s_left[k] = now;    // parked again from the start, for as long as its count says
                }
                std::fclose(f);
            }
            // What the other copies learnt too (Skiller, 2026-11-11, from «IA que juega»: «lo que aprende una lo usan
            // todas»): their parked tiles from ..\SacredA*\, counted at most twice (10 min), since a gate shut for one
            // hero may stand open for another further on.
            const std::wstring self = s_file.substr(0, s_file.find_last_of(L"\\/"));
            const std::wstring games = self.substr(0, self.find_last_of(L"\\/") + 1);
            WIN32_FIND_DATAW fd{};
            if (HANDLE h = FindFirstFileW((games + L"SacredA*").c_str(), &fd); h != INVALID_HANDLE_VALUE)
            {
                int shared = 0;
                do
                {
                    const std::wstring dir = games + fd.cFileName;
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || _wcsicmp(dir.c_str(), self.c_str()) == 0)
                    {
                        continue;
                    }
                    if (FILE* f = _wfopen((dir + L"\\SacredBild-parked.txt").c_str(), L"rb"))
                    {
                        unsigned long long k = 0;
                        int n = 0;
                        while (std::fscanf(f, "%llu %d", &k, &n) == 2)
                        {
                            if (!s_times.count(k) && n > 0)
                            {
                                s_times[k] = std::min(n, 2);
                                s_left[k] = now;
                                ++shared;
                            }
                        }
                        std::fclose(f);
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
                LOG("AutoAttack: {} parked tiles taken from the other copies", shared);
            }
        }
        static const DWORD kLeftMs[] = {120000, 600000, 1800000, 3600000};
        if (const auto it = s_left.find(key);
            it != s_left.end() && now - it->second < kLeftMs[std::min(s_times[key], 4) - 1])
        {
            return true;
        }
        static int32_t s_movedX = 0, s_movedY = 0;
        static DWORD s_aroundSince = 0;
        if (key != s_key || distance < s_best - kTileSize)
        {
            s_key = key;
            s_best = distance;
            s_since = now;
            s_aroundSince = 0;
            return false;
        }
        cObjectManager* om = cObjectManager::instance();
        if (goingAround(s_movedX, s_movedY, s_aroundSince, om ? om->hero() : nullptr, now))
        {
            s_since = now;
            return false;
        }
        if (now - s_since > 20000 && HeroProfile::current().mounted && !s_offFor.count(key))
        {
            s_offFor.insert(key);
            g_onFootUntil = now + 60000;
            s_since = now;
            return false;
        }
        if (now - s_since > 20000)
        {
            const int times = ++s_times[key];
            LOG("AutoAttack: no nearer to tile {},{} in 20 s ({:.0f} tiles, time {}): left {} min, playing on from here",
                static_cast<int>(wx / kTileSize), static_cast<int>(wy / kTileSize), distance / kTileSize, times,
                kLeftMs[std::min(times, 4) - 1] / 60000);
            s_left[key] = now;
            s_key = 0;
            if (FILE* f = _wfopen(s_file.c_str(), L"wb"))
            {
                for (const auto& [k, n] : s_times)
                {
                    std::fprintf(f, "%llu %d\n", static_cast<unsigned long long>(k), n);
                }
                std::fclose(f);
            }
            return true;
        }
        return false;
    }

    bool talkedLately(uint32_t id, DWORD now)
    {
        const auto it = g_talkedAt.find(id);
        return it != g_talkedAt.end() && now - it->second < kTalkAgainMs;
    }

    void forgetTalks()
    {
        g_talkedAt.clear();
        g_talkFails.clear();    // a quest step done: who would not speak may now
    }

    // The companions kept with the hero on a long walk (the player's wish, 2026-11-09: the agent looks after its
    // mercenary): one fallen over 10 tiles behind is waited for, one over 18 tiles off is walked back to; a wait lasts
    // 20 s at most (a companion stuck behind a wall), then the walk goes on for 30 s before waiting again.
    // True while it waits: the caller's walk is held, not failed.
    bool waitForCompanions(DWORD now, const cCreature* self)
    {
        static DWORD s_waitSince = 0, s_goOnUntil = 0;
        if (s_goOnUntil && static_cast<int>(now - s_goOnUntil) < 0)
        {
            return false;
        }
        cObjectManager* objects = cObjectManager::instance();
        const cCreature* lag = nullptr;
        double farthest = 0.0;
        for (uint32_t id : g_hirelings)
        {
            const cCreature* h = objects ? objects->creature(id) : nullptr;
            if (!h || h->health <= 0)
            {
                continue;
            }
            const double d = std::hypot(static_cast<double>(h->x - self->x), static_cast<double>(h->y - self->y)) / kTileSize;
            if (d > 10.0 && d < 80.0 && d > farthest)
            {
                farthest = d;
                lag = h;
            }
        }
        if (!lag)
        {
            // Caught up: a while on before waiting again (the live verifier, 2026-11-11: A5 47 waits in 19 min,
            // a fight now and then; a player does not stop every few steps for the hireling).
            if (s_waitSince)
            {
                s_goOnUntil = now + 45000;
            }
            s_waitSince = 0;
            return false;
        }
        if (!s_waitSince)
        {
            s_waitSince = now;
            LOG("AutoAttack: a companion is {:.0f} tiles behind: waiting for it", farthest);
        }
        if (now - s_waitSince > 8000)
        {
            LOG("AutoAttack: the companion did not come up in 8 s ({:.0f} tiles): going on", farthest);
            s_waitSince = 0;
            s_goOnUntil = now + 60000;
            return false;
        }
        if (farthest > 18.0)
        {
            if (!ClickWalk::routing())
            {
                ClickWalk::walkTo(lag->x, lag->y, false);
            }
        }
        else
        {
            ClickWalk::cancel();
        }
        return true;
    }

    // A shut door ahead on a trip: within 6 tiles, toward the goal (the player, 2026-11-10: «al agente aún le cuesta
    // detectar puertas»; before, doors were only used shut in or stuck).
    struct DoorAhead
    {
        int32_t heroX, heroY, goalX, goalY;
    };

    bool acceptDoorAhead(uint32_t id, void* context)
    {
        if (!acceptDoor(id, nullptr))
        {
            return false;
        }
        const auto* a = static_cast<const DoorAhead*>(context);
        cObjectManager* objects = cObjectManager::instance();
        const cObject* o = objects ? objects->object(id) : nullptr;
        if (!o)
        {
            return false;
        }
        const double dx = o->x - a->heroX, dy = o->y - a->heroY;
        const double gx = a->goalX - a->heroX, gy = a->goalY - a->heroY;
        const double dl = std::hypot(dx, dy), gl = std::hypot(gx, gy);
        return dl > 1.0 && gl > 1.0 && (dx * gx + dy * gy) / (dl * gl) > 0.5 && dl < gl + 2.0 * kTileSize;
    }

    bool doorOnTheWay(DWORD now, const cCreature* self, int32_t x, int32_t y)
    {
        static DWORD s_at = 0;
        if (s_at && now - s_at < 3000)
        {
            return false;
        }
        s_at = now;
        DoorAhead ahead{self->x, self->y, x, y};
        const float range = g_doorRange;
        g_doorRange = 6.0f * kTileSize;
        AimAssist::Target door;
        const bool found = AimAssist::findObject(g_doorRange, &acceptDoorAhead, &ahead, door);
        g_doorRange = range;
        if (!found)
        {
            return false;
        }
        g_doorTried[door.id] = now;
        LOG("AutoAttack: a shut door on the way ({:x}): opened", door.id);
        remember("abre una puerta cerrada en el camino");
        attack(door.id);
        return true;
    }

    // A shut door or gate near the goal, whichever way it lies from the hero (Manolito, block AB, 2026-11-11: the yard
    // at 3049,2711 has its gate DOOR_GATE01_A in the south wall, and the agent walked straight into the east wall; the
    // door on the way looks only ahead). Used when the walk got no nearer: the door within 8 tiles of the goal and 30
    // of the hero.
    struct DoorNearGoal
    {
        int32_t goalX, goalY;
    };

    bool acceptDoorNearGoal(uint32_t id, void* context)
    {
        if (!acceptDoor(id, nullptr))
        {
            return false;
        }
        const auto* g = static_cast<const DoorNearGoal*>(context);
        cObjectManager* objects = cObjectManager::instance();
        const cObject* o = objects ? objects->object(id) : nullptr;
        return o && std::hypot(static_cast<double>(o->x - g->goalX), static_cast<double>(o->y - g->goalY)) <= 8.0 * kTileSize;
    }

    bool doorNearGoal(DWORD now, int32_t x, int32_t y)
    {
        DoorNearGoal goal{x, y};
        const float range = g_doorRange;
        g_doorRange = 30.0f * kTileSize;
        AimAssist::Target door;
        const bool found = AimAssist::findObject(g_doorRange, &acceptDoorNearGoal, &goal, door);
        g_doorRange = range;
        if (!found)
        {
            return false;
        }
        g_doorTried[door.id] = now;
        g_doorHoldUntil = now + 1500;
        LOG("AutoAttack: no nearer to tile {},{}: the shut door {:x} near it used", static_cast<int>(x / kTileSize),
            static_cast<int>(y / kTileSize), door.id);
        remember("usa la puerta cerca del destino");
        attack(door.id);
        return true;
    }

    // A goal no walk gets nearer to and no shut door beside it: a way into another layer near it (stairs, a house's
    // door, a cave: what MapMemory marked where the hero once changed layer, or a house's landing from the world
    // files) walked onto, each tried once in 3 min. The solver, 2026-11-12: A3 and A8 «no way found (8 tiles)» to a
    // goal inside a house at 3174,2794; A8 had been inside (layer 1) through the marks at 3171,2796..2797.
    bool entranceNearGoal(DWORD now, const cCreature* self, int32_t x, int32_t y)
    {
        static std::unordered_map<uint64_t, DWORD> s_tried;
        const int layer = *(reinterpret_cast<const uint8_t*>(self) + 0x24);
        const int gx = static_cast<int>(x / kTileSize), gy = static_cast<int>(y / kTileSize);
        const int htx = static_cast<int>(self->x / kTileSize), hty = static_cast<int>(self->y / kTileSize);
        std::vector<std::pair<int, int>> ways = MapMemory::waysOut(layer, gx, gy, 10);
        for (const Teleports::Interior& in : Teleports::interiors())
        {
            if (!in.start && layer == 0 && std::abs(in.outX - gx) <= 10 && std::abs(in.outY - gy) <= 10)
            {
                ways.emplace_back(in.outX, in.outY);
            }
        }
        int bx = 0, by = 0;
        double best = 1e18;
        for (const auto& [ex, ey] : ways)
        {
            const uint64_t k = (static_cast<uint64_t>(static_cast<uint32_t>(ex)) << 32) | static_cast<uint32_t>(ey);
            if (const auto t = s_tried.find(k); t != s_tried.end() && now - t->second < 180000)
            {
                continue;
            }
            const double d = std::hypot(ex - gx, ey - gy);
            if (d < best)
            {
                best = d;
                bx = ex;
                by = ey;
            }
        }
        if (best > 1e17)
        {
            return false;
        }
        s_tried[(static_cast<uint64_t>(static_cast<uint32_t>(bx)) << 32) | static_cast<uint32_t>(by)] = now;
        // Into a house on foot, as a player gets off at its door (A5 2026-11-12 01:20: on horseback by the way in at
        // 3095,2784, 3-tile walks every 4 s that never moved it, until the 90 s rule).
        const cObjectManager* om = cObjectManager::instance();
        if (const cCreature* hero = om ? const_cast<cObjectManager*>(om)->hero() : nullptr;
            hero && *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + 0x1EC))
        {
            g_onFootUntil = now + 60000;
        }
        if (std::hypot(bx - htx, by - hty) <= 1.5)
        {
            // On it already: a step off, the next try walks back onto it (a way in fires on stepping on).
            const double a = now * 0.001;
            ClickWalk::walkTo(static_cast<int32_t>(self->x + std::cos(a) * 3 * kTileSize),
                              static_cast<int32_t>(self->y + std::sin(a) * 3 * kTileSize), false);
            s_tried.erase((static_cast<uint64_t>(static_cast<uint32_t>(bx)) << 32) | static_cast<uint32_t>(by));
            LOG("AutoAttack: no nearer to tile {},{}: on the way in at {},{}: a step off to step on again", gx, gy, bx, by);
            return true;
        }
        LOG("AutoAttack: no nearer to tile {},{}: to the way in at {},{} ({:.0f} tiles from the goal)", gx, gy, bx, by, best);
        Findings::note("door", self->x, self->y, "goal {},{}: to the way in at {},{}", gx, gy, bx, by);
        remember("usa la entrada cerca del destino");
        ClickWalk::walkTo(static_cast<int32_t>((bx + 0.5) * kTileSize), static_cast<int32_t>((by + 0.5) * kTileSize), false);
        // The order held 20 s: the roam's travel to the goal took it over within 10 s and the hero never reached the
        // way in (the solver's fix L, 2026-11-12: A2, A3, A4).
        g_doorHoldUntil = now + 20000;
        return true;
    }

    // The ways out of where the hero is shut in: those MapMemory saw it jump through, plus the game's own (a tpstart
    // tile at hand, the stairs anchors beside an interior's landing inside, a house's door in the yard).
    // The game's own first (`gameCount` of them): MapMemory's can be wrong (a save loaded looked like a jump, A5
    // 2026-11-12, two «portals» inside a cave tried for 10 min while the stairs anchor sat by its landing).
    std::vector<std::pair<int, int>> waysOutAround(int layer, int htx, int hty, size_t* gameCount = nullptr)
    {
        std::vector<std::pair<int, int>> ways;
        // The game's own: a tpstart tile at hand, or the stairs anchors beside an interior's landing inside (its
        // way out is there; Manolito, block L).
        for (const Teleports::Interior& in : Teleports::interiors())
        {
            if (in.start && std::abs(in.outX - htx) <= Whereabouts::kRadius && std::abs(in.outY - hty) <= Whereabouts::kRadius)
            {
                ways.emplace_back(in.outX, in.outY);
            }
            else if (!in.start && std::abs(in.inX - htx) <= Whereabouts::kRadius && std::abs(in.inY - hty) <= Whereabouts::kRadius)
            {
                for (const auto& a : Teleports::anchorsNear(layer, in.inX, in.inY, 6))
                {
                    ways.push_back(a);
                }
            }
            else if (!in.start && layer == 0 && std::abs(in.outX - htx) <= Whereabouts::kRadius &&
                     std::abs(in.outY - hty) <= Whereabouts::kRadius)
            {
                // A house's door in the yard (HQ_HE_3_3_1's tprausz tiles, Manolito): the landing itself and the
                // stairs anchors beside it.
                ways.emplace_back(in.outX, in.outY);
                for (const auto& a : Teleports::anchorsNear(layer, in.outX, in.outY, 6))
                {
                    ways.push_back(a);
                }
            }
        }
        if (gameCount)
        {
            *gameCount = ways.size();
        }
        for (const auto& w : MapMemory::waysOut(layer, htx, hty, Whereabouts::kRadius))
        {
            if (std::find(ways.begin(), ways.end(), w) == ways.end())
            {
                ways.push_back(w);
            }
        }
        return ways;
    }

    // Shut in with a goal outside (a farming zone, a shop, the quest): the way out first, the game's own ahead, each
    // tried once in 2 min (A5 2026-11-12: farming sent it 3199 tiles off from inside a cave, between the quest's
    // tries of its way out, so it never got there).
    bool towardWayOut(DWORD now, const cCreature* self, int32_t x, int32_t y)
    {
        if (!Whereabouts::area().shutIn ||
            Whereabouts::reachable(static_cast<int>(x / kTileSize), static_cast<int>(y / kTileSize)) == 1)
        {
            return false;
        }
        static std::unordered_map<uint64_t, DWORD> s_tried;
        static DWORD s_askAt = 0;
        static uint64_t s_told = 0;
        const int layer = *(reinterpret_cast<const uint8_t*>(self) + 0x24);
        const int htx = static_cast<int>(self->x / kTileSize), hty = static_cast<int>(self->y / kTileSize);
        for (const auto& [ex, ey] : waysOutAround(layer, htx, hty))
        {
            const uint64_t wk = (static_cast<uint64_t>(static_cast<uint32_t>(ex)) << 32) | static_cast<uint32_t>(ey);
            if (const auto t = s_tried.find(wk); t != s_tried.end() && now - t->second < 120000)
            {
                continue;
            }
            if (std::abs(ex - htx) <= 1 && std::abs(ey - hty) <= 1)
            {
                s_tried[wk] = now;
                continue;
            }
            const int32_t wx = static_cast<int32_t>((ex + 0.5) * kTileSize);
            const int32_t wy = static_cast<int32_t>((ey + 0.5) * kTileSize);
            if (approachStalled(wx, wy, std::hypot(static_cast<double>(wx - self->x), static_cast<double>(wy - self->y)), now))
            {
                s_tried[wk] = now;
                continue;
            }
            if (wk != s_told)
            {
                s_told = wk;
                LOG("AutoAttack: shut in, the goal at {},{} outside: to the way out at {},{} first",
                    static_cast<int>(x / kTileSize), static_cast<int>(y / kTileSize), ex, ey);
            }
            if (!ClickWalk::routing() && now - s_askAt > 4000)
            {
                s_askAt = now;
                ClickWalk::walkTo(wx, wy, false);
            }
            return true;
        }
        return false;
    }

    bool travel(DWORD now, const cCreature* self, int32_t x, int32_t y, DWORD& askAt)
    {
        if (doorOnTheWay(now, self, x, y))
        {
            g_travel.lastAt = now;
            return true;
        }
        if (waitForCompanions(now, self))
        {
            g_travel.lastAt = now;
            g_travel.bestAt = now;      // the wait is no lack of progress
            return true;
        }
        if (towardWayOut(now, self, x, y))
        {
            g_travel.lastAt = now;
            g_travel.bestAt = now;      // going out first is no lack of progress
            return true;
        }
        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(x / kTileSize)) << 32) |
                             static_cast<uint32_t>(y / kTileSize);
        if (const auto it = g_travelBlocked.find(key); it != g_travelBlocked.end())
        {
            if (static_cast<int>(now - it->second) < 0)
            {
                return false;
            }
            g_travelBlocked.erase(it);
        }
        const double d = std::hypot(static_cast<double>(x - self->x), static_cast<double>(y - self->y));
        Travel& t = g_travel;
        // A new goal, or back from a fight or a collect (the time away is no lack of progress).
        if (std::abs(x - t.x) > 3 * kTileSize || std::abs(y - t.y) > 3 * kTileSize)
        {
            t = Travel{x, y, d, now, now, 0, 0, self->x, self->y};
        }
        if (now - t.lastAt > 2000)
        {
            t.bestAt = now;
        }
        t.lastAt = now;
        if (d < t.best - 2.0 * kTileSize)
        {
            t.best = d;
            t.bestAt = now;
            t.fails = 0;
            t.aroundSince = 0;
        }
        else if (goingAround(t.movedX, t.movedY, t.aroundSince, self, now))
        {
            t.bestAt = now;
        }
        if (t.detourUntil && static_cast<int>(now - t.detourUntil) < 0)
        {
            return true;
        }
        t.detourUntil = 0;
        if (now - t.bestAt > 20000 && HeroProfile::current().mounted &&
            !(g_onFootUntil && static_cast<int>(now - (g_onFootUntil - 60000)) < 300000))
        {
            g_onFootUntil = now + 60000;    // on foot before a step aside (once in 5 min)
            t.bestAt = now;
            return true;
        }
        if (now - t.bestAt > 20000)
        {
            ClickWalk::cancel();
            if (doorNearGoal(now, x, y))
            {
                t.detourUntil = now + 8000;
                t.bestAt = now;
                return true;
            }
            if (entranceNearGoal(now, self, x, y))
            {
                t.detourUntil = now + 10000;
                t.bestAt = now;
                return true;
            }
            if (++t.fails >= 3)
            {
                // Each time again no way, left longer (the player, 2026-11-11: the Daemon «no está teniendo mucha
                // eficiencia», 40 s lost every 3 min on a quest goal with no way to it): 3, 15, 45 min, then 2 h.
                static std::unordered_map<uint64_t, int> s_noWay;
                static const DWORD kNoWayMs[] = {180000, 900000, 2700000, 7200000};
                const int times = std::min(++s_noWay[key], 4);
                LOG("AutoAttack: no way found to tile {},{} ({:.0f} tiles, time {}): left {} min", static_cast<int>(x / kTileSize),
                    static_cast<int>(y / kTileSize), d / kTileSize, times, kNoWayMs[times - 1] / 60000);
                g_travelBlocked[key] = now + kNoWayMs[times - 1];
                t = Travel{};
                return false;
            }
            const double angle = (std::rand() % 360) * 3.14159265358979 / 180.0;
            const int32_t sx = self->x + static_cast<int32_t>(std::cos(angle) * 8.0 * kTileSize);
            const int32_t sy = self->y + static_cast<int32_t>(std::sin(angle) * 8.0 * kTileSize);
            LOG("AutoAttack: no nearer to tile {},{} in 20 s (try {}): a step aside first", static_cast<int>(x / kTileSize),
                static_cast<int>(y / kTileSize), t.fails);
            ClickWalk::walkTo(sx, sy, false);
            t.detourUntil = now + 6000;
            t.bestAt = now;
            return true;
        }
        if (!ClickWalk::routing() && now - askAt > 4000)
        {
            askAt = now;
            ClickWalk::walkTo(x, y, false);
        }
        return true;
    }

    // The stuck guard (the player's wish, 2026-11-09: no stall ever): out of the fights, a hero that has not got
    // 8 tiles away from where it stood for 60 s is stuck, whatever the reason (a talk waited out, a goal unreachable,
    // a yard with no way on). Step by step, 30 s apart: first all «left for now» marks are forgotten (who was spoken
    // to, goals travel gave up on), then a walk of 15 tiles each time a new way round (the tile reachable nearest it).
    // Fights and collects (no roam for 3 s) and any jump (a portal, a teleport) start the watch again.
    bool stuckGuard(DWORD now, const cCreature* self)
    {
        // Where the hero stood every 2 s: stuck while all of the last 60 s (or more) fit in a box of 16 tiles. Seen
        // live (2026-11-09): back and forth between a chest and the quest, never 8 tiles from one spot for long.
        struct Spot
        {
            DWORD at;
            int32_t x, y;
        };
        static std::vector<Spot> s_spots;
        static DWORD s_lastCall = 0, s_anchorAt = 0, s_sampleAt = 0;
        static int s_stage = 0;
        // Backing out: leg by leg to the trail's points, a few tiles each, until 20 tiles from where it was stuck.
        static size_t s_backIdx = 0;
        static int32_t s_backFromX = 0, s_backFromY = 0;
        static DWORD s_backSince = 0, s_backOrderAt = 0;
        if (g_backing && now - s_lastCall > 3000)
        {
            // Roaming left off meanwhile (a fight, a collect): the backing out is stale, the trail noted again.
            g_backing = false;
            LOG("AutoAttack: stuck guard: backing out left off (the agent busy with something else)");
        }
        if (g_backing)
        {
            const double away = std::hypot(static_cast<double>(self->x - s_backFromX), static_cast<double>(self->y - s_backFromY));
            if (away > 20.0 * kTileSize || now - s_backSince > 60000 || s_backIdx >= g_trail.size())
            {
                g_backing = false;
                LOG("AutoAttack: stuck guard: backed out {:.0f} tiles the way it came ({} s)", away / kTileSize,
                    (now - s_backSince) / 1000);
                Findings::note("stuck", self->x, self->y, "backed out {:.0f} tiles along its trail", away / kTileSize);
                g_trail.resize(std::min(g_trail.size(), s_backIdx + 1));
                s_spots.clear();
                s_anchorAt = now;
                g_homeX = self->x;
                g_homeY = self->y;
                g_roamStuck = 0;
                g_roamAt = 0;
                return false;
            }
            // The leg: the point furthest back along the trail still within 4 tiles of the hero.
            while (s_backIdx > 0 && std::hypot(static_cast<double>(self->x - g_trail[s_backIdx - 1].x),
                                               static_cast<double>(self->y - g_trail[s_backIdx - 1].y)) < 4.0 * kTileSize)
            {
                --s_backIdx;
            }
            const Crumb& c = g_trail[s_backIdx];
            if (std::hypot(static_cast<double>(self->x - c.x), static_cast<double>(self->y - c.y)) < kTileSize && s_backIdx == 0)
            {
                s_backIdx = g_trail.size();     // the trail's start reached: done on the next call
                return true;
            }
            if (now - s_backOrderAt > 700)
            {
                s_backOrderAt = now;
                ClickWalk::walkTo(c.x, c.y, false);
            }
            s_lastCall = now;
            return true;
        }
        const bool jumped = !s_spots.empty() && std::hypot(static_cast<double>(self->x - s_spots.back().x),
                                                           static_cast<double>(self->y - s_spots.back().y)) > 40.0 * kTileSize;
        if (!s_lastCall || now - s_lastCall > 3000 || jumped)
        {
            s_spots.clear();
            s_anchorAt = now;
            s_stage = 0;
        }
        s_lastCall = now;
        if (now - s_sampleAt >= 2000)
        {
            s_sampleAt = now;
            s_spots.push_back({now, self->x, self->y});
        }
        int32_t minX = self->x, maxX = self->x, minY = self->y, maxY = self->y;
        for (const Spot& sp : s_spots)
        {
            if (now - sp.at <= 60000)
            {
                minX = std::min(minX, sp.x);
                maxX = std::max(maxX, sp.x);
                minY = std::min(minY, sp.y);
                maxY = std::max(maxY, sp.y);
            }
        }
        while (s_spots.size() > 40)
        {
            s_spots.erase(s_spots.begin());
        }
        if (std::max(maxX - minX, maxY - minY) > 16.0 * kTileSize)
        {
            // Getting somewhere: the watch starts over.
            s_anchorAt = now;
            s_stage = 0;
            return false;
        }
        if (now - s_anchorAt < 60000u + 30000u * static_cast<DWORD>(s_stage))
        {
            return false;
        }
        ++s_stage;
        // A door near (the player, 2026-11-10: «el agente no abre puertas», a Seraphim stuck before a gate in a
        // dungeon): used first, a try of this stage.
        {
            AimAssist::Target door;
            if (AimAssist::findObject(g_doorRange, &acceptDoor, nullptr, door))
            {
                g_doorTried[door.id] = now;
                LOG("AutoAttack: stuck guard: a door near ({:x}): used, as a click on it", door.id);
                Findings::note("door", self->x, self->y, "stuck: door {:x} used", door.id);
                attack(door.id);
                g_doorHoldUntil = now + 6000;
                return true;
            }
        }
        // Shut in (Whereabouts): the ways out MapMemory saw (a portal end, the arrival of a cave's entry) walked onto,
        // each tried once in 3 minutes; standing on one already, a step off first (a teleport fires on stepping on).
        // Seen twice (2026-11-10, a Gladiator 3 h, a Seraphim 16 min): into a cave from 3180,2670, shut in at
        // 4819..4828 x 367..387, the stuck guard only walking about inside.
        if (Whereabouts::area().shutIn)
        {
            static std::unordered_map<uint64_t, DWORD> s_wayTried;
            const int layer = *(reinterpret_cast<const uint8_t*>(self) + 0x24);
            const int htx = static_cast<int>(self->x / kTileSize), hty = static_cast<int>(self->y / kTileSize);
            int bx = 0, by = 0;
            double best = 1e18;
            size_t gameWays = 0;
            std::vector<std::pair<int, int>> ways = waysOutAround(layer, htx, hty, &gameWays);
            for (size_t wi = 0; wi < ways.size(); ++wi)
            {
                const auto [ex, ey] = ways[wi];
                const uint64_t wk = (static_cast<uint64_t>(static_cast<uint32_t>(ex)) << 32) | static_cast<uint32_t>(ey);
                if (const auto t = s_wayTried.find(wk); t != s_wayTried.end() && now - t->second < 180000)
                {
                    continue;
                }
                // The game's own ways ahead of those MapMemory remembers, however near.
                const double d = std::hypot(ex - htx, ey - hty) + (wi < gameWays ? 0.0 : 10000.0);
                if (d < best)
                {
                    best = d;
                    bx = ex;
                    by = ey;
                }
            }
            if (best < 1e17)
            {
                best = std::fmod(best, 10000.0);
                if (best <= 1.5)
                {
                    // On it: a step off, the next stage walks back onto it.
                    const double a = now * 0.001;
                    ClickWalk::walkTo(static_cast<int32_t>(self->x + std::cos(a) * 3 * kTileSize),
                                      static_cast<int32_t>(self->y + std::sin(a) * 3 * kTileSize), false);
                    LOG("AutoAttack: stuck guard: shut in, on the way out at {},{}: a step off to step on again", bx, by);
                    return true;
                }
                s_wayTried[(static_cast<uint64_t>(static_cast<uint32_t>(bx)) << 32) | static_cast<uint32_t>(by)] = now;
                LOG("AutoAttack: stuck guard: shut in: to the way out at {},{} ({:.0f} tiles)", bx, by, best);
                Findings::note("stuck", self->x, self->y, "shut in: to the way out at {},{}", bx, by);
                ClickWalk::walkTo(static_cast<int32_t>((bx + 0.5) * kTileSize), static_cast<int32_t>((by + 0.5) * kTileSize),
                                  false);
                return true;
            }
        }
        if (s_stage == 1)
        {
            // Stuck again near where the marks were last forgotten (seen live 2026-11-11, A3: 20 min looping at 24 tiles
            // from a quest creature behind a fence): forgetting would send it back to the same goal, so the goal it
            // was going for is left 10 min instead.
            static DWORD s_forgotAt = 0;
            static int32_t s_forgotX = 0, s_forgotY = 0;
            if (s_forgotAt && now - s_forgotAt < 600000 &&
                std::hypot(static_cast<double>(self->x - s_forgotX), static_cast<double>(self->y - s_forgotY)) <
                    30.0 * kTileSize)
            {
                const auto leave = [&](int32_t gx, int32_t gy) {
                    if (!gx && !gy)
                    {
                        return;
                    }
                    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(gx / kTileSize)) << 32) |
                                         static_cast<uint32_t>(gy / kTileSize);
                    g_travelBlocked[key] = now + 600000;
                };
                leave(g_questX, g_questY);
                leave(g_travel.x, g_travel.y);
                LOG("AutoAttack: stuck guard: stuck again where it was {} s ago: the goal at tile {},{} left 10 min",
                    (now - s_forgotAt) / 1000, static_cast<int>(g_questX / kTileSize),
                    static_cast<int>(g_questY / kTileSize));
                Findings::note("stuck", self->x, self->y, "stuck again: the goal at tile {},{} left 10 min",
                               static_cast<int>(g_questX / kTileSize), static_cast<int>(g_questY / kTileSize));
                g_questX = g_questY = 0;
                g_travel = Travel{};
                s_forgotAt = 0;
                return false;
            }
            s_forgotAt = now;
            s_forgotX = self->x;
            s_forgotY = self->y;
            LOG("AutoAttack: stuck guard: 60 s within 16 tiles out of the fights: the marks «left for now» forgotten");
            Findings::note("stuck", self->x, self->y, "60 s within a box of 16 tiles out of the fights: marks forgotten");
            forgetTalks();
            g_travelBlocked.clear();
            return false;
        }
        // First the way it came: points of the trail more than 20 tiles back from here, if it has them.
        if (s_stage == 2 && g_trail.size() > 3)
        {
            g_backing = true;
            s_backIdx = g_trail.size() - 1;
            s_backFromX = self->x;
            s_backFromY = self->y;
            s_backSince = now;
            s_backOrderAt = 0;
            LOG("AutoAttack: stuck guard: backing out the way it came ({} points of trail)", g_trail.size());
            Findings::note("stuck", self->x, self->y, "backing out along its trail ({} points)", g_trail.size());
            return true;
        }
        const double angle = s_stage * 2.39996;     // the golden angle: each try a new way round
        const int tx = static_cast<int>((self->x + std::cos(angle) * 15.0 * kTileSize) / kTileSize);
        const int ty = static_cast<int>((self->y + std::sin(angle) * 15.0 * kTileSize) / kTileSize);
        int nx = tx, ny = ty;
        Whereabouts::nearestReachable(tx, ty, nx, ny);
        const int32_t wx = static_cast<int32_t>((nx + 0.5) * kTileSize), wy = static_cast<int32_t>((ny + 0.5) * kTileSize);
        LOG("AutoAttack: stuck guard: still nowhere after {} s: a walk to tile {},{} (try {})", (now - s_anchorAt) / 1000,
            nx, ny, s_stage - 1);
        Findings::note("stuck", self->x, self->y, "still nowhere after {} s: walk to tile {},{} (try {})",
                       (now - s_anchorAt) / 1000, nx, ny, s_stage - 1);
        ClickWalk::walkTo(wx, wy, false);
        MapMemory::setRoute(true, wx, wy);
        g_homeX = wx;
        g_homeY = wy;
        g_roamStuck = 0;
        g_roamAt = now;
        g_walkX = wx;
        g_walkY = wy;
        return true;
    }

    void roam(DWORD now)
    {
        if ((!Config::autoAttack.roam && !Config::autoAttack.quests) || g_retreatUntil)
        {
            g_quietSince = 0;
            return;
        }
        if (!g_quietSince)
        {
            g_quietSince = now;
        }
        cObjectManager* objects = cObjectManager::instance();
        cEngine* engine = FrameHooks::engine();
        cCreature* self = objects && engine ? objects->creature(engine->controlled) : nullptr;
        cCreature* hero = objects ? objects->hero() : nullptr;
        if (self && hero && stuckGuard(now, self))
        {
            return;
        }
        if (static_cast<int>(now - g_doorHoldUntil) < 0 || static_cast<int>(now - g_craftHoldUntil) < 0)
        {
            return;     // on the way to the door just clicked, or by a craftsman whose window is being worked
        }
        // Shut in with a door that opens near (the player, 2026-11-10: «la cueva tiene puertas que el agente no sabe
        // abrir»; the cave was worth staying in, only the way on was shut): opened at once, the way on before the way
        // out, every 8 s at most, each door once in 2 minutes.
        static DWORD s_doorAt = 0;
        if (self && hero && (!s_doorAt || now - s_doorAt > 8000) && Whereabouts::area().shutIn)
        {
            s_doorAt = now;
            const float range = g_doorRange;
            g_doorRange = 20.0f * kTileSize;
            AimAssist::Target door;
            const bool found = AimAssist::findObject(g_doorRange, &acceptDoor, nullptr, door);
            g_doorRange = range;
            if (found)
            {
                g_doorTried[door.id] = now;
                setIntent(Intent::Escape, "shut in: the doors first, the way on");
                LOG("AutoAttack: shut in, a door that opens near ({:x}): opened, the way on", door.id);
                Findings::note("door", self->x, self->y, "shut in: door {:x} opened", door.id);
                attack(door.id);
                g_doorHoldUntil = now + 6000;
                return;
            }
        }
        if (!self || !hero || now - g_quietSince < kRoamQuietMs || (g_roamAt && now - g_roamAt < kRoamStepMs) ||
            hero->health * 100 < hero->maxHealth * 60)
        {
            return;
        }
        // The step under way: the next one once it is there, or has stopped walking (the hero is slow: a tile a
        // second or so), or after a while at most.
        if (g_roamAt)
        {
            const double tx = g_walkX - self->x, ty = g_walkY - self->y;
            const bool there = tx * tx + ty * ty < 0.36 * kTileSize * kTileSize;
            const bool walking = CombatState::action(self) == CombatState::Walking;
            if (!there && walking && now - g_roamAt < kRoamStepMaxMs)
            {
                return;
            }
        }
        const int layer = *(reinterpret_cast<const uint8_t*>(self) + kLayer);
        // A new home: the first roam, or the hero far from where the last step left it (a teleport, a portal, stairs
        // to another layer).
        const double jx = self->x - g_roamFromX, jy = self->y - g_roamFromY;
        if (!g_homeSet || layer != g_roamLayer || ((g_roamFromX || g_roamFromY) && jx * jx + jy * jy > (40 * kTileSize) * (40 * kTileSize)))
        {
            g_homeX = self->x;
            g_homeY = self->y;
            g_homeSet = true;
            g_roamLayer = layer;
            g_roamAt = 0;
            g_roamStuck = 0;
            g_roamAngle = jitter(0, 628) / 100.0;
        }
        // The backpack full: off to the nearest merchant (however far, as to a quest), the shop opened with a click on
        // him as the player's; Gear sells and puts on the upgrades, then the agent closes it (onFrame).
        if (Gear::wantsShop() && !Gear::shopOpen())
        {
            const Merchant* best = nullptr;
            double bestDistance = 0.0;
            for (const Merchant& m : g_merchants)
            {
                const auto failed = g_shopFailedAt.find(m.id);
                double d = std::hypot(static_cast<double>(m.x - self->x), static_cast<double>(m.y - self->y));
                if (m.layer == layer && d <= 150.0 * kTileSize)
                {
                    d = wayLength(self->x, self->y, m.x, m.y);    // the walk, not the straight line
                }
                // One in a house (its floor another layer) only near: a click on him sends the hero there by the game's
                // own path finder, which opens the doors on the way (the player: «algunos comerciantes se encuentran
                // dentro de casas»).
                // Nor one across the world (the player, 2026-11-10: the Daemon, level 3, set off for a merchant 427
                // tiles off through higher grounds and got stuck on the way): 150 tiles at most.
                if ((m.layer != layer && d > 30.0 * kTileSize) || d > 150.0 * kTileSize ||
                    (failed != g_shopFailedAt.end() && now - failed->second < 300000))
                {
                    continue;
                }
                if (!best || d < bestDistance)
                {
                    best = &m;
                    bestDistance = d;
                }
            }
            if (best)
            {
                g_questTravel = true;
                setIntent(Intent::Shop, "the backpack full or no healing potion: to a merchant");
                if (best->id != g_shopId)
                {
                    LOG("AutoAttack: backpack full, off to merchant {:x} ({:.0f} tiles)", best->id, bestDistance / kTileSize);
                    g_shopId = best->id;
                    g_shopTries = 0;
                    g_shopClickAt = 0;
                }
                // Far: the agent's travel; near (15 tiles) or in a house: clicks on him, the game's own path through
                // the doors, more tries for the walk.
                const bool closeBy = bestDistance <= 15.0 * kTileSize || best->layer != layer;
                if (!closeBy)
                {
                    if (!travel(now, self, best->x, best->y, g_shopAskAt))
                    {
                        g_shopFailedAt[best->id] = now;
                        g_shopId = 0;
                    }
                }
                else if (!g_shopClickAt || now - g_shopClickAt > 3000)
                {
                    if (++g_shopTries > (bestDistance > 3.0 * kTileSize ? 10 : 5))
                    {
                        LOG("AutoAttack: merchant {:x} would not trade, left 5 min", best->id);
                        g_shopFailedAt[best->id] = now;
                        g_shopId = 0;
                    }
                    else
                    {
                        LOG("AutoAttack: opening the shop of {:x} (try {})", best->id, g_shopTries);
                        g_shopClickAt = now;
                        attack(best->id);
                    }
                }
                MapMemory::setRoute(true, best->x, best->y);
                g_roamAt = 0;
                return;
            }
            if (!g_shopTold)
            {
                g_shopTold = true;
                LOG("AutoAttack: backpack full and no merchant known within 150 tiles on this layer");
            }
        }
        // Runes of other classes (Gear::wantsRuneSwap): off to the nearest combo master as to a merchant, a click on
        // him opens his window. The swap itself waits for Manolito's block S; meanwhile the window's UI objects are
        // logged once it opens and the trip is left 30 min.
        {
            static uint32_t s_masterId = 0;
            static DWORD s_leftAt = 0, s_clickAt = 0, s_askAt = 0;
            static int s_tries = 0;
            const uint8_t mode = *(reinterpret_cast<const uint8_t*>(self) + 0xFC);
            const uint32_t npc = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(self) + 0x100);
            if (s_masterId && mode == 0xA && npc == s_masterId)
            {
                LOG("AutoAttack: at craftsman {:x} ({} runes of other classes): his window open", s_masterId,
                    Gear::foreignRunes());
                // The hero stays by him while Gear works his window (the live verifier, 2026-11-10: four stable
                // visits, the hero back on its quest 6 s after the window opened and no «Gear: horse» line).
                g_craftHoldUntil = now + 25000;     // a forge of 12 steps takes longer than 15 s (the code reviewer)
                static bool s_windowsLogged = false;
                if (const uint8_t* ui = !s_windowsLogged ? *reinterpret_cast<const uint8_t* const*>(0x17EEBBC) : nullptr;
                    ui && (s_windowsLogged = true))
                {
                    for (int i = 0; i < 48; ++i)
                    {
                        const auto* w = *reinterpret_cast<const uint8_t* const*>(ui + 0x80 + 4 * i);
                        if (w && !IsBadReadPtr(w, 0x14))
                        {
                            LOG("AutoAttack: ui window {} at +{:x}: vtable {:x}, +0x10 {:x}", i, 0x80 + 4 * i,
                                *reinterpret_cast<const uint32_t*>(w), *reinterpret_cast<const uint32_t*>(w + 0x10));
                        }
                    }
                }
                // A minute's pause only: each want backs off by itself once tried (Gear), and a 30 min bar here
                // kept the next craftsman away (the live verifier, 2026-11-10: the stable's visit barred the combo
                // master's).
                s_leftAt = now - 1800000 + 60000;
                s_masterId = 0;
            }
            const bool toMaster = Gear::wantsRuneSwap() || Gear::wantsCombo();
            const bool toSmith = !toMaster && Gear::wantsSocket();
            if ((toMaster || toSmith || Gear::wantsHorse()) && !Gear::wantsShop() && (!s_leftAt || now - s_leftAt > 1800000))
            {
                const Merchant* best = nullptr;
                double bestDistance = 0.0;
                for (const Merchant& m : toMaster ? g_masters : toSmith ? g_smiths : g_stables)
                {
                    double d = std::hypot(static_cast<double>(m.x - self->x), static_cast<double>(m.y - self->y));
                    if (m.layer == layer && d <= 300.0 * kTileSize)
                    {
                        d = wayLength(self->x, self->y, m.x, m.y);    // the walk, not the straight line
                    }
                    if ((m.layer == layer || d < 30.0 * kTileSize) && (!best || d < bestDistance))
                    {
                        best = &m;
                        bestDistance = d;
                    }
                }
                // The quests are the game's progress (the player, 2026-11-10: «el objetivo principal del agente siempre
                // tiene que ser las misiones»): a combo, a rune swap, a socket or a horse is no trip away from one,
                // only when the craftsman is at hand (40 tiles) or no quest is open.
                int32_t qx = 0, qy = 0;
                double qr = 0.0;
                uint32_t qwho = 0;
                const bool questOpen = Config::autoAttack.quests && questTarget(self->x, self->y, qx, qy, qr, qwho);
                if (best && questOpen && bestDistance > 40.0 * kTileSize)
                {
                    static DWORD s_toldAt = 0;
                    if (!s_toldAt || now - s_toldAt > 300000)
                    {
                        s_toldAt = now;
                        LOG("AutoAttack: a craftsman {:.0f} tiles off: the quest first, he waits till one is near",
                            bestDistance / kTileSize);
                    }
                    best = nullptr;
                }
                if (best)
                {
                    g_questTravel = true;
                    setIntent(Intent::Shop, !toMaster && !toSmith ? "no horse: to a stable"
                                            : !toMaster            ? "something to socket: to a smith"
                                            : Gear::wantsCombo() ? "a combo to learn: to a combo master"
                                                               : "runes of other classes: to a combo master to swap them");
                    if (best->id != s_masterId)
                    {
                        LOG("AutoAttack: {} runes of other classes; off to the craftsman {:x} ({:.0f} tiles)",
                            Gear::foreignRunes(), best->id, bestDistance / kTileSize);
                        s_masterId = best->id;
                        s_tries = 0;
                        s_clickAt = 0;
                    }
                    if (bestDistance > 15.0 * kTileSize && best->layer == layer)
                    {
                        if (!travel(now, self, best->x, best->y, s_askAt))
                        {
                            LOG("AutoAttack: craftsman {:x} out of reach: left 30 min", best->id);
                            s_leftAt = now;
                            s_masterId = 0;
                        }
                    }
                    else if (!s_clickAt || now - s_clickAt > 3000)
                    {
                        if (++s_tries > 10)
                        {
                            LOG("AutoAttack: craftsman {:x} would not open his window: left 30 min", best->id);
                            s_leftAt = now;
                            s_masterId = 0;
                        }
                        else
                        {
                            LOG("AutoAttack: speaking to the craftsman {:x} (try {})", best->id, s_tries);
                            s_clickAt = now;
                            attack(best->id);
                        }
                    }
                    MapMemory::setRoute(true, best->x, best->y);
                    g_roamAt = 0;
                    return;
                }
            }
        }
        // Held by the game: talking or trading with an NPC (the hero's +0xFC 0xA, the NPC at +0x100; Manolito, block
        // M): no walk that would break the talk; a minute at most (a talk the agent left open).
        {
            static DWORD s_talkSince = 0;
            const uint8_t mode = *(reinterpret_cast<const uint8_t*>(self) + 0xFC);
            if (mode == 0xA)
            {
                if (!s_talkSince)
                {
                    s_talkSince = now;
                    LOG("AutoAttack: the hero talks or trades (NPC {:x}): standing", *reinterpret_cast<const uint32_t*>(
                                                                                   reinterpret_cast<const uint8_t*>(self) + 0x100));
                }
                if (now - s_talkSince < 60000)
                {
                    return;
                }
            }
            else
            {
                s_talkSince = 0;
            }
        }
        // Quests: off to the quest's target (however far: ClickWalk routes it over the map memory), fighting only
        // what attacks on the way; there, the roaming looks around it.
        int32_t qx = 0, qy = 0;
        double qr = 0.0;
        uint32_t who = 0;
        g_questTravel = false;
        bool questHere = false;
        Whereabouts::update(layer, static_cast<int>(self->x / kTileSize), static_cast<int>(self->y / kTileSize));
        // A position watch of its own (the solver, case A7, 2026-11-12 06:52: 190 «going to look» on one spot 8 tiles
        // from the quest's goal, the Dwarf boxed in by a bench and a fence): every quest return zeroes g_roamAt, so the
        // roam's own progress test never ran. 25 s per step on one spot (3 tiles), no dialog and no collect: the walk
        // cancelled, the book's goal parked 3 min per step, a walk back along the trail to a point 8+ tiles off.
        {
            static int32_t s_px = 0, s_py = 0;
            static DWORD s_since = 0;
            static int s_step = 0;
            // Busy (collecting, talking, at a shop or a craftsman): the clock starts again (the solver, 2026-11-12: 2 of 8
            // watch trips came right after a collection, the time spent picking up counted as standing still).
            const bool busy = UiNav::npcDialogOpen() || g_collecting || Gear::shopOpen() || Gear::craftsmanOpen();
            if (!s_since || busy ||
                std::hypot(static_cast<double>(self->x - s_px), static_cast<double>(self->y - s_py)) > 3.0 * kTileSize)
            {
                s_px = self->x;
                s_py = self->y;
                s_since = now;
                s_step = 0;
            }
            else if (now - s_since > 25000u * static_cast<DWORD>(s_step + 1))
            {
                ++s_step;
                LOG("AutoAttack: no progress for {} s on this spot (step {}): the goal left {} min, a walk out",
                    (now - s_since) / 1000, s_step, 3 * s_step);
                Findings::note("stuck", self->x, self->y, "no progress for {} s (step {})", (now - s_since) / 1000, s_step);
                ClickWalk::cancel();
                if (!g_questBook.empty())
                {
                    const QuestGoal& g = g_questBook.front();
                    g_travelBlocked[(static_cast<uint64_t>(static_cast<uint32_t>(g.x / kTileSize)) << 32) |
                                    static_cast<uint32_t>(g.y / kTileSize)] = now + 180000u * static_cast<DWORD>(s_step);
                }
                for (auto it = g_trail.rbegin(); it != g_trail.rend(); ++it)
                {
                    if (it->layer == layer &&
                        std::hypot(static_cast<double>(it->x - self->x), static_cast<double>(it->y - self->y)) > 8.0 * kTileSize)
                    {
                        ClickWalk::walkTo(it->x, it->y, false);
                        break;
                    }
                }
                s_since = now;
                g_roamAt = 0;
                return;
            }
        }
        // On another layer (a house's floor, a cellar) only a goal near at hand: the journal's tiles are the
        // overworld's, but seen live (2026-11-09) the Vampiress starts in her house on layer 2 with the main quest 3
        // tiles off, and left it alone (no quest off layer 0) to roam against the walls for 20 minutes.
        const int heroLevel = *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(hero) + 0x3FE);
        // A quest whose ground is more than 2 levels above the hero: farming first, where the enemies fit, until the
        // hero has the levels (the player: «si pretende farmear, el foco no es ir a la quest»).
        auto questTooHard = [&](int32_t x, int32_t y) {
            // Where the hero died lately, the goal waits too, however near (the analyst, 2026-11-11: A3, a level 4
            // Battle Mage, loaded beside an archers' camp at 3148,2722 on its quest's way and died there 5 times in
            // 32 min, out of potions each time): farming elsewhere first, as long as the place stays shunned (30 min
            // after one death, longer after more).
            // Held 2 min once decided (the solver, 2026-11-12: A5 flipped farm/quest 19 times, A2 6, as the hero walked
            // in and out of the shunned place's edge).
            static DWORD s_shunUntil = 0;
            const DWORD tick = GetTickCount();
            if (static_cast<int>(tick - s_shunUntil) < 0 ||
                nearDeathSpot(static_cast<int>(x / kTileSize), static_cast<int>(y / kTileSize)) ||
                nearDeathSpot(static_cast<int>(self->x / kTileSize), static_cast<int>(self->y / kTileSize)))
            {
                if (static_cast<int>(tick - s_shunUntil) >= 0)
                {
                    s_shunUntil = tick + 120000;
                }
                setIntent(Intent::Farm, "the quest's way is where the hero died lately: farming elsewhere first");
                return true;
            }
            if (nearHopelessSpot(static_cast<int>(x / kTileSize), static_cast<int>(y / kTileSize), tick))
            {
                setIntent(Intent::Farm, "the quest's goal is by a fight given up as hopeless lately: farming elsewhere first");
                return true;
            }
            // Outmatched lately (two hopeless fights in 10 min): every quest but one at hand waits while the hero farms
            // where it can win (A3 2026-11-12: a quest of a 1-4 region still taken, its way by an archers' camp,
            // 4 deaths in 20 min there).
            if (outmatched(tick) &&
                std::hypot(static_cast<double>(x - self->x), static_cast<double>(y - self->y)) > 15.0 * kTileSize)
            {
                setIntent(Intent::Farm, "outmatched here: farming where the enemies are not above the hero first");
                return true;
            }
            // A goal at hand is no trip into hard ground: the Daemon's start (a tutorial yard, region 59 marked
            // levels 45-80, its main quest 8 tiles off behind a script's blocker) farmed instead, shut in, 14 min.
            if (std::hypot(static_cast<double>(x - self->x), static_cast<double>(y - self->y)) < 60.0 * kTileSize)
            {
                return false;
            }
            const uintptr_t holder = *reinterpret_cast<const uintptr_t*>(0xAD5C40);
            const uint8_t* world = holder ? *reinterpret_cast<const uint8_t* const*>(holder) : nullptr;
            int lo = 0, hi = 0;
            if (!world || !sectorLevels(world, static_cast<int>(x / kTileSize), static_cast<int>(y / kTileSize), lo, hi) ||
                lo <= heroLevel + Mind::now().questLevelSlack)
            {
                return false;
            }
            setIntent(Intent::Farm, std::format("the quest's ground is levels {}-{}, the hero {}: farming first", lo, hi,
                                                heroLevel));
            return true;
        };
        // A quest with its time running: never failed for a detour (the player, 2026-11-11: «prohíbe al agente
        // fracasar misiones, el fracaso cuenta como muerte»): no farming pause, no «farming first» for its ground.
        // Only a time running short (under a game day) counts (the coordinator, 2026-11-12: A3's side quest 152 with
        // 3952 game min left let every quest of the book past «farming first» and the outmatched hold, and the Battle
        // Mage at level 4 walked on to the archers' camp and died there 3 times in 17 min).
        const bool deadline = std::any_of(g_questBook.begin(), g_questBook.end(),
                                          [](const QuestGoal& g) { return g.minutesLeft > 0 && g.minutesLeft <= 1440; });
        if (deadline && g_questPauseUntil)
        {
            g_questPauseUntil = 0;
            LOG("AutoAttack: a quest's time is running: the farming pause dropped");
        }
        // Neither a fight nor a quest done in 8 min while seeing to quests (the analyst, 2026-11-11: A5 «looking around»
        // its goals with 16 under way, 3 fights in 19 min, 8 of 9 half hours under 1600 experience): farming 10 min
        // first, a player gains levels before going on.
        if (!deadline && g_intent == Intent::Quest && g_farmKnown && Config::autoAttack.farm && g_lastTargetAt &&
            now - g_lastTargetAt > 480000 && (!g_questsDoneAt || now - g_questsDoneAt > 480000) &&
            (!g_questPauseUntil || static_cast<int>(now - g_questPauseUntil) >= 0))
        {
            g_questPauseUntil = now + 600000;
            LOG("AutoAttack: 8 min with neither a fight nor a quest done: the quests left 10 min, farming");
        }
        if (Config::autoAttack.quests && !(g_questPauseUntil && static_cast<int>(now - g_questPauseUntil) < 0) &&
            !(g_questNoWayUntil && static_cast<int>(now - g_questNoWayUntil) < 0) &&
            questTarget(self->x, self->y, qx, qy, qr, who) &&
            (layer == 0 || std::hypot(static_cast<double>(qx - self->x), static_cast<double>(qy - self->y)) < 60.0 * kTileSize) &&
            !(!deadline && Config::autoAttack.farm && g_farmKnown && questTooHard(qx, qy)))
        {
            questHere = true;
            setIntent(Intent::Quest, who && who != 0xFFFFFFFFu ? "a quest creature to speak to" : "a quest's goal");
            // A goal inside a house or cave: its interior lies far off on the map (HQ_HE_3_3_1's at 5859..5863,
            // 3851..3886, its doors 3433,2697 and 3436,2735; Manolito): the hero outside goes to the door whose inside
            // landing is nearest the goal, and steps on it (the game's own jump), not to the goal's tile.
            static DWORD s_onDoorSince = 0;
            if (layer == 0)
            {
                const int gtx = static_cast<int>(qx / kTileSize), gty = static_cast<int>(qy / kTileSize);
                const int htx = static_cast<int>(self->x / kTileSize), hty = static_cast<int>(self->y / kTileSize);
                const Teleports::Interior* door = nullptr;
                double bestIn = 40.0;
                for (const Teleports::Interior& in : Teleports::interiors())
                {
                    // Only a door nearer than the goal itself (a house's inside placed far off on the map): one 2893
                    // tiles off for a goal 37 tiles away swapped with it every second (the player saw it looping,
                    // 2026-11-10: «quiere abrir puertas a km de distancia»).
                    const double d = std::hypot(in.inX - gtx, in.inY - gty);
                    const double toDoor = std::hypot(in.outX - htx, in.outY - hty);
                    const double toGoal = std::hypot(gtx - htx, gty - hty);
                    if (d < bestIn && std::hypot(in.inX - htx, in.inY - hty) > 60.0 && toDoor < toGoal * 0.8 &&
                        toDoor < 300.0)
                    {
                        bestIn = d;
                        door = &in;
                    }
                }
                if (door)
                {
                    int dx = door->outX, dy = door->outY;
                    if (!door->start)
                    {
                        double bestA = 1e9;
                        for (const auto& a : Teleports::anchorsNear(0, door->outX, door->outY, 6))
                        {
                            if (const double d = std::hypot(a.first - htx, a.second - hty); d < bestA)
                            {
                                bestA = d;
                                dx = a.first;
                                dy = a.second;
                            }
                        }
                    }
                    static int s_doorToldX = -1;
                    if (s_doorToldX != dx)
                    {
                        s_doorToldX = dx;
                        LOG("AutoAttack: the quest's goal {},{} lies inside (its landing {},{}): to its door at {},{}", gtx,
                            gty, door->inX, door->inY, dx, dy);
                    }
                    setIntent(Intent::Quest, "the quest's goal inside a house: to its door");
                    noteDoor(dx, dy);
                    qx = static_cast<int32_t>((dx + 0.5) * kTileSize);
                    qy = static_cast<int32_t>((dy + 0.5) * kTileSize);
                    qr = 0.0;
                    if (std::hypot(dx - htx, dy - hty) <= 1.0)
                    {
                        // On it and nothing yet: a step off, the next look steps on again.
                        if (!s_onDoorSince)
                        {
                            s_onDoorSince = now;
                        }
                        else if (now - s_onDoorSince > 4000)
                        {
                            s_onDoorSince = 0;
                            ClickWalk::walkTo(self->x + static_cast<int32_t>(2 * kTileSize), self->y, false);
                            g_roamAt = 0;
                            return;
                        }
                    }
                    else
                    {
                        s_onDoorSince = 0;
                    }
                }
            }
            // A quest zone (a wide radius, e.g. the arena's 22 tiles) is reached on its edge too: its centre can be
            // a wall (seen live: the arena's 3786,330), so travel there fails while the hero already stands in it.
            // A creature to speak to that is not loaded yet: its spot is gone to (3 tiles), not the zone's edge (the
            // stuck solver, 2026-11-11: «hablar con X» goals ringed at 29-35 tiles from the journal's centre, 1450 of
            // 1586 «going to look», the creature at 3091,2783 never spoken to by any copy). Once loaded, the talk below.
            // Loaded within 60 tiles, the creature itself is the goal (the stuck solver's check at 21:15: 90 % of «going
            // to look» still in the 29-35 tile ring, the creatures loaded, so the zone's edge counted as there).
            const bool speakTo = who && who != 0xFFFFFFFFu;
            const cObject* speaker = speakTo && objects ? objects->object(who) : nullptr;
            if (speaker && std::max(std::abs(static_cast<double>(speaker->x - self->x)),
                                    std::abs(static_cast<double>(speaker->y - self->y))) < 60.0 * kTileSize)
            {
                qx = speaker->x;
                qy = speaker->y;
            }
            // A fixed goal is gone to at the standable tile nearest it, not its centre (the stuck solver, 2026-11-11:
            // 3174,2794 lies inside a house's solid block, its yard 3 tiles off; walks to the blocked centre stopped
            // 32-38 tiles out, A7 and A8 for hours), and reached within 2 tiles of it. The zone's wide edge only shut
            // in (the arena behind its gate).
            bool fixedGoal = false;
            if (!speakTo && !Whereabouts::area().shutIn)
            {
                int nx = 0, ny = 0;
                if (Whereabouts::nearestReachable(static_cast<int>(qx / kTileSize), static_cast<int>(qy / kTileSize), nx, ny))
                {
                    qx = static_cast<int32_t>((nx + 0.5) * kTileSize);
                    qy = static_cast<int32_t>((ny + 0.5) * kTileSize);
                    fixedGoal = true;
                }
            }
            const double qd = std::hypot(static_cast<double>(qx - self->x), static_cast<double>(qy - self->y));
            const bool zone = qr >= 8.0 * kTileSize && !speakTo && !fixedGoal;
            if (qd > (zone ? qr + 10.0 * kTileSize : speakTo || fixedGoal ? std::min(qr, (speakTo ? 3.0 : 2.0) * kTileSize) : qr))
            {
                g_questTravel = true;
                if (qx != g_questX || qy != g_questY)
                {
                    LOG("AutoAttack: off to the quest at tile {},{} ({:.0f} tiles)", static_cast<int>(qx / kTileSize),
                        static_cast<int>(qy / kTileSize),
                        std::hypot(static_cast<double>(qx - self->x), static_cast<double>(qy - self->y)) / kTileSize);
                    g_questX = qx;
                    g_questY = qy;
                }
                // An escort (read from the quest's objective): the one escorted walks slower; the hero goes in
                // stretches of 5 s and waits 2.5 s for them.
                static DWORD s_escortWalkSince = 0, s_escortWaitUntil = 0;
                if (Lore::questKind() == Lore::QuestKind::Escort)
                {
                    if (s_escortWaitUntil && static_cast<int>(now - s_escortWaitUntil) < 0)
                    {
                        MapMemory::setRoute(true, qx, qy);
                        g_roamAt = 0;
                        return;
                    }
                    s_escortWaitUntil = 0;
                    if (!s_escortWalkSince || now - s_escortWalkSince > 15000)
                    {
                        s_escortWalkSince = now;
                    }
                    else if (now - s_escortWalkSince > 5000)
                    {
                        ClickWalk::cancel();
                        s_escortWaitUntil = now + 2500;
                        s_escortWalkSince = 0;
                        return;
                    }
                }
                // Shut in (a closed yard, the arena behind its portcullis) with the target outside: no travel, which
                // only shuffles; the reachable tile nearest the target (by the gate, where a script may open the
                // way), and the rest of the agent's play inside (chests, fights) from there.
                bool waitShut = false;
                if (const Whereabouts::Area& where = Whereabouts::area(); where.shutIn)
                {
                    const int gtx = static_cast<int>(qx / kTileSize), gty = static_cast<int>(qy / kTileSize);
                    int nx = 0, ny = 0;
                    if (Whereabouts::reachable(gtx, gty) != 1 && Whereabouts::nearestReachable(gtx, gty, nx, ny))
                    {
                        waitShut = true;
                        // A way out found before (a portal end, a door or stairs: MapMemory saw the hero jump there)
                        // within reach: stepped on, each tried once in 2 min; the Gladiator's coliseum is left by
                        // its teleport tiles (tpstart_g_03/04 -> 3293,2717, Manolito), not by a path.
                        static std::unordered_map<uint64_t, DWORD> s_triedWayOut;
                        const int htx = static_cast<int>(self->x / kTileSize), hty = static_cast<int>(self->y / kTileSize);
                        for (const auto& [ex, ey] : waysOutAround(layer, htx, hty))
                        {
                            const uint64_t wk = (static_cast<uint64_t>(static_cast<uint32_t>(ex)) << 32) | static_cast<uint32_t>(ey);
                            if (const auto t = s_triedWayOut.find(wk); t != s_triedWayOut.end() && now - t->second < 120000)
                            {
                                continue;
                            }
                            bool reach = false;
                            for (int dy = -1; dy <= 1 && !reach; ++dy)
                            {
                                for (int dx = -1; dx <= 1 && !reach; ++dx)
                                {
                                    reach = Whereabouts::reachable(ex + dx, ey + dy) == 1;
                                }
                            }
                            if (!reach)
                            {
                                continue;
                            }
                            if (std::abs(ex - htx) <= 1 && std::abs(ey - hty) <= 1)
                            {
                                LOG("AutoAttack: on the way out at {},{}: no jump yet, left 2 min", ex, ey);
                                s_triedWayOut[wk] = now;
                                continue;
                            }
                            const int32_t ewx = static_cast<int32_t>((ex + 0.5) * kTileSize);
                            const int32_t ewy = static_cast<int32_t>((ey + 0.5) * kTileSize);
                            if (approachStalled(ewx, ewy,
                                                std::hypot(static_cast<double>(ewx - self->x), static_cast<double>(ewy - self->y)),
                                                now))
                            {
                                s_triedWayOut[wk] = now;
                                continue;
                            }
                            static uint64_t toldWay = 0;
                            if (wk != toldWay)
                            {
                                toldWay = wk;
                                LOG("AutoAttack: shut in, the quest outside: trying the way out found at {},{}", ex, ey);
                            }
                            g_questTravel = false;
                            if (!ClickWalk::routing() && now - g_questAskAt > 4000)
                            {
                                g_questAskAt = now;
                                ClickWalk::walkTo(static_cast<int32_t>((ex + 0.5) * kTileSize),
                                                  static_cast<int32_t>((ey + 0.5) * kTileSize), false);
                            }
                            MapMemory::setRoute(true, static_cast<int32_t>((ex + 0.5) * kTileSize),
                                                static_cast<int32_t>((ey + 0.5) * kTileSize));
                            g_roamAt = 0;
                            return;
                        }
                        static uint64_t toldShut = 0;
                        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(gtx)) << 32) | static_cast<uint32_t>(gty);
                        if (key != toldShut)
                        {
                            toldShut = key;
                            LOG("AutoAttack: shut in ({} tiles) and the quest's target {},{} lies outside: to {},{}, the "
                                "nearest reachable tile, waiting for the way out",
                                where.tiles, gtx, gty, nx, ny);
                        }
                        const int32_t wx = static_cast<int32_t>((nx + 0.5) * kTileSize);
                        const int32_t wy = static_cast<int32_t>((ny + 0.5) * kTileSize);
                        // A chest near holds the walk to the quest's centre only for its first minute (the stuck solver,
                        // 2026-11-11: chests already failed kept the hero ringing the zone's edge, 94 % of «going to look»).
                        const bool chestNear = g_chestId && g_chestBest < 40.0 * kTileSize &&
                                               (g_chestHeading != g_chestId || now - g_chestSince < 60000);
                        const double wd = std::hypot(static_cast<double>(wx - self->x), static_cast<double>(wy - self->y));
                        if (!chestNear && wd > 0.6 * kTileSize && !approachStalled(wx, wy, wd, now))
                        {
                            g_questTravel = false;
                            if (!ClickWalk::routing() && now - g_questAskAt > 4000)
                            {
                                g_questAskAt = now;
                                ClickWalk::walkTo(wx, wy, false);
                            }
                            MapMemory::setRoute(true, wx, wy);
                            g_roamAt = 0;
                            return;
                        }
                        g_homeX = wx;       // there: chests and fights inside meanwhile, roaming from here
                        g_homeY = wy;
                    }
                }
                if (!waitShut && travel(now, self, qx, qy, g_questAskAt))
                {
                    MapMemory::setRoute(true, qx, qy);
                    g_roamAt = 0;
                    return;
                }
                g_questTravel = false;     // no way there for now: the rest of the agent's play meanwhile
                questHere = false;
                // Held 30 s (A2 2026-11-12: the quest chosen again the next frame, quest/farm flipped every 150 ms
                // for minutes, then the 4502AE crash): the farming goes on meanwhile.
                g_questNoWayUntil = now + 30000;
            }
            else
            {
            // At the goal and nothing comes of it (the analyst, 2026-11-11: A5 «looking around it» 25 min, 3 fights
            // in 28 min, the creature not speaking): after 6 min there the goal is left 15 min, a player goes and
            // does something else before coming back.
            static int32_t s_atGoalX = 0, s_atGoalY = 0;
            static DWORD s_atGoalSince = 0;
            if (std::abs(qx - s_atGoalX) > 3 * kTileSize || std::abs(qy - s_atGoalY) > 3 * kTileSize || !s_atGoalSince ||
                now - s_atGoalSince > 900000)
            {
                s_atGoalX = qx;
                s_atGoalY = qy;
                s_atGoalSince = now;
            }
            else if (now - s_atGoalSince > 360000)
            {
                const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(qx / kTileSize)) << 32) |
                                     static_cast<uint32_t>(qy / kTileSize);
                g_travelBlocked[key] = now + 900000;
                if (!g_questBook.empty())
                {
                    // The book's own goal (qx, qy may be a door or the reachable tile nearest it).
                    const QuestGoal& g = g_questBook.front();
                    g_travelBlocked[(static_cast<uint64_t>(static_cast<uint32_t>(g.x / kTileSize)) << 32) |
                                    static_cast<uint32_t>(g.y / kTileSize)] = now + 900000;
                }
                LOG("AutoAttack: 6 min at the quest's goal {},{} and nothing came of it: left 15 min",
                    static_cast<int>(qx / kTileSize), static_cast<int>(qy / kTileSize));
                s_atGoalSince = 0;
                questHere = false;
                g_roamAt = 0;
                return;
            }
            g_homeX = qx;
            g_homeY = qy;
            // A fixed destination (no creature): the journal's radius is wide (the arena's 22 tiles) but the script
            // may want the hero much nearer its centre (Manolito, FunkCode op 0x3F: the arena fight starts within 10
            // tiles of 3786,330, a wall). To the reachable tile nearest the centre, then the rest of the play there.
            if (who == 0xFFFFFFFFu)
            {
                const int gtx = static_cast<int>(qx / kTileSize), gty = static_cast<int>(qy / kTileSize);
                int nx = 0, ny = 0;
                if (Whereabouts::nearestReachable(gtx, gty, nx, ny))
                {
                    const int32_t wx = static_cast<int32_t>((nx + 0.5) * kTileSize);
                    const int32_t wy = static_cast<int32_t>((ny + 0.5) * kTileSize);
                    g_homeX = wx;
                    g_homeY = wy;
                    const bool chestNear = g_chestId && g_chestBest < 40.0 * kTileSize &&
                                               (g_chestHeading != g_chestId || now - g_chestSince < 60000);
                    const double wd = std::hypot(static_cast<double>(wx - self->x), static_cast<double>(wy - self->y));
                    // Shut in, the nearest tile reached and nothing happening: the script may wait on a trigger
                    // elsewhere in the yard (the arena's barriers open only after the «Equipo» tutorial, fired by the
                    // corner at 3803,345; seen live 2026-11-10: 3 min on the steps at 3794,333, «no way on» every 4.5 s).
                    // After 10 s there, 60 s across the shut-in ground: to its middle, then roaming from it; a new
                    // nearest tile (a barrier opened) ends it.
                    static uint64_t s_pinKey = 0;
                    static DWORD s_pinSince = 0, s_yardUntil = 0;
                    const uint64_t pinKey = (static_cast<uint64_t>(static_cast<uint32_t>(nx)) << 32) | static_cast<uint32_t>(ny);
                    const Whereabouts::Area& where = Whereabouts::area();
                    bool yard = false;
                    if (pinKey != s_pinKey || !where.shutIn)
                    {
                        s_pinKey = pinKey;
                        s_pinSince = s_yardUntil = 0;
                    }
                    else if (s_yardUntil && static_cast<int>(now - s_yardUntil) < 0)
                    {
                        yard = true;
                    }
                    else if (s_yardUntil)
                    {
                        s_yardUntil = s_pinSince = 0;       // over: back to the tile, the yard again in 10 s
                    }
                    else if (!s_pinSince && wd <= 1.5 * kTileSize)
                    {
                        s_pinSince = now;
                    }
                    else if (s_pinSince && now - s_pinSince > 10000)
                    {
                        LOG("AutoAttack: at {},{} by the quest's centre and nothing: 60 s across the shut-in ground", nx, ny);
                        s_yardUntil = now + 60000;
                        yard = true;
                    }
                    int mx = 0, my = 0;
                    if (yard && Whereabouts::nearestReachable((where.minX + where.maxX) / 2, (where.minY + where.maxY) / 2, mx, my))
                    {
                        const int32_t yx = static_cast<int32_t>((mx + 0.5) * kTileSize), yy = static_cast<int32_t>((my + 0.5) * kTileSize);
                        g_homeX = yx;
                        g_homeY = yy;
                        if (static_cast<int>(s_yardUntil - now) > 40000 &&
                            std::hypot(static_cast<double>(yx - self->x), static_cast<double>(yy - self->y)) > 1.5 * kTileSize)
                        {
                            if (!ClickWalk::routing() && now - g_questAskAt > 4000)
                            {
                                g_questAskAt = now;
                                ClickWalk::walkTo(yx, yy, false);
                            }
                            MapMemory::setRoute(true, yx, yy);
                            g_roamAt = 0;
                            return;
                        }
                    }
                    if (!yard && !chestNear && wd > 0.6 * kTileSize && !approachStalled(wx, wy, wd, now))
                    {
                        static uint64_t toldCentre = 0;
                        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(nx)) << 32) | static_cast<uint32_t>(ny);
                        if (key != toldCentre)
                        {
                            toldCentre = key;
                            LOG("AutoAttack: in the quest's zone: to {},{}, the reachable tile nearest its centre {},{}",
                                nx, ny, gtx, gty);
                        }
                        if (!ClickWalk::routing() && now - g_questAskAt > 4000)
                        {
                            g_questAskAt = now;
                            ClickWalk::walkTo(wx, wy, false);
                        }
                        MapMemory::setRoute(true, wx, wy);
                        g_roamAt = 0;
                        return;
                    }
                }
            }
            // There: the quest's creature, if one, is spoken to.
            cObject* npc = who && who != 0xFFFFFFFFu ? objects->object(who) : nullptr;
            // The quest's creature far away (the Gladiator's arena fight names one 5000 tiles off, its opponent
            // only comes when the fight starts): nothing to go for here, the rest of the agent's play meanwhile.
            if (const double away = npc ? std::max(std::abs(static_cast<double>(npc->x - self->x)),
                                                  std::abs(static_cast<double>(npc->y - self->y))) / kTileSize : 0.0;
                away > 60.0)
            {
                static uint32_t farId = 0;
                if (who != farId)
                {
                    farId = who;
                    LOG("AutoAttack: quest creature {:x} is {:.0f} tiles away, not here: playing on", who, away);
                }
                npc = nullptr;
            }
            // A kill quest: the target a living enemy of the hero's, gone for even beyond the fight range.
            // Not one left as too strong for now, nor any with no healing potion left (the live verifier, 2026-11-11:
            // the Dark Elf marked 5dd too strong, went for it as the quest's target 33 s on with no potion and died).
            // With no potion only one not clearly weaker is left (the reviewer: a foe far below the hero would hold
            // the quest forever when there is no gold for potions either).
            cCreature* questFoe = npc ? npc->asCreature() : nullptr;
            const auto levelOf = [](const cCreature* c)
            { return static_cast<int>(*reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(c) + 0x3FE)); };
            // Nor into a fight foreseen under 30 % to win (the live verifier, 2026-11-11: attacked at 8 % among six).
            const CombatLearn::Forecast& questFc = CombatLearn::forecast();
            const bool questLost = questFc.foes > 0 && questFc.win < 0.3f;
            if (questFoe && questFoe->health > 0 && hero->isEnemy(questFoe) &&
                (g_unreachable.count(who & 0xFFFF) || g_unreachable.count(who) || questLost ||
                 (Gear::healingLeft() == 0 && levelOf(questFoe) + 3 > levelOf(hero)) ||
                 (AgentSave::deathsSinceSave() >= 2 && levelOf(questFoe) > levelOf(hero))))
            {
                static uint32_t s_toldFoe = 0;
                if (s_toldFoe != who)
                {
                    s_toldFoe = who;
                    LOG("AutoAttack: quest target {:x} is an enemy, but {}: not now", who,
                        questLost ? "the fight is foreseen lost" : Gear::healingLeft() == 0 ? "no healing potion left" : "too strong for now");
                }
                questFoe = nullptr;
                npc = nullptr;
            }
            if (cCreature* foe = questFoe; foe && foe->health > 0 && hero->isEnemy(foe))
            {
                if (now - g_questAskAt > 2000)
                {
                    if (who != g_talkId)
                    {
                        g_talkId = who;
                        LOG("AutoAttack: quest target {:x} is an enemy: attacking it", who);
                    }
                    g_questAskAt = now;
                    attack(who);
                }
                MapMemory::setRoute(true, foe->x, foe->y);
                g_roamAt = 0;
                return;
            }
            const auto talked = g_talkedAt.find(who);
            const auto fails = g_talkFails.find(who);
            const int failed = fails != g_talkFails.end() ? fails->second : 0;
            const DWORD again = failed >= 4 ? 0xFFFFFFFFu : kTalkAgainMs << (2 * std::max(failed - 1, 0));
            const bool talkNow = talkable(npc) && !isHireling(who) && failed < 4 &&
                                 (talked == g_talkedAt.end() || now - talked->second > again);
            if (speakTo && !talkNow)
            {
                // What keeps the talk off, once per creature (the stuck solver: it failed with no word).
                static uint32_t s_whyId = 0;
                if (s_whyId != who)
                {
                    s_whyId = who;
                    LOG("AutoAttack: quest creature {:x} not spoken to: loaded {}, talkable {}, hireling {}, failed {}, "
                        "spoken {} s ago, {:.0f} tiles off",
                        who, npc != nullptr, talkable(npc), isHireling(who), failed,
                        talked == g_talkedAt.end() ? -1 : static_cast<int>((now - talked->second) / 1000),
                        npc ? std::max(std::abs(static_cast<double>(npc->x - self->x)),
                                       std::abs(static_cast<double>(npc->y - self->y))) / kTileSize : -1.0);
                }
            }
            if (talkNow)
            {
                // Spoken to on foot: a rider never got a word out of one (the stuck solver, 2026-11-11: only the Dwarf
                // and the Daemon, who never ride, spoke to quest creatures; the riders ringed them for hours).
                if (std::max(std::abs(static_cast<double>(npc->x - self->x)), std::abs(static_cast<double>(npc->y - self->y))) <
                        12.0 * kTileSize &&
                    HeroProfile::current().mounted)
                {
                    g_onFootUntil = now + 60000;
                }
                if (who != g_talkId)
                {
                    g_talkId = who;
                    g_talkTries = 0;
                    g_talkAt = 0;
                }
                if (!g_talkAt || now - g_talkAt > 4000)
                {
                    if (++g_talkTries > 6)
                    {
                        const int times = ++g_talkFails[who];
                        LOG("AutoAttack: quest creature {:x} would not speak (time {}), left {}", who, times,
                            times >= 4 ? "for good" : "a while");
                        Findings::note("quest", self->x, self->y, "quest creature {:x} would not speak after 6 tries", who);
                        g_talkedAt[who] = now;
                        g_talkId = 0;
                    }
                    else
                    {
                        LOG("AutoAttack: speaking to the quest creature {:x} (try {})", who, g_talkTries);
                        g_talkAt = now;
                        attack(who);
                    }
                }
                MapMemory::setRoute(true, npc->x, npc->y);
                g_roamAt = 0;
                return;
            }
            }
        }
        // A chest not yet opened within 150 tiles: there (the collect opens it once it is near). Given up after 90 s
        // on the way to one (behind a wall, across water).
        if (Config::autoAttack.pickup && Config::autoAttack.chests && layer == 0 && g_chestId &&
            (!questHere || g_chestBest < 40.0 * kTileSize))
        {
            // The time on one chest adds up across switches: seen live (2026-11-09), the quest and a chest 35 tiles
            // off took turns at the 40-tile line for minutes, each turn starting the 90 s again.
            static std::unordered_map<uint32_t, DWORD> s_chestTime;
            static DWORD s_chestTick = 0;
            if (g_chestId != g_chestHeading)
            {
                g_chestHeading = g_chestId;
                g_chestSince = now;
                LOG("AutoAttack: off to a chest not yet opened ({:x}, {:.0f} tiles)", g_chestId, g_chestBest / kTileSize);
            }
            else if (now - s_chestTick < 5000)
            {
                s_chestTime[g_chestId] += now - s_chestTick;
            }
            s_chestTick = now;
            if (s_chestTime.size() > 256)
            {
                s_chestTime.clear();
            }
            // A chest where the hero was just hurt with no enemy to answer: left like an unreachable one, else its
            // walk takes the hero straight back into the hits (the review of the findings, case 34).
            if (g_unseenSpotAt && now - g_unseenSpotAt < 120000 && std::abs(g_chestX - g_unseenSpotX) < 6 * kTileSize &&
                std::abs(g_chestY - g_unseenSpotY) < 6 * kTileSize)
            {
                LOG("AutoAttack: chest {:x} where the hero is hurt with no enemy seen, left", g_chestId);
                lootFailed(g_chestId, now);
                lootFailed(g_chestId, now);
                g_chestId = 0;
                g_chestHeading = 0;
            }
            else if (s_chestTime[g_chestId] > 90000 || now - g_chestSince > 90000)
            {
                LOG("AutoAttack: chest {:x} not reached in 90 s, left", g_chestId);
                Findings::note("unreachable", self->x, self->y, "chest {:x} not reached in 90 s of walks to it", g_chestId);
                s_chestTime.erase(g_chestId);
                lootFailed(g_chestId, now);
                lootFailed(g_chestId, now);
                g_chestId = 0;
                g_chestHeading = 0;
            }
            else
            {
                if (!ClickWalk::routing() && now - g_questAskAt > 4000)
                {
                    g_questAskAt = now;
                    ClickWalk::walkTo(g_chestX, g_chestY, false);
                }
                MapMemory::setRoute(true, g_chestX, g_chestY);
                g_roamAt = 0;
                return;
            }
        }
        // Nothing worth doing in this zone and another zone's quest known: off there (the farming zone and the
        // roaming come after, for a zone with nothing and no other goal known).
        if (Config::autoAttack.quests && !questHere && layer == 0 && g_zone.valid && !g_zone.worth && g_zone.next)
        {
            static int32_t toldX = 0, toldY = 0;
            if (g_zone.nextX != toldX || g_zone.nextY != toldY)
            {
                toldX = g_zone.nextX;
                toldY = g_zone.nextY;
                LOG("AutoAttack: nothing to do in region {}: off to region {} ({:.0f} tiles)", g_zone.region,
                    g_zone.nextRegion, g_zone.nextDistance / kTileSize);
            }
            if (travel(now, self, g_zone.nextX, g_zone.nextY, g_questAskAt))
            {
                MapMemory::setRoute(true, g_zone.nextX, g_zone.nextY);
                g_roamAt = 0;
                return;
            }
        }
        // No quest to see to: off to the farming zone (many enemies of the hero's level), roaming there.
        if (Config::autoAttack.farm && !questHere && layer == 0 && g_farmKnown)
        {
            if (g_intent != Intent::Farm)
            {
                setIntent(Intent::Farm, "no quest to see to: experience where the enemies fit the hero");
            }
            const double away = std::hypot(static_cast<double>(g_farmX - self->x), static_cast<double>(g_farmY - self->y));
            if (away > 20.0 * kTileSize)
            {
                // The way to farm is farming too: all the loot around is taken (no quest waits).
                if (travel(now, self, g_farmX, g_farmY, g_questAskAt))
                {
                    MapMemory::setRoute(true, g_farmX, g_farmY);
                    g_roamAt = 0;
                    return;
                }
            }
            g_homeX = g_farmX;
            g_homeY = g_farmY;
        }
        if (!Config::autoAttack.roam)
        {
            return;
        }
        if (questHere)
        {
            // At the quest's goal: still the quest, looking around it (not «explore»: the analyst saw the two
            // swapping every second, 172 lines in 10 minutes).
            setIntent(Intent::Quest, "at the quest's goal: looking around it");
            // Within the goal's ring but walled off from it (the player, 2026-11-12: A5 behind a house by the grave
            // digger's yard at 3190,2773, asking for the goal every 4 s for minutes): after 30 s still 3 tiles or more
            // off, the way in near the goal, as travel() does when no walk gets nearer.
            static DWORD s_offGoalSince = 0;
            const double offGoal = std::hypot(static_cast<double>(qx - self->x), static_cast<double>(qy - self->y));
            if ((qx || qy) && offGoal > 3.0 * kTileSize)
            {
                if (!s_offGoalSince)
                {
                    s_offGoalSince = now;
                }
                else if (now - s_offGoalSince > 30000 && entranceNearGoal(now, self, qx, qy))
                {
                    s_offGoalSince = now;
                    g_roamAt = now;
                    return;
                }
            }
            else
            {
                s_offGoalSince = 0;
            }
        }
        else if (g_intent != Intent::Farm || !g_farmKnown)
        {
            // Exploring (the player, 2026-11-10): first the ground unseen, then the loot on it, a fight only in danger
            // (seek off: only those that strike are fought).
            setIntent(Intent::Explore, "nothing to see to around: the ground unseen, its loot, a fight only if attacked");
        }
        // Exploring takes in the caves, cellars and houses near whose inside fits the hero's level (the player: the
        // agent never went into an interior on purpose): to its way in (a stairs anchor by its outside landing, or a
        // tpstart tile), each once a game; in when the hero lands by its inside.
        if (g_intent == Intent::Explore && layer == 0)
        {
            static int s_goingIn = -1;
            static DWORD s_goingSince = 0;
            static std::unordered_set<int> s_doneInteriors;
            const std::vector<Teleports::Interior>& all = Teleports::interiors();
            const int htx = static_cast<int>(self->x / kTileSize), hty = static_cast<int>(self->y / kTileSize);
            if (s_goingIn >= 0)
            {
                const Teleports::Interior& in = all[s_goingIn];
                if (std::abs(htx - in.inX) < 30 && std::abs(hty - in.inY) < 30)
                {
                    LOG("AutoAttack: into the interior at {},{}: exploring it", in.inX, in.inY);
                    s_doneInteriors.insert(s_goingIn);
                    s_goingIn = -1;
                }
                else if (now - s_goingSince > 180000)
                {
                    LOG("AutoAttack: the way into {},{} not found in 3 min, left", in.inX, in.inY);
                    s_doneInteriors.insert(s_goingIn);
                    s_goingIn = -1;
                }
            }
            if (s_goingIn < 0)
            {
                const uintptr_t holder = *reinterpret_cast<const uintptr_t*>(0xAD5C40);
                const uint8_t* world = holder ? *reinterpret_cast<const uint8_t* const*>(holder) : nullptr;
                double best = 80.0;
                for (int i = 0; i < static_cast<int>(all.size()); ++i)
                {
                    const Teleports::Interior& in = all[i];
                    int lo = 0, hi = 0;
                    const double d = std::hypot(in.outX - htx, in.outY - hty);
                    if (d < best && !s_doneInteriors.count(i) && in.outX < 3900 && world &&
                        sectorLevels(world, in.inX, in.inY, lo, hi) && lo <= heroLevel + 2 && hi + 4 >= heroLevel)
                    {
                        best = d;
                        s_goingIn = i;
                    }
                }
                if (s_goingIn >= 0)
                {
                    s_goingSince = now;
                    const Teleports::Interior& in = all[s_goingIn];
                    LOG("AutoAttack: an interior of the hero's level at {:.0f} tiles (its way in by {},{}; inside {},{})",
                        best, in.outX, in.outY, in.inX, in.inY);
                    setIntent(Intent::Explore, "an interior of its level near: going in to explore it");
                }
            }
            if (s_goingIn >= 0)
            {
                const Teleports::Interior& in = all[s_goingIn];
                std::vector<std::pair<int, int>> doors;
                if (in.start)
                {
                    doors.emplace_back(in.outX, in.outY);
                }
                else
                {
                    doors = Teleports::anchorsNear(0, in.outX, in.outY, 6);
                }
                if (doors.empty())
                {
                    s_doneInteriors.insert(s_goingIn);
                    s_goingIn = -1;
                }
                else
                {
                    auto [dx, dy] = doors.front();
                    for (const auto& d : doors)
                    {
                        if (std::hypot(d.first - htx, d.second - hty) < std::hypot(dx - htx, dy - hty))
                        {
                            dx = d.first;
                            dy = d.second;
                        }
                    }
                    const int32_t wx = static_cast<int32_t>((dx + 0.5) * kTileSize), wy = static_cast<int32_t>((dy + 0.5) * kTileSize);
                    noteDoor(dx, dy);
                    if (std::hypot(dx - htx, dy - hty) > 1.5)
                    {
                        if (travel(now, self, wx, wy, g_questAskAt))
                        {
                            MapMemory::setRoute(true, wx, wy);
                            g_roamAt = 0;
                            return;
                        }
                    }
                    else if (!g_roamAt || now - g_roamAt > 3000)
                    {
                        // On the way in and nothing yet: a step off, the next look steps on again.
                        g_roamAt = now;
                        ClickWalk::walkTo(self->x + static_cast<int32_t>(2 * kTileSize), self->y, false);
                        return;
                    }
                }
            }
        }
        MapMemory::visit(layer, self->x, self->y);
        // No further since the last order: that way is blocked (water, a wall), and the map remembers it.
        if (g_roamAt)
        {
            // Progress along the way it was sent: under half the step is blocked (a pathfinder stopping at the water
            // after a tile, as much as no step at all).
            const double mx = self->x - g_roamFromX, my = self->y - g_roamFromY;
            const double wx = g_walkX - g_roamFromX, wy = g_walkY - g_roamFromY;
            const double way = std::max(std::sqrt(wx * wx + wy * wy), 0.5 * kTileSize);
            const double along = (mx * wx + my * wy) / way;     // toward the step's end
            if (along < 0.5 * std::min(way, Config::autoAttack.roamStep * kTileSize))
            {
                ++g_roamStuck;
                // The square just ahead of where it stopped is what it could not step on.
                MapMemory::blocked(layer, static_cast<int32_t>(self->x + std::cos(g_roamAngle) * kTileSize),
                                   static_cast<int32_t>(self->y + std::sin(g_roamAngle) * kTileSize));
            }
            else
            {
                g_roamStuck = 0;
            }
        }
        if (g_roamStuck >= 8)
        {
            // Hemmed in: a new home here, so the hero roams from where it is.
            LOG("AutoAttack: no way on from here, roaming from this spot on");
            g_homeX = self->x;
            g_homeY = self->y;
            g_roamStuck = 0;
            // Hemmed in again and again on the same spot (the player, 2026-11-11: the Dwarf on the arena's tiers above
            // its fence, «no way on» every 4.5 s for minutes, the quest's goal below): the steps straight out fail, so
            // a routed walk to ground 12 tiles off (stairs found by the route), and the quest's goal left 10 min.
            static int s_hemmed = 0;
            static DWORD s_hemmedAt = 0;
            static int32_t s_hemmedX = 0, s_hemmedY = 0;
            if (now - s_hemmedAt < 30000 &&
                std::hypot(static_cast<double>(self->x - s_hemmedX), static_cast<double>(self->y - s_hemmedY)) < 6.0 * kTileSize)
            {
                ++s_hemmed;
            }
            else
            {
                s_hemmed = 1;
                s_hemmedX = self->x;
                s_hemmedY = self->y;
            }
            s_hemmedAt = now;
            if (s_hemmed >= 3)
            {
                s_hemmed = 0;
                if (g_questX || g_questY)
                {
                    g_travelBlocked[(static_cast<uint64_t>(static_cast<uint32_t>(g_questX / kTileSize)) << 32) |
                                    static_cast<uint32_t>(g_questY / kTileSize)] = now + 600000;
                }
                if (!g_questBook.empty())
                {
                    const QuestGoal& g = g_questBook.front();
                    g_travelBlocked[(static_cast<uint64_t>(static_cast<uint32_t>(g.x / kTileSize)) << 32) |
                                    static_cast<uint32_t>(g.y / kTileSize)] = now + 600000;
                }
                const double a = (std::rand() % 360) * 3.14159265358979 / 180.0;
                int nx = static_cast<int>((self->x + std::cos(a) * 12.0 * kTileSize) / kTileSize);
                int ny = static_cast<int>((self->y + std::sin(a) * 12.0 * kTileSize) / kTileSize);
                Whereabouts::nearestReachable(nx, ny, nx, ny);
                const int32_t wx = static_cast<int32_t>((nx + 0.5) * kTileSize), wy = static_cast<int32_t>((ny + 0.5) * kTileSize);
                LOG("AutoAttack: hemmed in 3 times here: a routed walk to tile {},{} and the quest's goal left 10 min", nx, ny);
                ClickWalk::walkTo(wx, wy, false);
                MapMemory::setRoute(true, wx, wy);
                g_homeX = wx;
                g_homeY = wy;
                g_roamAt = now;
                return;
            }
        }
        // Of 16 directions, the one toward the ground least seen (a few cells ahead, nearer ones counting more), not
        // known blocked, keeping roughly the way it goes (no back and forth), and back toward home beyond the leash.
        const double hx = g_homeX - self->x, hy = g_homeY - self->y;
        const double homeDistance = std::sqrt(hx * hx + hy * hy);
        double bestAngle = g_roamAngle;
        float bestCost = 1e9f;
        // The way is kept for some steps unless blocked or past the leash: steps that wander back and forth see nothing.
        // A step's end the hero can stand on, by the game's own tile test: the path finder drops a walk to a tree, a
        // rock or water at once (seen live in a forest: walking, then standing, not a pixel moved, step after step).
        // The step's length out (menu), else shorter down to one tile; none: that way is closed (marked so on the
        // map). The tile's middle.
        const auto stepEnd = [&](double a, int32_t& x, int32_t& y) {
            for (int step = std::clamp(Config::autoAttack.roamStep, 1, 20); step >= 1; --step)
            {
                const double tiles = step;
                const int tx = static_cast<int>(std::floor((self->x + std::cos(a) * tiles * kTileSize) / kTileSize));
                const int ty = static_cast<int>(std::floor((self->y + std::sin(a) * tiles * kTileSize) / kTileSize));
                const int ok = ClickWalk::walkable(layer, tx, ty);
                if (ok != 0)
                {
                    x = static_cast<int32_t>((tx + 0.5) * kTileSize);
                    y = static_cast<int32_t>((ty + 0.5) * kTileSize);
                    return true;
                }
                MapMemory::blocked(layer, static_cast<int32_t>((tx + 0.5) * kTileSize), static_cast<int32_t>((ty + 0.5) * kTileSize));
            }
            return false;
        };
        int32_t keptX = 0, keptY = 0;
        ++g_roamSteps;
        const bool rethink = g_roamStuck > 0 || g_roamSteps >= 6 || homeDistance > kRoamLeash || !g_roamAt ||
                             !stepEnd(g_roamAngle, keptX, keptY);
        if (rethink)
        {
            g_roamSteps = 0;
        }
        bool found = !rethink;
        int32_t bestX = keptX, bestY = keptY;
        const bool avoidAggro = g_intent != Intent::Farm && !Config::autoAttack.seek && !g_aggroFoes.empty();
        float aggroCost[16] = {};
        int bestK = -1;
        for (int k = 0; k < 16 && rethink; ++k)
        {
            const double a = k * 3.14159265 / 8.0;
            const double ux = std::cos(a), uy = std::sin(a);
            int32_t endX = 0, endY = 0;
            if (!stepEnd(a, endX, endY))
            {
                continue;
            }
            float cost = 0.0f;
            for (int d = 1; d <= 4; ++d)
            {
                const double reach = d * 3.0 * kTileSize;
                cost += MapMemory::cost(layer, static_cast<int32_t>(self->x + ux * reach),
                                        static_cast<int32_t>(self->y + uy * reach)) / d;
            }
            // Exploring (not farming nor seeking a fight): the ways within reach of foes not after the hero yet cost
            // (the player: «informar al agente el rango de aggro de los enemigos, para no meterse en peleas
            // innecesariamente»); a pack, each of it.
            if (avoidAggro)
            {
                float closeBy = 0.0f;
                for (const AggroFoe& f : g_aggroFoes)
                {
                    if (f.onHero)
                    {
                        continue;
                    }
                    for (int d = 1; d <= 4; ++d)
                    {
                        const double px = self->x + ux * d * 3.0 * kTileSize, py = self->y + uy * d * 3.0 * kTileSize;
                        if (std::hypot(f.x - px, f.y - py) < kAggroTiles * kTileSize)
                        {
                            closeBy += 0.6f / d;
                            break;
                        }
                    }
                }
                cost += std::min(closeBy, 4.0f);
                aggroCost[k] = std::min(closeBy, 4.0f);
            }
            // Turning away from the way it goes costs a little; turning back, more.
            const double turn = std::fabs(std::remainder(a - g_roamAngle, 2.0 * 3.14159265));
            cost += static_cast<float>(turn) * 0.35f;
            // Beyond the leash, away from home costs; toward it is free.
            if (homeDistance > kRoamLeash)
            {
                const double toward = (ux * hx + uy * hy) / homeDistance;   // 1 straight home, -1 straight away
                cost += static_cast<float>((1.0 - toward) * (homeDistance / kRoamLeash) * 2.0);
            }
            cost += jitter(0, 30) / 100.0f;
            if (cost < bestCost)
            {
                bestCost = cost;
                bestAngle = a;
                bestX = endX;
                bestY = endY;
                found = true;
                bestK = k;
            }
        }
        if (avoidAggro && bestK >= 0)
        {
            static DWORD s_aggroToldAt = 0;
            float worst = 0.0f;
            for (float c : aggroCost)
            {
                worst = std::max(worst, c);
            }
            if (worst > 0.0f && now - s_aggroToldAt > 30000)
            {
                s_aggroToldAt = now;
                LOG("AutoAttack: {} hostile near, not after the hero yet: exploring around them (the way taken {:.1f} "
                    "of aggro, the worst {:.1f})", g_aggroFoes.size(), aggroCost[bestK], worst);
                remember(std::format("rodea {} enemigos que aún no le atacan", g_aggroFoes.size()));
            }
        }
        MapMemory::save(now, 60000);
        if (!found)
        {
            // Closed in on every side as far as the tile test tells: wait, as a hit or the player may free it.
            g_roamAt = now;
            g_roamStuck = 0;
            MapMemory::setRoute(false);
            return;
        }
        g_roamAngle = bestAngle;
        g_roamFromX = self->x;
        g_roamFromY = self->y;
        g_walkX = bestX;
        g_walkY = bestY;
        sendWalk();
        MapMemory::setRoute(true, g_walkX, g_walkY);
        if (!g_roamAt || now - g_roamAt > 10000)
        {
            LOG("AutoAttack: nothing around, going to look ({:.0f} tiles from where it began, {} cells of this layer "
                "seen)", homeDistance / kTileSize, MapMemory::cellsOn(layer));
        }
        g_roamAt = now;
    }

    // The agent's own collect: the loot within LootRange (items, chests and other containers, hiding places and
    // wells) taken one by one, each with the order a click on it gives. Never the game's collect (the A key and the
    // taskbar button stay the player's, as in the plain game). The auto attack waits meanwhile; a click of the
    // player's or a hit taken ends it.
    constexpr DWORD kCollectMaxMs = 90000;
    DWORD g_collectStart = 0;
    int g_collectHealth = 0;

    DWORD g_collectEndedAt = 0;    // see autoCollect

    void endCollect(const char* why)
    {
        if (g_collecting)
        {
            LOG("AutoAttack: collect over ({})", why);
            g_collectEndedAt = GetTickCount();
        }
        g_collecting = false;
        g_collectOne = false;
        g_target = 0;
        g_looting = false;
        g_pending = 0;
    }

    void startCollect(DWORD now)
    {
        ClickWalk::cancel();    // the travel's next stretch would cut the stooping short
        g_collecting = true;
        g_collectStart = now;
        g_target = 0;
        g_looting = false;
        g_pending = 0;
        int maximum = 0;
        g_collectHealth = 0;
        AimAssist::heroHealth(g_collectHealth, maximum);
        Tactics::reset();
        LOG("AutoAttack: collect (the agent's own)");
    }

    void collectFrame(DWORD now)
    {
        if (!Controller::worldRunning() || !heroAlive())
        {
            // A window the loot opened (the storage chest in town, a trader): never gone for again. Only with the hero
            // at it: on the way there the window is the player's own (the inventory, Esc, a loading), and the loot is
            // merely left for now.
            if (g_target && g_lootAtSince && heroAlive())
            {
                LOG("AutoAttack: loot {:x} opened a window, left for good", g_target);
                g_lootDone.insert(lootKey(g_target));
                ++g_lootCount;
            }
            endCollect("no hero or world");
            return;
        }
        if (static_cast<int>(now - g_collectStart) > static_cast<int>(g_collectOne ? 8000u : kCollectMaxMs))
        {
            endCollect("time out");
            return;
        }
        int health = 0, maximum = 0;
        if (AimAssist::heroHealth(health, maximum))
        {
            if (g_collectHealth && health < g_collectHealth)
            {
                if (!g_collectOne)
                {
                    endCollect("hit");
                    return;
                }
            }
            g_collectHealth = health;
        }

        // One at a time: no new order while the hero stoops for one.
        if ((g_pickupUntil && static_cast<int>(now - g_pickupUntil) < 0))
        {
            return;
        }
        if (g_target)
        {
            AimAssist::Target current;
            const float reach = static_cast<float>(Config::autoAttack.lootRange) * 1.5f;
            if (!isFixed(g_target) && !AimAssist::findObject(reach, &isCurrent, nullptr, current))
            {
                g_target = 0;           // gone from the ground: being picked up
                g_pickupUntil = now + kPickupMs;
                return;
            }
            if (!lootDone(g_target, now))
            {
                // Again only when the hero stalls and stands idle: a new order during the pick up (the hero stooping)
                // restarts it, cut short again and again; at the loot after a walk there (far loot) the hero stands
                // with no pick up order yet, so it gets one.
                const int sinceSent = static_cast<int>(now - g_sentAt);
                const bool stalled = g_lootAtSince || static_cast<int>(now - g_lootBestAt) > static_cast<int>(kLootNudgeMs);
                cObjectManager* objects = cObjectManager::instance();
                const bool idle = objects && CombatState::action(objects->hero()) == CombatState::Standing;
                // Just arrived after a walk there (the order sent was the walk): the use order now, the hero still
                // walking or not (seen: a well reached, 3 s standing at it, used only on the retry).
                const bool arrivedUnsent = g_lootAtSince && static_cast<int>(g_sentAt - g_lootAtSince) < 0 &&
                                           now - g_lootAtSince > 300;
                if (((idle && stalled) || arrivedUnsent) && sinceSent > static_cast<int>(kLootNudgeMs) && attack(g_target))
                {
                    g_sentAt = now;
                }
                return;
            }
            // Done for good only when the hero stood at it and it is no item still lying there (an opened container,
            // a used well); an item at its feet is tried again soon, one not reached a minute later.
            if (g_lootStale)
            {
                // nothing marked: the id names another thing now, the next look finds the right one
            }
            else if (g_lootAtSince && !g_lootStill)
            {
                if (g_lootDone.size() > 4096)
                {
                    g_lootDone.clear();
                }
                g_lootDone.insert(lootKey(g_target));
                ++g_lootCount;
            }
            else
            {
                lootFailed(g_target, now, g_lootStill);
            }
            g_lootStill = false;
            g_lootStale = false;
            g_target = 0;
            if (g_collectOne)
            {
                endCollect("one piece, back to the fight");
                return;
            }
        }
        uint32_t next = 0;
        if (!findLoot(next))
        {
            endCollect("nothing left");
            return;
        }
        if (attack(next))
        {
            LOG("AutoAttack: collect {:x}", next);
            g_target = next;
            g_looting = true;
            g_sentAt = now;
            g_lootAtSince = 0;
            g_lootStill = false;
            g_lootStale = false;
            g_lootBestAt = now;
            g_lootBest = 1e9f;
        }
        else
        {
            lootFailed(next, now);
        }
    }

    // Collect always ([AutoAttack] AutoCollect): with loot within LootRange and no fight on, the agent's own collect
    // runs. Not while enemies attack the hero, it was hurt lately or the auto attack
    // has an enemy; a while after each collect, so loot it could not take is not gone for again and again.
    constexpr DWORD kAutoCollectLookMs = 500;
    constexpr DWORD kAutoCollectRestMs = 3000;
    constexpr DWORD kAutoCollectHurtMs = 3000;
    DWORD g_autoCollectLookAt = 0;
    DWORD g_autoHurtAt = 0;
    int g_autoLastHealth = 0;
    // Never in the way of the player: only a while after their last click, with the hero standing.
    constexpr DWORD kAutoCollectIdleMs = 2500;
    constexpr DWORD kAutoCollectStillMs = 1000;
    DWORD g_autoMovedAt = 0;
    int32_t g_autoHeroX = 0, g_autoHeroY = 0;

    // An enemy fighting the hero (or what it rides), one of its hirelings, or one the player attacked.
    bool engaged(cObjectManager* objects, uint32_t id, uint32_t self, uint32_t hero)
    {
        if (g_playerTargets.count(id))
        {
            return true;
        }
        cObject* foe = objects->object(id);
        const uint32_t aim = foe ? aimOf(objects, foe) : 0;
        return aim && (aim == hero || aim == self || isHireling(aim));
    }

    bool underAttack()
    {
        cObjectManager* objects = cObjectManager::instance();
        cEngine* engine = FrameHooks::engine();
        cCreature* hero = objects ? objects->hero() : nullptr;
        if (!objects || !engine || !hero)
        {
            return true;
        }
        AimAssist::Foe foes[kMaxFoes];
        const int count =
            AimAssist::enemies(static_cast<float>(Config::autoAttack.range) * kEngagedReach, foes, kMaxFoes);
        for (int i = 0; i < count; ++i)
        {
            if (engaged(objects, foes[i].id, engine->controlled, hero->id))
            {
                return true;
            }
        }
        return false;
    }

    void autoCollect(DWORD now)
    {
        int health = 0, maximum = 0;
        if (AimAssist::heroHealth(health, maximum))
        {
            if (g_autoLastHealth && health < g_autoLastHealth)
            {
                g_autoHurtAt = now;
            }
            g_autoLastHealth = health;
        }
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        if (hero && (hero->x != g_autoHeroX || hero->y != g_autoHeroY))
        {
            g_autoHeroX = hero->x;
            g_autoHeroY = hero->y;
            g_autoMovedAt = now;
        }
        // The hero standing still is the player's play left alone; walking for the agent (roaming, after a fight) it
        // never stands, and loot it passes is taken on the way.
        if ((g_autoCollectLookAt && static_cast<int>(now - g_autoCollectLookAt) < 0) ||
            (g_quietUntil && static_cast<int>(now - g_quietUntil) < static_cast<int>(kAutoCollectIdleMs)) ||
            (!g_on && static_cast<int>(now - g_autoMovedAt) < static_cast<int>(kAutoCollectStillMs)))
        {
            return;
        }
        g_autoCollectLookAt = now + kAutoCollectLookMs;
        if (g_target || g_pending || Tactics::busy(now) || heroCasting() ||
            (g_collectEndedAt && static_cast<int>(now - g_collectEndedAt) < static_cast<int>(kAutoCollectRestMs)) ||
            (g_autoHurtAt && static_cast<int>(now - g_autoHurtAt) < static_cast<int>(kAutoCollectHurtMs)))
        {
            return;
        }
        uint32_t loot = 0;
        if (!findLoot(loot) || underAttack())
        {
            return;
        }
        LOG("AutoAttack: loot about ({:x}), collecting", loot);
        startCollect(now);
    }

    // A game loaded or another hero: ids and tiles are the old game's, and what they say (loot done or failed,
    // enemies the player attacked, hirelings, enemies out of reach) would hold for whatever now has them.
    uint32_t g_gameHero = 0;
    bool g_wasLoading = false;

    void resetGame(const char* why)
    {
        LOG("AutoAttack: {}, per-game state cleared", why);
        MapMemory::forgetHero();
        endCollect(why);
        g_lootDone.clear();
        g_lootFailures.clear();
        g_playerTargets.clear();
        g_followers.clear();
        g_hirelings.clear();
        g_unreachable.clear();
        g_unreachableAt.clear();
        g_unreachableTimes.clear();
        g_farAimLooks.clear();
        g_lastWorld.clear();
        g_lastDistance.clear();
        g_lastHealth = 0;
        g_hurtAt = 0;
        g_autoLastHealth = 0;
        g_autoHurtAt = 0;
        g_retreatUntil = 0;
        g_homeSet = false;
        g_quietSince = 0;
        g_roamAt = 0;
        g_noRetreatUntil = 0;
        g_pickupUntil = 0;
        g_gameHero = 0;     // the hero after the load is adopted without a second clear
        Tactics::reset();
    }

    void checkNewGame()
    {
        cEngine* engine = FrameHooks::engine();
        const bool loading = engine && (engine->flags & cEngine::loading);
        if (loading && !g_wasLoading)
        {
            resetGame("game loading");
        }
        g_wasLoading = loading;
        cObjectManager* objects = loading ? nullptr : cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        if (hero && hero->id != g_gameHero)
        {
            if (g_gameHero)
            {
                resetGame("another hero");
            }
            g_gameHero = hero->id;
        }
    }

    // The player's teleport (Home menu): off while the agent plays, so the agent never moves but by the game's own ways.
    void drawTeleport()
    {
        static int s_x = 0, s_y = 0, s_layer = 0, s_pick = -1;
        int hx = 0, hy = 0, hl = 0;
        if (!Teleports::heroAt(hx, hy, hl))
        {
            ImGui::TextDisabled("Sin héroe en el mundo.");
            return;
        }
        ImGui::Text("El héroe está en %d, %d (capa %d)", hx, hy, hl);
        if (g_on)
        {
            ImGui::TextWrapped("Solo para cuando juegas tú: el agente está activo (F11 lo para). El agente nunca se "
                               "teletransporta; usa solo las entradas, escaleras y portales del juego.");
            return;
        }
        ImGui::TextWrapped("En el mapa (tecla M): Alt + clic izquierdo lleva al héroe a ese punto.");
        if (ImGui::Button("Aquí##tp"))
        {
            s_x = hx;
            s_y = hy;
            s_layer = hl;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220);
        ImGui::InputInt3("Casilla x, y, capa##tp", &s_x);
        const std::vector<Teleports::Interior>& all = Teleports::interiors();
        if (!all.empty())
        {
            char label[96];
            std::snprintf(label, sizeof label, "%s", s_pick >= 0 && s_pick < static_cast<int>(all.size()) ? "elegido" : "un interior conocido");
            ImGui::SetNextItemWidth(260);
            if (ImGui::BeginCombo("Interiores##tp", label))
            {
                for (int i = 0; i < static_cast<int>(all.size()); ++i)
                {
                    const Teleports::Interior& in = all[i];
                    char item[96];
                    std::snprintf(item, sizeof item, "%s %d,%d (fuera %d,%d)##tp%d", in.start ? "salto" : "interior", in.inX,
                                  in.inY, in.outX, in.outY, i);
                    if (ImGui::Selectable(item, s_pick == i))
                    {
                        s_pick = i;
                        s_x = in.inX;
                        s_y = in.inY;
                        s_layer = hl;
                    }
                }
                ImGui::EndCombo();
            }
        }
        if (ImGui::Button("Teletransportar##tp") && !Teleports::playerTeleport(s_x, s_y, s_layer))
        {
            LOG("Teleports: the player's teleport to {},{} (layer {}) refused", s_x, s_y, s_layer);
        }
    }

    // The ModKit menu's section: what changes applies at once.
    void drawMenu()
    {
        Config::AutoAttack& c = Config::autoAttack;
        if (ImGui::Checkbox("Activo ahora", &g_on))
        {
            LOG("AutoAttack: {} (menu)", g_on ? "on" : "off");
        }
        ImGui::SetItemTooltip("Lo mismo que la tecla F11.");
        ImGui::Checkbox("Activo al arrancar", &c.enabled);
        int mode = c.seek ? 1 : 0;
        ImGui::TextUnformatted("Contra quién lucha:");
        ImGui::RadioButton("Solo quien me ataca (y a quien ataco yo)", &mode, 0);
        ImGui::RadioButton("Buscar combate (cualquier enemigo en el radio)", &mode, 1);
        c.seek = mode == 1;
        ImGui::Checkbox("Explorar cuando no hay nadie", &c.roam);
        ImGui::SetItemTooltip("Paso a paso hacia lo que aún no ha visto (el mapa del agente), con la vida por encima del 60 %%.");
        ImGui::SliderInt("Paso al explorar (casillas)", &c.roamStep, 1, 20);
        ImGui::SetItemTooltip("Lo lejos que va en cada paso al explorar: más largo, explora más rápido; "
                              "el buscador de caminos rodea lo que no se puede pisar.");
        ImGui::Checkbox("Usar el cursor del juego, como un jugador", &c.humanCursor);
        ImGui::SetItemTooltip("Cada acción espera a que el cursor del juego llegue a su objetivo, movido como la mano de "
                              "un buen jugador: reacción, movimiento rápido y alguna corrección. El ratón de Windows no "
                              "se mueve.");
        ImGui::Checkbox("Ir a la misión activa", &c.quests);
        ImGui::SetItemTooltip("Sin combate cerca, va al destino de la misión (la principal primero, si no la más "
                              "cercana), sin límite de distancia; allí explora alrededor.");
        ImGui::Checkbox("Ir a por cofres sin abrir", &c.chests);
        ImGui::SetItemTooltip("Sin misión a la que ir ni enemigos cerca, va al cofre sin abrir más cercano (hasta 150 "
                              "casillas); si no llega en 90 s, lo deja.");
        ImGui::Checkbox("Ir a zonas de farmeo", &c.farm);
        ImGui::SetItemTooltip("Sin misión que hacer ni enemigos cerca, va a la zona con más enemigos de su nivel (lo más "
                              "cerca posible) y explora allí.");
        ImGui::Checkbox("Seguir jugando en segundo plano", &c.background);
        ImGui::SetItemTooltip("El agente sigue con la ventana del juego sin foco (no usa tu ratón ni tu teclado).");
        // The same switches as in the tactics section, here with the fight.
        ImGui::Checkbox("Usar habilidades (artes de combate)##fight", &Config::tactics.arts);
        ImGui::Checkbox("Usar consumibles##fight", &Config::tactics.consumables);
        ImGui::Checkbox("El agente recoge el botín", &c.pickup);
        ImGui::SetItemTooltip("Con su propia recogida (no usa la tecla A, que queda como en el juego): objetos del "
                              "suelo, cofres, escondites y pozos del radio, uno a uno, sin combate cerca.");
        ImGui::BeginDisabled(!c.pickup);
        ImGui::Checkbox("Recoger antes que luchar (sin peligro de morir)", &c.lootInFight);
        ImGui::SetItemTooltip("Con el 60 % de vida o más, recoge primero y luego lucha; con menos, solo lo que esté más cerca que el enemigo "
                              "(uno, y vuelve a la pelea).");
        {
            static const char* kGrades[] = {"Todo (desde blanco)", "Desde azul (mágico)", "Desde amarillo (raro)",
                                            "Solo dorado (único)"};
            ImGui::Combo("Equipo que recoge", &c.minRarity, kGrades, 4);
            ImGui::SetItemTooltip("El equipo por debajo de este grado se queda en el suelo; pociones, runas y oro siempre.");
            ImGui::SliderInt("Oro suficiente por nivel", &c.rarityGoldPerLevel, 0, 20000);
            ImGui::SetItemTooltip("El filtro solo actúa con este oro por nivel del héroe; con menos, recoge todo el "
                                  "equipo para equiparse y venderlo.");
            ImGui::Checkbox("Siempre las piezas de set (verde)", &c.keepSets);
        }
        ImGui::Checkbox("También sin el auto ataque activo", &c.autoCollect);
        ImGui::SetItemTooltip("Apagado: solo recoge tras los combates que hace por ti.");
        ImGui::EndDisabled();
        ImGui::SliderInt("Radio de ataque (px)", &c.range, 100, 1500);
        ImGui::SliderInt("Radio de recogida (px)", &c.lootRange, 100, 3000);
        ImGui::SliderInt("Pausa tras tu clic (ms)", &c.pauseMs, 0, 3000);
        ImGui::SliderInt("Repetir orden (ms)", &c.repeatMs, 0, 3000);
        ImGui::SetItemTooltip("Cada cuánto se repite la orden de ataque; 0 = una sola vez por objetivo.");
    }

    void saveMenu(const std::wstring& ini)
    {
        const Config::AutoAttack& c = Config::autoAttack;
        ModkitMenu::write(ini, L"AutoAttack", L"Enabled", c.enabled);
        ModkitMenu::write(ini, L"AutoAttack", L"Seek", c.seek);
        ModkitMenu::write(ini, L"AutoAttack", L"Roam", c.roam);
        ModkitMenu::write(ini, L"AutoAttack", L"RoamStep", c.roamStep);
        ModkitMenu::write(ini, L"AutoAttack", L"Quests", c.quests);
        ModkitMenu::write(ini, L"AutoAttack", L"HumanCursor", c.humanCursor);
        ModkitMenu::write(ini, L"AutoAttack", L"Background", c.background);
        ModkitMenu::write(ini, L"AutoAttack", L"Farm", c.farm);
        ModkitMenu::write(ini, L"AutoAttack", L"Chests", c.chests);
        ModkitMenu::write(ini, L"AutoAttack", L"LootInFight", c.lootInFight);
        ModkitMenu::write(ini, L"AutoAttack", L"MinRarity", c.minRarity);
        ModkitMenu::write(ini, L"AutoAttack", L"KeepSets", c.keepSets);
        ModkitMenu::write(ini, L"AutoAttack", L"RarityGoldPerLevel", c.rarityGoldPerLevel);
        ModkitMenu::write(ini, L"AutoAttack", L"Pickup", c.pickup);
        ModkitMenu::write(ini, L"AutoAttack", L"AutoCollect", c.autoCollect);
        ModkitMenu::write(ini, L"AutoAttack", L"Range", c.range);
        ModkitMenu::write(ini, L"AutoAttack", L"LootRange", c.lootRange);
        ModkitMenu::write(ini, L"AutoAttack", L"Pause", c.pauseMs);
        ModkitMenu::write(ini, L"AutoAttack", L"Repeat", c.repeatMs);
    }
}

namespace
{
    // The portal box opens when the hero steps on a portal (on the way to loot, an enemy, a quest) and holds the
    // whole game in its modal loop: no frames, so onFrame cannot see it. A thread of its own closes it (Escape, the
    // box's Cancel) once it has stood 1.5 s with the agent on; the player's own portal trips are theirs.
    void watchPortalBox()
    {
        DWORD since = 0, toldAt = 0;
        for (;;)
        {
            Sleep(500);
            const bool on = *static_cast<volatile bool*>(&g_on);
            if (!on || !UiNav::portalBoxOpen())
            {
                since = 0;
                continue;
            }
            const DWORD now = GetTickCount();
            if (!since)
            {
                since = now;
            }
            else if (now - since > 1500 && now - toldAt > 2000)
            {
                LOG("AutoAttack: the portal box opened under the agent (a portal stepped on): closed");
                toldAt = now;
                Inject::key(VK_ESCAPE, true);
                Inject::key(VK_ESCAPE, false);
            }
        }
    }
}

void AutoAttack::install()
{
    std::thread(&watchPortalBox).detach();
    // The controller installs the world pick hook; without it, we do.
    if (!Config::controller.enabled)
    {
        AimAssist::install();
    }
    g_on = Config::autoAttack.enabled;
    ModkitMenu::add({"Ataque automático", &drawMenu, &saveMenu});
    ModkitMenu::add({"Lo que lee el agente", &Lore::drawMenu, nullptr});
    ModkitMenu::add({"Misiones del agente", &drawQuestBook, nullptr});
    ModkitMenu::add({"Dónde está el héroe", &Whereabouts::drawMenu, nullptr});
    ModkitMenu::add({"Zona del agente", &drawZone, nullptr});
    ModkitMenu::add({"Teletransporte (solo jugador)", &drawTeleport, nullptr});
    AgentOverlay::install();
    Teleports::install();
    GrannyAsync::installModelGuard();
    Tactics::install();
    LOG("AutoAttack: {} (key 0x{:x}, range {} px, pause {} ms, repeat {} ms)", g_on ? "on" : "off",
        Config::autoAttack.key, Config::autoAttack.range, Config::autoAttack.pauseMs, Config::autoAttack.repeatMs);
}

// The intent and its reason in Spanish, for the player's overlay (the log keeps English).
static std::string intentSpanish()
{
    static const char* const names[] = {"Explorar", "Farmear", "Misión", "Tienda", "Recuperarse", "Escapar"};
    static const std::pair<const char*, const char*> reasons[] = {
        {"low on health", "poca vida, sin poción o pelea prevista perdida: retrocede"},
        {"shut in", "encerrado: primero las puertas, luego la salida"},
        {"the backpack full", "mochila llena o sin pociones de curación: a un mercader"},
        {"the quest's ground is levels", "la zona de la misión es de más nivel: farmear antes"},
        {"a quest creature", "hablar con un personaje de la misión"},
        {"a quest's goal", "ir al objetivo de la misión"},
        {"the quest's goal inside", "el objetivo está dentro de una casa: a su puerta"},
        {"no quest to see to", "sin misión: experiencia donde los enemigos son de su nivel"},
        {"at the quest's goal", "en el objetivo de la misión: mira alrededor"},
        {"nothing to see to", "descubrir terreno, recoger botín, luchar solo si le atacan"},
        {"an interior of its level", "un interior de su nivel cerca: entra a explorarlo"},
    };
    std::string text = names[std::clamp(static_cast<int>(g_intent), 0, 5)];
    for (const auto& [en, es] : reasons)
    {
        if (g_intentWhy.rfind(en, 0) == 0)
        {
            return text + ": " + es;
        }
    }
    return g_intentWhy.empty() ? text : text + ": " + g_intentWhy;
}

// The agent's overlay over the world (AgentOverlay): what it is about, where it goes, what it fights, the aggro reach.
static void publishOverlay(DWORD now, cCreature* self, float sx, float sy)
{
    using AgentOverlay::Layer;
    AgentOverlay::begin(self->x, self->y, sx, sy);
    cObjectManager* objects = cObjectManager::instance();
    // The scale: the target, else the first foe on screen.
    {
        float tx = 0, ty = 0;
        bool scaled = false;
        if (g_target && objects)
        {
            if (cCreature* t = objects->creature(g_target); t && AimAssist::screenOf(g_target, tx, ty))
            {
                AgentOverlay::calibrate(t->x, t->y, tx, ty);
                scaled = true;
            }
        }
        for (size_t i = 0; i < g_aggroFoes.size() && !scaled && i < 12; ++i)
        {
            if (AimAssist::screenOf(g_aggroFoes[i].id, tx, ty))
            {
                AgentOverlay::calibrate(g_aggroFoes[i].x, g_aggroFoes[i].y, tx, ty);
                scaled = true;
            }
        }
    }
    if (AgentOverlay::shown(Layer::Intent))
    {
        static const uint32_t colors[] = {IM_COL32(120, 200, 255, 255), IM_COL32(255, 200, 60, 255),
                                          IM_COL32(120, 255, 120, 255), IM_COL32(255, 160, 255, 255),
                                          IM_COL32(255, 120, 90, 255), IM_COL32(255, 80, 80, 255)};
        AgentOverlay::heroText(intentSpanish(),
                               colors[std::clamp(static_cast<int>(g_intent), 0, 5)]);
    }
    if (AgentOverlay::shown(Layer::Aggro))
    {
        int drawn = 0;
        for (const AggroFoe& f : g_aggroFoes)
        {
            if (std::hypot(static_cast<double>(f.x - self->x), static_cast<double>(f.y - self->y)) < 30.0 * kTileSize &&
                ++drawn <= 24)
            {
                AgentOverlay::ring(f.x, f.y, 11.2f, f.onHero ? IM_COL32(255, 60, 60, 200) : IM_COL32(255, 160, 40, 110),
                                   f.onHero ? 2.0f : 1.0f);
            }
        }
    }
    if (AgentOverlay::shown(Layer::Route))
    {
        if (g_questTravel && (g_questX || g_questY))
        {
            AgentOverlay::way(g_questX, g_questY, IM_COL32(120, 255, 120, 230),
                              g_intent == Intent::Shop ? "tienda" : "misión");
        }
        else if (ClickWalk::walking() && (g_walkX || g_walkY))
        {
            AgentOverlay::way(g_walkX, g_walkY, IM_COL32(120, 200, 255, 200), "paso");
        }
    }
    if (AgentOverlay::shown(Layer::Doors) && g_doorAt && now - g_doorAt < 5000)
    {
        AgentOverlay::way(g_doorX, g_doorY, IM_COL32(200, 90, 255, 240), "puerta");
        AgentOverlay::ring(g_doorX, g_doorY, 1.0f, IM_COL32(200, 90, 255, 240), 2.5f);
    }
    if (AgentOverlay::shown(Layer::Target) && g_target)
    {
        if (float tx = 0, ty = 0; AimAssist::screenOf(g_target, tx, ty))
        {
            AgentOverlay::around(tx, ty, 26.0f, g_collecting ? IM_COL32(255, 230, 60, 230) : IM_COL32(255, 60, 60, 230));
        }
    }
    AgentOverlay::end(now);
}

// The agent's mind map: a note for Obsidian (Mermaid mindmap) rewritten every 3 s, the agent on or off ([Mind] Path,
// default SacredBild-mente.md next to the game). Off, it shows what it would do if switched on.
static std::string mindSafe(std::string text)
{
    for (char& c : text)
    {
        if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '"' || c == '`')
        {
            c = ' ';
        }
    }
    return text;
}

static void writeMind(DWORD now, cCreature* self, int layer, int heroLevel)
{
    static DWORD s_at = 0;
    static std::wstring s_path;
    if (s_at && now - s_at < 3000)
    {
        return;
    }
    s_at = now;
    if (s_path.empty())
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe;
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
        wchar_t path[MAX_PATH] = {};
        GetPrivateProfileStringW(L"Mind", L"Path", L"", path, MAX_PATH, (dir + L"SacredBild.ini").c_str());
        s_path = path[0] ? path : dir + L"SacredBild-mente.md";
        char narrow[MAX_PATH * 2] = {};
        WideCharToMultiByte(CP_UTF8, 0, s_path.c_str(), -1, narrow, sizeof narrow, nullptr, nullptr);
        LOG("AutoAttack: the agent's mind map to {}", narrow);
    }
    std::string m;
    const auto node = [&](int depth, const std::string& text) {
        m += std::string(2 * depth, ' ') + mindSafe(text) + "\n";
    };
    SYSTEMTIME t;
    GetLocalTime(&t);
    char at[16];
    std::snprintf(at, sizeof at, "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    m += std::string("---\nactualizado: ") + at + "\n---\n# Mente del agente\n\n";
    m += g_on ? "Activo (F11). Lo que piensa y por qué.\n\n"
              : "Inactivo (F11 lo enciende). Lo que haría si lo encendieras.\n\n";
    m += "```mermaid\nmindmap\n";
    m += std::format("  root((Héroe nivel {} · {}))\n", heroLevel, g_on ? "activo" : "inactivo");
    // Intent: the one it follows, or the one it would take.
    int32_t qx = 0, qy = 0;
    double qr = 0.0;
    uint32_t who = 0;
    const bool quest = questTarget(self->x, self->y, qx, qy, qr, who);
    const double questTiles = quest ? std::hypot(static_cast<double>(qx - self->x), static_cast<double>(qy - self->y)) / kTileSize : 0.0;
    node(2, g_on ? "Intención" : "Haría");
    if (g_on)
    {
        node(3, intentSpanish());
        node(3, g_intentSince ? std::format("desde hace {} s", (now - g_intentSince) / 1000) : std::string("aún sin decidir"));
    }
    else
    {
        node(3, Gear::wantsShop() ? "Tienda: mochila llena o sin pociones"
                : quest           ? std::format("Misión a {:.0f} casillas", questTiles)
                : g_farmKnown     ? "Farmear donde los enemigos son de su nivel"
                                  : "Explorar lo que no ha visto");
    }
    node(2, "Opciones");
    node(3, quest ? std::format("Misión: a {:.0f} casillas{}", questTiles, who ? ", hablar con alguien" : "")
                  : "Misión: ninguna a la vista");
    node(3, g_farmKnown ? std::format("Farmear: zona a {:.0f} casillas",
                                      std::hypot(static_cast<double>(g_farmX - self->x), static_cast<double>(g_farmY - self->y)) / kTileSize)
                        : "Farmear: zona aún sin elegir");
    node(3, std::format("Tienda: {}, {} pociones", Gear::wantsShop() ? "hace falta" : "no hace falta",
                        std::max(0, Gear::healingLeft())));
    node(3, std::format("Explorar: {} celdas vistas en esta capa", MapMemory::cellsOn(layer)));
    node(2, "Combate");
    if (const CombatLearn::Forecast& fc = CombatLearn::forecast(); fc.foes > 0)
    {
        node(3, std::format("{} enemigos, ganar {:.0f} %", fc.foes, fc.win * 100.0f));
        node(3, std::format("perdería {:.0f} % de vida en {:.0f} s", fc.expected * 100.0f, fc.seconds));
    }
    else
    {
        node(3, "nadie con quien pelear");
    }
    int after = 0;
    for (const AggroFoe& f : g_aggroFoes)
    {
        after += f.onHero ? 1 : 0;
    }
    node(3, std::format("aggro: {} cerca, {} a por él", g_aggroFoes.size(), after));
    node(2, "Mundo");
    if (g_zone.valid)
    {
        node(3, std::format("zona región {}, niveles {}-{}", g_zone.region, g_zone.lo, g_zone.hi));
        node(3, std::format("{} misiones, {} enemigos, {} cofres", g_zone.quests, g_zone.enemies, g_zone.chests));
    }
    node(3, std::format("capa {}, casilla {},{}", layer, static_cast<int>(self->x / kTileSize), static_cast<int>(self->y / kTileSize)));
    node(2, "Recuerda");
    for (auto it = g_decisions.rbegin(); it != g_decisions.rend() && it - g_decisions.rbegin() < 4; ++it)
    {
        node(3, *it);
    }
    m += "```\n\n## Últimas decisiones\n\n";
    for (auto it = g_decisions.rbegin(); it != g_decisions.rend(); ++it)
    {
        m += "- " + *it + "\n";
    }
    // Written whole and swapped in, so Obsidian never reads half a note.
    const std::wstring tmp = s_path + L".tmp";
    if (FILE* f = _wfopen(tmp.c_str(), L"wb"))
    {
        std::fwrite(m.data(), 1, m.size(), f);
        std::fclose(f);
        MoveFileExW(tmp.c_str(), s_path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

bool AutoAttack::on()
{
    return g_on;
}

// The agent knows when it is stuck (the player, 2026-11-11: «el agente tiene que identificar cuando está atascado, y
// avisarte para que revises el motivo del atasco»): 150 s in the game world with no progress of any kind (not 6 tiles
// from where it stood, no blow that told, no loot taken, no quest done), not in a shop or a load, alive. Then a report
// line to SacredBild-stuck.txt beside the game (read by the coordinator's watcher, which looks into it) and to the log
// (the mosaic shows it). Again only after 10 min, or once it got going and stuck 20 tiles elsewhere.
static void watchStuck(DWORD now, const cCreature* self)
{
    static int32_t s_x = 0, s_y = 0;
    static DWORD s_since = 0, s_hurt = 0, s_loot = 0, s_quests = 0, s_toldAt = 0;
    static int32_t s_toldX = 0, s_toldY = 0;
    const bool moved = std::hypot(static_cast<double>(self->x - s_x), static_cast<double>(self->y - s_y)) > 6 * kTileSize;
    // A shop open over 3 min is no business of a player's (A1, 2026-11-11: its trade flag left set by a death in the
    // shop kept this from ever telling).
    static DWORD s_shopSince = 0;
    const bool shop = Gear::shopOpen();
    s_shopSince = shop ? (s_shopSince ? s_shopSince : now) : 0;
    const bool busy = AgentSave::worldChanging() || (shop && now - s_shopSince < 180000) || !heroAlive();
    if (!s_since || moved || busy || g_lastHurtAt != s_hurt || g_lootCount != s_loot || g_questsDoneAt != s_quests)
    {
        s_x = self->x;
        s_y = self->y;
        s_since = now;
        s_hurt = g_lastHurtAt;
        s_loot = g_lootCount;
        s_quests = g_questsDoneAt;
        return;
    }
    // Escalated (the stuck solver, 2026-11-11, A3 35 min and A2 10 min still on open ground after taking a horse): at
    // 20 s still on horseback off it (the solver, 2026-11-12: a 2-tile lane at 3189,2812 held A1, A3 and A6 mounted
    // 90 s each time; a player gets off at once), at 5 min the last save loaded.
    static DWORD s_offFor = 0, s_reloadFor = 0;
    if (now - s_since >= 20000 && s_offFor != s_since && HeroProfile::current().mounted)
    {
        s_offFor = s_since;
        g_onFootUntil = now + 60000;
        LOG("AgentStuck: 20 s in one place on horseback (layer {}): off the horse", reinterpret_cast<const uint8_t*>(self)[0x24]);
    }
    if (now - s_since >= 300000 && s_reloadFor != s_since)
    {
        s_reloadFor = s_since;
        LOG("AgentStuck: 5 min in one place (layer {}): the last save loaded", reinterpret_cast<const uint8_t*>(self)[0x24]);
        AgentSave::reloadStuck();
    }
    if (now - s_since < 150000)
    {
        return;
    }
    const bool elsewhere =
        std::hypot(static_cast<double>(self->x - s_toldX), static_cast<double>(self->y - s_toldY)) > 20 * kTileSize;
    if (s_toldAt && now - s_toldAt < 600000 && !elsewhere)
    {
        return;
    }
    s_toldAt = now;
    s_toldX = self->x;
    s_toldY = self->y;
    const HeroProfile::Data hero = HeroProfile::current();
    const int layer = *(reinterpret_cast<const uint8_t*>(self) + 0x24);
    SYSTEMTIME t;
    GetLocalTime(&t);
    const std::string line = std::format(
        "{:02d}:{:02d}:{:02d} | {} nivel {} | casilla {},{} capa {} | {} | mision {},{} | objetivo {:x} | {} s sin avanzar",
        t.wHour, t.wMinute, t.wSecond, HeroProfile::className(hero.cls), hero.level, static_cast<int>(self->x / kTileSize),
        static_cast<int>(self->y / kTileSize), layer, intentSpanish(), static_cast<int>(g_questX / kTileSize),
        static_cast<int>(g_questY / kTileSize), g_target, (now - s_since) / 1000);
    LOG("AgentStuck: {}", line);
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = exe;
    path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"SacredBild-stuck.txt";
    if (FILE* f = _wfopen(path.c_str(), L"ab"))
    {
        std::fwrite(line.data(), 1, line.size(), f);
        std::fwrite("\r\n", 1, 2, f);
        std::fclose(f);
    }
}

// Fighting at the weapon's distance (the player, 2026-11-11: «las armas con mayor distancia de rango son para atacar
// desde lo más lejos posible, y las de cuerpo a cuerpo son más eficientes en distancias cortas»; «no he visto el combate
// a distancia»). With a bow, crossbow, throwing weapon or gun an enemy that comes within 3 tiles gets a step back, to
// about 6 tiles off, away from where the enemies stand, as a good player keeps his distance and shoots again. Not when
// shut in, not more than 4 steps in 30 s (cornered, it fights where it stands), and never with low health on a dead end.
// Melee weapons (and the staff or spear, whose reach the game's own walk-up already strikes from) close in as before.
// True: a step back went out (no attack order this time).
static bool keepDistance(DWORD now, uint32_t id)
{
    static DWORD s_stepUntil = 0;
    static DWORD s_steps[4] = {};
    static int s_next = 0;
    if (s_stepUntil && static_cast<int>(now - s_stepUntil) < 0)
    {
        return true;    // the step under way: let it be walked
    }
    const HeroProfile::Data hero = HeroProfile::current();
    if (!hero.valid || !hero.ranged || hero.mounted || Whereabouts::area().shutIn)
    {
        return false;
    }
    cObjectManager* objects = cObjectManager::instance();
    cEngine* engine = FrameHooks::engine();
    cCreature* self = objects && engine ? objects->creature(engine->controlled) : nullptr;
    if (!self)
    {
        return false;
    }
    // The enemies near, by the world: the nearest and where they stand together.
    AimAssist::Foe foes[16];
    const int count = AimAssist::enemies(static_cast<float>(Config::autoAttack.range), foes, 16);
    double nearest = 1e9, cx = 0, cy = 0, weight = 0;
    for (int i = 0; i < count; ++i)
    {
        const cObject* o = objects->object(foes[i].id);
        if (!o)
        {
            continue;
        }
        const double dx = o->x - static_cast<double>(self->x), dy = o->y - static_cast<double>(self->y);
        const double d = std::hypot(dx, dy) / kTileSize;
        nearest = std::min(nearest, d);
        if (d < 6.0)
        {
            const double w = 1.0 / std::max(d, 0.5);
            cx += dx * w;
            cy += dy * w;
            weight += w;
        }
    }
    if (nearest >= 3.0 || weight <= 0)
    {
        return false;
    }
    int recent = 0;
    for (DWORD at : s_steps)
    {
        recent += at && now - at < 30000;
    }
    // Cornered (no more steps back, or no way back below): the weapon for the close fight, if a slot has one.
    static DWORD s_closeAt = 0;
    const auto closeIn = [&] {
        if (now - s_closeAt > 3000 && Gear::chooseWeaponFor(id))
        {
            s_closeAt = now;
            LOG("AutoAttack: cornered {:.1f} tiles from an enemy: the close weapon", nearest);
        }
    };
    if (recent >= 4)
    {
        closeIn();
        return false;
    }
    // Away from them, turned aside up to 90 degrees when that way is a wall or a fence.
    const double away = std::atan2(-cy, -cx);
    const int layer = *(reinterpret_cast<const uint8_t*>(self) + 0x24);
    for (double turn : {0.0, 0.5, -0.5, 1.0, -1.0, 1.57, -1.57})
    {
        const double a = away + turn;
        const double tx = self->x / kTileSize + std::cos(a) * 4.0, ty = self->y / kTileSize + std::sin(a) * 4.0;
        bool clear = true;
        for (int k = 1; k <= 8 && clear; ++k)
        {
            clear = ClickWalk::walkable(layer, static_cast<int>(self->x / kTileSize + std::cos(a) * k * 0.5),
                                        static_cast<int>(self->y / kTileSize + std::sin(a) * k * 0.5)) != 0;
        }
        if (!clear || Whereabouts::reachable(static_cast<int>(tx), static_cast<int>(ty)) == 0)
        {
            continue;
        }
        g_walkX = static_cast<int32_t>(tx * kTileSize);
        g_walkY = static_cast<int32_t>(ty * kTileSize);
        sendWalk();
        s_steps[s_next] = now;
        s_next = (s_next + 1) % 4;
        s_stepUntil = now + 700 + jitter(0, 250);
        LOG("AutoAttack: keeping the distance (weapon kind {}): an enemy {:.1f} tiles off, a step back to {:.0f},{:.0f}, "
            "then {:x} again",
            hero.weaponKind, nearest, tx, ty, id);
        return true;
    }
    closeIn();
    return false;
}

void AutoAttack::onFrame()
{
    Teleports::onFrame();
    // The game's cinema mode (cUI_Manager +8 bit 0x10, the script's SetCinemaMode; Manolito, block M): not to be
    // skipped (the hero held for the script's NPC_Goto, MoveOver, Teleport); the agent stands, only noted here.
    if (cUI_Manager* ui = cUI_Manager::instance())
    {
        static bool s_cinema = false;
        const bool cinema = (ui->flags & cUI_Manager::cinematic) != 0;
        if (cinema != s_cinema)
        {
            s_cinema = cinema;
            LOG("AutoAttack: cinema mode {} (the script's scene: the agent stands)", cinema ? "on" : "off");
        }
    }
    // Menus present from the UI thread, the game from the render thread; around loading both may.
    static std::mutex frameLock;
    const std::unique_lock lock(frameLock, std::try_to_lock);
    if (!lock)
    {
        return;
    }
    const DWORD now = GetTickCount();
    // The player's keys and clicks only while the game has the focus; the agent itself (orders, Enter posted to the
    // game's window) also plays on in the background ([AutoAttack] Background).
    const bool focused = Focus::foreground();
    const bool foreground = focused || Config::autoAttack.background;
    AutoStart::onFrame();
    AgentSave::onFrame();
    // A load: the fight, the retreat and the enemies given up on were of the game left (the live verifier, 2026-11-11:
    // A1 «stepping back over» 3 min after it died, in the game loaded since; object ids are new there too).
    {
        static bool s_wasChanging = false;
        const bool changing = AgentSave::worldChanging();
        static int s_deaths = -1;    // the count read from disk at start is no new death
        if (s_deaths < 0)
        {
            s_deaths = AgentSave::deathsSinceSave();
        }
        if (AgentSave::deathsSinceSave() > s_deaths)
        {
            if (g_farmKnown)
            {
                AgentSave::noteDeathSpot(static_cast<int>(g_farmX / kTileSize), static_cast<int>(g_farmY / kTileSize));
            }
            if (g_aggroHeroX || g_aggroHeroY)
            {
                AgentSave::noteDeathSpot(static_cast<int>(g_aggroHeroX / kTileSize), static_cast<int>(g_aggroHeroY / kTileSize));
            }
            LOG("AutoAttack: died: the farming zone at tile {},{} and where it fell ({},{}) shunned 30 min",
                g_farmKnown ? static_cast<int>(g_farmX / kTileSize) : -1, g_farmKnown ? static_cast<int>(g_farmY / kTileSize) : -1,
                static_cast<int>(g_aggroHeroX / kTileSize), static_cast<int>(g_aggroHeroY / kTileSize));
            g_farmSector = -1;
            if (s_deaths < 2 && AgentSave::deathsSinceSave() >= 2)
            {
                LOG("AutoAttack: careful from now on (2 deaths from the same save): none above the hero's level sought, the retreat sooner");
            }
        }
        s_deaths = AgentSave::deathsSinceSave();
        if (changing && !s_wasChanging)
        {
            g_retreatUntil = 0;
            g_retreatDoomed = false;
            g_retreatExtended = 0;
            g_retreatStartAt = 0;
            g_noRetreatUntil = 0;
            g_unreachable.clear();
            g_unreachableAt.clear();
            g_aggroFoes.clear();
            g_shotBy.clear();
            g_target = 0;
            // A shop trip and a walk of the game left (A1, 2026-11-11: dead with a shop open, then 27 min with no
            // order and no stuck report after the load).
            g_pending = 0;
            g_shopId = 0;
            g_shopTold = false;
            ClickWalk::cancel();
        }
        s_wasChanging = changing;
    }
    checkNewGame();

    // The toggle key, on its press and only while the game has the focus.
    const bool keyDown = focused && physicallyDown(Config::autoAttack.key);
    if (keyDown && !g_keyWasDown)
    {
        g_on = !g_on;
        LOG("AutoAttack: {}", g_on ? "on" : "off");
        g_homeSet = false;     // roaming starts from here
        MessageBeep(g_on ? MB_ICONASTERISK : MB_ICONEXCLAMATION);
    }
    g_keyWasDown = keyDown;

    if (focused && playerClicks())
    {
        g_quietUntil = now + static_cast<DWORD>(Config::autoAttack.pauseMs);
        endCollect("player's click");
        Tactics::reset();
    }

    // The player's own attacks, while their click holds the auto attack back and a moment after.
    if (g_quietUntil && static_cast<int>(now - g_quietUntil) < 1500)
    {
        notePlayerTarget();
    }
    // The quest creature's dialog: the first answer (Enter) until it closes; then it is done for a while. Read at a
    // player's pace (each page a few seconds on screen), so it shows that the agent speaks.
    if (g_on && UiNav::npcDialogOpen())
    {
        Lore::readDialog(UiNav::npcDialogText());    // what it is told, read before it answers
    }
    static DWORD s_talkAnswerAt = 0, s_talkOpenAt = 0;
    static int s_talkAnswers = 0;
    if (g_talkId && UiNav::npcDialogOpen())
    {
        if (!s_talkOpenAt)
        {
            s_talkOpenAt = now;
        }
        if (static_cast<int>(now - s_talkOpenAt) > 2500 && static_cast<int>(now - s_talkAnswerAt) > 3000)
        {
            if (++s_talkAnswers > 12)
            {
                LOG("AutoAttack: quest dialog of {:x} goes on and on, left to the player", g_talkId);
                g_talkedAt[g_talkId] = now;
                g_talkId = 0;
                s_talkAnswers = 0;
                return;
            }
            LOG("AutoAttack: quest dialog of {:x}, answer 1 (Enter)", g_talkId);
            s_talkAnswerAt = now;
            Inject::key(VK_RETURN, true);
            Inject::key(VK_RETURN, false);
        }
        return;
    }
    s_talkOpenAt = 0;
    if (g_talkId && s_talkAnswers && static_cast<int>(now - s_talkAnswerAt) > 1500)
    {
        LOG("AutoAttack: spoke to {:x} ({} answers)", g_talkId, s_talkAnswers);
        g_talkedAt[g_talkId] = now;
        g_talkId = 0;
        s_talkAnswers = 0;
    }
    // A stone's dialogs (a gravestone: "Abrir", then the outcome's "Aceptar"): the first answer each time, as Enter
    // picks it; once none is open any more, the stone is done (enemies it raised are the fight's).
    static DWORD s_answerAt = 0;
    static int s_answers = 0;
    if (g_collecting && foreground && UiNav::npcDialogOpen() && fixedStone(g_target))
    {
        if (static_cast<int>(now - s_answerAt) > 700)
        {
            if (++s_answers > 4)
            {
                LOG("AutoAttack: stone {:x} keeps its dialog open, left to the player", g_target);
                g_lootDone.insert(lootKey(g_target));
                ++g_lootCount;
                s_answers = 0;
                endCollect("dialog");
                return;
            }
            LOG("AutoAttack: stone {:x}: dialog, answer 1 (Enter)", g_target);
            s_answerAt = now;
            Inject::key(VK_RETURN, true);
            Inject::key(VK_RETURN, false);
        }
        return;
    }
    // Any other NPC dialog left open with the agent on (a quest's next page after the talk seemed over, a creature
    // clicked on the way): the first answer after 4 s, as the agent would otherwise stand there for good.
    static DWORD s_strayOpenAt = 0, s_strayAnswerAt = 0;
    if (g_on && !s_answers && UiNav::npcDialogOpen() && static_cast<int>(now - g_craftHoldUntil) >= 0)
    {
        if (!s_strayOpenAt)
        {
            s_strayOpenAt = now;
        }
        // The loot's clocks stand still meanwhile (seen live, 2026-11-09: a help page at the start cost the loot
        // gone for its 6 s, «not reached»).
        g_lootBestAt = now;
        if (g_lootAtSince)
        {
            g_lootAtSince = now;
        }
        if (static_cast<int>(now - s_strayOpenAt) > 4000 && static_cast<int>(now - s_strayAnswerAt) > 3000)
        {
            LOG("AutoAttack: an NPC dialog left open, answer 1 (Enter)");
            s_strayAnswerAt = now;
            Inject::key(VK_RETURN, true);
            Inject::key(VK_RETURN, false);
        }
        return;
    }
    s_strayOpenAt = 0;
    if (s_answers && !UiNav::npcDialogOpen() && static_cast<int>(now - s_answerAt) > 900)
    {
        if (g_collecting && g_target)
        {
            LOG("AutoAttack: stone {:x} used ({} answers)", g_target, s_answers);
            g_lootDone.insert(lootKey(g_target));
            ++g_lootCount;
            g_target = 0;
        }
        s_answers = 0;
    }
    // A dialog opened while collecting: an NPC who spoke up as the hero passed, more often than the loot's own text
    // (fixtures with text are left out by their flags and kind already). The collect stops and leaves the dialog to
    // the player; nothing is marked, so the loot is tried again on the next collect.
    if (g_collecting && foreground && UiNav::npcDialogOpen())
    {
        LOG("AutoAttack: a dialog opened while going for {:x}, left to the player", g_target);
        endCollect("dialog");
        return;
    }
    if (g_collecting)
    {
        if (!foreground)
        {
            endCollect("focus lost");
        }
        else
        {
            collectFrame(now);
            return;
        }
    }
    // Collect always (AutoCollect), and also whenever the auto attack fights for the player: the loot of each fight
    // taken once it is over, before the next (the player's own play keeps to A unless AutoCollect is on).
    if (Config::autoAttack.pickup && (Config::autoAttack.autoCollect || g_on) && !g_collecting && foreground &&
        !Controller::drives() && !ClickWalk::walking() &&
        Controller::worldRunning() && heroAlive())
    {
        autoCollect(now);
        if (g_collecting)
        {
            return;
        }
    }
    // The agent's map follows the hero always, the auto attack on or off (the radar only while it looks around).
    if (Controller::worldRunning())
    {
        cObjectManager* objects = cObjectManager::instance();
        cEngine* engine = FrameHooks::engine();
        if (cCreature* self = objects && engine ? objects->creature(engine->controlled) : nullptr)
        {
            const int layer = *(reinterpret_cast<const uint8_t*>(self) + 0x24);
            MapMemory::visit(layer, self->x, self->y);
            noteTrail(now, self->x, self->y, layer);
            watchTutorial(now);
            ride(now, self);
            if (g_on)
            {
                watchStuck(now, self);
                // The doors on the screen noted for the routes every 3 s, before one is needed (each is noted as it
                // is weighed: acceptDoor), so a plan goes through a gate rather than around a wall.
                static DWORD s_doorScanAt = 0;
                if (now - s_doorScanAt > 3000)
                {
                    s_doorScanAt = now;
                    AimAssist::Target none;
                    AimAssist::findObject(4000.0f, [](uint32_t id, void*) {
                        acceptDoor(id, nullptr);
                        return false;
                    }, nullptr, none);
                }
            }
            // A level gained by the same hero (one up, not a load's change): saved as for a quest done.
            {
                static int s_level = 0;
                static uint32_t s_hero = 0;
                const int level = *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(self) + 0x3A8 + 0x56);
                if (AgentSave::worldChanging())
                {
                    s_level = 0;
                }
                else
                {
                    if (s_hero == self->id && s_level && level == s_level + 1)
                    {
                        AgentSave::levelUp();
                    }
                    s_level = level;
                    s_hero = self->id;
                }
            }
            writeMind(now, self, layer,
                      *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(self) + 0x3A8 + 0x56));
            scanQuestMarks(now, self->x, self->y,
                           *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(self) + 0x3A8 + 0x56),
                           static_cast<uint8_t>(layer));
            if (float sx, sy; AimAssist::hero(sx, sy))
            {
                MapMemory::wantGround(layer, self->x, self->y, sx, sy);
                if (g_on)
                {
                    publishOverlay(now, self, sx, sy);
                }
            }
            if (!g_on)
            {
                MapMemory::beginRadar();
                MapMemory::endRadar();
                MapMemory::setRoute(false);
            }
            MapMemory::save(now, 60000);
        }
    }
    // Gear: put on what is better when calm, sell and buy at a merchant (its own settings, the auto attack on or off).
    // The merchant trip: the shop the agent opened is the trip's; once Gear has nothing more there, it is closed.
    // The inventory left open (a help window's, the player's earlier): with it open a pick up stays on the cursor
    // instead of going to the backpack (seen live 2026-11-09). Closed (I, its key) at once while the agent collects,
    // else after 3 s open with no click of the player's for 8 s; never at a shop nor at a combo master (A2 2026-11-12:
    // closed with the first rune in the swap's slot, 19 visits and no swap).
    static DWORD s_inventorySince = 0, s_inventoryClosedAt = 0;
    {
        cUI_Manager* ui = cUI_Manager::instance();
        cUI_Window2* inventory = ui && (ui->flags & cUI_Manager::inGame) ? ui->window(cUI_Manager::Window::inventory) : nullptr;
        if (g_on && foreground && inventory && inventory->visible() && !Gear::shopOpen() && !Gear::craftsmanOpen())
        {
            if (!s_inventorySince)
            {
                s_inventorySince = now;
            }
            const bool playerBusy = g_quietUntil && static_cast<int>(now - g_quietUntil) < 8000;
            if (now - s_inventoryClosedAt > 1500 &&
                (g_collecting || (now - s_inventorySince > 3000 && !playerBusy)))
            {
                LOG("AutoAttack: the inventory is open{}: closed", g_collecting ? " while collecting" : "");
                s_inventoryClosedAt = now;
                Inject::key('I', true);
                Inject::key('I', false);
            }
        }
        else
        {
            s_inventorySince = 0;
        }
    }
    if (g_shopId && Gear::shopOpen() && g_shopTries)
    {
        LOG("AutoAttack: shop of {:x} open", g_shopId);
        Gear::tripOpened();
        g_shopTries = 0;
    }
    if (Gear::tripDone())
    {
        LOG("AutoAttack: shop of {:x} closed, back to play", g_shopId);
        Inject::key(VK_ESCAPE, true);
        Inject::key(VK_ESCAPE, false);
        g_shopId = 0;
        g_shopTold = false;
    }
    if (foreground && !Controller::drives() && heroAlive())
    {
        Gear::onFrame(now, !g_collecting && !g_target && !g_pending && !g_retreatUntil && !Tactics::busy(now));
        HeroBuild::onFrame(now, !g_target && !g_retreatUntil && !Tactics::busy(now));
    }
    // A walk the player asked for on the agent's map: left be unless the hero is attacked.
    if (!g_on || !foreground || Controller::drives() || !Controller::worldRunning() || !heroAlive() ||
        (g_quietUntil && static_cast<int>(now - g_quietUntil) < 0) || (ClickWalk::walking() && !underAttack()))
    {
        g_target = 0;
        g_pending = 0;
        g_lastHealth = 0;
        Tactics::reset();
        return;
    }

    // A combat art under way: no order that would cut it short.
    Tactics::onFrame(now);
    if (Tactics::busy(now) || (g_pickupUntil && static_cast<int>(now - g_pickupUntil) < 0))
    {
        return;
    }
    // An art the player cast himself (a right click): it outlasts the pause after his click, and an order now would
    // cut it short. Nothing goes out, and Tactics casts nothing over it, until it is done.
    if (heroCasting())
    {
        return;
    }

    // A look at the situation now and then, as a player glances around, not every frame. With nothing aimed at the
    // looks come sooner, so a new attacker is answered quickly, yet at a steady pace: Tactics measures health and
    // damage between looks.
    if (!g_thinkAt || static_cast<int>(now - g_thinkAt) >= 0)
    {
        const bool idle = !g_target && !g_pending;
        g_thinkAt = now + (idle ? jitter(kIdleThinkMinMs, kIdleThinkMaxMs) : jitter(kThinkMinMs, kThinkMaxMs));
        Choice choice;
        if (!decide(now, choice))
        {
            g_target = 0;
            g_pending = 0;
            if (g_lootInstead)
            {
                g_lootInstead = false;
                startCollect(now);
                g_collectOne = !g_lootInsteadAll;
                return;
            }
            roam(now);
            return;
        }
        g_quietSince = 0;
        g_roamAt = 0;
        if (choice.id == g_target)
        {
            g_pending = 0;
        }
        else if (choice.id != g_pending)
        {
            // A new aim: acted on after a moment, sooner under threat.
            g_pending = choice.id;
            g_actAt = now + (choice.urgent ? jitter(kReactUrgentMinMs, kReactUrgentMaxMs)
                                           : jitter(kReactMinMs, kReactMaxMs));
        }
    }

    uint32_t id = g_target;
    if (g_pending && static_cast<int>(now - g_actAt) >= 0)
    {
        id = g_pending;
        g_pending = 0;
    }
    if (!id)
    {
        return;
    }
    if (id == g_target)
    {
        // The order again about every Repeat ms.
        const int sinceSent = static_cast<int>(now - g_sentAt);
        if (g_repeatMs <= 0 || sinceSent < g_repeatMs)
        {
            return;
        }
    }
    if (id != g_target)
    {
        ClickWalk::cancel();
        // A new enemy: the weapon slot that hurts it most first (at most every 3 s; no art under way here).
        static DWORD s_slotAt = 0;
        if (!g_looting && now - s_slotAt > 3000 && Gear::chooseWeaponFor(id))
        {
            s_slotAt = now;
        }
    }
    if (!g_looting && keepDistance(now, id))
    {
        return;
    }
    if (attack(id))
    {
        if (id != g_target)
        {
            // One left as too strong or out of reach is taken again only once it came into the melee: said so (the
            // live verifier, 2026-11-10: «too strong for now» then «target» 2 min on, the log not telling why).
            if (g_unreachable.count(id))
            {
                LOG("AutoAttack: target {:x} (left for now, but it came into the melee)", id);
            }
            else if (shotAtHero(id, now))
            {
                LOG("AutoAttack: target {:x}, the one shooting at the hero", id);
            }
            else
            {
                LOG("AutoAttack: target {:x}", id);
            }
            targetTaken(now, id);
        }
        g_target = id;
        g_sentAt = now;
        g_attackAt.store(now, std::memory_order_relaxed);
        const int repeat = Config::autoAttack.repeatMs;
        g_repeatMs = repeat > 0 ? repeat * static_cast<int>(jitter(80, 125)) / 100 : 0;
    }
    else if (id == g_target)
    {
        g_target = 0;
    }
}

void AutoAttack::footFor(unsigned long ms)
{
    g_onFootUntil = GetTickCount() + ms;
}

bool AutoAttack::foughtWithin(unsigned long ms)
{
    const DWORD at = g_attackAt.load(std::memory_order_relaxed);
    return at && GetTickCount() - at < ms;
}

double AutoAttack::nearestHostileTiles()
{
    double nearest = 99.0 * kTileSize;
    for (const AggroFoe& f : g_aggroFoes)
    {
        nearest = std::min(nearest, std::hypot(static_cast<double>(f.x - g_aggroHeroX), static_cast<double>(f.y - g_aggroHeroY)));
    }
    return nearest / kTileSize;
}

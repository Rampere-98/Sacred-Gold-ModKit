#include "game/auto_attack.h"
#include "game/combat_learn.h"
#include "game/gear.h"
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
#include "game/ui_nav.h"
#include "input/inject.h"
#include "log.h"
#include "sacred/engine.h"
#include "sacred/object.h"
#include "sacred/ui.h"

#include <imgui.h>
#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
            loot = (flags & kFixtureFlag) ? Loot::None : Loot::Item;
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

    void lootFailed(uint32_t id, DWORD now, bool soon = false)
    {
        id = lootKey(id);
        if (g_lootFailures.size() > 1024)
        {
            g_lootFailures.clear();
        }
        LootFailure& f = g_lootFailures[id];
        ++f.count;
        f.retryAt = now + (soon ? kLootSoonMs : kLootRetryMs * f.count);
    }
    // Loot the hero stood at and that stayed: an opened container, an item that did not fit. Never gone for again.
    std::unordered_set<uint32_t> g_lootDone;

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
        return tiles <= kLootMaxTiles;     // all of it before going on, on a trip too (the player's wish)
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

    int32_t g_walkX = 0, g_walkY = 0;
    DWORD g_roamAt = 0;     // the last roaming step (0: not roaming)

    // On the window's thread, as the player's clicks (HeroMove).
    void walkThere(void*)
    {
        cEngine* engine = FrameHooks::engine();
        cObjectManager* objects = cObjectManager::instance();
        cCreature* self = engine && objects ? objects->creature(engine->controlled) : nullptr;
        if (!self)
        {
            return;
        }
        cOrder order;
        order.type = cOrder::move;
        order.mode = cOrder::walk;
        order.x = g_walkX;
        order.y = g_walkY;
        // The path-finding walk to the spot a click gives (follow 0). follow 1 is hold-to-walk: it heads for the
        // cursor and ends within a tenth of a second unless the left button is held (seen live: walking, then 2,
        // then standing, not a pixel moved).
        order.follow = 0;
        engine->sendOrder(self, order);
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
        Inject::call(&walkThere, nullptr);
        return true;
    }

    // Orders the creature the mouse commands (the hero, or its mount) to attack, pick up or open `id`, as a click
    // on it does.
    bool attack(uint32_t id)
    {
        cEngine* engine = FrameHooks::engine();
        cObjectManager* objects = cObjectManager::instance();
        if (!engine || !objects)
        {
            return false;
        }
        cCreature* self = objects->creature(engine->controlled);
        if (self && isFixed(id))
        {
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
            const auto handler = reinterpret_cast<const OrderHandler*>(self->vtable)[6];
            handler(self, nullptr, &order);
            return true;
        }
        cObject* target = objects->object(id);
        if (!self || !target)
        {
            return false;
        }
        // Mounted, a click on a creature goes to the rider, not the mount (seen in the orders the world mouse
        // handler gives): the horse would only walk up to it.
        if (cCreature* hero = objects->hero(); hero && hero != self && target->asCreature())
        {
            self = hero;
        }
        if (!target->asCreature() && walkFirst(self, target->x, target->y))
        {
            return true;
        }
        UseOrder order;
        order.target = id;
        order.x = target->x;
        order.y = target->y;
        const auto handler = reinterpret_cast<const OrderHandler*>(self->vtable)[6];
        handler(self, nullptr, &order);
        return true;
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
    std::unordered_map<uint32_t, DWORD> g_unreachable;      // enemies given up on, until then
    std::unordered_map<uint32_t, float> g_unreachableAt;    // ... and how far they were then (pixels)
    std::unordered_map<uint32_t, int> g_unreachableTimes;   // ... how many times so far (the wait grows each time)
    std::unordered_map<uint32_t, int> g_farAimLooks;        // looks in a row an enemy aimed at the hero from afar
    uint32_t g_lastShooter = 0;                             // the last one taken for the shooter (logged once)
    int g_unseenHits = 0;                                   // hits in a row with no enemy to answer
    DWORD g_meleeAt = 0;                                    // the last look with an enemy in the melee
    DWORD g_unseenHitAt = 0;
    std::unordered_map<uint32_t, std::pair<int32_t, int32_t>> g_lastWorld;   // each enemy's world position, last look
    constexpr DWORD kStuckMs = 4000;        // no nearer to the target in this long, not fighting: out of reach
    constexpr DWORD kUnreachableMs = 20000;

    void targetTaken(DWORD now)
    {
        g_targetSince = now;
        g_targetBest = 0.0f;
        g_targetBestAt = now;
    }

    // --- Stepping back -------------------------------------------------------------------------------------------

    DWORD g_retreatUntil = 0;       // stepping back until then at the latest (0: not stepping back)
    DWORD g_retreatWalkAt = 0;      // the last walk order away
    DWORD g_noRetreatUntil = 0;     // after a long retreat, fight again for a while first
    constexpr DWORD kRetreatMaxMs = 12000, kRetreatPauseMs = 8000;
    constexpr double kRetreatWay = 8 * kTileSize;

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
        for (int i = 0; i < count; ++i)
        {
            const float dx = foes[i].x - hx, dy = foes[i].y - hy;
            anyNear = anyNear || dx * dx + dy * dy < range * range;
            cObject* o = objects->object(foes[i].id);
            if (o && dx * dx + dy * dy < (range * 0.45f) * (range * 0.45f))
            {
                cx += o->x;
                cy += o->y;
                ++close;
            }
        }
        // Low on health, or below 40 % with those striking about to take the rest in moments.
        const bool low = health * 100 <= maximum * c.retreatPercent ||
                         (health * 100 <= maximum * 40 && CombatLearn::secondsToDie() < 2.5f);
        const bool noPotions = CombatState::noPotionsAt.load() != 0;
        if (g_retreatUntil)
        {
            const bool recovered = health * 100 >= maximum * std::min(c.retreatPercent + 20, 90);
            const bool tooLong = static_cast<int>(now - g_retreatUntil) >= 0;
            if (recovered || !noPotions || !anyNear || tooLong)
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
        else if (!c.retreat || !low || !noPotions || !close ||
                 (g_noRetreatUntil && static_cast<int>(now - g_noRetreatUntil) < 0))
        {
            return false;
        }
        else
        {
            g_retreatUntil = now + kRetreatMaxMs;
            g_retreatWalkAt = 0;
            LOG("AutoAttack: stepping back at {} of {} health, no healing potion, {} enemies close", health, maximum,
                close);
        }
        if (close && (!g_retreatWalkAt || now - g_retreatWalkAt >= 1000))
        {
            const double ax = self->x - cx / close, ay = self->y - cy / close;
            const double len = std::sqrt(ax * ax + ay * ay);
            const double ux = len > 1.0 ? ax / len : 1.0, uy = len > 1.0 ? ay / len : 0.0;
            g_walkX = static_cast<int32_t>(self->x + ux * kRetreatWay);
            g_walkY = static_cast<int32_t>(self->y + uy * kRetreatWay);
            Inject::call(&walkThere, nullptr);
            g_retreatWalkAt = now;
        }
        return true;
    }

    // Which enemy to fight. Those attacking the hero, what it rides or a hireling, and those the player attacked
    // (engaged), and with Seek any other within Range; of them, weighing the situation: those close, coming at the
    // hero (by their own steps, in the world) or shooting at it from afar first, the badly wounded finished off, the
    // current one kept for a moment at least and then unless another is clearly better; one the hero gets no nearer
    // to is given up for a while. Loot is not the auto attack's: the player collects with A.
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
        }

        const float range = static_cast<float>(Config::autoAttack.range);
        const float melee = range * 0.45f;
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
        bool inMelee = false;
        for (int i = 0; i < count; ++i)
        {
            const float dx = foes[i].x - hx, dy = foes[i].y - hy;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (foes[i].id == g_target)
            {
                currentDistance = d;
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
            if (g_targetBest <= 0.0f || currentDistance < g_targetBest * 0.9f || currentDistance < range * 0.3f ||
                CombatState::action(self) == CombatState::Fighting ||
                CombatState::casting(self))
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
            const bool atHero = aim && (aim == heroId || aim == selfId);
            const bool atHireling = aim && !atHero && isHireling(aim);
            const bool engagedFoe = g_playerTargets.count(f.id) || atHero || atHireling;
            const bool current = f.id == g_target;
            {
                const auto looks = g_farAimLooks.find(f.id);
                learnFoes.push_back({f.id, f.health, f.maxHealth, atHero, atHireling,
                                     looks != g_farAimLooks.end() && looks->second >= 3});
            }
            // Given up on: only if it comes clearly nearer than it was then. Seen live (2026-11-09): one 122 px off
            // behind a graveyard's fence, within the melee look, was taken again at once, for minutes on end.
            if (const auto gone = g_unreachableAt.find(f.id);
                g_unreachable.count(f.id) && d > std::min(melee, gone != g_unreachableAt.end() ? gone->second * 0.6f : melee))
            {
                continue;
            }
            // Engaged ones within the look's reach (kEngagedReach Range): an archer shooting from afar is gone after
            // too. Seeking fights, any other enemy within Range as well. The current one while it is engaged or
            // within 1.5 Range.
            // On the way to the merchant with the backpack full, or on a trip to a quest: only those that attack (no
            // fight sought). Focus (the player's wish, 2026-11-10): a trip decided on is gone through to the end;
            // only danger (one that attacks) stops it on the way.
            const bool seek = Config::autoAttack.seek && !Gear::wantsShop() && !g_questTravel;
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
                cost /= std::clamp(std::sqrt(hardness), 0.7f, 1.6f);   // the harder it hits, the sooner it goes down
            }
            if (current)
            {
                // Stay on it: a player finishes the enemy they fight, unless another is far nearer; a new one at least
                // a moment (no back and forth).
                cost *= 0.4f;
                currentCandidate = true;
                currentEngaged = striking;
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
                if (d == distances.end() || d->second > range * 2.0f || (g_unreachable.count(foes[i].id) && d->second > melee))
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
        // Only with enemies seen (all out of reach): hurt with none at all is a script's or the ground's (the Daemon's
        // start), and walking off helps nothing (the review of the findings).
        if (hurt && !inMelee && !enemyEngaged && g_hurtAt == now && count > 0)
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
                g_walkX = static_cast<int32_t>(self->x + ax * 10 * kTileSize);
                g_walkY = static_cast<int32_t>(self->y + ay * 10 * kTileSize);
                Inject::call(&walkThere, nullptr);
                g_quietUntil = now + 4000;
                g_unseenHits = 0;
                LOG("AutoAttack: hurt again with no enemy to answer ({} seen): off 10 tiles away", seen);
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
                if (calm || safe || lootDistance < it->second)
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
        // Low on health with no potion: step back instead (unless an art is under way); no arts meanwhile.
        if (!Tactics::busy(now) && retreat(now, self, health, maximum, foes, count, hx, hy, range, objects))
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
        double distance = 0.0;
        bool blocked = false;
    };
    std::vector<QuestGoal> g_questBook;     // rebuilt by questTarget, nearest first
    int g_questsDone = 0, g_questsFailed = 0;
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
                g_questBook.push_back(g);
            }
            // A step done or failed (the journal moved on): who was spoken to lately may have the next one at once.
            // Seen live (2026-11-09): the arena won, Julius' next quest waited out the 2 min of «spoken to lately»
            // with the hero roaming the yard.
            if ((done != g_questsDone || failed != g_questsFailed) && (g_questsDone || g_questsFailed || done || failed))
            {
                LOG("AutoAttack: the journal moved on ({} done, {} failed): anyone may be spoken to again", done, failed);
                Findings::note("quest", heroX, heroY, "the journal moved on: {} done, {} failed", done, failed);
                forgetTalks();
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
            g.distance = std::hypot(static_cast<double>(g.x - heroX), static_cast<double>(g.y - heroY));
            g.blocked = goalBlocked(g.x, g.y, now);
        }
        std::stable_sort(g_questBook.begin(), g_questBook.end(), [](const QuestGoal& a, const QuestGoal& b) {
            if (a.blocked != b.blocked)
            {
                return !a.blocked;
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
            LOG("AutoAttack: quest book: {} goals ({} done, {} failed), going for the nearest: {} at {:.0f} tiles",
                g_questBook.size(), g_questsDone, g_questsFailed,
                g.kind == GoalKind::Journal ? (g.main ? "main quest" : "side quest")
                                            : g.kind == GoalKind::Offer ? "a quest offered" : "a report",
                g.distance / kTileSize);
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
        ImGui::TextDisabled("En orden de cercanía; la primera es la que sigue el agente.");
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
            if (sectorRegion(world, cx, cy) == z.region && sectorLevels(world, cx, cy, lo, hi) && lo <= heroLevel + 5)
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
            // Of its level: full worth; below it less and less (little experience); well above it, risky.
            float fit = 1.0f;
            if (heroLevel > hi)
            {
                fit = std::max(0.15f, static_cast<float>(hi) / static_cast<float>(heroLevel));
            }
            else if (lo > heroLevel + 5)
            {
                fit = 0.25f;
            }
            const double tiles = std::hypot(cx - heroX / kTileSize, cy - heroY / kTileSize);
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
        g_farmBuilding.clear();
        g_chestBuilding = 0;
        g_chestBest = 0.0;
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
                    // A living hostile creature on the surface, for the farming zones (sides 1 hero, 3 civilians, 4
                    // horses, 7 soldiers are not).
                    if (o && *reinterpret_cast<const uint32_t*>(o) >= 0x880000 && *reinterpret_cast<const uint32_t*>(o) < 0x8A0000 &&
                        (*reinterpret_cast<const uint32_t*>(o + 0xC) & 0xFFFF) == static_cast<uint32_t>(it - table) &&
                        !*(o + 0x24) && reinterpret_cast<cObject*>(const_cast<uint8_t*>(o))->asCreature() &&
                        *reinterpret_cast<const int32_t*>(o + 0x4D8) > 0)
                    {
                        const uint32_t side = *reinterpret_cast<const uint32_t*>(o + 0x1F0);
                        if (side && side <= 64 && side != 1 && side != 3 && side != 4 && side != 7)
                        {
                            const int tx = static_cast<int>(*reinterpret_cast<const int32_t*>(o + 0x1C) / kTileSize);
                            const int ty = static_cast<int>(*reinterpret_cast<const int32_t*>(o + 0x20) / kTileSize);
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
                        const double d = std::hypot(static_cast<double>(*reinterpret_cast<const int32_t*>(o + 0x1C) - heroX),
                                                    static_cast<double>(*reinterpret_cast<const int32_t*>(o + 0x20) - heroY));
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
    };
    Travel g_travel;
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
        if (const auto it = s_left.find(key); it != s_left.end() && now - it->second < 120000)
        {
            return true;
        }
        if (key != s_key || distance < s_best - kTileSize)
        {
            s_key = key;
            s_best = distance;
            s_since = now;
            return false;
        }
        if (now - s_since > 20000)
        {
            LOG("AutoAttack: no nearer to tile {},{} in 20 s ({:.0f} tiles): left 2 min, playing on from here",
                static_cast<int>(wx / kTileSize), static_cast<int>(wy / kTileSize), distance / kTileSize);
            s_left[key] = now;
            s_key = 0;
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
            s_waitSince = 0;
            return false;
        }
        if (!s_waitSince)
        {
            s_waitSince = now;
            LOG("AutoAttack: a companion is {:.0f} tiles behind: waiting for it", farthest);
        }
        if (now - s_waitSince > 20000)
        {
            LOG("AutoAttack: the companion did not come up in 20 s ({:.0f} tiles): going on", farthest);
            s_waitSince = 0;
            s_goOnUntil = now + 30000;
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

    bool travel(DWORD now, const cCreature* self, int32_t x, int32_t y, DWORD& askAt)
    {
        if (waitForCompanions(now, self))
        {
            g_travel.lastAt = now;
            g_travel.bestAt = now;      // the wait is no lack of progress
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
            t = Travel{x, y, d, now, now, 0, 0};
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
        }
        if (t.detourUntil && static_cast<int>(now - t.detourUntil) < 0)
        {
            return true;
        }
        t.detourUntil = 0;
        if (now - t.bestAt > 20000)
        {
            ClickWalk::cancel();
            if (++t.fails >= 3)
            {
                LOG("AutoAttack: no way found to tile {},{} ({:.0f} tiles): left 3 min", static_cast<int>(x / kTileSize),
                    static_cast<int>(y / kTileSize), d / kTileSize);
                g_travelBlocked[key] = now + 180000;
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
        if (s_stage == 1)
        {
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
                if (m.layer != layer || (failed != g_shopFailedAt.end() && now - failed->second < 300000))
                {
                    continue;
                }
                const double d = std::hypot(static_cast<double>(m.x - self->x), static_cast<double>(m.y - self->y));
                if (!best || d < bestDistance)
                {
                    best = &m;
                    bestDistance = d;
                }
            }
            if (best)
            {
                g_questTravel = true;
                if (best->id != g_shopId)
                {
                    LOG("AutoAttack: backpack full, off to merchant {:x} ({:.0f} tiles)", best->id, bestDistance / kTileSize);
                    g_shopId = best->id;
                    g_shopTries = 0;
                    g_shopClickAt = 0;
                }
                if (bestDistance > 3.0 * kTileSize)
                {
                    if (!travel(now, self, best->x, best->y, g_shopAskAt))
                    {
                        g_shopFailedAt[best->id] = now;
                        g_shopId = 0;
                    }
                }
                else if (!g_shopClickAt || now - g_shopClickAt > 3000)
                {
                    if (++g_shopTries > 5)
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
                LOG("AutoAttack: backpack full and no merchant known on this layer");
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
        // On another layer (a house's floor, a cellar) only a goal near at hand: the journal's tiles are the
        // overworld's, but seen live (2026-11-09) the Vampiress starts in her house on layer 2 with the main quest 3
        // tiles off, and left it alone (no quest off layer 0) to roam against the walls for 20 minutes.
        if (Config::autoAttack.quests && questTarget(self->x, self->y, qx, qy, qr, who) &&
            (layer == 0 || std::hypot(static_cast<double>(qx - self->x), static_cast<double>(qy - self->y)) < 60.0 * kTileSize))
        {
            questHere = true;
            // A quest zone (a wide radius, e.g. the arena's 22 tiles) is reached on its edge too: its centre can be
            // a wall (seen live: the arena's 3786,330), so travel there fails while the hero already stands in it.
            const double qd = std::hypot(static_cast<double>(qx - self->x), static_cast<double>(qy - self->y));
            const bool zone = qr >= 8.0 * kTileSize;
            if (qd > (zone ? qr + 10.0 * kTileSize : qr))
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
                        for (const auto& [ex, ey] : MapMemory::waysOut(layer, htx, hty, Whereabouts::kRadius))
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
                        const bool chestNear = g_chestId && g_chestBest < 40.0 * kTileSize;
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
            }
            else
            {
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
                    const bool chestNear = g_chestId && g_chestBest < 40.0 * kTileSize;
                    const double wd = std::hypot(static_cast<double>(wx - self->x), static_cast<double>(wy - self->y));
                    if (!chestNear && wd > 0.6 * kTileSize && !approachStalled(wx, wy, wd, now))
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
            if (cCreature* foe = npc ? npc->asCreature() : nullptr; foe && foe->health > 0 && hero->isEnemy(foe))
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
            if (talkable(npc) && !isHireling(who) && failed < 4 &&
                (talked == g_talkedAt.end() || now - talked->second > again))
            {
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
            if (s_chestTime[g_chestId] > 90000 || now - g_chestSince > 90000)
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
        Inject::call(&walkThere, nullptr);
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
    bool g_collecting = false;
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
    Tactics::install();
    LOG("AutoAttack: {} (key 0x{:x}, range {} px, pause {} ms, repeat {} ms)", g_on ? "on" : "off",
        Config::autoAttack.key, Config::autoAttack.range, Config::autoAttack.pauseMs, Config::autoAttack.repeatMs);
}

void AutoAttack::onFrame()
{
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
    if (g_on && !s_answers && UiNav::npcDialogOpen())
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
            scanQuestMarks(now, self->x, self->y,
                           *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(self) + 0x3A8 + 0x56),
                           static_cast<uint8_t>(layer));
            if (float sx, sy; AimAssist::hero(sx, sy))
            {
                MapMemory::wantGround(layer, self->x, self->y, sx, sy);
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
    // else after 3 s open with no click of the player's for 8 s; never at a shop.
    static DWORD s_inventorySince = 0, s_inventoryClosedAt = 0;
    {
        cUI_Manager* ui = cUI_Manager::instance();
        cUI_Window2* inventory = ui && (ui->flags & cUI_Manager::inGame) ? ui->window(cUI_Manager::Window::inventory) : nullptr;
        if (g_on && foreground && inventory && inventory->visible() && !Gear::shopOpen())
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
    if (attack(id))
    {
        if (id != g_target)
        {
            LOG("AutoAttack: target {:x}", id);
            targetTaken(now);
        }
        g_target = id;
        g_sentAt = now;
        const int repeat = Config::autoAttack.repeatMs;
        g_repeatMs = repeat > 0 ? repeat * static_cast<int>(jitter(80, 125)) / 100 : 0;
    }
    else if (id == g_target)
    {
        g_target = 0;
    }
}

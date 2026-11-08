#include "game/auto_attack.h"
#include "game/combat_learn.h"
#include "game/gear.h"
#include "game/map_memory.h"
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

    bool fixedUsable(uint32_t id)
    {
        const uint8_t* record = fixedRecord(id);
        if (!record)
        {
            return false;
        }
        const bool usable = *reinterpret_cast<const uint32_t*>(record + kFixedKind) == kFixedUsable;
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

    // Loot the hero failed at: opened containers, items it stood at without picking them up (a full inventory) or
    // could not reach. Tried again later, a few times, as the hero may have been busy or the way blocked.
    struct LootFailure
    {
        DWORD retryAt = 0;
        int count = 0;
    };
    std::unordered_map<uint32_t, LootFailure> g_lootFailures;
    // Not reached (behind a fence, across a river): tried again a minute later, once.
    constexpr int kLootTries = 2;
    constexpr DWORD kLootRetryMs = 60000;

    bool lootBlocked(uint32_t id, DWORD now)
    {
        id = lootKey(id);
        const auto it = g_lootFailures.find(id);
        return it != g_lootFailures.end() &&
               (it->second.count >= kLootTries || static_cast<int>(now - it->second.retryAt) < 0);
    }

    void lootFailed(uint32_t id, DWORD now)
    {
        id = lootKey(id);
        if (g_lootFailures.size() > 1024)
        {
            g_lootFailures.clear();
        }
        LootFailure& f = g_lootFailures[id];
        ++f.count;
        f.retryAt = now + kLootRetryMs * f.count;
    }
    // Loot the hero stood at and that stayed: an opened container, an item that did not fit. Never gone for again.
    std::unordered_set<uint32_t> g_lootDone;

    // The hero gets no nearer to the loot for this long: it cannot reach it (behind a fence, in a house).
    constexpr DWORD kLootStuckMs = 3500;
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
    DWORD g_lootAtSince = 0;        // since when the hero stands at it, 0 while it does not
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

    bool acceptLoot(uint32_t id, void*)
    {
        if (lootBlocked(id, GetTickCount()) || g_lootDone.count(lootKey(id)))
        {
            return false;
        }
        if (isFixed(id))
        {
            return fixedUsable(id);
        }
        cObjectManager* objects = cObjectManager::instance();
        cObject* object = objects ? objects->object(id) : nullptr;
        const Loot loot = object ? lootOf(object) : Loot::None;
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
            if (std::abs(dx) > reach || std::abs(dy) > reach || dx * dx + dy * dy >= best ||
                lootBlocked(id, GetTickCount()) || g_lootDone.count(lootKey(id)))
            {
                continue;
            }
            best = dx * dx + dy * dy;
            out = id;
        }
        return best < 1e30;
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
                return true;
            }
            return false;
        }
        if (!g_lootAtSince)
        {
            g_lootAtSince = now;
        }
        if (static_cast<int>(now - g_lootAtSince) > static_cast<int>(kLootAtMs))
        {
            LOG("AutoAttack: loot {:x} still there with the hero at it, left", id);
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
        order.follow = 1;
        engine->sendOrder(self, order);
    }

    // True if `self` is too far from (x, y) for the use order: a walk order went out instead.
    bool walkFirst(cCreature* self, int32_t x, int32_t y)
    {
        if (std::abs(x - self->x) <= kUseReach && std::abs(y - self->y) <= kUseReach)
        {
            return false;
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
    constexpr DWORD kHurtMs = 2500;
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
    std::unordered_map<uint32_t, int> g_farAimLooks;        // looks in a row an enemy aimed at the hero from afar
    std::unordered_map<uint32_t, std::pair<int32_t, int32_t>> g_lastWorld;   // each enemy's world position, last look
    constexpr DWORD kKeepTargetMs = 1200;   // a new target is kept at least this long (no back and forth)
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
            it = static_cast<int>(now - it->second) >= 0 ? g_unreachable.erase(it) : std::next(it);
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
                LOG("AutoAttack: no nearer to {:x} in {} s ({:.0f} px away): out of reach, left for {} s", g_target,
                    kStuckMs / 1000, currentDistance, kUnreachableMs / 1000);
                g_unreachable[g_target] = now + kUnreachableMs;
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
        bool keepCurrent = false;
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
            // Given up on: only if it comes close again.
            if (g_unreachable.count(f.id) && d > melee)
            {
                continue;
            }
            // Engaged ones within the look's reach (kEngagedReach Range): an archer shooting from afar is gone after
            // too. Seeking fights, any other enemy within Range as well. The current one while it is engaged or
            // within 1.5 Range.
            if (current ? !engagedFoe && d > range * 1.5f : !engagedFoe && (!Config::autoAttack.seek || d > range))
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
                keepCurrent = now - g_targetSince < kKeepTargetMs;
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
        // Never off one that strikes for one that does not (the jumping from one to another).
        if (keepCurrent || (currentEngaged && !enemyEngaged && g_target))
        {
            enemy = g_target;
        }
        CombatLearn::observe({now, heroObject, self, learnFoes.data(), static_cast<int>(learnFoes.size()),
                              g_hirelings.data(), static_cast<int>(g_hirelings.size())});
        g_lastDistance = std::move(distances);
        g_lastWorld = std::move(world);
        g_farAimLooks = std::move(farAim);

        bool chosen = false;
        if (enemy)
        {
            out = {enemy, threat};
            chosen = true;
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
    constexpr double kRoamWay = 2 * kTileSize;      // a step at a time, a look around between
    constexpr double kRoamLeash = 60 * kTileSize;
    DWORD g_quietSince = 0;
    DWORD g_roamAt = 0;
    double g_roamAngle = 0.0;
    int32_t g_roamFromX = 0, g_roamFromY = 0;   // the hero at the last walk order
    int32_t g_homeX = 0, g_homeY = 0;
    bool g_homeSet = false;
    int g_roamStuck = 0;    // walk orders in a row that got the hero no further
    int g_roamLayer = -1;
    int g_roamSteps = 0;    // steps since the way was chosen
    constexpr size_t kLayer = 0x24;     // a creature's layer: the surface, a dungeon, an interior

    void roam(DWORD now)
    {
        if (!Config::autoAttack.seek || g_retreatUntil)
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
        MapMemory::visit(layer, self->x, self->y);
        // No further since the last order: that way is blocked (water, a wall), and the map remembers it.
        if (g_roamAt)
        {
            // Progress along the way it was sent: under half the step is blocked (a pathfinder stopping at the water
            // after a tile, as much as no step at all).
            const double mx = self->x - g_roamFromX, my = self->y - g_roamFromY;
            const double along = mx * std::cos(g_roamAngle) + my * std::sin(g_roamAngle);
            if (along < 0.5 * kRoamWay)
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
        ++g_roamSteps;
        const bool rethink = g_roamStuck > 0 || g_roamSteps >= 6 || homeDistance > kRoamLeash || !g_roamAt;
        if (rethink)
        {
            g_roamSteps = 0;
        }
        for (int k = 0; k < 16 && rethink; ++k)
        {
            const double a = k * 3.14159265 / 8.0;
            const double ux = std::cos(a), uy = std::sin(a);
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
            }
        }
        g_roamAngle = bestAngle;
        MapMemory::save(now, 60000);
        g_roamFromX = self->x;
        g_roamFromY = self->y;
        g_walkX = static_cast<int32_t>(self->x + std::cos(g_roamAngle) * kRoamWay);
        g_walkY = static_cast<int32_t>(self->y + std::sin(g_roamAngle) * kRoamWay);
        Inject::call(&walkThere, nullptr);
        if (!g_roamAt || now - g_roamAt > 10000)
        {
            LOG("AutoAttack: nothing around, going to look ({:.0f} tiles from where it began, {} cells of this layer "
                "seen)", homeDistance / kTileSize, MapMemory::cellsOn(layer));
        }
        g_roamAt = now;
    }

    // A: the game's own collect (the taskbar's collect button, order 0x113: the hero gathers the items around it).
    // Improved: when the game's collect is done, what is left within LootRange (items it did not reach, chests and
    // other containers, which it never opens) is taken one by one, each with the order a click on it gives. The auto
    // attack waits meanwhile; a click of the player's, a hit taken or the A key again ends it.
    constexpr int kCollectKey = 'A';
    constexpr DWORD kCollectGameMinMs = 700;    // the game's collect gets this long to start moving the hero
    constexpr DWORD kCollectStillMs = 900;      // ... and is done once the hero stands still this long
    constexpr DWORD kCollectMaxMs = 30000;
    bool g_collectKeyWasDown = false;
    bool g_collecting = false;
    bool g_collectRest = false;     // the game's collect is done: taking the rest
    DWORD g_collectStart = 0;
    DWORD g_heroMovedAt = 0;
    int32_t g_heroX = 0, g_heroY = 0;
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
        g_collectRest = false;
        g_target = 0;
        g_looting = false;
        g_pending = 0;
    }

    void startCollect(DWORD now)
    {
        g_collecting = true;
        g_collectRest = false;
        g_collectStart = now;
        g_heroMovedAt = now;
        g_target = 0;
        g_looting = false;
        g_pending = 0;
        int maximum = 0;
        g_collectHealth = 0;
        AimAssist::heroHealth(g_collectHealth, maximum);
        Tactics::reset();
        LOG("AutoAttack: collect (A)");
    }

    // Whether the hero stopped moving lately (the game's collect done), keeping track of its position.
    bool heroStill(DWORD now)
    {
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        if (hero && (hero->x != g_heroX || hero->y != g_heroY))
        {
            g_heroX = hero->x;
            g_heroY = hero->y;
            g_heroMovedAt = now;
        }
        return static_cast<int>(now - g_collectStart) >= static_cast<int>(kCollectGameMinMs) &&
               static_cast<int>(now - g_heroMovedAt) >= static_cast<int>(kCollectStillMs);
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
        if (static_cast<int>(now - g_collectStart) > static_cast<int>(kCollectMaxMs))
        {
            endCollect("time out");
            return;
        }
        int health = 0, maximum = 0;
        if (AimAssist::heroHealth(health, maximum))
        {
            if (g_collectRest && g_collectHealth && health < g_collectHealth)
            {
                endCollect("hit");
                return;
            }
            g_collectHealth = health;
        }

        // First the game's own collect, until the hero stands still.
        if (!g_collectRest)
        {
            if (!heroStill(now))
            {
                return;
            }
            g_collectRest = true;
            LOG("AutoAttack: the game's collect is done, taking the rest");
        }

        // Then the rest, one at a time: no new order while the hero stoops for one.
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
                // Again only when the hero stalls: a new order restarts the pick up.
                const int sinceSent = static_cast<int>(now - g_sentAt);
                const bool stalled = static_cast<int>(now - g_lootBestAt) > static_cast<int>(kLootNudgeMs);
                if (!g_lootAtSince && stalled && sinceSent > static_cast<int>(kLootNudgeMs) && attack(g_target))
                {
                    g_sentAt = now;
                }
                return;
            }
            if (g_lootAtSince)
            {
                if (g_lootDone.size() > 4096)
                {
                    g_lootDone.clear();
                }
                g_lootDone.insert(lootKey(g_target));
            }
            else
            {
                lootFailed(g_target, now);
            }
            g_target = 0;
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
            g_lootBestAt = now;
            g_lootBest = 1e9f;
        }
        else
        {
            lootFailed(next, now);
        }
    }

    // Collect always ([AutoAttack] AutoCollect): with loot within LootRange and no fight on, A is pressed for the
    // player, the same collect as theirs. Not while enemies attack the hero, it was hurt lately or the auto attack
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
        if ((g_autoCollectLookAt && static_cast<int>(now - g_autoCollectLookAt) < 0) ||
            (g_quietUntil && static_cast<int>(now - g_quietUntil) < static_cast<int>(kAutoCollectIdleMs)) ||
            static_cast<int>(now - g_autoMovedAt) < static_cast<int>(kAutoCollectStillMs))
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
        Inject::key(kCollectKey, true);
        Inject::key(kCollectKey, false);
        startCollect(now);
    }

    // A game loaded or another hero: ids and tiles are the old game's, and what they say (loot done or failed,
    // enemies the player attacked, hirelings, enemies out of reach) would hold for whatever now has them.
    uint32_t g_gameHero = 0;
    bool g_wasLoading = false;

    void resetGame(const char* why)
    {
        LOG("AutoAttack: {}, per-game state cleared", why);
        endCollect(why);
        g_lootDone.clear();
        g_lootFailures.clear();
        g_playerTargets.clear();
        g_followers.clear();
        g_hirelings.clear();
        g_unreachable.clear();
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
        // The same switches as in the tactics section, here with the fight.
        ImGui::Checkbox("Usar habilidades (artes de combate)##fight", &Config::tactics.arts);
        ImGui::Checkbox("Usar consumibles##fight", &Config::tactics.consumables);
        int collect = c.autoCollect ? 1 : 0;
        ImGui::TextUnformatted("Recogida:");
        ImGui::RadioButton("Solo al pulsar A", &collect, 0);
        ImGui::RadioButton("Recoger siempre (sin combate cerca)", &collect, 1);
        c.autoCollect = collect == 1;
        ImGui::Checkbox("Mejorar la recogida con A", &c.pickup);
        ImGui::SetItemTooltip("Tras la recogida del juego (tecla A), toma lo que quede en el radio y abre los cofres.");
        ImGui::SliderInt("Radio de ataque (px)", &c.range, 100, 1500);
        ImGui::SliderInt("Radio de recogida (px)", &c.lootRange, 100, 2000);
        ImGui::SliderInt("Pausa tras tu clic (ms)", &c.pauseMs, 0, 3000);
        ImGui::SliderInt("Repetir orden (ms)", &c.repeatMs, 0, 3000);
        ImGui::SetItemTooltip("Cada cuánto se repite la orden de ataque; 0 = una sola vez por objetivo.");
    }

    void saveMenu(const std::wstring& ini)
    {
        const Config::AutoAttack& c = Config::autoAttack;
        ModkitMenu::write(ini, L"AutoAttack", L"Enabled", c.enabled);
        ModkitMenu::write(ini, L"AutoAttack", L"Seek", c.seek);
        ModkitMenu::write(ini, L"AutoAttack", L"Pickup", c.pickup);
        ModkitMenu::write(ini, L"AutoAttack", L"AutoCollect", c.autoCollect);
        ModkitMenu::write(ini, L"AutoAttack", L"Range", c.range);
        ModkitMenu::write(ini, L"AutoAttack", L"LootRange", c.lootRange);
        ModkitMenu::write(ini, L"AutoAttack", L"Pause", c.pauseMs);
        ModkitMenu::write(ini, L"AutoAttack", L"Repeat", c.repeatMs);
    }
}

void AutoAttack::install()
{
    // The controller installs the world pick hook; without it, we do.
    if (!Config::controller.enabled)
    {
        AimAssist::install();
    }
    g_on = Config::autoAttack.enabled;
    ModkitMenu::add({"Ataque automático", &drawMenu, &saveMenu});
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
    const bool foreground = Focus::foreground();
    checkNewGame();

    // The toggle key, on its press and only while the game has the focus.
    const bool keyDown = foreground && physicallyDown(Config::autoAttack.key);
    if (keyDown && !g_keyWasDown)
    {
        g_on = !g_on;
        LOG("AutoAttack: {}", g_on ? "on" : "off");
        g_homeSet = false;     // roaming starts from here
        MessageBeep(g_on ? MB_ICONASTERISK : MB_ICONEXCLAMATION);
    }
    g_keyWasDown = keyDown;

    if (foreground && playerClicks())
    {
        g_quietUntil = now + static_cast<DWORD>(Config::autoAttack.pauseMs);
        endCollect("player's click");
        Tactics::reset();
    }

    // A: the game collects; we follow it up, and the auto attack waits. A again while taking the rest ends it.
    const bool collectDown = foreground && physicallyDown(kCollectKey) && !physicallyDown(VK_CONTROL) &&
                             !physicallyDown(VK_MENU);
    if (collectDown && !g_collectKeyWasDown && Controller::worldRunning())
    {
        if (g_collecting && g_collectRest)
        {
            endCollect("A again");
        }
        else if (!g_collecting)
        {
            startCollect(now);
        }
    }
    g_collectKeyWasDown = collectDown;
    // The player's own attacks, while their click holds the auto attack back and a moment after.
    if (g_quietUntil && static_cast<int>(now - g_quietUntil) < 1500)
    {
        notePlayerTarget();
    }
    // A dialog opened while collecting: an NPC who spoke up as the hero passed, more often than the loot's own text
    // (fixtures with text are left out by their flags and kind already). The collect stops and leaves the dialog to
    // the player; nothing is marked, so the loot is tried again on the next A.
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
        else if (Config::autoAttack.pickup)
        {
            collectFrame(now);
            return;
        }
        else if (!heroStill(now))
        {
            return;     // only the game's collect, which the auto attack must not cut short
        }
        else
        {
            endCollect("the game's collect is done");
        }
    }
    // Collect always (AutoCollect), and also whenever the auto attack fights for the player: the loot of each fight
    // taken once it is over, before the next (the player's own play keeps to A unless AutoCollect is on).
    if ((Config::autoAttack.autoCollect || g_on) && !g_collecting && foreground && !Controller::drives() &&
        Controller::worldRunning() && heroAlive())
    {
        autoCollect(now);
        if (g_collecting)
        {
            return;
        }
    }
    // Gear: put on what is better when calm, sell and buy at a merchant (its own settings, the auto attack on or off).
    if (foreground && !Controller::drives() && heroAlive())
    {
        Gear::onFrame(now, !g_collecting && !g_target && !g_pending && !g_retreatUntil && !Tactics::busy(now));
    }
    if (!g_on || !foreground || Controller::drives() || !Controller::worldRunning() || !heroAlive() ||
        (g_quietUntil && static_cast<int>(now - g_quietUntil) < 0))
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

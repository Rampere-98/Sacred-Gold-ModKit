#include "game/auto_attack.h"
#include "config/auto_attack.h"
#include "config/controller.h"
#include "game/aim_assist.h"
#include "game/controller.h"
#include "game/focus.h"
#include "game/frame_hooks.h"
#include "game/ui_nav.h"
#include "input/inject.h"
#include "log.h"
#include "sacred/engine.h"
#include "sacred/object.h"

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

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
    };

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
    constexpr int kLootTries = 3;
    constexpr DWORD kLootRetryMs = 15000;

    bool lootBlocked(uint32_t id, DWORD now)
    {
        const auto it = g_lootFailures.find(id);
        return it != g_lootFailures.end() &&
               (it->second.count >= kLootTries || static_cast<int>(now - it->second.retryAt) < 0);
    }

    void lootFailed(uint32_t id, DWORD now)
    {
        if (g_lootFailures.size() > 1024)
        {
            g_lootFailures.clear();
        }
        LootFailure& f = g_lootFailures[id];
        ++f.count;
        f.retryAt = now + kLootRetryMs * f.count;
    }
    // Fixtures the game ranks as ground items (signposts, plaques: cSpecial3D like some loot) whose click opened a
    // dialog; they keep their ids, so they stay here.
    std::unordered_set<uint32_t> g_fixtures;

    // The hero gets no nearer to the loot for this long: it cannot reach it (behind a fence, in a house).
    constexpr DWORD kLootStuckMs = 3500;
    // How long it may stand at the loot before we leave it.
    constexpr DWORD kLootAtMs = 3000;
    // A loot order is given once (giving it again restarts the pick up, so gold farther than a second away was
    // never taken); again only after this long without the hero getting nearer.
    constexpr DWORD kLootNudgeMs = 1200;
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

    // The current target if it is still there and within reach, else the nearest enemy in range (`range` pixels).
    bool chooseTarget(AimAssist::Target& t, float range)
    {
        float hx, hy;
        if (!AimAssist::hero(hx, hy))
        {
            return false;
        }
        if (g_target && AimAssist::locate(g_target, AimAssist::Kind::Enemy, t))
        {
            // Keep chasing it up to half again the range, then look for a closer one.
            const float dx = t.x - hx, dy = t.y - hy;
            const float reach = range * 1.5f;
            if (dx * dx + dy * dy <= reach * reach)
            {
                return true;
            }
        }
        return AimAssist::find(AimAssist::Kind::Enemy, 0.0f, 0.0f, range, 360.0f, t);
    }

    bool acceptLoot(uint32_t id, void*)
    {
        if (lootBlocked(id, GetTickCount()) || g_fixtures.count(id))
        {
            return false;
        }
        cObjectManager* objects = cObjectManager::instance();
        cObject* object = objects ? objects->object(id) : nullptr;
        return object && lootOf(object) != Loot::None;
    }

    bool isCurrent(uint32_t id, void*)
    {
        return id == g_target;
    }

    // The loot we go for while it is there, else the nearest within range.
    bool chooseLoot(AimAssist::Target& t)
    {
        const float range = static_cast<float>(Config::autoAttack.lootRange);
        if (g_looting && g_target && AimAssist::findObject(range * 1.5f, &isCurrent, nullptr, t))
        {
            return true;
        }
        return AimAssist::findObject(range, &acceptLoot, nullptr, t);
    }

    // Whether the hero is done with the loot it goes for: it gives up after a while, or after standing at it (an
    // opened container stays, and so does an item when the inventory is full).
    bool lootDone(uint32_t id, DWORD now)
    {
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        cObject* loot = objects ? objects->object(id) : nullptr;
        if (!hero || !loot)
        {
            return true;
        }
        const float dx = static_cast<float>(loot->x - hero->x), dy = static_cast<float>(loot->y - hero->y);
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
        cObject* target = objects->object(id);
        if (!self || !target)
        {
            return false;
        }
        UseOrder order;
        order.target = id;
        order.x = target->x;
        order.y = target->y;
        const auto handler = reinterpret_cast<const OrderHandler*>(self->vtable)[6];
        handler(self, nullptr, &order);
        return true;
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

    // The toggle key, on its press and only while the game has the focus.
    const bool keyDown = foreground && physicallyDown(Config::autoAttack.key);
    if (keyDown && !g_keyWasDown)
    {
        g_on = !g_on;
        LOG("AutoAttack: {}", g_on ? "on" : "off");
        MessageBeep(g_on ? MB_ICONASTERISK : MB_ICONEXCLAMATION);
    }
    g_keyWasDown = keyDown;

    if (foreground && playerClicks())
    {
        g_quietUntil = now + static_cast<DWORD>(Config::autoAttack.pauseMs);
        g_target = 0;
    }
    // A dialog opened while the hero went for loot, the player leaving the mouse alone: the loot was a fixture the
    // flags did not give away, and the dialog its text. Close it and never go there again.
    if (g_on && foreground && g_looting && g_target && static_cast<int>(now - g_quietUntil) >= 0 &&
        UiNav::npcDialogOpen())
    {
        LOG("AutoAttack: loot {:x} opened a dialog, closed and left", g_target);
        g_fixtures.insert(g_target);
        Inject::key(VK_RETURN, true);
        Inject::key(VK_RETURN, false);
        g_target = 0;
        g_looting = false;
        return;
    }
    if (!g_on || !foreground || Controller::drives() || !Controller::worldRunning() || !heroAlive() ||
        static_cast<int>(now - g_quietUntil) < 0)
    {
        g_target = 0;
        return;
    }

    // Loot on the ground first, always; then the nearest enemy within Range.
    AimAssist::Target t;
    bool looting = false;
    if (Config::autoAttack.pickup)
    {
        if (g_looting && g_target && lootDone(g_target, now))
        {
            lootFailed(g_target, now);
            g_target = 0;
        }
        looting = chooseLoot(t);
    }
    if (!looting && !chooseTarget(t, static_cast<float>(Config::autoAttack.range)))
    {
        g_target = 0;
        return;
    }
    if (t.id == g_target && looting == g_looting)
    {
        // Enemies: the order again every Repeat ms. Loot: only when the hero stalls.
        const int repeat = Config::autoAttack.repeatMs;
        const int sinceSent = static_cast<int>(now - g_sentAt);
        // At the loot, the pick up may take a moment: once more only after twice the stall time.
        const bool lootAgain = g_lootAtSince ? sinceSent > static_cast<int>(2 * kLootNudgeMs)
                                             : static_cast<int>(now - g_lootBestAt) > static_cast<int>(kLootNudgeMs) &&
                                                   sinceSent > static_cast<int>(kLootNudgeMs);
        const bool again = looting ? lootAgain : repeat > 0 && sinceSent >= repeat;
        if (!again)
        {
            return;
        }
    }
    if (attack(t.id))
    {
        if (t.id != g_target || looting != g_looting)
        {
            LOG("AutoAttack: {} {:x}", looting ? "loot" : "target", t.id);
            g_lootAtSince = 0;
            g_lootBestAt = now;
            g_lootBest = 1e9f;
        }
        g_target = t.id;
        g_looting = looting;
        g_sentAt = now;
    }
}

#pragma once

#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

// What the combat helpers (AutoPotion, the auto attack's Tactics) tell each other. Header only, the same in each mod
// branch, so that every branch builds on its own and merges without conflict.
namespace CombatState
{
    // When AutoPotion last drank a healing potion (tick), 0 for never: health regained in the next moments is the
    // potion's, not a combat art's.
    inline std::atomic<DWORD> potionAt{0};
    // A combat art's effect is measured until this tick: AutoPotion holds back drinks that are not urgent, which would
    // pass for the art's healing.
    inline std::atomic<DWORD> measuringUntil{0};
    // When AutoPotion last found no healing potion left (the game's usePotion drank none), 0 while it has some.
    inline std::atomic<DWORD> noPotionsAt{0};

    // A creature's action, the low byte of cCreature +0x150 (seen live, 2026-11-08): 0 standing, 1 walking, 3
    // fighting, 6 dead (the game's isEnemy reads it), 9 casting a combat art, from the moment the art goes out until it
    // is done (2.5-4 s for an art of several blows), 4 casting a spell (about 1.5 s). A cast the game refuses (the art
    // still recharging) never gets 9 or 4.
    constexpr size_t kAction = 0x150;
    enum Action : uint8_t
    {
        Standing = 0,
        Walking = 1,
        Fighting = 3,
        CastingSpell = 4,
        Dead = 6,
        CastingArt = 9,
    };

    inline uint8_t action(const void* creature)
    {
        return creature ? *(reinterpret_cast<const uint8_t*>(creature) + kAction) : 0;
    }

    // Casting a combat art of either kind (a weapon art or a spell).
    inline bool casting(const void* creature)
    {
        const uint8_t a = action(creature);
        return a == CastingArt || a == CastingSpell;
    }

    // Whether tick `at` (0: never) lies less than `ms` before `now`.
    inline bool within(DWORD at, DWORD now, DWORD ms)
    {
        return at != 0 && now - at < ms;
    }
}

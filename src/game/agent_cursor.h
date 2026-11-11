#pragma once
#include <windows.h>
#include <cstdint>

// The agent acts through the game's own cursor ([AutoAttack] HumanCursor; the player, 2026-11-11: «que el agente use
// el cursor del juego para hacer las acciones … algo similar al comportamiento humano», «tiene que jugar como un
// proplayer»). The game reads this cursor in place of the mouse's (getClientCursorPos), draws it and highlights what
// lies under it; the system mouse is never moved. Each order of the agent's (attack, use, pick up, walk, combat art)
// waits until the cursor is on its target, and the cursor gets there as a quick player's hand does: a short reaction
// to a new target, a flick whose time grows with the log of the distance (Fitts), on a slightly curved, minimum-jerk
// path, now and then overshooting a far target and correcting, then tracking a moving one. A target off the screen
// is aimed at the screen's edge in its direction, as a player clicks there. Never more than 1.5 s held: the order
// goes out then wherever the cursor is.
namespace AgentCursor
{
    // What the cursor goes to: an object (followed as it moves) or, with id 0 or off the screen, a world point.
    struct Aim
    {
        uint32_t id = 0;
        int32_t x = 0, y = 0;
    };

    // The option is on and the agent plays in the game world (not the controller).
    bool active();
    // The agent has the cursor: the game must not warp the system cursor meanwhile (any thread).
    bool owns();
    // The cursor the game reads (client pixels); false: the mouse's own.
    bool cursor(POINT& client);

    // Aims the cursor at `aim`; true once it is on it (always with the option off, or 1.5 s after the aim was given).
    bool reach(const Aim& aim);
    // Calls fn(context) on the presenting thread once the cursor is on `aim` (at once if it is, or with the option
    // off). A newer act replaces one still waiting, as a player's next decision replaces the last.
    void act(const Aim& aim, void (*fn)(void*), void* context);

    // Once per presented frame, on the presenting thread, before the agent's own frame.
    void onFrame();
}

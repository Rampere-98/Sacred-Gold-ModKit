#pragma once

// The laboratory's cheats (the player, 2026-11-14: «implementa al ModKit todas las trampas posibles separadas por
// categoría»). They are conditions of a test, never the agent's own doing: the agent has no permission for them and
// none of its logic asks for one. They exist only in the laboratory copies: SacredA4 and SacredA5 (the coordinator's,
// set in their ini for a test) and SacredA7 and SacredA8 (the player's own choice, in their menu). In the player's
// folder and in every other copy there is no menu, no key is read and nothing here acts.
// [Trampas] in SacredBild.ini, a key a cheat; each one on is told in the log at the start and when it changes.
namespace Cheats
{
    enum class Id
    {
        // Economy
        GoldNoEnd,          // the gold topped up to a million by the game's own addGold
        // Character
        HealthNoEnd,        // the hero's health back to full every frame (experimental: a plain write)
        Count
    };

    // True only in a laboratory copy.
    bool available();
    // True when the cheat is on (never outside a laboratory copy).
    bool on(Id id);
    // Once a presented frame (any thread): what is on is applied on the engine's thread.
    void onFrame();
}

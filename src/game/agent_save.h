#pragma once

#include <ctime>

// The agents' saved games (the player, 2026-11-11): each copy SacredA<n> keeps its game in its own slot,
// GAME<9+n>.PAK (GAME10..GAME17), by the game's own save; saved when a quest is done, never after the hero died since
// the last save; a hero that dies loads its last save; the copy's own game loaded once it starts. Never anywhere but
// those copies (the player's own game folder and its GAME01..05 are left alone).
namespace AgentSave
{
    // Each frame, with the agent.
    void onFrame();
    // A quest done (the journal's count went up): a save is due once the game allows it.
    void questDone();
    // In the start menu: the copy's saved game loaded straight from there, as the menu's Load does (Manolito, block S
    // 2b); false with none (a new game then). Asked once a start.
    bool loadFromMenu();
    // In the start menu: a new campaign game with the class `gameClass` (the game's numbers: 1 Seraphim .. 9 Daemon) on
    // Bronze, begun as the hero selection's Start does (Manolito, block S 10), with no clicks; false if it cannot.
    bool newGameFromMenu(int gameClass);
    // A load asked for lately (30 s) or under way: the world's sectors are being torn down and rebuilt, nothing of them
    // to be read from other threads (the live verifier, 2026-11-11: two crashes in ClickWalk::walkable right after a
    // load).
    bool worldChanging();
    // Deaths since this copy's slot was last written: the same game loaded again and again (kept across the loads).
    int deathsSinceSave();

    // Where the hero died (tiles) within the last 30 min, kept on disk across the copy's restarts.
    struct DeathSpot
    {
        int x, y;
        std::time_t at;
    };
    void noteDeathSpot(int x, int y);
    bool nearDeathSpot(int x, int y);   // within 20 tiles of one
}

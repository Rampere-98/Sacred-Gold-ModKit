#pragma once

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
}

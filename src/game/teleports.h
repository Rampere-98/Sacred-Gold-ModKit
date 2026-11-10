#pragma once
#include <cstdint>
#include <vector>

// The game's ways in and out of interiors (Manolito, blocks L, AGENT_ADDRESSES «Interiores y llaves»): the script's
// teleport places in bin\TYPE_NPC_<class>\DefPos.bin (records of 100 bytes: a name, then x and y in tiles at +0x40 and
// +0x44): tpstart_<X> is the tile that jumps to tptarget_<X>; a house's or cave's interior pairs tpreinz<X> (where
// the hero lands inside) with tprausz<X> (where it lands outside again). The entries the hero walks onto are the
// anchors of the stairs tree [0xAB4498] (an AVL of 0x18-byte nodes: key the tile clicked, val the anchor walked to,
// both x | y << 13 | layer << 26).
namespace Teleports
{
    struct Interior
    {
        int inX = 0, inY = 0;       // landing inside (the way out is beside it)
        int outX = 0, outY = 0;     // landing outside (the way in is beside it)
        bool start = false;         // a tpstart/tptarget pair: inX/inY the target, outX/outY the start tile itself
    };

    // Read once (the first call); empty if the file is not there.
    const std::vector<Interior>& interiors();

    // The stairs tree's anchors on `layer` within `radius` tiles of (tx, ty): where the hero walks to go through.
    std::vector<std::pair<int, int>> anchorsNear(int layer, int tx, int ty, int radius);

    // World units per tile (the hero's x and y at +0x1C/+0x20, its layer at +0x24).
    constexpr double kWorldPerTile = 53.66563;

    // The controlled hero's tile and layer; false with no hero.
    bool heroAt(int& tx, int& ty, int& layer);

    // The player's own teleport (the ModKit menu, never the agent: the player, 2026-11-10: «no quiero que el agente se
    // teleporte sin recursos proporcionados del propio juego, pero me gustaría tener teleport como opción cuando yo
    // vaya a jugar»). Writes the hero's position and layer.
    bool playerTeleport(int tx, int ty, int layer);

    // The player's Alt+click on the world map (M): the hero to the tile clicked (layer 0), never with the agent on.
    // Call inside a Patch transaction.
    void install();

    // Each frame: a teleport onto ground not loaded then is set onto the nearest walkable tile once it loads.
    void onFrame();
}

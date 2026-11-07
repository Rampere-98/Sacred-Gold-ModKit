#pragma once
#include <cstdint>

namespace Sacred
{
    struct cWorldView;
    struct Sector;
}

// [Render] WideSectors: 5x5 map sectors around the camera instead of the game's 3x3. The view keeps the 3x3 it loads
// (cWorldView::sectors, from its center sector's neighbor ids) for everything else; here the ring around it is loaded
// through the map data's sector cache as well (cMapData::getSector, ENG 00634340), and the tile row walk
// (renderTileRow, ENG 0062AE90) and the water draw (drawWaterTiles) are patched to look sectors up in a 25-entry
// table: their reads of the view's array become absolute reads of the table, the "< 9" checks "< 25", and the
// sector steps at a sector edge those of a 5-wide grid (one row up: -5 instead of -3; up and right: -4 instead of
// -2). Rows then carry 5x5 indices (Resolution's row walk computes them), and the water list stores them too.
// Ring sectors get the sector's keep flag from update() until the frame is drawn (release()), so a full sector cache
// does not unload them under the walk, and only then.
namespace WideSectors
{
    constexpr int kSide = 5;
    constexpr int kCount = kSide * kSide;
    constexpr int kTiles = kSide * 64;      // tiles per side of the 5x5
    constexpr int kInner = 64;              // tile row/column of the 5x5 where the game's 3x3 starts

    // Resolves and patches the game code; the game's own 3x3 walk stays as it is if anything does not match.
    void install();
    bool active();

    // Once per frame, on the render thread, before the row walk: the game's 3x3 in the middle, the ring around it.
    void update(Sacred::cWorldView* view);

    // After the frame is drawn (the flip): lets go of the ring sectors' keep flag, so the game's sector cache may
    // unload them between frames as it would without the ring. The game puts a part of the map's doors and other
    // objects back only when it loads their sector again: a ring that stayed loaded kept the doors of a place left
    // by two sectors parked (gone) when the hero came back. update() takes them again (loading them if they went).
    void release();

    // The sector at 5x5 index `slot` (row * 5 + column), nullptr if none is loaded there.
    Sacred::Sector* at(int slot);

    // The game's 3x3 index (row * 3 + column) as a 5x5 one.
    constexpr int fromGame(int slot) { return (slot / 3 + 1) * kSide + slot % 3 + 1; }
}

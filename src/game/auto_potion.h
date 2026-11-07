#pragma once

// Healing potions drunk by themselves ([AutoPotion]): once the hero misses as much health as a potion heals, the
// game's own healing potion key (Space, the taskbar's first potion button) is pressed. How much a potion heals is
// learned from the potions drunk: the health gained right after one. Until the first is measured, it drinks at
// [AutoPotion] FirstPercent missing; at EmergencyPercent health or less it always drinks. Only in the running game
// world, with the game in front, never while the chat line is open (Space would type) or a dialog or popup is.
namespace AutoPotion
{
    // Once per presented frame, on the presenting thread.
    void onFrame();
}

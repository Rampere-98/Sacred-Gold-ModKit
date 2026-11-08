#pragma once

// Healing potions drunk by themselves ([AutoPotion]): once the hero misses as much health as a potion heals, a
// healing potion is drunk through the game's own usePotion (what Space, the taskbar's first potion button, calls),
// which also says whether one was left. How much a potion heals is learned from the potions drunk: the health gained
// right after one (a gain cut by blows meanwhile only raises it). Until the first is measured, it drinks at
// [AutoPotion] FirstPercent missing; at EmergencyPercent health or less it always drinks, and with none left it looks
// again every few seconds while in danger. While the auto attack measures a combat art's effect it holds back drinks
// that are not urgent (they would pass for the art's healing). Only in the running game world, with the game in
// front, never while the chat line is open or a dialog or popup is.
namespace AutoPotion
{
    // Once per presented frame, on the presenting thread.
    void onFrame();
}

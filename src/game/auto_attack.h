#pragma once

// Auto attack for keyboard and mouse play ([AutoAttack]): a key toggles it; while it is on, the hero attacks the enemy
// nearest to it within a range (AimAssist finds it on the screen) and takes the next one once it dies. While nothing
// hurts the hero it also picks up the items around (gold too) and opens chests and other containers ([AutoAttack]
// Pickup), whichever of loot and enemy is nearer first; while it loses health the enemies around come first. It gives the
// hero the order a click on the enemy gives, directly, so the mouse and the game's cursor stay the player's: the
// inventory and other windows can be used meanwhile. A mouse button hands control back at once, for [AutoAttack]
// Pause ms. It only acts in the running game world (no menus, dialogs or popups) and never while the controller
// drives the game.
namespace AutoAttack
{
    // Hooks it needs; call inside a Patch transaction.
    void install();

    // Once per presented frame, on the presenting thread.
    void onFrame();
}

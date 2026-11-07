#pragma once

// Auto attack for keyboard and mouse play ([AutoAttack]): a key toggles it; while it is on and the player leaves the
// mouse alone, the hero attacks the enemy nearest to it within a range, as if the player held the left button on
// that enemy (AimAssist picks it, Inject holds the button, the game's cursor points at it), and takes the next one
// once it dies. Any mouse button or a moved mouse hands control back at once, for [AutoAttack] Pause ms. It only
// acts in the game world (no menus, dialogs or open windows) and never while the controller drives the game.
namespace AutoAttack
{
    // Hooks it needs; call inside a Patch transaction.
    void install();

    // Once per presented frame, on the presenting thread.
    void onFrame();
}

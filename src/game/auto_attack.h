#pragma once

// Auto attack for keyboard and mouse play ([AutoAttack]): a key toggles it; while it is on, the hero fights the
// enemies within a range (AimAssist finds them on the screen), picks up the items around (gold too) and opens chests
// and other containers ([AutoAttack] Pickup); signposts and other fixtures are left alone. What comes first is weighed
// as a player would: enemies close by, closing in or hurting the hero before loot (but for what lies at its feet);
// with nothing threatening, whichever is nearer. It looks around every few hundred ms and acts on a new aim after a
// short, uneven reaction time, keeping its aim unless another is clearly better. It gives the
// hero the order a click on the target gives, directly, so the mouse and the game's cursor stay the player's: the
// inventory and other windows can be used meanwhile. A mouse button hands control back at once, for [AutoAttack]
// Pause ms. It only acts in the running game world (no menus, dialogs or popups) and never while the controller
// drives the game.
namespace AutoAttack
{
    // Hooks it needs; call inside a Patch transaction.
    void install();

    // Once per presented frame, on the presenting thread.
    void onFrame();

    // The agent plays (F11 / the menu's «Activo ahora»).
    bool on();

    // Tiles from the hero to the nearest living hostile creature on the surface (read with the quest marks, every
    // few seconds; 99 with none within 40 tiles).
    double nearestHostileTiles();

    // The hero attacked an enemy within the given ms (read from any thread).
    bool foughtWithin(unsigned long ms);

    // The hero kept on foot (off the horse) for the given ms: a save waits for it.
    void footFor(unsigned long ms);
}

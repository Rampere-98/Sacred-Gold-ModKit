#pragma once

// The agent's single gate (the player, 2026-11-14: «no quiero que el agente mueva un dedo sin permiso de acción»).
// Every kind of thing the agent does on the game has a permission of its own: No, Suggest (told in the menu and
// done only once the player says so) or Auto. The parts ask allowed() right before acting; nothing acts on the
// game without asking. In the player's own folder everything starts at No; a training copy (SacredA<n>) starts at
// Auto. [Permisos] in SacredBild.ini, one key a kind. A file SacredBild-stop beside the exe turns everything off.
namespace Act
{
    enum class Kind
    {
        Walk,           // walking: exploring, travelling to a quest, routes
        Attack,         // choosing an enemy and attacking it
        Arts,           // casting combat arts and combos
        ArtBar,         // putting arts into the bar's slots
        LifePotion,     // the potion of life (AutoPotion)
        OtherPotions,   // antidote, mentor, concentration, undead death, the hirelings' healing
        PickUp,         // loot from the ground
        Chests,         // chests, hiding places, gravestones
        Doors,          // opening doors and gates
        Talk,           // speaking to characters, answering their dialogs
        Mount,          // mounting, a wild horse too
        Dismount,
        CallHorse,
        Equip,          // putting a better item on
        WeaponSlots,    // filling and switching the weapon slots
        Sell,
        Buy,            // potions and better items at a merchant
        BuyHorse,
        Smith,          // socketing at the smith
        Combos,         // the combo master: combos and rune swaps
        Runes,          // reading runes
        Scrolls,        // reading scrolls and books
        Attributes,     // spending attribute points
        Skills,         // spending skill points, learning new skills
        SaveLoad,       // saving and loading the game
        Windows,        // closing windows (inventory, shop, portal box)
        Retreat,        // stepping back, dodging, fleeing
        Count
    };

    enum class Mode : unsigned char { No, Suggest, Auto };

    // True when the agent may do this now. `why` (a short English phrase, may be null) is what it wants it for:
    // shown as the suggestion in Suggest mode and written to SacredBild-act.jsonl. Any thread.
    bool allowed(Kind kind, const char* why = nullptr);

    // The same without asking: true on Auto (or while a «do it» of the player's lasts). For the places that only look
    // for something to do; the suggestion is made where the thing is about to be done.
    bool automatic(Kind kind);

    // The same without asking: true on Auto (or while a «do it» of the player's lasts). For the places that only look
    // for something to do; the suggestion is made where the thing is about to be done.
    bool automatic(Kind kind);

    // The same without asking: true on Auto (or while a «do it» of the player's lasts). For the places that only look
    // for something to do; the suggestion is made where the thing is about to be done.
    bool automatic(Kind kind);

    // True when nothing at all is allowed (the stop file, or every kind at No): the parts may skip their work.
    bool nothing();

    Mode mode(Kind kind);
    const char* name(Kind kind);    // the ini key, English

    // Once, early (reads the ini, adds the menu section).
    void install();
}

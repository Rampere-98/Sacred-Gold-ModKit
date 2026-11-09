#pragma once
#include <cstdint>
#include <string>

// What the agent reads (the player's wish, 2026-11-09: "that the agent reads and learns from what it is told"): the
// game's own texts, from its text table (global.res, the language the game runs in). The quest it goes for, its
// title and objective (journal record +0x24 / +0x28, text keys) and from the objective what kind of quest it is; and
// every NPC dialog or help window it answers. All of it kept in SacredBild-read.txt and shown in the ModKit menu.
namespace Lore
{
    enum class QuestKind
    {
        Unknown,
        Talk,       // speak to someone, report back
        Kill,       // kill a creature or a number of them
        Fetch,      // bring, find, collect something
        Escort,     // escort or lead someone: the hero keeps them close
        Go,         // go somewhere
    };

    // A text of the game's by its key (a bit-31 key is a global.res id, as the game reads them); "" if none.
    std::wstring text(uint32_t key);
    std::string textUtf8(uint32_t key);

    // The quest the agent goes for: read once per quest (title, objective), its kind told and kept.
    QuestKind readQuest(uint32_t titleKey, uint32_t objectiveKey);
    QuestKind questKind();
    // How many the objective asks for ("Mata a diez...", "Kill 3..."); 0 if it says none.
    int questCount();

    // An NPC dialog or help window open now: its text, once per text.
    void readDialog(const std::wstring& text);

    const char* kindName(QuestKind kind);

    // The ModKit menu: what was read last.
    void drawMenu();
}

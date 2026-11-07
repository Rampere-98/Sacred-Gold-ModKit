#pragma once
#include <string>

// The ModKit's settings menu: Home (or [ModKit] MenuKey) shows and hides it over the game (Overlay, Dear ImGui), in
// game and in the menus alike. Every mod adds a section of its own; what it changes applies at once, and Save writes
// it to SacredBild.ini so the next start keeps it. Needs [DDraw] Backend=d3d9 (the overlay's backend).
namespace ModkitMenu
{
    struct Section
    {
        const char* title;                          // UTF-8
        void (*draw)();                             // ImGui widgets editing the mod's settings (Config::...)
        void (*save)(const std::wstring& ini);      // writes them with WritePrivateProfileStringW
    };

    // Adds a section; call once, from the mod's install().
    void add(const Section& section);

    // Once per presented frame, on the presenting thread: the key.
    void onFrame();

    // For sections: write an int or a bool to the ini.
    void write(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int value);
}

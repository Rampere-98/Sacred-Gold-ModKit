#pragma once
#include <cwchar>

// [Tactics] in SacredBild.ini (read by config.cpp): combat arts and consumables used with the auto attack.
namespace Config
{
    struct Tactics
    {
        // What a combat art slot (6-0) is used as. Auto learns it from what the art does.
        enum class Art : int
        {
            Auto,
            Heal,       // heals the hero: only when it misses health
            Area,       // hits the enemies around the target: only with several
            Single,     // one strong blow: only at enemies worth it
            Buff,       // a lasting effect on the hero: at the start of a hard fight
            Off,
        };
        static constexpr int slotCount = 5;

        bool arts = true;               // combat arts
        Art slots[slotCount] = {};
        bool consumables = true;        // the potions other than healing ones (those are [AutoPotion]'s)
        bool undeadDeath = true;        // Q
        bool mentor = true;             // W
        bool antidote = true;           // E
        bool concentration = true;      // R
        bool healHirelings = true;      // B: a healing potion to the hirelings
    };
    inline Tactics tactics;

    // The ini spelling of an art slot's use.
    inline const wchar_t* artKey(Tactics::Art art)
    {
        constexpr const wchar_t* keys[] = {L"auto", L"heal", L"area", L"single", L"buff", L"off"};
        const int i = static_cast<int>(art);
        return i >= 0 && i < 6 ? keys[i] : keys[0];
    }

    inline Tactics::Art parseArt(const wchar_t* text)
    {
        for (int i = 0; i < 6; ++i)
        {
            if (_wcsicmp(text, artKey(static_cast<Tactics::Art>(i))) == 0)
            {
                return static_cast<Tactics::Art>(i);
            }
        }
        return Tactics::Art::Auto;
    }
}

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
        // Single-target arts at any enemy worth more than a few blows, not only at strong ones (a caster's main
        // attack); off: only at strong enemies or when hard pressed.
        bool singleAlways = true;
        int artRange = 260;             // single and area arts only at targets within this many screen pixels (0: any)
        int buffEverySeconds = 45;      // a buff again after this long at the earliest
        // Low on health with no healing potion left and enemies close: step back from them instead of fighting on.
        bool retreat = true;
        int retreatPercent = 25;        // ... at or below this percent of health
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

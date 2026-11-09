#pragma once
#include "fmt.h"

#include <string>
#include <type_traits>

// The agent's notebook (the player's wish, 2026-11-09: the agent writes down what it finds out). Each finding one
// line in SacredBild-findings.txt next to the game: when, the character (class, level), where (layer, tile), what kind
// and what was seen, with the evidence (ids, distances, times) so each one can be checked against SacredBild.log. The
// same kind at the same tile is written once in 10 minutes.
namespace Findings
{
    // kind: a short word (stuck, shooter, unseen-hit, loot-retry, quest, unreachable...). x, y: world units.
    void write(const char* kind, int x, int y, const std::string& text);

    template <class... Args>
    void note(const char* kind, int x, int y, std::format_string<std::decay_t<Args>...> fmt, Args&&... args)
    {
        write(kind, x, y, Fmt::vformat(fmt.get(), std::make_format_args(args...)));
    }
}

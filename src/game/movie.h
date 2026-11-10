#pragma once

// Intro and cutscene movies (WMV). The game plays them with DirectShow's multimedia streams (amstream), which decode
// into DirectDraw surfaces and only work on a real DirectDraw; its loop also stops processing window messages.
// With [DDraw] MediaFoundation (always with Backend=d3d9) they play through Media Foundation instead: video into a
// texture drawn over the whole screen (aspect kept), audio to the default device, and the window stays responsive.
namespace Movie
{
    // Hooks the game's movie functions; call inside a Patch transaction.
    void install();

    // While the game's own movie loop plays with the agent on: the state its key poll reads (Escape down once, then
    // all up), true if it is one of them.
    bool agentKey(int key, short& state);
}

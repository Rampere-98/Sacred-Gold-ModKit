#pragma once
#include <windows.h>

// Freezes (the player's report, 2026-11-09: "the map freezes", "it freezes a while at the start of each game"): a
// watcher thread notices when the presenting thread has not presented for a while and samples where it is (its
// instruction pointer and the return addresses on its stack into Sacred.exe, SacredBild and granny.dll), so the log
// names the code a freeze spends its time in.
namespace HitchWatch
{
    void install();
    // Every presented frame, from the presenting thread.
    void onFrame(DWORD thread);
}

#pragma once

// GrannyAdvanceTime (granny.dll, animation library) advances every loaded animation once per frame at the start
// of the render loop. It runs on a worker thread instead, overlapping whatever the game does next; every other
// Granny call, from any thread, first waits for it to finish. Granny work keeps its order and never overlaps.
namespace GrannyAsync
{
    // Patches the game's granny.dll imports; call once from DllMain (not inside a Detours transaction).
    void install();

    // Once per presented frame: logs where the game had to wait for the worker, every few seconds.
    void onFrame();

    // Detours cGrannyModelManager::getModel (ENG 00411F50) so a model is loaded only with the worker idle (Manolito,
    // block AI: a weapon switched mid-fight loads its .gr2 and textures, read straight from the file's memory, not
    // through a Granny import; 5 crashes in sprintf over a garbage texture name). Call inside a Detours transaction.
    void installModelGuard();
}

#pragma once
#include <windows.h>

// Keyboard and mouse input handed to the game as if the player had typed or clicked: key and button messages reach
// the game's window procedure on the window's own thread, and its polls (GetAsyncKeyState, GetKeyboardState) see the
// keys and buttons held. The messages travel as private WM_APP messages that Focus's subclass of the window unwraps
// and hands to the game's procedure, so the player's own input never looks like them (InputMode).
namespace Inject
{
    void setWindow(HWND window);

    // WM_KEYDOWN / WM_KEYUP, with WM_CHAR for the key's character as TranslateMessage would; and the held state.
    void key(int vk, bool down);
    // The held state only, what GetAsyncKeyState / GetKeyboardState report (for Alt, whose message would open the
    // window menu).
    void hold(int vk, bool down);
    // WM_xBUTTONDOWN / UP for VK_LBUTTON, VK_RBUTTON or VK_MBUTTON (the game reads the position through
    // getClientCursorPos), and the held state. `doubleClick`: WM_xBUTTONDBLCLK instead of the down message, what
    // Windows sends for the second click of a double click (the game's window class asks for them).
    void button(int vk, bool down, bool doubleClick = false);
    void wheel(int delta);
    // WM_MOUSEMOVE: the game moves its cursor on mouse messages only.
    void mouseMove();
    // Calls fn(context) on the window's thread once the messages posted before it are handled.
    void call(void (*fn)(void*), void* context);
    // Calls fn(context) on the game's engine thread, between two of its frames (Manolito, block AX-3, 2026-11-14:
    // the window's thread only pumps messages; the logic, Granny, the equipment and the UI live on the thread
    // cEngine's constructor starts, its id at [0x17E7D40]. A call made from the window's thread into them raced
    // the engine: the path finder's global grid, A2's crash at 4FA524; the models' queues). Queued here and run by
    // drainEngine().
    void engineCall(void (*fn)(void*), void* context);
    // The same, in its turn among the keys and mouse messages posted before it (an order after the click it follows):
    // through the window's queue first, then the engine's.
    void engineCallOrdered(void (*fn)(void*), void* context);
    // From the frame hook: runs what is queued, only when the caller is the engine thread.
    void drainEngine();
    // Everything held goes up.
    void releaseAll();

    bool held(int vk);
    // Adds the held keys to a GetKeyboardState array.
    void merge(BYTE* keys);

    // For Focus's window procedure: if `message` is an injected one, hands it to `game` and returns true.
    bool unwrap(HWND window, UINT message, WPARAM wParam, LPARAM lParam, WNDPROC game, LRESULT& result);
}

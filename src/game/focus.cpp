#include "game/focus.h"
#include "game/agent_cursor.h"
#include "game/controller.h"
#include "game/sacred_addr.h"
#include "input/inject.h"
#include "input/input_mode.h"
#include "overlay/overlay.h"
#include "config/auto_attack.h"
#include "game/movie.h"
#include "config/display.h"
#include "log.h"
#include "patch.h"
#include "sig.h"

#include <atomic>
#include <mutex>

namespace
{
    using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
    using GetKeyboardStateFn = BOOL(WINAPI*)(PBYTE);
    using SetCursorPosFn = BOOL(WINAPI*)(int, int);
    using SetWindowsHookExAFn = HHOOK(WINAPI*)(int, HOOKPROC, HINSTANCE, DWORD);

    GetAsyncKeyStateFn g_origGetAsyncKeyState = nullptr;
    GetKeyboardStateFn g_origGetKeyboardState = nullptr;
    SetCursorPosFn g_origSetCursorPos = nullptr;
    SetWindowsHookExAFn g_origSetWindowsHookExA = nullptr;
    HOOKPROC g_gameKeyboardHook = nullptr;

    HWND g_window = nullptr;
    std::atomic<WNDPROC> g_origWindowProc{nullptr};
    std::mutex g_clipMutex;
    bool g_clipped = false;
    RECT g_target = {};     // the client area clipped to
    RECT g_clip = {};       // what Windows made of it

    // One of the game's windows (the main window, a dialog, a message box) is in the foreground.
    bool active()
    {
        DWORD process = 0;
        HWND foreground = GetForegroundWindow();
        return foreground && GetWindowThreadProcessId(foreground, &process) && process == GetCurrentProcessId();
    }

    // Keys and buttons the controller holds (Inject) count as pressed; while SacredBild's own screen is open the game
    // sees nothing pressed.
    SHORT WINAPI hookGetAsyncKeyState(int key)
    {
        const SHORT state = g_origGetAsyncKeyState(key);   // also consumes the "pressed since last call" bit
        if (short agent = 0; Movie::agentKey(key, agent))
        {
            return agent;
        }
        if (!active() || Overlay::isOpen())
        {
            return 0;
        }
        return Inject::held(key) ? static_cast<SHORT>(state | 0x8000) : state;
    }

    // The window procedure reads it for each key message (ToAscii).
    BOOL WINAPI hookGetKeyboardState(PBYTE keys)
    {
        const BOOL ok = g_origGetKeyboardState(keys);
        if (ok && keys)
        {
            Inject::merge(keys);
        }
        return ok;
    }

    // Not while the controller has the cursor: the game warps it to keep it on its world spot as the view scrolls.
    // Nor while the agent plays (the player, 2026-11-10: «no quiero que mi ratón sea usado por el agente, tiene que
    // funcionar de forma independiente»): the game never moves the real mouse then; its clicks go in-process.
    BOOL WINAPI hookSetCursorPos(int x, int y)
    {
        return active() && !Controller::ownsCursor() && !AgentCursor::owns() && !Config::autoAttack.enabled ? g_origSetCursorPos(x, y) : TRUE;
    }

    // The player's own input (never the controller's injected messages): who is in charge, the controller or the
    // keyboard and mouse.
    void noteInput(UINT message, WPARAM wParam)
    {
        switch (message)
        {
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            // Windows can turn an Xbox pad's input into gamepad keys (VK_GAMEPAD_A 0xC3 .. 0xDA) for the focused
            // window: that is the controller, not the keyboard.
            if (wParam < 0xC3 || wParam > 0xDA)
            {
                InputMode::keyboardUsed(message, wParam);
            }
            break;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN: case WM_MOUSEWHEEL:
            InputMode::keyboardUsed(message, wParam);
            break;
        case WM_MOUSEMOVE:
            InputMode::mouseMoved();
            break;
        default:
            break;
        }
    }

    // The game's hook swallows the Windows keys, Ctrl+Esc, Alt+Tab and Alt+Esc. Only in the foreground, and Alt+Tab /
    // Alt+Esc not at all: with the cursor confined to the window they are the way out, and the windowed Direct3D 9
    // device doesn't mind losing the foreground the way the original fullscreen mode did.
    LRESULT CALLBACK keyboardHook(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION)
        {
            const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
            const bool altSwitch = (key->vkCode == VK_TAB || key->vkCode == VK_ESCAPE) && (key->flags & LLKHF_ALTDOWN);
            if (altSwitch || !active())
            {
                return CallNextHookEx(nullptr, code, wParam, lParam);
            }
        }
        return g_gameKeyboardHook(code, wParam, lParam);
    }

    HHOOK WINAPI hookSetWindowsHookExA(int id, HOOKPROC proc, HINSTANCE module, DWORD thread)
    {
        if (id == WH_KEYBOARD_LL && proc)
        {
            g_gameKeyboardHook = proc;
            proc = &keyboardHook;
        }
        return g_origSetWindowsHookExA(id, proc, module, thread);
    }

    void releaseLocked()
    {
        if (g_clipped)
        {
            ClipCursor(nullptr);
            g_clipped = false;
        }
    }

    void release()
    {
        std::scoped_lock lock(g_clipMutex);
        releaseLocked();
    }

    // Releases the cursor right away when the window loses the foreground, is moved, or a menu opens; onFrame
    // confines it again. The close button and the system menu's Close don't quit (too easy to hit by accident);
    // Alt+F4 still does.
    LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        LRESULT injected = 0;
        if (Inject::unwrap(window, message, wParam, lParam, g_origWindowProc.load(), injected))
        {
            return injected;
        }
        noteInput(message, wParam);
        if ((message == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE) || (message == WM_ACTIVATEAPP && !wParam) ||
            message == WM_ENTERSIZEMOVE || message == WM_ENTERMENULOOP || message == WM_DESTROY)
        {
            release();
        }
        if (message == WM_SYSKEYDOWN && wParam == VK_F4)
        {
            PostMessageA(window, WM_CLOSE, 0, 0);   // what DefWindowProc would do, but Close is disabled
            return 0;
        }
        if (message == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_CLOSE && HIWORD(lParam) != 0xFFFF)
        {
            return 0;   // by mouse (HIWORD -1 = by accelerator)
        }
        if (message == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_KEYMENU && lParam == 0)
        {
            return 0;   // Alt alone (the game shows item names with it) doesn't enter the menu; Alt+Space still opens it
        }
        if (Overlay::message(window, message, wParam, lParam))
        {
            return 0;   // SacredBild's own screen has the keyboard and mouse
        }
        return CallWindowProcA(g_origWindowProc.load(), window, message, wParam, lParam);
    }

    // A window procedure of the game itself (read through GetWindowLongPtrA, which gives the game's ANSI procedures
    // as addresses; one set through SetWindowLongPtrW reads as a handle outside any module).
    bool gameProc(WNDPROC proc)
    {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const auto addr = reinterpret_cast<uintptr_t>(proc);
        return addr >= base && addr < base + nt->OptionalHeader.SizeOfImage;
    }

    // Subclasses the main window. `onlyOverGame`: only if the game replaced its window procedure (it does when a game
    // starts), not over a subclass another module put over this one (opengl32's with DXVK under RenderDoc, overlays):
    // that one calls windowProc in turn, and windowProc calling it back recursed until the stack overflowed.
    void subclass(HWND window, bool onlyOverGame)
    {
        const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrA(window, GWLP_WNDPROC));
        if (current && current != &windowProc && (!onlyOverGame || gameProc(current)))
        {
            g_origWindowProc = current;
            SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&windowProc));
        }
    }

    // The window's client area on the screen, if the cursor should be confined to it now.
    bool clipRect(RECT& rect)
    {
        HWND window = g_window;
        if (!window || GetForegroundWindow() != window || IsIconic(window))
        {
            return false;
        }
        // Holding Alt lets the cursor go (as in many newer games); once released, the cursor is taken again when it
        // is back over the window.
        if (GetAsyncKeyState(VK_MENU) < 0)
        {
            return false;
        }
        // Not while the window is moved or sized, or its system menu is open.
        GUITHREADINFO gui = {};
        gui.cbSize = sizeof(gui);
        if (GetGUIThreadInfo(GetWindowThreadProcessId(window, nullptr), &gui) &&
            (gui.flags & (GUI_INMOVESIZE | GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE)))
        {
            return false;
        }
        POINT origin = {};
        if (!GetClientRect(window, &rect) || !ClientToScreen(window, &origin))
        {
            return false;
        }
        OffsetRect(&rect, origin.x, origin.y);
        return !IsRectEmpty(&rect);
    }
}

namespace
{
    // [Display] NoActivate (the agent copies): the window the player was in, given back the foreground once the
    // game has taken it at its start (it waits for its activation there: kept from it, it hung, 2026-11-11), within
    // its first 90 s; later a copy brought forward by the player stays in front.
    HWND g_playerWindow = nullptr;
    DWORD g_startedAt = 0;
    bool g_givenBack = false;

    bool ours(HWND window)
    {
        DWORD process = 0;
        return window && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId();
    }

    void giveForegroundBack()
    {
        if (!Config::display.noActivate || g_givenBack)
        {
            return;
        }
        const HWND foreground = GetForegroundWindow();
        if (!ours(foreground))
        {
            if (foreground)
            {
                g_playerWindow = foreground;    // where the player is now
            }
            return;
        }
        if (GetTickCount() - g_startedAt > 90000)
        {
            g_givenBack = true;
            return;
        }
        if (g_playerWindow && IsWindow(g_playerWindow) && SetForegroundWindow(g_playerWindow))
        {
            g_givenBack = true;
            LOG("Focus: NoActivate: the foreground given back to the player's window");
        }
    }
}

void Focus::install()
{
    g_origGetAsyncKeyState = static_cast<GetAsyncKeyStateFn>(
        Patch::iat("USER32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(&hookGetAsyncKeyState)));
    g_origSetCursorPos = static_cast<SetCursorPosFn>(
        Patch::iat("USER32.dll", "SetCursorPos", reinterpret_cast<void*>(&hookSetCursorPos)));
    g_origSetWindowsHookExA = static_cast<SetWindowsHookExAFn>(
        Patch::iat("USER32.dll", "SetWindowsHookExA", reinterpret_cast<void*>(&hookSetWindowsHookExA)));
    g_origGetKeyboardState = static_cast<GetKeyboardStateFn>(
        Patch::iat("USER32.dll", "GetKeyboardState", reinterpret_cast<void*>(&hookGetKeyboardState)));
    if (!g_origGetKeyboardState)
    {
        g_origGetKeyboardState = &GetKeyboardState;
    }
    if (Config::display.noActivate)
    {
        g_startedAt = GetTickCount();
        const HWND foreground = GetForegroundWindow();
        g_playerWindow = ours(foreground) ? nullptr : foreground;
        LOG("Focus: NoActivate: the foreground goes back to the player's window once the game is up");
    }
    LOG("Focus: input only in the foreground, ClipCursor={}", Config::display.clipCursor);

    // The window procedure no longer pauses rendering when the game loses the foreground: the menu loop's last
    // frame was often cleared without its UI (a black screen), and switching back during an intro movie resumed the
    // menu under it. [Display] FpsLimitInactive paces the frames in the background.
    // and eax, 0xFFFDFFFF -> and eax, 0xFFFFFFFF; or eax, edi -> mov eax, eax
    constexpr uint8_t kKeep[] = {0x8B, 0xC0};
    if (Patch::verify(Sacred::Addr::activateAppResumeAnd, {0x25, 0xFF, 0xFF, 0xFD, 0xFF}) &&
        Patch::verify(Sacred::Addr::activateAppPauseOr, {0x0B, 0xC7}) &&
        Patch::imm32(Sacred::Addr::activateAppResumeAnd + 1, 0xFFFDFFFF, 0xFFFFFFFF) &&
        Patch::write(Sacred::Addr::activateAppPauseOr, kKeep, sizeof(kKeep)))
    {
        LOG("Focus: rendering continues in the background");
    }
    // With the game's fullscreen mode (Settings.cfg FULLSCREEN : 1, what SacredBild shows as a borderless window) the
    // deactivation takes other branches, one in each of the game's two window procedures: the same pause, Sleep(50),
    // the driver's release, then ShowWindow(window, SW_MINIMIZE), so selecting another window minimized the game.
    // The pause goes as above and SW_MINIMIZE becomes SW_SHOWNA (the window stays as it is).
    //   ENG 00812EC0: or eax, edi (edi = 0x20000); push 6 at +0x27
    //   ENG 00811D16: or ecx, 0x20000; push 6 at +0x2B
    static uintptr_t fullA = 0, fullB = 0;
    const Sig::Entry fullSigs[] = {
        {"focus: fullscreen deactivation", "0B C7 6A 32 A3 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? 8B 82 B4 00 "
         "00 00 50 8B 08 FF 51 4C 8B 15 ?? ?? ?? ?? 6A 06", 0, Sig::Take::Match, &fullA},
        {"focus: fullscreen deactivation 2", "81 C9 00 00 02 00 6A 32 89 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? A1 ?? ?? ?? ?? "
         "8B 80 B4 00 00 00 50 8B 08 FF 51 4C 8B 15 ?? ?? ?? ?? 6A 06", 0, Sig::Take::Match, &fullB},
    };
    Sig::resolve(fullSigs);
    constexpr uint8_t kShowNa[] = {0x6A, SW_SHOWNA};
    int unminimized = 0;
    if (fullA && Patch::verify(fullA, {0x0B, 0xC7}) && Patch::verify(fullA + 0x27, {0x6A, SW_MINIMIZE}) &&
        Patch::write(fullA, kKeep, sizeof(kKeep)) && Patch::write(fullA + 0x27, kShowNa, sizeof(kShowNa)))
    {
        ++unminimized;
    }
    if (fullB && Patch::verify(fullB + 0x2B, {0x6A, SW_MINIMIZE}) && Patch::imm32(fullB + 2, 0x00020000, 0) &&
        Patch::write(fullB + 0x2B, kShowNa, sizeof(kShowNa)))
    {
        ++unminimized;
    }
    LOG("Focus: the fullscreen mode no longer minimizes the window in the background ({} of 2 places)", unminimized);
}

void Focus::windowCreated(HWND window)
{
    g_window = window;
    Inject::setWindow(window);
    subclass(window, false);
    EnableMenuItem(GetSystemMenu(window, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);   // greys the close button
}

bool Focus::foreground()
{
    return active();
}

HWND Focus::window()
{
    return g_window;
}

void Focus::onFrame()
{
    giveForegroundBack();
    HWND window = g_window;
    if (!window)
    {
        return;
    }
    subclass(window, true);
    // The mouse is never held in the window while the agent plays (the player's mouse stays the player's).
    if (!Config::display.clipCursor || Config::autoAttack.enabled)
    {
        return;
    }
    RECT rect;
    const bool clip = clipRect(rect);
    std::scoped_lock lock(g_clipMutex);
    if (!clip)
    {
        releaseLocked();
        return;
    }
    if (!g_clipped)
    {
        // Only once the cursor is over the client area: a windowed game activated by a click on its title bar can
        // still be dragged.
        POINT cursor;
        if (!GetCursorPos(&cursor) || !PtInRect(&rect, cursor))
        {
            return;
        }
    }
    else
    {
        RECT current;
        if (EqualRect(&rect, &g_target) && GetClipCursor(&current) && EqualRect(&current, &g_clip))
        {
            return;     // still in place
        }
    }
    // New, moved or resized, or reset by Windows (e.g. a UAC prompt).
    if (ClipCursor(&rect) && GetClipCursor(&g_clip))   // Windows clamps it to the screen
    {
        g_target = rect;
        g_clipped = true;
    }
}

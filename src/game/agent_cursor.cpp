#include "game/agent_cursor.h"
#include "config/auto_attack.h"
#include "game/agent_overlay.h"
#include "game/aim_assist.h"
#include "game/auto_attack.h"
#include "game/controller.h"
#include "game/focus.h"
#include "input/inject.h"
#include "log.h"
#include "sacred/engine.h"

#include <algorithm>
#include <atomic>
#include <cmath>

using namespace Sacred;
using AgentCursor::Aim;

namespace
{
    // A quick player's hand (the player: «como un proplayer»): reaction, flick time and accuracy.
    constexpr int kReactMinMs = 90, kReactMaxMs = 170;     // a new target: before the hand moves
    constexpr double kFlickBaseMs = 50.0, kFlickPerBitMs = 55.0, kFlickWidth = 40.0;   // Fitts: a + b log2(1 + D/W)
    constexpr double kOnTarget = 9.0;                       // pixels: the cursor is on it
    constexpr double kNewTarget = 30.0;                     // pixels: a target this far from the last is a new one
    constexpr int kClickMinMs = 20, kClickMaxMs = 45;       // on it, before the click
    constexpr DWORD kGiveUpMs = 1500;                       // never held longer: the order goes out anyway
    constexpr float kEdge = 24.0f;                          // an aim off the screen: this far inside its edge

    std::atomic<int64_t> g_shown{0};    // the cursor the game reads, x << 32 | y (read on the window's thread too)
    std::atomic<bool> g_on{false};

    struct Move
    {
        double sx = 0, sy = 0;      // start
        double cx = 0, cy = 0;      // the curve's control point
        double ex = 0, ey = 0;      // end
        double startMs = 0, ms = 0;
        bool active = false;
        bool correction = false;    // an overshoot to correct when it ends
    };

    double g_x = -1, g_y = -1;      // the cursor, client pixels (-1: not placed yet)
    Move g_move;
    double g_reactUntil = 0;
    double g_tx = 0, g_ty = 0;      // the target's screen point
    bool g_hasTarget = false;
    Aim g_aim;
    DWORD g_aimAt = 0;              // when this aim was given
    double g_onSince = 0;           // since when the cursor has been on it (0: not on it)
    void (*g_fn)(void*) = nullptr;  // the act waiting for the cursor
    void* g_context = nullptr;
    DWORD g_lastRequest = 0;

    uint32_t g_seed = 0x9E3779B9u;
    double random01()
    {
        g_seed ^= g_seed << 13;
        g_seed ^= g_seed >> 17;
        g_seed ^= g_seed << 5;
        return (g_seed & 0xFFFFFF) / 16777216.0;
    }
    double between(double lo, double hi)
    {
        return lo + (hi - lo) * random01();
    }

    double nowMs()
    {
        static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return c.QuadPart * 1000.0 / freq;
    }

    bool clientSize(float& w, float& h)
    {
        RECT r;
        HWND window = Focus::window();
        if (!window || !GetClientRect(window, &r) || r.right <= 0 || r.bottom <= 0)
        {
            return false;
        }
        w = static_cast<float>(r.right);
        h = static_cast<float>(r.bottom);
        return true;
    }

    // The aim on the screen: the object's centre, else the world point by the isometric view (AgentOverlay's
    // projection, anchored on the hero); a point off the screen is pulled in to its edge along the hero's line to it.
    bool screenPoint(const Aim& aim, double& x, double& y)
    {
        float sx = 0, sy = 0;
        if (aim.id && AimAssist::screenOf(aim.id, sx, sy))
        {
            x = sx;
            y = sy;
            return true;
        }
        cObjectManager* objects = cObjectManager::instance();
        const cCreature* hero = objects ? objects->hero() : nullptr;
        float hx = 0, hy = 0, w = 0, h = 0;
        const double scale = AgentOverlay::scale();
        if (!hero || scale <= 0 || !AimAssist::hero(hx, hy) || !clientSize(w, h))
        {
            return false;
        }
        const double dx = static_cast<double>(aim.x) - hero->x, dy = static_cast<double>(aim.y) - hero->y;
        double px = hx + scale * 0.894427 * (dx - dy), py = hy + scale * 0.447214 * (dx + dy);
        const double minX = kEdge, minY = kEdge, maxX = w - kEdge, maxY = h - kEdge;
        if (px < minX || px > maxX || py < minY || py > maxY)
        {
            const double vx = px - hx, vy = py - hy;
            double t = 1.0;
            if (vx < 0) t = std::min(t, (minX - hx) / vx);
            if (vx > 0) t = std::min(t, (maxX - hx) / vx);
            if (vy < 0) t = std::min(t, (minY - hy) / vy);
            if (vy > 0) t = std::min(t, (maxY - hy) / vy);
            t = std::clamp(t, 0.0, 1.0);
            px = hx + vx * t;
            py = hy + vy * t;
        }
        x = px;
        y = py;
        return true;
    }

    void show()
    {
        const int64_t packed = (static_cast<int64_t>(std::lround(g_x)) << 32) |
                               static_cast<uint32_t>(static_cast<int32_t>(std::lround(g_y)));
        if (g_shown.exchange(packed) != packed)
        {
            Inject::mouseMove();    // the game moves its cursor on mouse messages only
        }
    }

    // A flick from the cursor to (x, y): Fitts' time, a bend of a few percent of the way to one side, and for a far
    // target now and then a few percent past it, corrected when the flick ends.
    void flick(double x, double y, double now)
    {
        const double dx = x - g_x, dy = y - g_y;
        const double d = std::hypot(dx, dy);
        double ex = x, ey = y;
        g_move.correction = false;
        if (d > 250.0 && random01() < 0.3)
        {
            const double over = std::min(d * between(0.02, 0.05), 24.0);
            ex += dx / d * over;
            ey += dy / d * over;
            g_move.correction = true;
        }
        const double bend = d * between(-0.08, 0.08);
        g_move.sx = g_x;
        g_move.sy = g_y;
        g_move.ex = ex;
        g_move.ey = ey;
        g_move.cx = (g_x + ex) * 0.5 - (d > 0 ? dy / d : 0) * bend;
        g_move.cy = (g_y + ey) * 0.5 + (d > 0 ? dx / d : 0) * bend;
        g_move.ms = (kFlickBaseMs + kFlickPerBitMs * std::log2(1.0 + d / kFlickWidth)) * between(0.85, 1.15);
        g_move.startMs = now;
        g_move.active = d > 1.0;
    }

    bool on()
    {
        return g_hasTarget && !g_move.active && g_reactUntil <= nowMs() &&
               std::hypot(g_tx - g_x, g_ty - g_y) <= kOnTarget;
    }

    bool sameAim(const Aim& a, const Aim& b)
    {
        return a.id ? a.id == b.id : !b.id && std::abs(a.x - b.x) < 40 && std::abs(a.y - b.y) < 40;
    }

    void setAim(const Aim& aim)
    {
        g_lastRequest = GetTickCount();
        if (g_aimAt && sameAim(aim, g_aim))
        {
            g_aim = aim;    // the same target: its newest point, the clock goes on
            return;
        }
        g_aim = aim;
        g_aimAt = GetTickCount();
        g_onSince = 0;
    }
}

bool AgentCursor::active()
{
    const bool active = Config::autoAttack.humanCursor && AutoAttack::on() && !Controller::drives() &&
                        Controller::worldRunning();
    g_on = active;
    return active;
}

bool AgentCursor::owns()
{
    return g_on;
}

bool AgentCursor::cursor(POINT& client)
{
    if (!g_on || g_x < 0)
    {
        return false;
    }
    const int64_t v = g_shown.load();
    client.x = static_cast<LONG>(v >> 32);
    client.y = static_cast<LONG>(static_cast<int32_t>(v & 0xFFFFFFFF));
    return true;
}

bool AgentCursor::reach(const Aim& aim)
{
    if (!active())
    {
        return true;
    }
    setAim(aim);
    double x = 0, y = 0;
    if (!screenPoint(aim, x, y))
    {
        return true;    // nowhere to put it on the screen (no scale yet): not held
    }
    if (GetTickCount() - g_aimAt > kGiveUpMs)
    {
        static DWORD s_told = 0;
        if (GetTickCount() - s_told > 60000)
        {
            s_told = GetTickCount();
            LOG("AgentCursor: {} ms without getting onto the target ({:.0f},{:.0f}): the order goes out anyway",
                kGiveUpMs, x, y);
        }
        return true;
    }
    return on() && g_onSince > 0 && nowMs() - g_onSince >= kClickMinMs;
}

void AgentCursor::act(const Aim& aim, void (*fn)(void*), void* context)
{
    if (reach(aim))
    {
        g_fn = nullptr;
        fn(context);
        return;
    }
    g_fn = fn;
    g_context = context;
}

void AgentCursor::onFrame()
{
    const bool wasOn = g_on;
    if (!active())
    {
        if (wasOn)
        {
            g_fn = nullptr;
            g_aimAt = 0;
            g_hasTarget = false;
        }
        return;
    }
    const double now = nowMs();
    float w = 0, h = 0;
    if (g_x < 0)
    {
        float hx = 0, hy = 0;
        if (!AimAssist::hero(hx, hy))
        {
            return;
        }
        g_x = hx + 60;
        g_y = hy - 40;
        LOG("AgentCursor: on, the agent acts through the game's cursor");
    }
    // An aim no one asked for again in a second is dropped: the cursor rests where it is.
    if (g_aimAt && GetTickCount() - g_lastRequest > 1000 && !g_fn)
    {
        g_aimAt = 0;
        g_hasTarget = false;
    }
    double tx = 0, ty = 0;
    if (g_aimAt && screenPoint(g_aim, tx, ty))
    {
        const bool fresh = !g_hasTarget || std::hypot(tx - g_tx, ty - g_ty) > kNewTarget;
        if (fresh && !g_move.active && std::hypot(tx - g_x, ty - g_y) > kOnTarget)
        {
            // A new target: a moment to see it, then the flick.
            if (g_reactUntil < now || !g_hasTarget)
            {
                g_reactUntil = now + between(kReactMinMs, kReactMaxMs);
            }
        }
        g_tx = tx;
        g_ty = ty;
        g_hasTarget = true;
    }
    if (g_hasTarget && !g_move.active && g_reactUntil <= now && std::hypot(g_tx - g_x, g_ty - g_y) > kOnTarget)
    {
        if (std::hypot(g_tx - g_x, g_ty - g_y) > kNewTarget)
        {
            flick(g_tx, g_ty, now);
        }
        else
        {
            // Close: the hand follows a target that moves, slightly behind it.
            g_x += (g_tx - g_x) * 0.35;
            g_y += (g_ty - g_y) * 0.35;
        }
    }
    if (g_move.active)
    {
        // A target that moved on during the flick: the hand's end goes with it (not for an overshoot's end).
        if (!g_move.correction && g_hasTarget)
        {
            g_move.ex = g_tx;
            g_move.ey = g_ty;
        }
        const double u = std::clamp((now - g_move.startMs) / std::max(g_move.ms, 1.0), 0.0, 1.0);
        const double s = u * u * u * (10.0 - 15.0 * u + 6.0 * u * u);     // minimum jerk
        const double a = (1 - s) * (1 - s), b = 2 * (1 - s) * s, c = s * s;
        g_x = a * g_move.sx + b * g_move.cx + c * g_move.ex;
        g_y = a * g_move.sy + b * g_move.cy + c * g_move.ey;
        if (u >= 1.0)
        {
            g_move.active = false;
            if (g_move.correction && g_hasTarget)
            {
                // The small move back onto it, without a new reaction.
                const double d = std::hypot(g_tx - g_x, g_ty - g_y);
                flick(g_tx, g_ty, now);
                g_move.correction = false;
                g_move.ms = std::max(60.0, std::min(g_move.ms, 50.0 + d * 1.5));
            }
        }
    }
    if (clientSize(w, h))
    {
        g_x = std::clamp(g_x, 0.0, static_cast<double>(w - 1));
        g_y = std::clamp(g_y, 0.0, static_cast<double>(h - 1));
    }
    if (on())
    {
        if (g_onSince == 0)
        {
            g_onSince = now - between(0, kClickMaxMs - kClickMinMs);   // a click a little sooner or later
        }
    }
    else
    {
        g_onSince = 0;
    }
    show();
    if (g_fn && g_aimAt)
    {
        const Aim aim = g_aim;
        if (reach(aim))
        {
            void (*fn)(void*) = g_fn;
            g_fn = nullptr;
            fn(g_context);
        }
    }
}

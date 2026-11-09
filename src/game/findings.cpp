#include "game/findings.h"
#include "game/hero_profile.h"
#include "log.h"
#include "sacred/object.h"

#include <windows.h>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace
{
    constexpr double kTile = 53.66563;
    constexpr DWORD kRepeatMs = 600000;     // the same kind at the same tile once in 10 min

    std::wstring path()
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring p = exe;
        const size_t slash = p.find_last_of(L"\\/");
        return (slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1)) + L"SacredBild-findings.txt";
    }

    std::mutex g_lock;
    std::unordered_map<std::string, DWORD> g_lastAt;
}

void Findings::write(const char* kind, int x, int y, const std::string& text)
{
    const int tx = static_cast<int>(x / kTile), ty = static_cast<int>(y / kTile);
    int layer = -1;
    if (Sacred::cObjectManager* objects = Sacred::cObjectManager::instance())
    {
        if (const Sacred::cCreature* hero = objects->hero())
        {
            layer = *(reinterpret_cast<const uint8_t*>(hero) + 0x24);
        }
    }
    const DWORD now = GetTickCount();
    const std::string key = Fmt::format("{}|{}|{}|{}", kind, layer, tx, ty);
    std::lock_guard<std::mutex> hold(g_lock);
    if (const auto it = g_lastAt.find(key); it != g_lastAt.end() && now - it->second < kRepeatMs)
    {
        return;
    }
    if (g_lastAt.size() > 4096)
    {
        g_lastAt.clear();
    }
    g_lastAt[key] = now;
    const HeroProfile::Data hero = HeroProfile::current();
    SYSTEMTIME t;
    GetLocalTime(&t);
    const std::string line =
        Fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02} | {} L{} | layer {} tile {},{} | {} | {}\n", t.wYear, t.wMonth,
                    t.wDay, t.wHour, t.wMinute, t.wSecond, HeroProfile::className(hero.cls), hero.level, layer, tx, ty,
                    kind, text);
    if (FILE* f = _wfopen(path().c_str(), L"a"))
    {
        std::fputs(line.c_str(), f);
        std::fclose(f);
    }
    LOG("Findings: {} at {},{}: {}", kind, tx, ty, text);
}

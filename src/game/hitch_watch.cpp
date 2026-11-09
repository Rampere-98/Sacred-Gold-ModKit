#include "game/hitch_watch.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>

namespace
{
    constexpr DWORD kHitchMs = 300;     // a frame this late counts as a freeze
    constexpr DWORD kSampleMs = 250;
    constexpr int kMaxSamples = 12;

    std::atomic<DWORD> g_thread{0};
    std::atomic<DWORD> g_lastFrame{0};

    struct Module
    {
        uintptr_t base = 0, end = 0;
        const char* name = "";
    };
    Module g_modules[3];

    void findModules()
    {
        const struct { const wchar_t* file; const char* name; } wanted[] = {
            {nullptr, "Sacred.exe"}, {L"granny.dll", "granny"}, {nullptr, "SacredBild"}};
        for (int i = 0; i < 3; ++i)
        {
            HMODULE m = nullptr;
            if (i == 2)
            {
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&findModules), &m);
            }
            else
            {
                m = GetModuleHandleW(wanted[i].file);
            }
            if (!m)
            {
                continue;
            }
            auto* base = reinterpret_cast<uint8_t*>(m);
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
            g_modules[i] = {reinterpret_cast<uintptr_t>(base), reinterpret_cast<uintptr_t>(base) + nt->OptionalHeader.SizeOfImage,
                wanted[i].name};
        }
    }

    const Module* moduleOf(uintptr_t a)
    {
        for (const Module& m : g_modules)
        {
            if (m.base && a >= m.base && a < m.end)
            {
                return &m;
            }
        }
        return nullptr;
    }

    // Raw sample, taken while the thread is suspended: nothing here may allocate or lock (it may hold the heap lock).
    struct Sample
    {
        uintptr_t eip = 0;
        uint32_t stack[512] = {};
        size_t words = 0;
    };

    bool take(HANDLE thread, Sample& s)
    {
        if (SuspendThread(thread) == static_cast<DWORD>(-1))
        {
            return false;
        }
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        bool ok = GetThreadContext(thread, &c) != 0;
        if (ok)
        {
            s.eip = c.Eip;
            MEMORY_BASIC_INFORMATION mbi{};
            size_t bytes = 0;
            if (VirtualQuery(reinterpret_cast<void*>(c.Esp), &mbi, sizeof(mbi)))
            {
                const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                bytes = std::min<size_t>(sizeof(s.stack), regionEnd - c.Esp);
            }
            std::memcpy(s.stack, reinterpret_cast<void*>(c.Esp), bytes);
            s.words = bytes / 4;
        }
        ResumeThread(thread);
        return ok;
    }

    std::string describe(const Sample& s)
    {
        auto name = [](uintptr_t a) {
            const Module* m = moduleOf(a);
            return m ? Fmt::format("{}+{:x}", m->name, a - m->base) : Fmt::format("{:x}", a);
        };
        std::string out = name(s.eip) + " <";
        int found = 0;
        for (size_t i = 0; i < s.words && found < 14; ++i)
        {
            const uintptr_t v = s.stack[i];
            if (moduleOf(v))
            {
                out += " " + name(v);
                ++found;
            }
        }
        return out;
    }

    void watch()
    {
        findModules();
        static Sample sample;   // large: not on this thread's stack
        DWORD hitchStart = 0;
        int samples = 0;
        DWORD nextSample = 0;
        for (;;)
        {
            Sleep(50);
            const DWORD last = g_lastFrame.load();
            const DWORD tid = g_thread.load();
            const DWORD now = GetTickCount();
            if (!last || !tid)
            {
                continue;
            }
            const DWORD late = now - last;
            if (late < kHitchMs)
            {
                if (hitchStart)
                {
                    LOG("Hitch: the presenting thread froze {} ms", now - hitchStart + kHitchMs);
                    hitchStart = 0;
                }
                continue;
            }
            if (!hitchStart)
            {
                hitchStart = now;
                samples = 0;
                nextSample = now;
            }
            if (samples >= kMaxSamples || static_cast<int>(now - nextSample) < 0)
            {
                continue;
            }
            nextSample = now + kSampleMs;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
            if (!thread)
            {
                continue;
            }
            const bool ok = take(thread, sample);
            CloseHandle(thread);
            if (ok)
            {
                ++samples;
                LOG("Hitch: {} ms without a frame, at {}", late, describe(sample));
            }
        }
    }
}

void HitchWatch::install()
{
    std::thread(watch).detach();
    LOG("Hitch watch: a frame later than {} ms logs where the presenting thread is", kHitchMs);
}

void HitchWatch::onFrame(DWORD thread)
{
    g_thread.store(thread);
    g_lastFrame.store(GetTickCount());
}

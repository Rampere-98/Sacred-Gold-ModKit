#include "log.h"

#include <windows.h>
#include <cstdio>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    std::mutex g_mutex;
    FILE* g_file = nullptr;
}

void Log::init(const wchar_t* path)
{
    std::scoped_lock lock(g_mutex);
    if (!g_file)
    {
        // Keep the two previous runs' logs (SacredBild.log -> .prev.log -> .prev2.log).
        auto with = [&](const wchar_t* suffix) {
            std::wstring name = path;
            const size_t dot = name.rfind(L'.');
            name.insert(dot == std::wstring::npos ? name.size() : dot, suffix);
            return name;
        };
        MoveFileExW(with(L".prev").c_str(), with(L".prev2").c_str(), MOVEFILE_REPLACE_EXISTING);
        // Every run's log is also kept, stamped by when it was last written, in SacredBild-logs beside it (the newest
        // 60): the analyst lost an hour of play to the two-deep rotation with the game restarted every few minutes.
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &data))
        {
            const std::wstring full = path;
            const size_t slash = full.find_last_of(L"\\/");
            const std::wstring dir = (slash == std::wstring::npos ? std::wstring() : full.substr(0, slash + 1)) +
                                     L"SacredBild-logs\\";
            CreateDirectoryW(dir.c_str(), nullptr);
            FILETIME local;
            SYSTEMTIME t;
            FileTimeToLocalFileTime(&data.ftLastWriteTime, &local);
            FileTimeToSystemTime(&local, &t);
            wchar_t name[64];
            swprintf(name, 64, L"SacredBild-%04u%02u%02u-%02u%02u%02u.log", t.wYear, t.wMonth, t.wDay, t.wHour,
                     t.wMinute, t.wSecond);
            CopyFileW(path, (dir + name).c_str(), FALSE);
            std::vector<std::wstring> kept;
            WIN32_FIND_DATAW found;
            if (HANDLE h = FindFirstFileW((dir + L"SacredBild-*.log").c_str(), &found); h != INVALID_HANDLE_VALUE)
            {
                do
                {
                    kept.push_back(found.cFileName);
                } while (FindNextFileW(h, &found));
                FindClose(h);
            }
            std::sort(kept.begin(), kept.end());
            for (size_t i = 0; i + 60 < kept.size(); ++i)
            {
                DeleteFileW((dir + kept[i]).c_str());
            }
        }
        MoveFileExW(path, with(L".prev").c_str(), MOVEFILE_REPLACE_EXISTING);
        g_file = _wfsopen(path, L"w", _SH_DENYWR);
    }
}

void Log::write(std::string_view line)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::scoped_lock lock(g_mutex);
    if (!g_file)
    {
        return;
    }
    std::fprintf(g_file, "%02u:%02u:%02u.%03u [%5lu] %.*s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
        GetCurrentThreadId(), static_cast<int>(line.size()), line.data());
    std::fflush(g_file);
}

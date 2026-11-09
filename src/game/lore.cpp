#include "game/lore.h"
#include "log.h"
#include "sacred/text.h"

#include <imgui.h>
#include <windows.h>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <mutex>
#include <string>

using namespace Sacred;

namespace
{
    std::mutex g_lock;
    std::deque<std::string> g_read;          // the last texts read, newest first (for the menu)
    std::wstring g_lastDialog;
    uint32_t g_questTitle = 0, g_questObjective = 0;
    Lore::QuestKind g_kind = Lore::QuestKind::Unknown;
    int g_count = 0;
    std::string g_questLine;

    std::string utf8(const std::wstring& w)
    {
        if (w.empty())
        {
            return {};
        }
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    // The game's markup out: <n> line breaks, \dAARRGGBB colors.
    std::wstring plain(const std::wstring& t)
    {
        std::wstring out;
        for (size_t i = 0; i < t.size(); ++i)
        {
            if (t.compare(i, 3, L"<n>") == 0)
            {
                out += L'\n';
                i += 2;
            }
            else if (t[i] == L'\\' && i + 1 < t.size() && t[i + 1] == L'd' && i + 10 <= t.size())
            {
                i += 9;
            }
            else if (t[i] != L'\r')
            {
                out += t[i];
            }
        }
        while (!out.empty() && std::iswspace(out.front()))
        {
            out.erase(out.begin());
        }
        return out;
    }

    std::wstring lower(std::wstring s)
    {
        for (wchar_t& c : s)
        {
            c = static_cast<wchar_t>(std::towlower(c));
        }
        return s;
    }

    bool has(const std::wstring& s, std::initializer_list<const wchar_t*> words)
    {
        for (const wchar_t* w : words)
        {
            if (s.find(w) != std::wstring::npos)
            {
                return true;
            }
        }
        return false;
    }

    int countIn(const std::wstring& s)
    {
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (std::iswdigit(s[i]))
            {
                return std::wcstol(s.c_str() + i, nullptr, 10);
            }
        }
        static const struct { const wchar_t* word; int n; } kWords[] = {
            {L" dos ", 2}, {L" tres ", 3}, {L" cuatro ", 4}, {L" cinco ", 5}, {L" seis ", 6}, {L" siete ", 7},
            {L" ocho ", 8}, {L" nueve ", 9}, {L" diez ", 10}, {L" doce ", 12}, {L" quince ", 15}, {L" veinte ", 20},
            {L" two ", 2}, {L" three ", 3}, {L" four ", 4}, {L" five ", 5}, {L" six ", 6}, {L" ten ", 10},
            {L" twenty ", 20},
        };
        const std::wstring padded = L" " + s + L" ";
        for (const auto& w : kWords)
        {
            if (padded.find(w.word) != std::wstring::npos)
            {
                return w.n;
            }
        }
        return 0;
    }

    Lore::QuestKind classify(const std::wstring& objective)
    {
        const std::wstring s = lower(objective);
        using K = Lore::QuestKind;
        if (has(s, {L"escolt", L"condúce", L"conduce", L"acompaña", L"llévanos", L"escort", L"lead "}))
        {
            return K::Escort;
        }
        if (has(s, {L"mata", L"acaba con", L"destruye", L"elimina", L"derrota", L"vence", L"lucha", L"combate",
                    L"pelea", L"kill", L"destroy", L"defeat", L"slay", L"fight", L"battle"}))
        {
            return K::Kill;
        }
        if (has(s, {L"trae", L"consígue", L"consigue", L"recupera", L"busca", L"encuentra", L"recoge", L"ve por",
                    L"bring", L"find", L"fetch", L"retrieve", L"collect", L"get "}))
        {
            return K::Fetch;
        }
        if (has(s, {L"habla", L"informa", L"díselo", L"speak", L"talk", L"report", L"tell "}))
        {
            return K::Talk;
        }
        if (has(s, {L"ve a", L"dirígete", L"viaja", L"go to", L"travel"}))
        {
            return K::Go;
        }
        return K::Unknown;
    }

    void keep(const std::string& line)
    {
        g_read.push_front(line);
        while (g_read.size() > 12)
        {
            g_read.pop_back();
        }
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe;
        path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"SacredBild-read.txt";
        if (FILE* f = _wfopen(path.c_str(), L"ab"))
        {
            SYSTEMTIME t;
            GetLocalTime(&t);
            std::fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s\r\n\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                         t.wSecond, line.c_str());
            std::fclose(f);
        }
    }
}

std::wstring Lore::text(uint32_t key)
{
    if (!key || key == 0xFFFFFFFFu)
    {
        return {};
    }
    cTextResources* texts = cTextResources::instance();
    const wchar_t* const* t = texts ? texts->text(key) : nullptr;
    if (!t || !t[0] || t[1] <= t[0] || t[1] - t[0] > 20000)
    {
        return {};
    }
    return plain(std::wstring(t[0], t[1]));
}

std::string Lore::textUtf8(uint32_t key)
{
    return utf8(text(key));
}

Lore::QuestKind Lore::readQuest(uint32_t titleKey, uint32_t objectiveKey)
{
    const std::lock_guard lock(g_lock);
    if (titleKey == g_questTitle && objectiveKey == g_questObjective)
    {
        return g_kind;
    }
    g_questTitle = titleKey;
    g_questObjective = objectiveKey;
    const std::wstring title = text(titleKey), objective = text(objectiveKey);
    g_kind = classify(objective.empty() ? title : objective);
    g_count = countIn(lower(objective));
    g_questLine = "Misión «" + utf8(title) + "»: " + utf8(objective) + " -> " + kindName(g_kind) +
                  (g_count ? " (" + std::to_string(g_count) + ")" : "");
    LOG("Lore: {}", g_questLine);
    keep(g_questLine);
    return g_kind;
}

Lore::QuestKind Lore::questKind()
{
    const std::lock_guard lock(g_lock);
    return g_kind;
}

int Lore::questCount()
{
    const std::lock_guard lock(g_lock);
    return g_count;
}

void Lore::readDialog(const std::wstring& raw)
{
    const std::lock_guard lock(g_lock);
    if (raw.empty() || raw == g_lastDialog)
    {
        return;
    }
    g_lastDialog = raw;
    const std::string line = utf8(plain(raw));
    std::string first = line.substr(0, line.find('\n'));
    LOG("Lore: read «{}» ({} characters)", first.substr(0, 80), line.size());
    keep(line);
}

const char* Lore::kindName(QuestKind kind)
{
    switch (kind)
    {
    case QuestKind::Talk: return "hablar";
    case QuestKind::Kill: return "matar";
    case QuestKind::Fetch: return "traer";
    case QuestKind::Escort: return "escoltar";
    case QuestKind::Go: return "ir";
    default: return "sin saber";
    }
}

void Lore::drawMenu()
{
    const std::lock_guard lock(g_lock);
    if (!g_questLine.empty())
    {
        ImGui::TextWrapped("%s", g_questLine.c_str());
        ImGui::Separator();
    }
    if (g_read.empty())
    {
        ImGui::TextDisabled("Aún no ha leído nada.");
        return;
    }
    for (const std::string& line : g_read)
    {
        ImGui::TextWrapped("%s", line.substr(0, 400).c_str());
        ImGui::Separator();
    }
}

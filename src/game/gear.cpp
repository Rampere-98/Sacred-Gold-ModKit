#include "game/gear.h"
#include "game/modkit_menu.h"
#include "input/inject.h"
#include "log.h"
#include "sacred/object.h"
#include "sig.h"

#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

using namespace Sacred;

// Addresses and layouts: docs/ITEMS_RE.md (ENG build; found by their bytes).
namespace
{
    struct Settings
    {
        bool equip = false;
        bool sell = false;
        bool buy = false;
        int keepGoldPercent = 50;   // buying spends at most the rest
    };
    Settings g_settings;

    uintptr_t g_inventoryPtr = 0, g_typeMgrPtr = 0;     // the globals holding cInventory* and the type manager
    uintptr_t g_getEntry = 0, g_findFree = 0, g_putItem = 0, g_removeItem = 0, g_typeSize = 0, g_getInfo = 0,
              g_getValue = 0, g_equip = 0, g_getEquipped = 0, g_canUse = 0, g_slotAccepts = 0, g_price = 0,
              g_addGoldCall = 0;
    uintptr_t g_addGold = 0;
    const Sig::Entry kSigs[] = {
        {"cInventory", "8B 35 ?? ?? ?? ?? 85 F6 74 1A 8B CE C7 05 ?? ?? ?? ?? 00 00 00 00 E8 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 83 C4 04 5E C3 90 90 90 90 90 90 90 90 90 8B 44 24 04 66 3D 20 00", 2,
            Sig::Take::Abs32, &g_inventoryPtr},
        {"typeMgr", "8B 0D ?? ?? ?? ?? 55 57 89 5C 24 28", 2, Sig::Take::Abs32, &g_typeMgrPtr},
        {"getEntry", "8B 44 24 04 66 3D 20 00 73 1B 25 FF FF 00 00", 0, Sig::Take::Match, &g_getEntry},
        {"findFree", "8A 44 24 10 53 55 56 57 8B F9 84 C0", 0, Sig::Take::Match, &g_findFree},
        {"putItem", "64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 83 EC 78 53 8B", 0,
            Sig::Take::Match, &g_putItem},
        {"removeItem", "83 EC 08 53 55 56 57 8B F9 33 D2 33 C0", 0, Sig::Take::Match, &g_removeItem},
        {"typeSize", "8B 44 24 04 53 56 ?? ?? ?? ?? 00 57 7D 36", 0, Sig::Take::Match, &g_typeSize},
        {"getInfo", "66 8B 44 24 04 56 66 3D FF FF 0F 85", 0, Sig::Take::Match, &g_getInfo},
        {"getValue", "55 56 8B F1 6A FF E8 ?? ?? ?? ?? 8B E8", 0, Sig::Take::Match, &g_getValue},
        {"equip", "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 ?? ?? ?? ?? 00 00 53 55 8B ?? ?? ?? ?? 00 00 33 DB 56 66 83 FD 12",
            0, Sig::Take::Match, &g_equip},
        {"getEquipped", "8B 44 24 04 56 ?? ?? ?? ?? 00 8D ?? ?? ?? ?? 00 00", 0, Sig::Take::Match, &g_getEquipped},
        {"canUseItem", "8B 44 24 04 56 57 6A 00 68 ?? ?? ?? ?? 8B F9 8B 0D ?? ?? ?? ?? 68 ?? ?? ?? ?? 6A 00 50 E8 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 8B F0 83 C4 14 85 F6 0F 84", 0,
            Sig::Take::Match, &g_canUse},
        {"slotAccepts", "53 56 8B 74 24 10 32 DB 81 FE 00 16 00 00", 0, Sig::Take::Match, &g_slotAccepts},
        {"price", "8B 44 24 04 8B 0D ?? ?? ?? ?? 56 57", 0, Sig::Take::Match, &g_price},
        // addGold (ENG 0057FD80) has a twin of the same bytes; it is taken from a call to it (the horse's healing).
        {"addGoldCall", "E8 ?? ?? ?? ?? 8B 56 0C 8B CB 52 E8 ?? ?? ?? ??", 0, Sig::Take::Match, &g_addGoldCall},
    };
    bool g_ready = false;

    using GetEntry = uint8_t*(__fastcall*)(void* inv, void*, int idx, int idx2);
    using FindFree = uint8_t(__fastcall*)(uint8_t* entry, void*, int w, int h, uint16_t* cell, int zero);
    using PutItem = uint8_t(__fastcall*)(uint8_t* entry, void*, int cell, uint32_t id, int w, int h, int net);
    using RemoveItem = uint8_t(__fastcall*)(uint8_t* entry, void*, uint32_t id, int net);
    using TypeSize = void(__fastcall*)(void* typeMgr, void*, uint32_t type, int* w, int* h);
    using GetInfo = uint8_t*(__fastcall*)(cObject* item, void*, int idx);
    using GetValue = uint32_t(__fastcall*)(cObject* item, void*);
    using Equip = uint32_t(__fastcall*)(cCreature* c, void*, int slot, uint32_t id, int net);
    using GetEquipped = uint32_t(__fastcall*)(cCreature* c, void*, int slot);
    using CanUse = uint8_t(__fastcall*)(cCreature* c, void*, uint32_t id, int classOnly);
    using SlotAccepts = uint8_t(__fastcall*)(void* typeMgr, void*, int slot, uint32_t type);
    using Price = uint32_t(__fastcall*)(void* inv, void*, uint32_t id, int buy);
    using AddGold = void(__fastcall*)(void* stats, void*, int delta);

    template <class F>
    F fn(uintptr_t a)
    {
        return reinterpret_cast<F>(a);
    }

    constexpr size_t kStats = 0x3A8;
    constexpr size_t kGold = kStats + 0x46;
    constexpr size_t kItemType = 0x10;
    constexpr size_t kItemFlags = 0x14;
    constexpr uint32_t kQuestItem = 0x800;
    constexpr int kSlots = 18;          // 0..17; 18 is the mount
    constexpr int kFirstStockPage = 0x14, kStockPages = 3;
    constexpr size_t kTrade = 0x23000;  // cInventory: the merchant traded with, 0 for none

    void* inventory() { return g_inventoryPtr ? *reinterpret_cast<void**>(g_inventoryPtr) : nullptr; }
    void* typeMgr() { return g_typeMgrPtr ? *reinterpret_cast<void**>(g_typeMgrPtr) : nullptr; }

    uint8_t* entryOf(int idx)
    {
        void* inv = inventory();
        return inv ? fn<GetEntry>(g_getEntry)(inv, nullptr, idx & 0xFFFF, idx & 0xFFFF) : nullptr;
    }

    // The backpack grid: cells of 12 bytes from +0x18, width/height at +0x115C/+0x115E.
    template <class F>
    void eachItem(uint8_t* entry, F&& f)
    {
        const int w = *reinterpret_cast<uint16_t*>(entry + 0x115C), h = *reinterpret_cast<uint16_t*>(entry + 0x115E);
        if (w <= 0 || h <= 0 || w * h > 0x100)
        {
            return;
        }
        for (int i = 0; i < w * h; ++i)
        {
            const uint8_t* cell = entry + 0x18 + 12 * i;
            const uint32_t id = *reinterpret_cast<const uint32_t*>(cell);
            // Only the item's top-left cell holds its id (+4 bit 0 marks every occupied cell, that one too: seen live).
            if (id)
            {
                f(id, i, static_cast<int>(cell[5]), static_cast<int>(cell[6]));
            }
        }
    }

    struct Worth
    {
        float primary = 0.0f;   // weapon damage (mean of min and max, all kinds) or armor (all kinds)
        float value = 0.0f;     // the game's value: mods and the rest
        bool weapon = false;
    };

    Worth worth(uint32_t id)
    {
        Worth w;
        cObjectManager* objects = cObjectManager::instance();
        cObject* item = objects ? objects->object(id) : nullptr;
        if (!item)
        {
            return w;
        }
        if (const uint8_t* info = fn<GetInfo>(g_getInfo)(item, nullptr, -1))
        {
            const auto* lo = reinterpret_cast<const uint16_t*>(info + 0x1C);
            const auto* hi = reinterpret_cast<const uint16_t*>(info + 0x24);
            const auto* armor = reinterpret_cast<const int16_t*>(info + 0x32);
            float damage = 0.0f, defense = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                damage += (lo[k] + hi[k]) * 0.5f;
                defense += std::max<int16_t>(armor[k], 0);
            }
            w.weapon = damage > 0.0f;
            w.primary = w.weapon ? damage : defense;
        }
        w.value = static_cast<float>(fn<GetValue>(g_getValue)(item, nullptr));
        return w;
    }

    // `candidate` clearly better than `current` (0: an empty slot).
    bool better(const Worth& candidate, uint32_t current, float margin)
    {
        if (!current)
        {
            return true;
        }
        const Worth now = worth(current);
        if (candidate.weapon != now.weapon)
        {
            return false;       // a shield for a weapon or the other way round: the player's choice
        }
        if (candidate.primary > now.primary * (1.0f + margin) + 0.5f)
        {
            return true;
        }
        const bool same = candidate.primary >= now.primary * (1.0f - margin * 0.5f);
        return same && candidate.value > now.value * (1.0f + 2.0f * margin) + 1.0f;
    }

    uint32_t typeOf(uint32_t id)
    {
        cObjectManager* objects = cObjectManager::instance();
        cObject* o = objects ? objects->object(id) : nullptr;
        return o ? *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(o) + kItemType) : 0;
    }

    uint32_t flagsOf(uint32_t id)
    {
        cObjectManager* objects = cObjectManager::instance();
        cObject* o = objects ? objects->object(id) : nullptr;
        return o ? *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(o) + kItemFlags) : 0;
    }

    // The slot `id` would best go to, and what is there now; -1 if it is better nowhere. Of the rings and amulets,
    // the weakest worn.
    int bestSlot(cCreature* hero, uint32_t id, float margin, uint32_t* replaced = nullptr, bool* fits = nullptr)
    {
        const uint32_t type = typeOf(id);
        void* types = typeMgr();
        if (!type || !types)
        {
            return -1;
        }
        const Worth w = worth(id);
        int best = -1;
        float bestGain = 0.0f;
        for (int slot = 0; slot < kSlots; ++slot)
        {
            if (!fn<SlotAccepts>(g_slotAccepts)(types, nullptr, slot, type))
            {
                continue;
            }
            if (fits)
            {
                *fits = true;
            }
            const uint32_t current = fn<GetEquipped>(g_getEquipped)(hero, nullptr, slot);
            // The weapon hands (12, 13) go with the taskbar's weapon sets: an empty one is the player's choice.
            if (!current && (slot == 12 || slot == 13))
            {
                continue;
            }
            if (!better(w, current, margin))
            {
                continue;
            }
            const Worth now = current ? worth(current) : Worth{};
            const float gain = current ? (w.primary - now.primary) + (w.value - now.value) * 0.01f : 1e9f;
            if (best < 0 || gain > bestGain)
            {
                best = slot;
                bestGain = gain;
                if (replaced)
                {
                    *replaced = current;
                }
            }
        }
        return best;
    }

    bool sizeOf(uint32_t id, int& w, int& h)
    {
        void* types = typeMgr();
        const uint32_t type = typeOf(id);
        if (!types || !type)
        {
            return false;
        }
        w = h = 0;
        fn<TypeSize>(g_typeSize)(types, nullptr, type, &w, &h);
        return w > 0 && h > 0;
    }

    // One piece put on: out of the backpack, onto the hero, what it wore into the backpack. False if nothing to do.
    bool equipOne(cCreature* hero, uint8_t* pack)
    {
        bool done = false;
        eachItem(pack, [&](uint32_t id, int cell, int w, int h) {
            if (done || (flagsOf(id) & kQuestItem) || !fn<CanUse>(g_canUse)(hero, nullptr, id, 0))
            {
                return;
            }
            uint32_t old = 0;
            const int slot = bestSlot(hero, id, 0.05f, &old);
            if (slot < 0)
            {
                return;
            }
            int ow = 0, oh = 0;
            if (old && !sizeOf(old, ow, oh))
            {
                return;
            }
            fn<RemoveItem>(g_removeItem)(pack, nullptr, id, 1);
            uint16_t free = 0;
            if (old && !fn<FindFree>(g_findFree)(pack, nullptr, ow, oh, &free, 0))
            {
                fn<PutItem>(g_putItem)(pack, nullptr, cell, id, w, h, 1);     // no room for the old one: as it was
                return;
            }
            const uint32_t previous = fn<Equip>(g_equip)(hero, nullptr, slot, id, 1);
            if (old && previous)
            {
                fn<PutItem>(g_putItem)(pack, nullptr, free, previous, ow, oh, 1);
            }
            LOG("Gear: put on {:x} (type {:x}) in slot {}{}", id, typeOf(id), slot, old ? ", the old one to the backpack" : "");
            done = true;
        });
        return done;
    }

    // At a merchant: one item sold (worse than the worn one for every slot it fits, or not for this class).
    bool sellOne(cCreature* hero, uint8_t* pack)
    {
        void* inv = inventory();
        bool done = false;
        eachItem(pack, [&](uint32_t id, int cell, int w, int h) {
            if (done || (flagsOf(id) & kQuestItem))
            {
                return;
            }
            bool fits = false;
            const bool classOk = fn<CanUse>(g_canUse)(hero, nullptr, id, 1) != 0;
            const int slot = classOk ? bestSlot(hero, id, 0.0f, nullptr, &fits) : -1;
            if (classOk && (!fits || slot >= 0))
            {
                return;     // potions, runes, gems...; or an upgrade (perhaps once the hero is stronger)
            }
            for (int page = 0; page < kStockPages; ++page)
            {
                uint8_t* stock = entryOf(kFirstStockPage + page);
                uint16_t free = 0;
                if (!stock || !fn<FindFree>(g_findFree)(stock, nullptr, w, h, &free, 0))
                {
                    continue;
                }
                const uint32_t price = fn<Price>(g_price)(inv, nullptr, id, 0);
                fn<RemoveItem>(g_removeItem)(pack, nullptr, id, 1);
                fn<PutItem>(g_putItem)(stock, nullptr, free, id, w, h, 1);
                fn<AddGold>(g_addGold)(reinterpret_cast<uint8_t*>(hero) + kStats, nullptr, static_cast<int>(price));
                LOG("Gear: sold {:x} (type {:x}) for {} gold{}", id, typeOf(id), price, classOk ? "" : " (not for this class)");
                done = true;
                return;
            }
        });
        (void)hero;
        return done;
    }

    // At a merchant: one item bought that would be clearly better than what the hero wears, within the gold to spend.
    bool buyOne(cCreature* hero, uint8_t* pack)
    {
        void* inv = inventory();
        const uint32_t gold = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + kGold);
        const uint32_t budget = static_cast<uint32_t>(gold * static_cast<uint64_t>(100 - g_settings.keepGoldPercent) / 100);
        // The cheapest upgrade seen over budget, told once per merchant (what it would buy with more gold).
        static uint32_t toldFor = 0;
        uint32_t cheapest = 0, cheapestPrice = 0;
        for (int page = 0; page < kStockPages; ++page)
        {
            uint8_t* stock = entryOf(kFirstStockPage + page);
            if (!stock)
            {
                continue;
            }
            bool done = false;
            eachItem(stock, [&](uint32_t id, int, int w, int h) {
                if (done || !fn<CanUse>(g_canUse)(hero, nullptr, id, 0) || bestSlot(hero, id, 0.15f) < 0)
                {
                    return;
                }
                const uint32_t price = fn<Price>(g_price)(inv, nullptr, id, 1);
                uint16_t free = 0;
                if (price > budget)
                {
                    if (!cheapest || price < cheapestPrice)
                    {
                        cheapest = id;
                        cheapestPrice = price;
                    }
                    return;
                }
                if (!fn<FindFree>(g_findFree)(pack, nullptr, w, h, &free, 0))
                {
                    return;
                }
                // As the shop's buy (ENG 006D9840) does it.
                cObject* item = cObjectManager::instance()->object(id);
                fn<RemoveItem>(g_removeItem)(stock, nullptr, id, 1);
                fn<AddGold>(g_addGold)(reinterpret_cast<uint8_t*>(hero) + kStats, nullptr, -static_cast<int>(price));
                if (uint8_t* info = item ? fn<GetInfo>(g_getInfo)(item, nullptr, -1) : nullptr)
                {
                    *reinterpret_cast<uint32_t*>(info + 4) &= ~0x200000u;
                }
                fn<PutItem>(g_putItem)(pack, nullptr, free, id, w, h, 1);
                LOG("Gear: bought {:x} (type {:x}) for {} gold (of {})", id, typeOf(id), price, gold);
                done = true;
            });
            if (done)
            {
                return true;
            }
        }
        const uint32_t merchant = *reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(inv) + kTrade);
        if (cheapest && toldFor != merchant)
        {
            toldFor = merchant;
            LOG("Gear: the cheapest upgrade here, {:x} (type {:x}), costs {} gold; {} to spend of {}", cheapest,
                typeOf(cheapest), cheapestPrice, budget, gold);
        }
        return false;
    }

    bool g_calm = false;
    std::atomic<bool> g_working{false};

    // On the window's thread, where the game's own windows do the same.
    void work(void*)
    {
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        void* inv = inventory();
        uint8_t* pack = hero ? entryOf(static_cast<int>(hero->id)) : nullptr;
        if (hero && inv && pack && hero->health > 0)
        {
            const bool shop = *reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(inv) + kTrade) != 0;
            if (shop && g_settings.sell && sellOne(hero, pack))
            {
            }
            else if (shop && g_settings.buy && buyOne(hero, pack))
            {
            }
            else if ((g_calm || shop) && g_settings.equip)
            {
                equipOne(hero, pack);
            }
        }
        g_working = false;
    }

    std::wstring iniPath()
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe;
        return path.substr(0, path.find_last_of(L"\\/") + 1) + L"SacredBild.ini";
    }

    void drawMenu()
    {
        if (!g_ready)
        {
            ImGui::TextDisabled("No disponible en esta versión del juego.");
            return;
        }
        ImGui::Checkbox("Equipar mejoras de la mochila (fuera de combate)", &g_settings.equip);
        ImGui::Checkbox("Vender lo que no usará (con un mercader abierto)", &g_settings.sell);
        ImGui::SetItemTooltip("Lo peor que lo equipado en todo hueco donde cabe, o de otra clase. Nunca objetos de misión ni "
                              "los que no son equipo (pociones, runas, gemas).");
        ImGui::Checkbox("Comprar mejoras (con un mercader abierto)", &g_settings.buy);
        ImGui::SliderInt("Oro que guarda (%)", &g_settings.keepGoldPercent, 0, 95);
    }

    void saveMenu(const std::wstring& ini)
    {
        ModkitMenu::write(ini, L"Gear", L"Equip", g_settings.equip);
        ModkitMenu::write(ini, L"Gear", L"Sell", g_settings.sell);
        ModkitMenu::write(ini, L"Gear", L"Buy", g_settings.buy);
        ModkitMenu::write(ini, L"Gear", L"KeepGoldPercent", g_settings.keepGoldPercent);
    }

    void install()
    {
        const std::wstring ini = iniPath();
        g_settings.equip = GetPrivateProfileIntW(L"Gear", L"Equip", 0, ini.c_str()) != 0;
        g_settings.sell = GetPrivateProfileIntW(L"Gear", L"Sell", 0, ini.c_str()) != 0;
        g_settings.buy = GetPrivateProfileIntW(L"Gear", L"Buy", 0, ini.c_str()) != 0;
        g_settings.keepGoldPercent =
            std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Gear", L"KeepGoldPercent", 50, ini.c_str())), 0, 95);
        const size_t missing = Sig::resolve(kSigs);
        if (g_addGoldCall && *reinterpret_cast<const uint8_t*>(g_addGoldCall) == 0xE8)
        {
            g_addGold = g_addGoldCall + 5 + *reinterpret_cast<const int32_t*>(g_addGoldCall + 1);
        }
        g_ready = missing == 0 && g_addGold;
        LOG("Gear: {} (equip {}, sell {}, buy {})", g_ready ? "ready" : "not available (signatures)", g_settings.equip,
            g_settings.sell, g_settings.buy);
        ModkitMenu::add({"Equipo y comercio", &drawMenu, &saveMenu});
    }
}

void Gear::onFrame(DWORD now, bool calm)
{
    static std::once_flag once;
    std::call_once(once, install);
    static DWORD nextAt = 0;
    if (!g_ready || !(g_settings.equip || g_settings.sell || g_settings.buy) ||
        (nextAt && static_cast<int>(now - nextAt) < 0) || g_working)
    {
        return;
    }
    nextAt = now + 1500;
    g_calm = calm;
    g_working = true;
    Inject::call(&work, nullptr);
}

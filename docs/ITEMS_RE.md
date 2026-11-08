# Sacred Gold: items, inventory, equipment and merchants (RE study, 2026-11-08)

Scope: what a mod needs to read the hero's items, compare them, equip them and buy/sell at merchants.
Source: static reading of the unpacked ENG exe (`Sacred.exe.unpacked.exe`, base 0x400000) and of `pak\Weapon.pak`.
Nothing here was verified live. Builds on `GAME_DATA.md` (§3, §5, §9) and `RE_NOTES.md`.

Legend: **OBSERVED** = seen in code or data at the address given. **INFERRED** = deduced from names, strings,
data values or layout, not confirmed by code reading or a live run.

Calling conventions below: `thiscall(this; args)` = MSVC thiscall (ecx = this), `ret N` = callee pops N bytes.
"id" / "ref" always means a cObject id (cObject `+0xC`), resolved with `cObjectManager::getData` `0x5FE000`.

## 0. Singletons and helpers

| What | Address | Notes |
|---|---|---|
| Object manager | `*0xAD5C40` | `getData` `0x5FE000` thiscall(id) -> cObject* (OBSERVED, `object.h`); `hero` `0x603E30` thiscall() -> local hero; `destroy` `0x5FBDB0` thiscall(id, 1, 0, 0) (log "cObjectManager::destroy: tried to delete local hero" `0x5FBE12`) (OBSERVED) |
| Type manager | `*0xAAB5E4` | type records 0x80 bytes at `typeMgr + typeId*0x80` (typeId < 0x7E60); weapon-info table `[typeMgr+0x563004]`, count `[typeMgr+0x563000]` (OBSERVED `0x4340B0`) |
| RTTI dynamic cast | `0x84A961` | cdecl(ptr, 0, srcTD, dstTD, 0). Type descriptors: cObject `0x8EB648`, cCreature `0x8EB660`, cObject3D `0x94CCD8`, **cItemBase `0x94EBC0`** (OBSERVED) |
| Inventory manager `cInventory` | `*0xAD4448` (created by `0x5DEBC0`, which also returns it) | 0x23008 bytes: 32 `cInventoryEntry` of 0x1180 bytes + trade state at `+0x23000` (OBSERVED `0x5DEBC0`). This is the "players' data" of RE_NOTES |
| Session / net singleton | `0x7D84A0` (getter) | `+0x14` = local player's creature id (used as the buyer id by every shop path) (OBSERVED) |
| UI manager | `0x756F40` (getter, called with one 0 arg) | window predicates: `0x75A9B0` merchant open (`+0x90` flags bit 0), `0x75AA50` blacksmith (`+0x8C`), `0x75AA10` chest (`+0xC0`), `0x75AA30` trade (`+0xD8`); posts UI events through vtable `+0` (OBSERVED) |
| cMouse held item | `0x655550` get / `0x655580` set / `0x6556F0` clear (cMouse `+0x68`) | the held entry is a 0x14-byte "cursor entry": u16 kind (1 = item), u16 `0x5FE720(id)`, u32 id, u32 `0x5FE870(id)`, 0, 0 (OBSERVED in `0x6B6C94`..; the two getters' meaning is open) |

## 1. Hero inventory (the backpack)

### 1.1 Where it is

- `cInventoryEntry* entry = cInventory::getEntry(cInventory*, u16 idx)` = **`0x5DEC90`** thiscall(idx), `ret 8`
  (a second ignored arg is pushed by every caller; pass it too), returns `inv + idx*0x1180` or 0 if idx >= 0x20 (OBSERVED).
  Callers pass the creature id: `getEntry(heroId & 0xFFFF)` (OBSERVED in buy `0x6D98E8`, sell, merc window).
- Entry indices in use (OBSERVED unless noted): players by id (the putItem code treats `+0x1160 <= 0x11` as a player
  and recomputes that hero's stats), **0x11** chest storage (shift-click target when the chest window is open, `0x6C0264`),
  **0x14, 0x15, 0x16** the merchant's three stock pages (weapons, armors, various; `cUI_Merchandise` ctor `0x6D95DC`
  uses `0x14 + counter`), **0x1A** the trade window (`0x6C02C4`).

### 1.2 cInventoryEntry layout (0x1180 bytes; ctor `0x5D7A50`, called as (0, 0, 0x14, 8, 0) from `0x5DF570`)

| Offset | Type | Meaning | Evidence |
|---|---|---|---|
| `+0x18` | cell[0x100], 12 bytes each | backpack grid, cell `i = y*width + x` at `entry + 0x18 + 12*i` | OBSERVED `0x5D7AA9` (init), `0x5D7DD3` (walk) |
| cell `+0` | u32 | item id of the item whose top-left cell this is (0 elsewhere) | OBSERVED `0x5D9F5F`, `0x5DA2E9` |
| cell `+4` | u8 | flags: bit 0 = occupied (set on the anchor cell too, seen live 2026-11-08; covered cells have id 0) | OBSERVED `0x5DA2F4` |
| cell `+5`, `+6` | u8, u8 | anchor cell: item width, height (cells); covered cell: anchor x, anchor y | OBSERVED `0x5DA02D`, `0x5DA301`, `0x5DAE39` |
| cell `+8` | u16 | anchor: the item's type id | OBSERVED `0x5DA03C` |
| `+0xC18` | 10 x 12 bytes | taskbar weapon sets: 5 slots x 2 hands, u32 item id at `+0` (weapon, shield/second hand) - items held here are not in the grid | INFERRED from fixInventory `"taskbarslot[%d][%d] ref[%d]"` `0x5DE95A`..`0x5DE999` |
| `+0xC90` | 10 x 20 bytes | taskbar art slots (RE_NOTES) | OBSERVED |
| `+0x115C` | u16 | grid width (default 0x14 = 20) | OBSERVED ctor args |
| `+0x115E` | u16 | grid height (default 8) | OBSERVED |
| `+0x1160` | u16 | owner creature id | OBSERVED `0x5DA085` |
| `+0x1162` | u16 | flags (`0x10` = changed) | OBSERVED `0x5D9F45` |
| `+0x116C/+0x1170/+0x1174` | vector of 20-byte entries | INFERRED: extra item list (quest items? see `addMainQuestItem` `0x5D8280`) | |

**Reading the backpack** (OBSERVED pattern from `0x5DA810`, `0x5D7D70`): for `y < height`, `x < width`:
`cell = entry + 0x18 + 12*(y*width + x)`; if `cell.id != 0` it is an item anchored there (covered cells hold 0).

### 1.3 cInventoryEntry functions (all thiscall on the entry)

| Function | Address | Signature | Notes |
|---|---|---|---|
| itemAt | `0x5DA2D0` | (u16 cell, bool followAnchor) -> id, `ret 8` | follows covered cells to their anchor (OBSERVED) |
| cellAtScreen | `0x5D9D60` | (x, y, u16* cellOut) -> bool, `ret 0xC` | UI hit test (OBSERVED) |
| canPlace | `0x5DACB0` | (u16 cell, w, h) -> bool, `ret 0xC` | (OBSERVED, called by putItem) |
| findFree | `0x5DABA0` | (w, h, u16* cellOut, 0) -> bool, `ret 0x10` | first free spot for a w x h item (OBSERVED in buy, moveItem) |
| hasSpaceFor | `0x5DADB0` | (itemId, 0) -> bool, `ret 8` | size from the type, then findFree (OBSERVED) |
| **putItem** | `0x5D9F00` | (u16 cell, itemId, w, h, bool net) -> bool, `ret 0x14` | removes the item from the world (`0x5FC8B0`), writes the cells, recomputes the owner hero's stats (`0x5796A0`), sends a net event in MP when `net` (OBSERVED) |
| **removeItem** | `0x5DA630` | (itemId, bool net) -> bool, `ret 8` | finds the anchor by id (OBSERVED) |
| count | `0x5DA810` | (bool) -> count | counts items passing `0x428110` (INFERRED) |
| hasKey | `0x5DB030` | (keyId) | log `"cInventory::hasKey(%d)"` (OBSERVED) |
| autoSort | `0x5DB280`, `0x5DB6B0`, `0x5DBEE0` | | "autoSortItemsDumb", "autoSortItems", "autoPutItems" logs (OBSERVED) |
| fixInventory | `0x5DE8E0` | () | drops dangling refs (grid, taskbar sets) (OBSERVED) |

cInventory (manager) functions: **moveItem `0x5DECC0`** thiscall(u16 srcIdx, u16 dstIdx, itemId, ?) `ret 0x10`:
find in src, findFree in dst, removeItem, putItem (OBSERVED, log `"cInventory::moveItem(src=%d,dst=%d, ref=%d)"`).
Item size: `typeMgr 0x428D60` thiscall(typeId, int* w, int* h) `ret 0xC` (OBSERVED).

## 2. Item instances (cItemBase and subclasses)

Classes: `cItemBase` vtable `0x891B0C` (ctor ends `0x5BE26D`, `ret 0x10`), `cWeapon3D` `0x891DE0`, `cArmor3D` `0x891A64`
(OBSERVED RTTI). Items are cObjects; inventory and equipment hold their ids.

| Offset | Type | Meaning | Evidence |
|---|---|---|---|
| `+0x0C` | u32 | object id | OBSERVED (`object.h`) |
| `+0x10` | u32 | type id (`TYPE_WEAPON_*`, `TYPE_ARMOR_*`, ...) | OBSERVED ctor `0x5BE235` |
| `+0x14` | u32 | flags: `0x800` quest item (tooltip `UI_TT_QUESTITEM` `0x5C5612`), `0x2` (not in world map), `0x100000` cleared on equip | OBSERVED reads, meaning of the last two INFERRED |
| `+0x18..+0x24` | | world position data, zeroed when the item goes into an inventory | OBSERVED `0x5D9FFD` |
| `+0x100/+0x104` | vector of 0x82-byte infos | **sockets**: the infos of the items inserted into this one | OBSERVED `0x5BEBCE`, `0x5BEC81` |
| `+0x10C/+0x110` | vector of 8-byte entries | INFERRED effect/visual handles (freed in the dtor `0x5BE2EA`) | |
| `+0x118` | info, 0x82 bytes | **the item's stats** (copied from the type's weapon info at creation, then rolled) | OBSERVED ctor `0x5BE242` |
| `+0x19C..+0x1DC` | | misc, `+0x1DC` float 0.1 | OBSERVED ctor |

Accessors (thiscall on the item):
- **getInfo `0x5BEB30`** (short idx) -> info*, `ret 4`: idx = -1 -> `item+0x118` (after swapping any min/max damage pair that is
  reversed); idx >= 0 -> socket idx's info (OBSERVED).
- **getValue `0x5BEC70`** () -> u32 = `info.+0x08` + the sockets' `+0x08` (OBSERVED). If `item+0x120` (= info +8) is 0 the
  tooltip computes a value with `0x5D3AB0(info, item, 0)` (OBSERVED `0x5C5723`).
- vtable `+0x38` canUse(creatureTypeId) -> bool (class check; cArmor3D logs "ItemType is not a hero!") (OBSERVED `0x54AC1D`);
  vtable `+0x40` (info*, int*) merges sockets into an info copy (INFERRED from the tooltip `0x5C54FD`);
  vtable `+0x44` **tooltip builder** `0x5C5360` (info override*, ..., flags) `ret 0x10` (OBSERVED).

### 2.1 The 0x82-byte info ("weapon info shared", item `+0x118`)

Offsets relative to the info (item `+0x118 + off`). Source: weapon.pak records (§2.2), the tooltip `0x5C5360`, canUseItem
`0x54ABE0`, getInfo `0x5BEB30`.

| Off | Type | Meaning | Evidence |
|---|---|---|---|
| `+0x00` | u32 | type id | OBSERVED in data (record 74 "Metallener Wappenschild" -> 0x4B0, consecutive) |
| `+0x04` | u16 | low byte: hero class mask (0x3F = all six base classes, 0x20 only on "Vampir..." items, 0x10 seraphim-like); high byte: kind bits | INFERRED from data |
| `+0x06` | u16 | flags (0x800F, 0x80CF, ...) | INFERRED |
| `+0x08` | u32 | **base value** (price basis) | OBSERVED `0x5BECDF`, `0x5C581A` |
| `+0x0C`, `+0x0D` | u8, u8 | INFERRED socket count / quality (e.g. 0x203, 0x303) | |
| `+0x14` | u8 | **minimum level** (vs hero `+0x3FE`) | OBSERVED `0x54AC70`, `INVENTAR_MIN_LEVEL` `0x5C64C3` |
| `+0x15` | u8 | min strength (vs stats `+0x10`) | OBSERVED `0x54AC62`, `INVENTAR_MIN_FORCE` |
| `+0x16` | u8 | min dexterity (vs stats `+0x14`) | OBSERVED `0x54AC54`, `INVENTAR_MIN_DEX` |
| `+0x17` | u8 | min charisma (vs stats `+0x1A`) | OBSERVED `0x54AC46`, `INVENTAR_MIN_CHARISMA` |
| `+0x18` | u8 | min endurance (vs stats `+0x12`) | OBSERVED `0x54AC7E`, `INVENTAR_MIN_END` |
| `+0x19` | u8 | INFERRED item level (shown with text 0x42E in the tooltip `0x5C5F90`) | |
| `+0x1A`, `+0x1B` | u8, u8 | required skill id (1-based `SKILL_*`), required skill level | OBSERVED `0x54AC8C`, `0x5C66AD` |
| `+0x1C` | u16[4] | **damage min**: physical, fire, magic, poison | OBSERVED swap pairs in `0x5BEB30`; data ("Giftiges..." = poison in slot 3, "Brennendes..." = fire in slot 1) |
| `+0x24` | u16[4] | **damage max**, same order | OBSERVED |
| `+0x2C`, `+0x2E` | s16, s16 | two more stats (text ids 0x432, 0x433); INFERRED attack speed / attack value | `0x5C6071` |
| `+0x32` | s16[4] | **armor / defense per damage type** (physical, fire, magic, poison) (block with text 0x4AB) | OBSERVED reads `0x5C678E`..; meaning INFERRED (armor items carry values only here) |
| `+0x4A` | 5 x (u16 id, u16) | INFERRED bonus modifiers (ids 0x2xx-0x3xx, `bin\wpmod.bin`) | data |
| `+0x6A` | u16[5] | INFERRED the modifiers' strengths | data |

Full weapon-info record (0x102 bytes, `typeMgr 0x434030` thiscall(typeId) -> record; `0x4340B0` returns record+0x80, the
0x82-byte part): `+0x1C`/`+0x1D` u8 width/height in cells, `+0x1E` u8 weapon subclass (`0x5C3397`, `0x43F29B`), `+0x23` u8 set id
(`"SetName%d"` `0x5C55BC`), `+0x28` German developer name (char[0x58]), `+0x80` the info (OBSERVED code + data).
Type record (0x80 bytes): `+0x18` u16 weapon-info index, `+0x2E` u8 **item category** (`0x426220`, used by the slot table §3.2).

### 2.2 Weapon.pak

Header 0x100 bytes ("WPN\x08", u32 count = 4883), then 4883 records of 0x102 bytes at `0x100 + i*0x102`, same layout as in
memory (OBSERVED: names at `record+0x28` every 258 bytes). This is the offline source for every item type's base stats and
German name (rolled instance values come from the save).

### 2.3 Hero attributes and gold (stats object = hero `+0x3A8`)

| Stats off | Hero off | Meaning | Evidence |
|---|---|---|---|
| `+0x10` | `+0x3B8` | strength | OBSERVED |
| `+0x12` | `+0x3BA` | endurance | OBSERVED |
| `+0x14` | `+0x3BC` | dexterity | OBSERVED |
| `+0x16`, `+0x18` | | INFERRED physical / mental regeneration | |
| `+0x1A` | `+0x3C2` | charisma (also lowers prices, passed to the price formula) | OBSERVED |
| `+0x24` | | u8[8] the hero's skill ids (1-based `SKILL_*`) | OBSERVED `0x582450` |
| `+0x2C`, `+0x34` | | u8[8] base and bonus skill levels | OBSERVED |
| `+0x46` | `+0x3EE` | **gold** (u32; the "anti-cheat field" of RE_NOTES) | OBSERVED `0x6D9930` (buy compare), `0x57FFCF` |
| `+0x56` | `+0x3FE` | level | OBSERVED |

- **getSkillLevel `0x582450`** thiscall(stats; skillId, 0) -> u8 base+bonus (0 if the hero lacks it), `ret 8`. Skill 0x15 =
  `SKILL_Handel` (trade, 1-based) is what the price uses; 0x21 = `SKILL_Schmieden` (OBSERVED uses).
- **addGold `0x57FD80`** thiscall(stats; int delta), `ret 4`: first runs the anti-cheat check (`0x590730`, kills the hero on
  mismatch), clamps to 0..0x7FFFFFFE, adds, plays the gold sound for the local player, then rewrites the XOR copies in the page
  at `[0x182EE5C]` / `[0x182EE60]` (OBSERVED `0x57FDB1`..`0x5800D4`). **Never write gold directly**; always use this (27
  callers: shops, scripts `AddGold`, horse healing ...).

## 3. Equipment

### 3.1 Storage

Equipped items: **creature `+0x1A4 + slot*4`**, u32 item id, 19 slots (0..0x12) (OBSERVED `0x556F5A`, `0x55523F`).
Empty body slots are not 0: unequip puts a "default item" (the bare body part, type from `typeMgr 0x426580(creatureType, slot)`,
created by `0x5FB530`) into the slot (OBSERVED `0x556EBF`..`0x556EE2`). Use `getEquipped` to skip those.

| Function | Address | Signature |
|---|---|---|
| **getEquipped** | `0x556F50` | thiscall(creature; u16 slot) -> id, 0 if empty or a default item (`0x426550`), `ret 4` (OBSERVED) |
| **equip** (`cCreature::equipment_equip`, log `0x9567CC`) | `0x555E00` | thiscall(creature; u16 slot, itemId, bool net) -> previous item id (0 if none / default), `ret 0xC` (OBSERVED) |
| **unequip** | `0x556A30` | thiscall(creature; u16 slot, bool net) -> removed item id, `ret 8`; recomputes stats (`0x5796A0`) and fills the default item (OBSERVED) |
| **canUseItem** | `0x54ABE0` | thiscall(creature; itemId, bool classOnly) -> bool, `ret 8`: item vtable `+0x38` class check; unless `classOnly`, level, strength, dexterity, charisma, endurance and the required skill (§2.1) (OBSERVED) |
| **slotAccepts** | `0x43EFC0` | thiscall(typeMgr; slot, typeId) -> bool, `ret 8` (OBSERVED) |
| autoEquip | `0x555260` | thiscall(creature; itemId, int* inOutPrev, ?) `ret 0xC`: first slot that accepts the item (and is empty and usable unless `*inOutPrev == -1`), then equip (OBSERVED; used by the hero loader `0x605EDE`..) |
| replaceRef | `0x555230` | thiscall(creature; oldId, newId) `ret 8`: swaps an id in all 19 slots (OBSERVED) |
| equipment_reset | `0x555D74` log | (OBSERVED string) |

equip does: for slot 0x12 it only stores the id at `+0x1EC` (the mount) and calls `0x55C640`; else checks the creature may
equip (`0x4266F0`), runs `unequip(slot, 0)`, destroys a default item in the slot (`0x5FBDB0`), stores the id, attaches the model
(`0x4498F0`, `0x44D4F0`, `0x44DD90`), recomputes slot stats (`0x555300`), sends net event 0x11E when `net` and MP
(`0x182EBEC`), removes the item from the world map, and hands an order (vtable `0x89095C`, type 0x107, the slot) to the
creature's vtable `+0x18` (the equip animation) (OBSERVED `0x555E00`..`0x5563A6`). Unequip sends 0x11D.

### 3.2 Slots

Names from the equipment window builder (`0x6B8300`..`0x6BA3BC`, one layout per hero class, control `i` at window
`+0x15C + 4*i` is creature slot `i` - the drop handler passes the control index to equip) and categories from
`slotAccepts`' jump table `0x43F35C` (category = type record `+0x2E`):

| Slot | UI name (OBSERVED) | Accepted category (OBSERVED) |
|---|---|---|
| 0 | `UI_EQ_HELMET` | 0x11 |
| 1 | `UI_EQ_ARMOR` | 6 (and valid type) |
| 2 | `UI_EQ_BELT` | 0x13 |
| 3 | `UI_EQ_BOOTS` | 0x12 |
| 4 | `UI_EQ_GLOVE` (second `UI_EQ_ARMS` in some layouts) | 0x18 |
| 5 | `UI_EQ_ARMS` (bracers) | 0x16 |
| 6, 7 | `UI_EQ_AMULET1`, `UI_EQ_AMULET2` | 0x14 |
| 8..11 | `UI_EQ_RING1..4` | 8 |
| 12 | (taskbar) weapon | 0xD with weapon subclass 0xC or 0xE; types 0x1600/0x1601 only here (INFERRED: ranged / default hand) |
| 13 | (taskbar) weapon / shield | 5 (shield) or 0xD with subclass not 0xC/0xE (+ `0x427E30`/`0x427E80` checks) |
| 14 | `UI_EQ_CANON` (dwarf) | 0x21 |
| 15 | `UI_EQ_SHOULDERR` / `UI_EQ_SHOULDER_R` | 0x15 |
| 16 | `UI_EQ_GREAVES` | 0x17 |
| 17 | `UI_EQ_WINGS` (seraphim) | 0x19 |
| 18 | - | the ridden horse's id (`+0x1EC`), see §5 |

Hirelings equip slots 12 and 13 (INFERRED: 12 for a 2-handed/ranged set, 13 otherwise; `0x6D69C2`..`0x6D6A39`), so the 12/13
split is "the two weapon hands"; the exact rule is open.

### 3.3 What the equipment window does on a drop (`cUI_Equipment` receiveEvent `0x6B6690`, OBSERVED `0x6B6B3F`..`0x6B6D06`)

1. Cursor holds an item (`0x655550`, kind 1); hero = `0x603E30`; hero alive (`0x549080`).
2. `hero.canUseItem(id, 0)`; on failure `canUseItem(id, 1)` picks the complaint sound (8 = requirements, 6 = class).
3. `typeMgr.slotAccepts(slot, item+0x10)`.
4. `old = hero.getEquipped(slot)`; `hero.equip(slot, id, 1)`; clear the cursor; play the type's sound (`0x426220` ... `0x67A7A0`).
5. If `old`: build a cursor entry for it and put it on the cursor (the player then drops it into the backpack).
Click on a filled slot with an empty cursor: `id = hero.unequip(slot, 1)` -> onto the cursor (`0x6B6D86`).

Inventory right-click/use (`cUI_InventoryTab` receiveEvent `0x6C0150`): for a category 0x1D item (horse armor) it equips the
**horse**: `horse = getData(hero.getHorse())` (`0x55C630` returns creature `+0x570`), `horse.canUseItem`,
`horse.unequip(0, 1)`, entry `removeItem`, old item `putItem` back (`0x5DAB10`, `0x5DA280`), `horse.equip(0, id, 1)`
(OBSERVED `0x6C0D92`..`0x6C0E80`). This is the reference sequence for "equip from backpack".

### 3.4 Recipe: equip an item from the backpack (INFERRED from 3.3, all calls OBSERVED)

```
hero = objMgr->hero();  inv = cInventory()->getEntry(hero->id & 0xFFFF, same);
if (!hero->canUseItem(id, false) || !typeMgr->slotAccepts(slot, item->typeId)) fail;
inv->removeItem(id, true);
old = hero->equip(slot, id, true);
if (old) { typeMgr->getSize(oldType, &w, &h); if (inv->findFree(w, h, &cell, 0)) inv->putItem(cell, old, w, h, true);
           else /* drop it: engine event 0x14, see 4.4 */ }
```

## 4. Merchants

### 4.1 Opening a shop and its state

- Talking to an NPC is creature state `+0xFC` = 10, handled by `0x529C60` (switch `0x539833`, table `0x539DE4`) (OBSERVED).
  For a merchant it posts UI events: show `UI_WND_INVENTORY`, show `UI_WND_MERCHANT` (code 0x11), then code **0x2E** sub **8**
  with (+0xC u16 merchant kind, +0x10 merchant creature id, +0x14 1) (OBSERVED `0x52A3C6`..`0x52A51F`).
- `cUI_Merchant` (UI manager `+0x90`, vtable `0x8969B8`, receiveEvent `0x6DBF50`): sub 8 with a new id -> `setMerchant`
  `0x6DBD20` (stores `+0x1D8` merchant id, `+0x1D4` kind, picks colors from table `0x9ECEB0` by `typeMgr 0x4345C0`) and forwards
  the event to its tab control `+0x154` (three `cUI_Merchandise` pages, 512x256, "UI_TABPAGE_WEAPONS/ARMORS/VARIOUS") (OBSERVED).
- show(bool) `0x6DBB70` (vtable `+0x24`): `cInventory::setTrade(merchantId, kind)` = `0x5DEE10` (`+0x23000` = merchant id,
  `+0x23004` = u16 kind), and (0, 0) on close (OBSERVED). `0x5DEE30` returns whether a trade is set (OBSERVED).
- **Is a shop open?** `uiMgr->+0x90` visible bit (`0x75A9B0`), or `[*0xAD4448 + 0x23000] != 0`.

### 4.2 Stock

- Pages = cInventory entries 0x14..0x16 (`cUI_Merchandise +0x154` = its entry; window `+0x3E8` = merchant id the stock was
  made for, `+0x3E4` kind, `+0x3F0` 16 x 8-byte (category, ...) acceptance table filled by `0x43B060` in `0x6DC754`..)
  (OBSERVED).
- Event sub 8 handler `0x6DB57B`: new merchant -> clear (`0x5DC420`, `0x6D9B90(1)`), generate (`0x6DA1A0`, `0x6DA320`), and
  extra random items `0x6D9DE0(typeMgr 0x43B1A0(difficulty *0x182EE44, 1, -1), 0, 0)` while the page has < 8 items; same
  merchant -> refresh a random part (`0x6DA320(n)`) (OBSERVED). Balance keys `MerchantParts`, `SlotProbBonusMerchant`
  (GAME_DATA) tune it (INFERRED).
- **Read the stock** exactly like a backpack (§1.2) on `getEntry(0x14..0x16)`.

### 4.3 Price

**`cInventory::price` `0x5DEE60`** thiscall(cInventory; itemId, bool buy) -> u32, `ret 8` (OBSERVED). It uses the local hero:
`factor = 0x61DB50(0x61D450(0x6021F0(heroId)))` (float, INFERRED region/merchant factor), `trade = getSkillLevel(0x15)`,
then `typeMgr 0x43C350(item.getValue(), heroLevel, trade, factor, charisma, buy)` `ret 0x18` (a log curve; buy and sell branch
at `0x43C3E4`) (OBSERVED). Balance keys `PricePromille`, `PriceDmg:`, `PriceRes:`, `PriceSlot:`, `PriceLevel:`,
`PriceBoni:` (`0x5EF3F9`..`0x5F0B92`) feed the value roll (INFERRED). `0x5BECF0` is a two-creature variant (buyer, seller)
`ret 0xC` (OBSERVED shape; used by trade, INFERRED).

### 4.4 Buy

`cUI_Merchandise` click with an empty cursor (`0x6DADD8`): cell -> `id = page.itemAt(cell, 1)`; if the hero has no room
(`hasSpaceFor`) -> purchase popup mode 4; if `buy(hero, id, 0)` fails -> mode 2 (not enough gold); else mode 1 (confirm, price).
The popup (`cUI_Purchase`, `+0xB4`) answers with code 0x2E sub 1, and `+0xC != 0` = OK -> `buy(heroId, window+0x3DC, 1)`
(OBSERVED `0x6DB263`..`0x6DB28B`).

**`cUI_Merchandise::buy` `0x6D9840`** thiscall(page; buyerId, itemId, bool doIt) -> bool, `ret 0xC` (OBSERVED):
```
buyer = cast<cCreature>(getData(buyerId)); item = cast<cItemBase>(getData(itemId));
typeMgr->getSize(item->type, &w, &h); inv = getEntry(buyerId); price = cInventory->price(itemId, 1);
if (buyer->stats.gold (+0x46) < price) return false; if (!inv->findFree(w, h, &cell, 0)) return false;
if (doIt) { page->entry->removeItem(itemId, 1); buyer->stats.addGold(-price);
            item->getInfo(-1)->flags(+4) &= ~0x200000; [MP: net event 0x16D]; inv->putItem(cell, itemId, w, h, 1); }
return true;
```
A mod can call it directly with `this` = the page that holds the item (`uiMgr+0x90 -> +0x154 -> tab pages`; the page's `+0x154`
entry pointer must be the one that holds the id), or replay its body (all calls above are OBSERVED).

### 4.5 Sell

Two OBSERVED UI paths, both: `price = cInventory->price(id, 0)`, take the item out of the hero, give gold, and tell the
merchant window to take the item with **code 0x2E sub 7, +0xC = item id** (handler `0x6DB465`: if the page's acceptance table
takes the item's category, `findFree` + `putItem` into the page, else `destroy(id, 1, 0, 0)` and log `0x9E16E0`).
- **Shift-click in the backpack while the shop is open** (`0x6C02E6`..`0x6C0435`): `id = entry.itemAt(cell, 1)`;
  `price = price(id, 0)`; `entry.removeItem(id, 1)`; post {code 0x2E, sub 7, id} to `UI_WND_MERCHANT`; `hero.stats.addGold(price)`.
  This is the simplest sell recipe.
- Drag onto the merchant page (`0x6DB153`): window `+0x3E0` = id, popup mode 3; on OK `addGold(price(id, 0))` and the same
  sub-7 event (`0x6DB2A5`..`0x6DB3D2`); on Cancel the item goes back to the cursor.
Shift-click with no window open (`0x6C043F`): if the hero passes `vtable+0x24(0x51, 0)` the item is destroyed for its
`vtable+0x24(3, 0)` value in gold, else engine event 0x14 drops it on the ground (INFERRED meaning of 0x51/3).

UI event object (cEventUI2, vtable `0x890DD0`, base `0x890890`; OBSERVED field use, sizes INFERRED): `+4` code (0x11 show
window / 0x2E shop), `+8` sub, `+0xC`.. `+0x18` params, `+0x38` and `+0x4C` char[0x20] target window name (`"UI_WND_MERCHANT"`)
and a second name (`0xAA453C`, empty); sent with `uiMgr->vtable[0](&event)`. Building one by hand is risky; for a mod the
direct calls (removeItem, addGold, page `findFree`/`putItem` or `destroy`) avoid it.

### 4.6 Recipe summary for a mod

| Goal | Calls |
|---|---|
| Is a shop open, which merchant | `uiMgr+0x90` bit 0; `[*0xAD4448+0x23000]` merchant id |
| List stock | entries 0x14..0x16 grid walk; per item `price(id, 1)` |
| Buy | `page.buy(localPlayerId, id, 0)` to test, then `(.., 1)` |
| Sell | `price(id, 0)`; `heroEntry.removeItem(id, 1)`; `stats.addGold(price)`; page entry `findFree`+`putItem` (or `destroy`) |

## 5. Hirelings and the horse

- `cUI_Mercenary` (vtable `0x8968CC`, receiveEvent `0x6D6C70`; GAME_DATA's "UI_WND_MERC" minimap/portraits window):
  hireling creature ids at window `+0x154 + i*0x194` (OBSERVED `0x6D6772`..`0x6D678C`). Dropping a weapon on a portrait:
  `merc.canUseItem(id, 0)`, `merc.unequip(12 or 13, 1)`, `merc.equip(12 or 13, id, 1)`; taking it back:
  hero entry `findFree` + `merc.unequip` + `putItem` (OBSERVED `0x6D67DD`..`0x6D6F73`). Hirelings use the same cCreature
  equipment (`+0x1A4`).
- Horse: owned horse id = creature `+0x570` (`0x55C630`); the ridden mount = slot 18 (`+0x1EC`, set by `equip(0x12, id)`, which
  also calls `0x55C640`) (OBSERVED). Horse armor (category 0x1D) goes into the **horse creature's slot 0** (§3.3). Bridles
  (`TYPE_HORSEBIT_*`) are used, not equipped (`UI_HORSE_BRIDDLE_USE` in the item tooltip `0x5C6D71`) (INFERRED).
  `cUI_Horse` (`+0xC4`, receiveEvent `0x6BE340`) shows the horse's stats and heals it for gold (`addGold` `0x6BE4EE`) (OBSERVED).

## 6. Other systems noticed

- Chest storage = cInventory entry 0x11; trade = 0x1A (shift-click moves items there with `moveItem`) (OBSERVED `0x6C023D`, `0x6C02A1`).
- `cCreature::pickupItem` `0x558080` (log `"cCreature::pickupItem (%d) byCreature=%d type=%d icount=%d"`, "Take:" lines):
  the pick-up of order 0x113; it calls canUseItem, slotAccepts, findFree and putItem (OBSERVED), so it may auto-equip
  (INFERRED) - a good hook for loot logging.
- `cItemUpgrade::receive_event` (`0x5CD4A2`..): runes / art upgrades applied to a hero (OBSERVED logs).
- Chests: `cChest3D::receive_event` `0x5C98F9`, `dropItems` `0x5C9E10` (OBSERVED logs).
- Potions: `cPotion3D::receive_event` `0x5CB9B3` (OBSERVED log).
- Item category names in German (model selection, `0x5C3130`): Schwert, Dolch, Degen, Saebel, Axt, 2HAxt, Bogen, Armbrust,
  Klingenwaffe, Kettenwaffe, Peitsche, Ring, Amulett, Beinschiene, Guertel, Speer, Keule, Stab, Zaumzeug, Fluegel, Item,
  Pistole, Muskete, Kanone (OBSERVED; a different index than the `+0x2E` category). EWT names table `0x949930`.

## Open questions

- Threading: which thread may call equip/putItem/addGold safely (UI events arrive through `cEngine_receiveEvent` `0x618090`).
- The full category enum (type record `+0x2E`) and the slot 12/13 rule (weapon subclass 0xC/0xE).
- Info fields `+0x04/+0x06`, `+0x0C/+0x0D` (sockets? quality?), `+0x19`, `+0x2C/+0x2E`, the modifier list `+0x4A`/`+0x6A`
  and how they feed the stats (`0x5796A0` recompute, `0x555300` per slot).
- Item quality / unique / set colour: only the set id (record `+0x23`) is located.
- `+0x116C` vector in cInventoryEntry, the cursor entry's `0x5FE720`/`0x5FE870` fields.
- Whether `cUI_Merchandise::buy` can be called on a page that is not the visible tab (it only uses its own `+0x154` entry, so
  likely yes).
- Multiplayer: every `net = 1` call also sends an event when `0x182EBEC` is set; in SP it is harmless.

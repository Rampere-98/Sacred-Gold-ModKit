# Configuration (`SacredBild.ini`)

Before the game starts, a settings window offers the settings below with tooltips: General (resolution, window frame,
frame limit, VSync, UI size, renderer), Advanced (everything else; the controller's buttons are set in game) and HUD layout (`[UI.Layout]`). Play saves the
changed ones to `SacredBild.ini`; Exit quits. "Don't show this window
again" sets `[Launcher] HideSettingsWindow=1`; holding Shift as the game starts shows it anyway.

A controller works the window too: the D-pad or left stick moves between the settings (left and right change a list's
value), A ticks a box, presses a button or opens a list, B closes an open list without changing it, LB and RB switch
tabs, Start is Play. Text fields need the keyboard.

In game, Home opens and closes the ModKit menu over the game (and its menus): every installed mod's settings, applied
at once; "Guardar en SacredBild.ini" writes them to the ini. Esc or Home closes it. It needs `[DDraw] Backend=d3d9`
(the overlay's backend).

| Section | Key | Default | Meaning |
|---|---|---|---|
| Launcher | HideSettingsWindow | 0 | 1 = no settings window before the game starts; holding Shift as the game starts shows it anyway. |
| Display | Width, Height | 0 | Render resolution; 0 = desktop. 1024x768 = unpatched game. |
| Display | Borderless | auto | Main window frame: `auto` = a frame when the window is smaller than the screen, `1` = never (the game's frameless window), `0` = always. The client area is `Width` x `Height` either way. |
| Display | ClipCursor | 1 | Confine the mouse to the game window while it is in the foreground; hold Alt to move it out. |
| Display | FpsLimit | 60 | The game's own in-game frame limit; 0 = off. |
| Display | FpsLimitInactive | 20 | Frame limit while the game is in the background, in game and in the menus; 0 = off. |
| Display | VSync | 1 | Direct3D 9 backend: present on the display's refresh (flip model); 0 = right away, not limited by the refresh rate (blit model). |
| Display | MaxFrameLatency | 1 | Direct3D 9 backend: frames the CPU may queue ahead of the GPU. |
| UI | Scale | 0 | UI scale; 0 = as large as fits the screen height, otherwise a factor (1 = native pixels), capped at that. |
| UI | ScaleMode | InGame | Where `Scale` applies: `InGame` = the in-game UI only; the menus, the full-screen windows in game (options, save/load, character export, map) and the loading screen always fill the screen height. `Full` = those too. |
| UI | LinearFilter | 1 | Bilinear filtering for the scaled UI instead of the game's point sampling. |
| UI | Anchor | 1 | In game, HUD windows placed by `[UI.Layout]`; 0 = all of the UI in the centered canvas. |
| UI.Layout | Taskbar, Chat | 2048,4096 | Where a window's 1024x768 layout goes: X,Y in 0..4096 of the room the screen leaves around it (0 = left/top edge, 2048 = centered, 4096 = right/bottom edge). |
| UI.Layout | Inventory | 0,4096 | |
| UI.Layout | Stats, Equipment, Minimap | 4096,0 | |
| UI.Layout | Portraits, Shops | 0,0 | Party portraits; blacksmith, merchant, combat art master, chest, cube and trade. |
| AutoAttack | Enabled | 0 | Auto attack is on when the game starts; `Key` toggles it either way. While it is on, in the game world (windows such as the inventory may be open), the hero attacks the nearest enemy within `Range` through the game's own attack order, without the mouse, and takes the next one once it dies. Never while the controller drives. |
| AutoAttack | Key | 122 | Virtual-key code of the toggle key (122 = F11); a beep says on or off. |
| AutoAttack | Range | 400 | Enemies within this many screen pixels of the hero; the current one is chased up to 1.5 times as far and kept at least 1.2 s. An enemy the hero gets no nearer to in 4 s (out of reach) is left for 20 s; enemies hitting from afar (ranged) are preferred over those that are not yet near, and those fighting the hero's hirelings count as fighting the hero. |
| AutoAttack | Pause | 700 | Milliseconds without auto attack after the player presses a mouse button (moving the mouse does not pause it). |
| AutoAttack | Pickup | 1 | The auto attack only fights, and only the enemies attacking the hero (or what it rides: a creature's aim, +0x104 its target's id with the target's position after it) and those the player attacked with a click of their own; others are left alone, however close. Of those it weighs the situation as a player would (enemies close by, closing in or hurting the hero first), looks around every few hundred ms, acts on a new aim after a short, uneven reaction time and stays on the enemy it fights. Items are the player's: the game's own collect (A, or the taskbar's collect button) gathers them, and the auto attack waits until the hero is done. `Pickup` improves A: once the game's collect is done (the hero stands still), what is left within `LootRange` is taken one by one with the order a click gives: items the game did not reach (all that Alt shows on the ground, no signposts) chests and other containers, which the game's collect never opens, and the map's fixed things the cursor shows the hand on (magic hiding places, wells, gold: the pick list's ids with bit 31; not runes nor signposts, whose click only opens their text); magic hiding places are not in the pick list (the cursor finds them by tile), so those within `LootRange`/40 tiles are found by their record's tile; loot further than 24 tiles on a tile axis, beyond the game's path finders, is walked to first, used with the same order a click gives. Loot it cannot take or reach is left; should a fixture open its text, it is closed and never taken again. A click, a hit taken or A again ends it. |
| AutoAttack | Seek | 0 | `1`: seek fights, going after any enemy within `Range` as well; `0`: only the enemies attacking the hero and those the player attacked. Also in the ModKit menu (Home), whose fight section also switches the combat arts and consumables (`[Tactics] Arts`, `Consumables`). |
| AutoAttack | AutoCollect | 0 | `1`: collect always: whenever anything the cursor shows the hand on lies within `LootRange` (items, containers, magic hiding places, wells) and no fight is on (no enemy attacking the hero, not hurt in the last 3 s, the auto attack not fighting), A is pressed for the player, the same collect, followed up as with `Pickup`; then 3 s before the next. `0`: only when the player presses A. Never in the player's way: only 2.5 s after their last click, with the hero standing for 1 s. Opened containers, used fixed things, items the hero stood at without taking and loot that opened a window (the storage chest in town) are never gone for again; loot not reached is tried once more a minute later. Also in the ModKit menu (Home). Works with the auto attack on or off. |
| AutoAttack | LootRange | 800 | Screen pixels around the hero within which `Pickup` looks for what the game's collect left. |
| AutoAttack | Repeat | 200 | The attack order is given again this often while the target lives, as a held click does (each order one blow: at 1000 the hero struck about once a second, far slower than holding the button); 0 = once per target. |
| AutoPotion | Enabled | 1 | Drinks healing potions by itself (through the game's usePotion, which tells whether one was left; the Space key if it is not found) once the hero misses as much health as a potion heals, learned from the health each potion gave. With none left it says so in the ModKit menu and only tries again every 3 s in an emergency. While a combat art's effect is being measured, only emergency drinks. Only in the running game world, never while the chat line or a dialog is open. |
| AutoPotion | FirstPercent | 50 | Before a potion's healing is known: drink once this percent of health is missing. |
| AutoPotion | EmergencyPercent | 25 | Always drink at or below this percent of health. |
| AutoPotion | Interval | 1500 | Milliseconds at least between two potions. |
| Tactics | Arts | 1 | With the auto attack, combat arts by their use: heals only when the hero misses health, area arts only with three or more enemies around the target (two when hard pressed), single blows at the target (see `SingleAlways`), buffs as a hard fight starts and every `BuffEvery` seconds. A cast counts only when the hero really casts (its action, cCreature +0x150, turns 9); one the game refuses (the art still recharging) teaches the art's recharge time, kept in `[Tactics.Learned]`, and the art is not tried again before it. A cast art runs to its end, no attack order cuts it short. Never while riding. An art is selected with its slot key (6-0) and cast with the game's own right-click order (0x105); the mouse stays free and the player's selected slot comes back afterwards. |
| Tactics | SingleAlways | 1 | `1`: the single-target art is the main attack, cast at any target not finished off easily (within 3 s) as soon as it is recharged. `0`: only at enemies worth it (1.8 times the usual health, over 8 s to kill with the plain attack) or when hard pressed. |
| Tactics | ArtRange | 260 | Screen pixels within which area, single and aimed buff arts are cast at the target (0: at any distance), so that the hero does not run off casting at enemies far away. |
| Tactics | BuffEvery | 45 | Seconds between two casts of a buff art during fights (5-600). |
| Tactics | Retreat | 1 | `1`: with health at or below `RetreatPercent`, no healing potion left and enemies close, the hero walks 8 tiles away from them (up to 12 s, until health is 20 points higher, a potion is found or no enemy is close). |
| Tactics | RetreatPercent | 25 | Health percent at which `Retreat` starts (5-60). |
| Tactics | Slot1 .. Slot5 | auto | Each art slot's use: `auto` learns it from what the art does (health regained = heal, several enemies hurt = area, the target hurt = single, nothing seen with the target close, four times = buff; what it learned is kept in `[Tactics.Learned]` for the next start), or `heal`, `area`, `single`, `buff`, `off`. |
| Tactics | Consumables | 1 | Potions other than the hero's healing ones (those are `[AutoPotion]`'s), drunk through the game's usePotion, which tells whether one was left. |
| Tactics | UndeadDeath, Mentor, Antidote, Concentration, HealHirelings | 1 | Undead Death (Q) when hard pressed by three or more; Mentor (W) in big fights (four enemies or a strong one), every 2 minutes at most; Antidote (E) when health drains with no enemy about; Concentration (R) when the arts wanted are recharging; a healing potion to the hirelings (B) when one that stays with the hero is below 45 % health. |
| Render | TextureBudgetMB | 0 | Texture memory the game may keep loaded; 0 = the game's value, at least 256. |
| Render | Batch | 1 | Merge the world view's draw calls. |
| Render | BatchNoClip | 1 | Merged draws skip Direct3D 7's software clipping; the GPU clips. |
| Render | BatchVertexBuffer | 1 | Merged draws go through vertex buffers instead of user memory. |
| Render | BatchModels | 1 | 3D model draws go through the batcher too (vertex buffers, merged where possible). |
| Render | GpuSkinning | 1 | Characters and their shadows skinned (animated) in a vertex shader instead of by Granny on the CPU; `Backend=d3d9` only. |
| Render | OffscreenPoses | 4 | Skeletons of characters not drawn in the last frames are posed every Nth frame (staggered) instead of every frame; one drawn after all is posed before it is drawn. 1 = every frame. Needs `GpuSkinning`. |
| Render | GroundMesh | 1 | The ground's tiles and blend layers kept in vertex buffers per sector, built once as they come into view, and drawn by a vertex shader in a few draws per frame; the game no longer rebuilds and draws every tile each frame. `Backend=d3d9` only. |
| Render | WideSectors | 1 | Load and draw the 5x5 map sectors around the camera (320x320 tiles) instead of 3x3, so a wide view zoomed out shows ground, water and objects in its corners instead of black. |
| Render | BatchGround | 1 | The ground's quad batcher hands its textures and quads to the batcher in one call instead of three device calls per quad. |
| Render | BatchSprites | 1 | The sprite batcher (water tiles, objects) hands its texture and quads to the batcher in one call instead of two device calls per flush (nearly every sprite). |
| Render | RecordIndex | 1 | Hash index (gtl::flat_hash_map) in front of the game's tile/object record caches. |
| Render | WarmFileCache | 1 | Read `PAK`, `World` and `mp3` once at startup in the background (low I/O priority, at most a quarter of the free memory, sound paks first), so the game's synchronous reads while playing come from Windows' file cache; a read from the idle SSD took ~8 ms on the render thread. |
| Render | FastMath | 1 | Faster replacements for the game's x87 math helpers with the same results: `__ftol` (float to integer) with SSE3's `fisttp`; the teleporter's ripple without x87 `fsin`/`fcos`. |
| Render | SoundLock | 1 | The sound system's lock as a user-mode lock (SRW lock) instead of a kernel mutex; the render thread takes it for every sound command, several per object and frame. |
| Render | AsyncAnimation | 1 | Advance Granny animations on a worker thread, overlapping the start of the frame. |
| Render | AnimationThreads | 0 | Threads that sample Granny's animation controls in the advance, split by skeleton; 0 = automatic (CPU cores - 2, at most 4), 1 = Granny's own walk on one thread. |
| Render | Atlas | 1 | Copy small textures into shared pages so more draws merge. |
| Render | AtlasPageSize | 8192 | Atlas page size in texels (clamped to the GPU limit, halved if the GPU refuses it). |
| Render | AtlasPages | 2 | Pages per texture format; the least recently used one is reused when full. |
| Render | AtlasMaxTextureSize | 512 | Larger textures are used directly. |
| Screenshot | Format | png | Print Screen saves `Capture\shotNNNN.png`; `jpg` = JPEG (quality 95) instead. |
| Net | Relay | 1 | Hosted games: the gameserver announces on every adapter and answers `Hosts` subscriptions. |
| Net | Port | 2105 | Host side: UDP port of the relay, the UDP game connection and the matchmaker. |
| Net | Hosts | | Hosts whose games are listed even without broadcasts: IPv4 addresses or names, comma-separated, optional `:port`. |
| Net | NoDelay | 1 | Game connection without Nagle's algorithm in both data flow modes (the game: LAN only). |
| Net | JoinTimeout | 30 | Seconds a joining player has to send its first message to a gameserver Sacred started (the game: 5). |
| Net | Udp | 0 | Game connection over UDP when the other side has it on too (hosting: accept it); otherwise TCP. |
| Net | Prefer | IPv6 | Joining over UDP a host with IPv4 and IPv6 addresses: `IPv6` or `IPv4` is tried first, the other one as well after a second without an answer. |
| Net | Matchmaker | | Matchmaking server, name or address with optional `:port` (default 2107); empty = none. |
| Net | Publish | 1 | Hosting with a `Matchmaker`: the game is listed there. |
| DDraw | Backend | d3d9 | `d3d9` = SacredBild's own Direct3D 9Ex backend; `chain` = the ddraw below. |
| DDraw | Chain | `SacredBild\DDrawCompat.dll` | `Backend=chain`: ddraw loaded behind SacredBild; empty = system ddraw. |
| DDraw | D3D9 | | `Backend=d3d9`: `d3d9.dll` to use (e.g. DXVK), relative to the game folder or absolute. Empty or not loadable: a `d3d9.dll` next to the exe, then Windows' own. |
| DDraw | MediaFoundation | 1 | Movies through Media Foundation; always on with `Backend=d3d9`, 0 = the game's own player with `Backend=chain`. |
| Debug | D3DStats | 0 | Frame statistics, hitches and texture memory in `SacredBild.log`, and the world view's time by pass (`passes:` lines). |
| Debug | Profiler | 0 | Sampling profiler; writes `SacredBild-profile.txt` every 15 s. |
| Debug | ProfilerIntervalUs | 500 | Sampling interval. |
| Debug | UiTrace | 0 | Scroll Lock logs one UI frame's draws (`UiTrace:` lines: position, UI frame, calling game code) and popups set during the next 5 s. |
| Debug | CrashDump | 1 | Minidump next to the exe on a crash: 0 = off, 1 = stacks and the memory they point to (small), 2 = all memory (for the game's globals; hundreds of MB). |
| Debug | SkinCheck | 0 | Compares Granny's CPU skinning of characters with per-vertex bone weights rebuilt from its meshes (`Skin check:` lines every 10 s): groundwork for skinning on the GPU. |
| Debug | AnimationCheck | 0 | With `AnimationThreads`: every 2 s the controls are sampled on one thread and on the threads from the same state and compared (`Animation threads:` lines); a difference keeps the walk on one thread. |
| Debug | MovieFallback | 0 | Movies always through the fallback (DirectShow into a system memory surface), as without the Media Engine. |

The controller's settings and bindings (`[Controller]`, `[Controller.Bindings]`) are in [Controller](Controller.md#settings).

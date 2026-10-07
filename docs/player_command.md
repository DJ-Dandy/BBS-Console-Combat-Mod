# PL::CPlayerCommand / PL::CCommandPlate (BBS FM PC/Steam): reference for the MP / menu mod

All addresses are rva (image base 0x140000000); `[x]` = where the claim can be checked. `cmd` = CPlayerCommand*,
`P` = CCommandPlate*, `pl` = PL::CPlayer*, `PD` = PlayerData (0x10f9eeb0), `G` = u32 at 0x10f9ee48.
(?) = inferred, not proven. L2D names (SetControl 1a7520, SetNodeFrame 1a7d30, Show 1a5b30 ...) are from l2d_api.md.

## 0. Corrections to the brief
- `cmd+0x298` is **not** a shotlock plate. It is the plate (type 5) of the context/reaction prompt that replaces
  Attack (Talk, Open, counter prompts...), fed from `cmd+0x248` [236f50, 236fd0, 237120]. Shotlock is opened by the
  player/pad code, not by this class (section 3, pad tests).
- `cmd+0x1a8` is the **command-style level** 0..2 (number of stacked style plates), not a menu level [2368de..236904].
- `cmd+0x68` is `[0x10f9ee40]` = CPlayerManager*; the pad object is `mgr+0x58`, its flag word `mgr+0xf8` [2302c0, 272160].
- A plate's "ready" test is only `P+0x64 >= 100` (+ two special flags); flag 0x2 is a display/reload flag and is
  set one frame **after** the use [2063c0, 206971].
- A deck item's use count is decremented when the item takes effect [2624e0 -> 239540], not at the button press.
- FUN_140239540(cmd) is `if (cmd+0x90) tail-jump 206cf0(cmd+0x90)` [239540]; 206cf0(P) does the decrement.
- Pad bits are the PS layout: 0x10 up, 0x20 right, 0x40 down, 0x80 left, 0x1000 triangle, 0x2000 circle,
  0x4000 cross, 0x8000 square; 0x400/0x800 = L1/R1 (?), 0x100/0x200 = L2/R2 (?).

## 1. CPlayerCommand layout (size 0x360: `operator new(0x360)` in 21b450; ctor 22fe30 -> init 2302c0)
vtable 0x647b80: [0] 230280 dtor (body 230040), [1] 2363d0 update, [9] 111e30 CTreeTask exec. Parent task = player
(ctor arg), plates are child tasks of cmd [203d60]. Singleton `[0x10f9ed40]`, also `pl+0x390` [21b450].

| off | type | meaning [evidence] |
|---|---|---|
| 0x20 | f32 | task dt of this frame (60 Hz ticks) |
| 0x60 / 0x64 | u32 | flag words, tables below |
| 0x68 | ptr | CPlayerManager* (pad = +0x58) [2302c0] |
| 0x70 | ptr | CPlayer* [22fe30] |
| 0x78 | COMMAND | command the player accepted / is executing; set only by 236f10 |
| 0x80 | COMMAND | request queued for the player; zeroed at 236640 in every update that gets that far |
| 0x88 | COMMAND | copy of the last deck command used [233d5b] |
| 0x90 | ptr | plate of the last deck command used (item decrement target) [233d70, 239540] |
| 0x98 | int | L2D bc01_00 layout 5: command-gauge window, normal style [236000] |
| 0x9c | int | gauge window currently shown (= 0x98, or a style / dl_01 instance; 0 during a change) [237f20] |
| 0xa0, 0xa4, 0xa8 | int | outgoing gauge window, fading old style window, seq 0x642 style-change effect [237f20, 238ba0] |
| 0xac, 0xb0, 0xb4 | int | seq 500, seq 0x44c (deck scroll arrows, shown when >= 2 plates), seq 0x76d [236000] |
| 0xb8 | int | shot_02 layout 1: shotlock lock counter [236000, 234490]; 0xbc/0xc0 its data handles |
| 0xc4 | int | bc01_00 layout 0xb: D-Link list window (control 1 opening, 0 open, 2 closed) [2388a0, 2334f0] |
| 0xc8 | int | seq 0x590: "D-Link available" prompt [2388a0] |
| 0xcc | s16[10][4] | digit UV rects for the lock counter [236000] |
| 0x120, 0x124 | f32 | gauge window position [236000] |
| 0x130 / 0x138 | f32 | command gauge target / displayed value, 100 per level (0..300) [231720, 231f20] |
| 0x134 | f32 | gain damping, x0.5 per gain while displayed < target [231720] |
| 0x13c, 0x140, 0x144 | f32 | decay hold timer; decay per tick (0.2); hold reload value (240.0) [231f20, 2302c0] |
| 0x148, 0x14c, 0x150 | f32 | gain factor mode 0 (1.0), fixed gains of modes 1, 2 (0) [231720] |
| 0x154, 0x158, 0x15c | f32 | gain multiplier at level 0 / 1 / 2 (1.0, 0.8, 0.55) [231720, 2302c0] |
| 0x160 | s16[16] | style affinity counters, index = style id - 0x151 [231900, 231d30] |
| 0x180 | COMMAND | shotlock command (PD+0x90; 0x12a while style 0x173) [234fa0] |
| 0x188 / 0x190 | COMMAND | current style (0x151 = normal; byte +2 = level) / next style candidate [237f20, 231d30] |
| 0x198 / 0x1a0 | COMMAND | active D-Link command / active illusion command (0x18b..0x18f) [237f20 mode 5, 238b40, 238660] |
| 0x1a8 | int | style level 0..2 |
| 0x1ac | s16 | gauge-window state: 2 normal, 3 style applied, 4 reverting, 5 D-Link applied, 7 reset [237f20, 234aa0] |
| 0x1ae | s16 | scripted D-Link step (1 pending, 2 done) [238440, 23641b..2364e9] |
| 0x1b0, 0x1b8, 0x1c0 | P | Attack plate (type 1, COMMAND at 0x818788) and style plates of level 1, 2 (type 4, 0x818790/98) [2302c0] |
| 0x1c8 | COMMAND | current attack-name command (1, or 2..0x10 / 0x2e by style) [230a90] |
| 0x1d0 / 0x1d8 | P / COMMAND | finisher plate (type 3) and its command [2313b0] |
| 0x1e0 | P[8] | deck plates (type 2); index i <-> working deck 0x8187b0[i], saved slot 0x8187a8[i] [235b50] |
| 0x220, 0x222 | u16 | number of deck plates (two copies) [235b50] |
| 0x224 | u16 | array index of the selected plate (recomputed by 235290) |
| 0x226 | s16 | list position of the cursor row; always 0 (< 0 would mean last) [2302c0, 237710, 235290] |
| 0x22a | s16 | last scroll direction (+1 up, -1 down, 0) [231d00, 233dc3] |
| 0x22c | s16 | index where the last use / manual move happened; auto-skip stops there; init = PD+0x30 [233d69, 235b50] |
| 0x22e | s16 | shortcut target index (from save option, FUN_14041dd00) [233cc8] |
| 0x230 / 0x238 | s16 / COMMAND | slot replaced by the "command change" status (-1 none) / its original [237ae0] |
| 0x232 | s16 | status follow-up counter (?) [23a130] |
| 0x234 | f32 | reload time factor, recomputed by 235140 |
| 0x240..0x290 | COMMAND[11] | action commands: 0x248 current prompt, 0x250 air dash, 0x258 ground dash, 0x260 avoid slide, 0x268 combo slide, 0x270 guard, 0x278 dash ability, 0x280 guard ability, 0x288 turn ability, 0x290 blow ability [235480, 237420] |
| 0x298 / 0x2a0 | P / f32 | prompt plate (type 5) / prompt timeout (120.0) [235480, 236fd0] |
| 0x2a8 | P[21] | D-Link list plates (type 6) [235900] |
| 0x350, 0x351, 0x352 | u8 | D-Link entries (two copies), array index of the selected entry [235900, 235020] |
| 0x354, 0x356 | s8 | D-Link cursor row (0), last list scroll direction [233200] |
| 0x358 | f32 | copy of the D-Link gauge value (`gauge+0x60`) [2366ac, 233200] |
| 0x35c | u32 | copy of PD+0x10 (enchant flags) [235140, 2392d0] |

**cmd+0x60**: 0x1 HUD parts hidden [234170, 2388a0] - 0x2 cleared by the ctor [22fe30] and set nowhere in the
code read (22fe30..2395c0 and the player code using pl+0x390); if set, 231d00 and the auto-skip do nothing, so
a mod can own it [231d00, 233ba0] - 0x8 manual scroll in progress [233db7] - 0x200 scroll in progress
[2375d0] - 0x400 a deck command was used this update [233d4d] - 0x800 scroll arrows shown [233a00] - 0x1000
follow-up window, set/cleared by the player [218a60, 21d6e0] - 0x2000 request in +0x80 not yet taken [236e99] -
0x4000 finisher / prompt command issued, waiting for its end [233f70, 236baf] - 0x8000 player has accepted a
command (+0x78 valid) [236f10] - 0x10000 close the prompt plate [236be5, 236c10] - 0x20000 finisher available
[236794, 236acb] -
0x40000 prompt shown [236f50] - 0x80000 D-Link list scrolling [233200] - 0x100000 list is in "end link" mode
[2388a0] - 0x200000 D-Link gauge full (written by the gauge, see player_gauge.md) - 0x400000 deck disabled
(grey base plates) and 0x800000 deck sealed, both recomputed every update from G bits 0x10000 / 0x20000, both
forced in player state 9 [236500..23654c] - 0x1000000 jump to a slot in progress [233c98, 237ae0] -
0x2000000 seal latch [233a00] - 0x4000000 restore saved selection on first update [235b50] - 0x8000000 HUD-off
latch [234170] - 0x10000000 D-Link list open [2388a0] - 0x20000000 lock counter shown [234490] - 0x80000000
battle-look latch / "re-skin plates" [2320e0, 237710].

**cmd+0x64**: 0x2 style change (or D-Link/illusion change) pending until `pl+0x318 & 0x20000` clears; gauge
gain is ignored meanwhile [236913, 231918] - 0x4 / 0x8 gauge moved / gained this frame [231f20, 231720] - 0x10
revert to normal style when the gauge reaches 0, 0x20 revert after a finisher (both set in ctor) [22fe30, 2367e3,
233f70] - 0x40 gauge frozen this frame = last frame's 0x8000 [2363d6] - 0x80 D-Link (or scripted link) selected /
active [233422, 238440] - 0x100 gauge full [231f20] - 0x800, 0x2000, 0x4000 D-Link upgrade sequence [2391b0,
2334f0] - 0x8000 freeze request written by player action states [253a50, 24e760, 2595e0, 2674d0, 353630] -
0x10000, 0x20000 set by the scripted link [238440] - 0x40000 forced-style mode (`mgr+0x1b1 != 0`) [22fe30, 231f20]
- 0x80000 only read (dtor: do not save the selection) [230040] - 0x100000 drop the whole deck at the next update,
0x200000 deck incomplete (arena command loss) [237c30, 23656a] - 0x400000 illusion form [237e20] - 0x800000 one
slot replaced by status [237ae0].

**G = [0x10f9ee48]** bits used here: 0x1 in battle (0x2 / 0x4 = enter / leave requests) [26bf60, 2ddcd0]; 0x2000
HUD off; 0x10000 deck disabled and 0x200000 D-Link disabled, both written by event script [2a9d60]; 0x20000 deck
sealed, set by the status code [241e00, 23b3a0, 23e4e0], cleared by 23a130; 0x80000 styles disabled (no affinity
count, no candidate) [231900, 231d30]; 0x100000 focus disabled; 0x10000000 arena command-loss rules [23e4e0,
273420, 233c74]. `[0x10f9ee90]` != 0 while a style resource set is loading: attack plates are held at 99 %
[27b821, 27c230, 206a8f].

## 2. CCommandPlate (size 0x70, vtable 0x646010, ctor 203d60, setup 2047b0, update 204880)
`+0x20` dt, `+0x30` type (1 Attack, 2 deck, 3 finisher, 4 style plate, 5 prompt, 6 D-Link entry), `+0x31` state,
`+0x33` array index, `+0x34/+0x38` x,y, `+0x3c/+0x40` slide target, `+0x44/+0x48` colour / target colour,
`+0x4c` L2D handle, `+0x50/+0x52` layout id / main node id, `+0x58` COMMAND*, `+0x60` flags, `+0x64` percent,
`+0x68` slide timer, `+0x6c` list position (0 = cursor row), `+0x6d` row in placement table 0x818300,
`+0x6e` target list position.
Deck plate states: 0 idle, 1 selected, 2 sliding, 3 used (one frame), 4 arrived on the cursor row [206930].
List position -> table row [205350]: 0 -> 0xb (cursor row, y 212), 1 -> 0xc (y 197), n-1 -> 0xa (y 227), 2 and
n-2 -> 0xd / 9 (off-screen), others row 0 (hidden): three plates are visible.
Flags: 0x1 hidden, 0x2 reloading / unavailable, 0x4 selected, 0x10/0x20 slide direction, 0x40 fading, 0x100
forced hidden (HUD off, list open) [204880 tail], 0x200 battle look (set in ctor), 0x400 finisher uses dl_01,
0x800 item, 0x2000 greyed [204450], 0x4000 illusion command 0xf2..0xf6, 0x40000000 attack plate held at 99 %,
0x80000000 L2D not created yet [204880 top].
Deck plate L2D = bc01_00 layout 3 [205350]: control 0 selected+ready, 3 unselected+ready, 4 reloading; node 0x5d
= reload bar, frame = percent; node 0x5a category icon (seq 0x20 attack, 0x21 magic, 0x22 item), node 0x5b item
count text, node 0 name.
- 2063c0 `COMMAND* Use(P)`: if `+0x64 >= 100` and !(flags & 0x40000000) and !((flags & 0x4000) and
  cmd+0x64 & 0xa00000): `+0x64 = 0`, state = 3, return `+0x58`; else 0. Only caller 233d40.
- 205f70 `Select(P)`: only for the cursor-row plate in state 0/4 while the deck is not sealed: control 0 (or 4 +
  bar frame if flag 2), flags |= 4, state 1; returns 1 / -1 / 0.
- 2060d0 `Refresh(P)` (edx unused): flags &= ~4, state 0, re-place by list position, control 4 + bar if flag 2
  else 3. This is the game's own "redraw" after every availability change.
- 2061e0 `Slide(P, dir, f32 time)`: target position = (pos + n + dir) % n, state 2; 2044c0 performs the slide.
- 206680 `SetPercent(P, f32)`; 203ea0 battle/field re-skin; 2040e0 replace the command of a plate.
- 206690, 2067a0, 206810, 206b80, 232d50, 233e80, 2330e0 have no callers (out-of-line copies of inlined code).

## 3. Per-frame update 2363d0 (annotated)
1. `+0x64`: bit 0x40 := old bit 0x8000, bit 0x8000 cleared [2363d6..2363f5].
2. 234170: HUD-off handling (G & 0x2000): hides every window; returns 1 -> the update ends (no input at all).
3. 234490: shotlock lock counter (+0xb8) while `pl+0x304 == 0x18`; returns 1 while shown -> end.
4. Scripted D-Link pending (`+0x1ae == 1`, +0x64 & 0x80): wait for pl+0x318 & 0x20000 to clear, then
   237f20(cmd,5) [23641b..2364f0].
5. Recompute +0x60 bits 0x400000 / 0x800000 [236500..23654c].
6. 2334f0: D-Link list and D-Link change handling (section 6); returns 1 -> `+0x64 |= 0x40`, end.
7. `if (+0x60 & 0x2000) return` [23655b]: a queued request has not been taken by the player yet.
8. Arena deck loss: 232820(cmd,0,1,-1,0.4) when +0x64 & 0x100000; clears 0x200000 once the plate count equals
   the number of non-empty PD deck slots [236565..23662f].
9. `+0x80 = 0` [236640]; 231f20 gauge tick: after the hold timer `+0x130 -= dt*0.2`; displayed value chases the
   target by 2*dt; sets +0x64 |= 0x100 and returns 1 when displayed >= (level+1)*100; skipped while 0x100 is set.
10. Style logic (skipped when +0x60 & 0x1000 or a COUNTER-category prompt is shown) [23665a..236b49]:
    D-Link mode (0x80): gauge copy <= 0 or deck-loss -> end link (2371d0(cmd,0), next = 0x151, 237f20(cmd,4));
    else full -> finisher available. Normal: displayed == 0 -> 232c10 (revert to normal style if flag 0x10);
    pending (0x2) -> wait, then apply: 237f20(cmd,3) + 230a90 [236836..2368cf]; full (0x100) -> **2368d4**:
    `level < 2 && cmd+0x190 != 0x151` -> style change [2368ff], else finisher [236a82].
11. 234aa0: gauge window state machine, pins node 0x46 to `+0x138`, calls 2320e0 (battle/field re-skin of the
    plates on G bit 0x1 edges); returns 1 while the revert animation runs -> end.
12. Input (below). 13. If `+0x80 != 0`: `+0x60 |= 0x2000`, `pl+0x570 = dt` [236e99]; `+0x60 &= ~0x10000`.

Other members: 232df0 square-button action commands; 233a00 deck; 233f70 finisher button + finisher end;
2376b0 push Attack/style plates to "covered" state (2058c0/205a10); 2313b0 build the finisher plate (PD+0x88 for
normal style, ids 0x12..0x20 for styles 0x152..0x160, D-Link / illusion variants) via 206400; 230a90 set the
attack name of the style; 2371d0(cmd, level) set style level, clamp the gauge to level*100 and close the prompt;
237f20(cmd, mode) apply a style / window change (3 style, 4 back to normal, 5 D-Link deck, 7 reset);
231d00 cursor advance; 237710 rebuild deck plates; 232c10 revert to normal style.

### Pad tests (all take `mgr+0x58`; all need pad flag bit 0 and lock timer `pad+0x54 <= 0`)
Primitives: 0ecd90(mask) = held & newly pressed ([0x8f64930] & [0x8f64934]); 0ecd40(mask) = pressed-or-repeat
[0x8f6493c]; 0eccd0(mask,-1) = all held. State written by 0ed040.
| function | mask | used for |
|---|---|---|
| 272860 | u16 [0x8221c0] "confirm", default 0x4000 cross (setter 54e4c0), pressed | Attack [236de3, 236e53], finisher [233f70], prompt [236b97] |
| 272c20 | 0x1000 triangle, pressed | deck command [233d12], D-Link confirm / end link [2333f1], follow-up [236c92] |
| 272760 | 0x8000 square, pressed | action commands [232df0], blow ability [236d10] |
| 272580 | 0x10 up -> +1 (cursor to previous index), 0x40 down -> -1 (next index); repeat; 0x210 / 0x140 in control scheme 2; sign flipped by save option bit 0x40000 | deck scroll [233d96], D-Link list scroll [23343a] |
| 272d30 | 0x80 left, pressed | jump to shortcut slot [233c67]; close D-Link list [2334f0] |
| 272cd0 | 0x20 right, pressed | open D-Link list [238902] |
272580 / 272cd0 / 272d30 return 0 while L1 or R1 is held in scheme 0 (`pad+0x20`, `pad+0x24` hold timers).
Shotlock: pad update 272160 sets pad flag 0x40 when flag 0x20 is set and 0x400 + 0x800 are both held; flag 0x20
is set by the player when the shotlock id > 0x11b and cmd+0x80 == 0; then `pl+0x31c |= 0x40` and 2176b0 calls
283aa0 (state 0x18) [21f9f0, 21773c]. cmd only supplies the command (234fa0) and the counter (234490).

### Input section of 2363d0, three exclusive modes
- Prompt shown (+0x60 & 0x40000) [236b66..236c6a]: confirm -> `+0x80 = *205860(P298)`, flag 0x4000; else 232df0 + 233a00.
- Follow-up window (+0x60 & 0x1000) [236c79..236d5f]: triangle re-queues `{+0x78 id, +0x7a level}` for ids 0x5c, 0x62,
  0x63, 0x6d, 0x6e, 0x6f, 0x75 **without touching any plate**; square queues +0x290 if pl+0x324 & 2; then 233a00.
- Normal [236d64..236e8a]: 233f70, 232df0, 233a00, then, if no finisher is available and +0x80 is still 0, Attack:
  plate `+0x1b0[level]` stable (2047f0) && confirm pressed && `pl+0x318 & 0xc0 != 0` (and pl+0x354 != 0x18e at
  level 0) -> `+0x80 = *2058e0(P)` (level 0) or `*205a30(P)`.

## 4. The deck button: 233a00
Order inside 233a00(cmd): return 0 if the D-Link list is open; if sealed: latch, hide arrows, return 0.
`P = 235290(cmd)` = plate on the cursor row. Pending jumps (0x4000000 restore, 0x1000000 shortcut) scroll by
2375d0. If P is not stable (2047f0: state 1 or 4) -> 205f70(P). Otherwise:
1. **Auto-skip** [233b53..233bd7]: if `(+0x60 & 0x208) == 0x200` (automatic scroll, not manual), `+0x230 == -1`,
   `P+0x64 < 100` and `+0x224 != +0x22c` -> 2375d0(cmd, +0x22a == -1 ? -1 : 1, 10.0), return. So after a use the
   cursor keeps stepping over plates that are not ready and stops on a ready plate or back on the used slot
   (the "everything used" case: one full lap, no sound, cursor returns).
2. Clear 0x208, `+0x22a = 0`; clear 0x400 (and start a jump if a status slot exists) [233bdc..233c05].
3. Only if `cmd+0x188 id == pl+0x354` (style not changing) and no COUNTER prompt [233c08..233c43]:
   2388a0 (D-Link open, section 6); left -> shortcut jump (SE 1) unless arena rules;
   **triangle** [233d0a..233d70]: `272c20` && `(pl+0x318 & 0xc0) == 0x40` && `240a80(pl) == 0` (not in the first
   part of state 0xc) && `cmd+0x80 == 0` && `rax = 2063c0(P)` != 0 -> `+0x60 |= 0x400; +0x80 = +0x88 = *rax;
   +0x22c = +0x224; +0x90 = P`.
   **Not ready: 2063c0 returns 0 and nothing else happens** (no sound, no flag, no queue) [233d48 je].
4. If 0x400 is not set: up/down -> flag 8, `+0x22a = dir`, 2375d0(cmd, dir, 10.0), SE 1, `+0x22c = +0x224`.
`pl+0x318 & 0xc0`: cleared every frame by 21f9f0, set by the player state: idle 0x40 [219200], action cancel
windows 0x80 or 0x40 [21c140: 0x40 only for MOVE commands and attack phase 5]. Deck needs exactly 0x40; Attack
accepts both.

After the use (next plate update, 206930 state 3 [206971]): `flags |= 2`, 2060d0(P), state 0,
`231d00(cmd, -1)` = if !(cmd+0x60 & 2): `+0x22a = -1`, 2375d0(cmd,-1,10.0) -> every plate 2061e0(P,-1,10.0),
`+0x60 |= 0x200` (cursor moves to the next index; 2375d0 needs >= 2 plates). Then the reload part of 206930
[206a1f..]: non-item with flag 2: `f = 235140(cmd, COMMAND*)`; if `f != 0`: `+0x64 += 1ce550(COMMAND*, f, dt)`;
< 100 -> bar frame; >= 100 -> (0x40000000 and [0x10f9ee90] != 0: hold at 99) else `+0x64 = 100` and, once
state <= 1, `flags &= ~2`, 2060d0(P). Item with flag 2: count != 0 -> 100, clear flag, redraw.
- 1ce550: `n = rl_seconds * 60 / dt * f`; returns `100 / n` (100 if n < 1) [1ce550]; rl = CommandParam+3.
- 235140 (factor, also stored in cmd+0x234): returns **0 while `pl+0x318 & 0x20000`** (style change: reload
  paused); else `(100 - bonus) * 0.01` with bonus = 10 x level of ability 0x1cf Attack Haste (category 1) or 0x1d0
  Magic Haste (category 2) via 221900, +25 if ability 0x1d9 Reload Boost and HP <= max/4; x0.67 if enchant bit 1
  (0x1a6 QuickReload) of PD+0x10; x2 if status `pl+0x324 & 0x200` (set at 23fc14, damage handler 23e4e0
  reaction 8, and by the uncalled 241fd0). HP / max HP = player vfuncs +0x78 / +0x80 (?).

How the request reaches the player: cmd sets +0x80 and 0x2000 [236e99]; the player's next update 220550 ->
21f9f0 -> **2172f0** [21fb3d]: reads `cmd+0x80`, sets a request bit in `pl+0x31c` by category (ATTACK 0x4,
MAGIC 0x10, ITEM 0x200000, FRIEND/NETWORK/DLINK-category/illusion 0x800000, MOVE 0x8, GUARD 0x100, COUNTER 0x200,
FINISH/SHOOTLOCK 0x20, TALK 0x800), `pl+0x34c = COMMAND`, `pl+0x3b0 = CommandParam row`, and 236f10(cmd, COMMAND)
(`cmd+0x78`, flag 0x8000, clears 0x2000). The state function then dispatches with 2176b0 (fresh) or 21c140
(chained). Request bits are wiped at the next 21f9f0 (`pl+0x31c &= 0xf113f000`), so a request the state did not
dispatch is lost while the plate is already reloading.

### Every writer of P+0x64
ctor 203d60 / reset 203e60 (0); 2047b0 (initial value: PD+0x28[slot] at room load [235b50] or the 237710
argument); 205350 at L2D creation (attack 100 -> 99 while [0x10f9ee90]; item 0 / 100 by count); 203ea0 (items,
battle/field switch); 2040e0 + 206680 (status slot replacement, 100); 2063c0 (0); 206930 (reload, clamp, 99
hold); 206c10 (item restock, 100); 206cf0 (item count 0 -> 0, else 100).
### Every reader
2063c0 (>= 100); 206930; 233a00 auto-skip (< 100); 2388a0 (selected plate == 100 chooses control 0/4 of the
D-Link prompt +0xc8); bar frame in 203ea0, 2040e0, 205350, 205f70, 2060d0, 2061e0, 206cf0; saved as s8 to
PD+0x28 by 230040 (dtor; skipped when +0x64 & 0x600080), 234c90 (save sync from 285640), 237710, 230e90, 232820.
### Flag 0x2
Writers: 206930 (set in state 3; cleared when reload ends / item has stock), 205350, 2040e0, 203ea0, 206c10,
206cf0; type 6: 2051a0, 204880 (by D-Link gauge), 205ee0. Readers: 205f70, 2060d0, 2061e0, 206930 (deck);
205a60, 205bc0, 205cf0, 205ee0 (D-Link). 2063c0 does not read it.
### Other things that change availability
- Rebuild 237710(cmd, deck|NULL, f32 pct, mode, n): mode 0 -> every plate = pct; mode 1/2 -> PD+0x28 values.
  237c10 / 2392b0 = (cmd,NULL,0,0,0): saved deck reloaded **at 0 %**; callers: "HP to 1" attack [241c90,
  23e4e0 case 0x38] and the camp menu after a deck edit [4216c0]. D-Link deck: 237f20 mode 5 -> pct 100; the
  normal deck percents are parked in PD+0x28 and restored (mode 1) when the link ends [237710, 237f20 mode 4].
- Seal: status `pl+0x324 & 0x80` (241e00) sets G 0x20000 -> cmd+0x60 0x800000: selection removed, 233a00 inert,
  D-Link refused; reload still runs. Status 0x40 (241aa0 -> 237ae0) replaces a random slot by command 0xe3.
- Arena: 232820 drops commands as prizes, 230e90 re-adds a picked one at 100 [273420].
- Room change: dtor 230040 writes PD+0x28[slot] and PD+0x30 (selected index); ctor reads them [235b50].
  285060 zeroes PD+0x28..0x30; leaf 285940 sets all eight to 100 (arena modules 34acd0..35eb10 only).
  Death / continue: no explicit reset found in 26cd80 / 26d1f0 / 240ac0 (?); plates come back from PD+0x28.
- HUD off: cmd update stops, plate updates (reload) continue [234170, 204880].

## 5. Life of a deck command on the player side (map)
- Start (all call 21ff60(pl, state), 21f9b0(pl, id) [`pl+0x312` id, `pl+0x316` previous id, `pl+0x3b0` param row],
  `pl+0x5e0 = category`, 21d060(pl,3), 21e490(pl, COMMAND) which starts the animation):
  ATTACK 229e80 (state 0x10, update 224310), MAGIC 26a120 (0x11, 2674d0), ITEM 262400 (0x12, 261e30),
  FRIEND / NETWORK / DLINK-category / illusion 25d3a0 (0x13, 2595e0), finisher 2591c0 (0x14, 253a50),
  MOVE 2430c0 (0x15), guard 25e8f0 (0x16), counter 239b20 (0x17), shotlock 283aa0 (0x18), talk 2523a0 (0x1b).
  Callers: 2176b0 (index 0) and 21c140 (chain index `pl+0x5a0`). **"A deck command really started" = entry of
  229e80 / 26a120 / 262400 / 25d3a0 with `pl+0x34c` id == `cmd+0x88` id** (rcx = pl, dx = chain index).
  If 21e490 returns 0 the starter calls 21c510 at once (command lost, plate already spent).
- End: 21c510(pl, ...) (back to neutral) or 21d060(pl, mode other than 3/4) (interrupted, e.g. damage 23e4e0):
  both zero `pl+0x34c` and call 236f10(cmd, 0) -> `cmd+0x78 = 0`, flags 0x8000/0x2000 cleared.
- Item effect: 2624e0(pl) from the item state [261f95, 261faf] (and 226760): ids 0xbc..0xc4 -> 239540(cmd)
  (use count), heal `pl+0x4a2 * param+0xa / 100`, focus param+0xc, x (ability 0x1ce + 10)*10 %; ids 0xc5..0xd2
  (ice cream) -> 239540 + 237c70(cmd, param+0x14, param+0x16) which forces a style.
- Command gauge gain: **231900(cmd, f32 damage, COMMAND hitCmd /*r8, by value*/, s16 kind)** from the hit
  handlers CPlayer vfunc 21b9e0 [21bb2e], CWeapon 293d40 [293e5a], 2d2400, 2d6380, 2fd320, 2fd720. It adds
  `CommandParam[hitCmd.id]+4` (fill) through 231720(cmd, fill, 0), gives 2 focus (208290), bumps the affinity
  counters +0x160 by the hit command's type / category / attribute / sub-category, then 231d30 picks the candidate.
  So the gauge fills **per hit**, by the fill value of the command that hit, not when a command is used.
  231720: x level multiplier, x1.5 with enchant 0x1bb GaugeBoost, min 1.0 per call, resets the hold timer;
  ignored while +0x64 & 0x40. D-Link styles gain through 230c80 (mode 3). 232330 / 2327d0 reset / subtract.
- Candidate style 231d30: level 0 -> styles 0x151+i, i < 7, counter >= 5 and PD+0xc bit i (i = 0 always);
  level 1 -> i = 7..15, must beat counter[0]; otherwise 0x151 (= finisher). `mgr+0x1b1` forces a style.
- Style change trigger: 2368d4..2368f9 (gauge full, level < 2, candidate != 0x151) -> [2368ff] level++,
  `+0x64 |= 2`, `pl+0x318 |= 0x20000`. Player: 2176b0 tail [2179d7] -> 28a950(pl,0) (state 0x19, update 286380,
  which copies cmd+0x190 to `pl+0x354` and finally clears pl+0x318 & 0x20000). cmd then applies it [23684a]:
  237f20(cmd,3) (`+0x188 = +0x190`, new gauge window), 230a90, gauge = level*100, counters cleared.
  238ba0 / 238d40 are out-of-line copies of the two branches.
- Finisher: [236a82] flag 0x20000, 2313b0 builds plate +0x1d0. 233f70: confirm button -> `+0x80 = *206650(P)`,
  flag 0x4000; player 2172f0 -> bit 0x20 -> 2591c0. When flag 0x8000 drops, 233f70 removes the plate and either
  reverts to normal style (flag 0x20 and not D-Link/illusion: 2371d0(cmd,0), 237f20(cmd,4)) or keeps the level.
- Revert when the gauge empties: 2367c8 -> 232c10. End of everything: 238590 (reset, via 28b340 / 237e20).

## 6. D-Link
- Build: 235900(cmd, list) makes one type-6 plate per entry of the 21-entry list `0x10f9ed50` (copy of
  PD+0xf0) whose COMMAND flag byte has bit 7; nothing if `pl+0x318` bit 31; 2051a0 creates the L2D (layout 10)
  hidden with flag 2. 239120 / 235ac0 refresh the list from PD.
- Open: **2388a0(cmd)** from 233a00 [233a58, 233c4c]. Refuses (hides +0xc8, returns 0) if G & 0x200000, no
  entries, `pl+0x324 & 0x80`, `cmd+0x64 & 0x600000`, or sealed. Right (272cd0) [238902] -> list mode: sets the
  "LinkEnd" entry (2042f0), `+0x60 |= 0x10000000 | 1`, shows +0xc4 (control 1), dims the gauge window, SE 7,
  **returns 1**. Not pressed: shows the prompt +0xc8 when the gauge is 100 or +0x60 & 0x200000, returns 0.
- While open (2334f0, +0xc4 control 0): 233200 = select / navigate: `+0x358` = gauge; entries get flag 2 when
  `+0x358 < 100` (except id 0x189) [204d9a]; up/down -> 205cf0 on every entry (SE 1) [233458..2334c4]; **triangle** with
  `cmd+0x78 == 0` and `(pl+0x318 & 0xc0) == 0x40` -> **205ee0(P)** [233415]: needs !(flag 2) and id > 0x162,
  returns COMMAND* -> `cmd+0x64 |= 0x80`, `+0x80 = COMMAND`. 2334f0 then [2338b6..233934]: `+0x190 = +0x80`,
  `+0x64 |= 2`, `pl+0x318 |= 0x20000 | 0x800`, level/finisher reset, list closed. In "end link" mode triangle
  queues id 0x189 and reverts to normal [23330b..2333c5]. Left [2337e9], G 0x200000, +0x64 & 0x200800 or the
  seal [233994..2339b0] close the list.
- Start: player 2176b0 -> 28a950(pl,0) -> state 0x19 (286380: 20d510(gauge, cmd) starts the drain, heals); when
  pl+0x318 & 0x20000 clears, 2334f0 calls 237f20(cmd,5): `+0x188 = +0x198 = +0x190`, dl_01 window, 232370 builds
  the link deck into 0x10f9ee00, 237710(cmd, deck, 100.0, 0, 0). Scripted links: 28afe0 / 28b120 -> 238440.
- Mod check: detour 2388a0 and return 0 (after `Show(cmd+0xc8, 0)`) -> the list can never open, so no D-Link
  can be chosen; to also stop a list that is already open, make 205ee0 return 0 (commit point of a start).

## 7. Items in the deck
- Item = `CommandData[id]+1 == 3`: plate flag 0x800 set at L2D creation [205350] or on replacement [2040e0].
- Uses = COMMAND.level byte (`P+0x58 -> +2`) = PD deck slot byte `PD+0x4a + slot*8`, slot = 0x8187a8[P+0x33]
  (235130). Max per slot = CommandParam+3 (Potion 5, Hi-Potion 4, ...). Count text node 0x5b.
- Press: normal 2063c0 path; the plate is ready again in the next update if count != 0 [206b15].
- Consume: 2624e0 -> 239540 -> 206cf0(P): count--, write PD; **if not in battle** (flag 0x200 clear) refill at
  once with `41d640(slot, 0)`; count 0 -> flag 2, control 4, `+0x64 = 0`.
- 41d640(slot, mode) moves stock from the inventory entry (`[0x10fb5280] + idx*10`) into the slot up to the
  max, writes PD and the save work, returns the new count.
- Restock: leaving battle (G bit 0x1 clears) 234aa0 -> 2320e0 -> 203ea0(P, 0) for every plate (41d640 + ready);
  entering battle 203ea0(P, 1). Item obtained outside battle: 3297b0 -> 206c10 on every plate [2aa5f4];
  2394f0(cmd) = 206c10 on all plates [34d7e9]. 41d570 rebalances stock in menus.
- At ctor a non-item level above CommandParam+5 is zeroed; item counts are kept [2302c0].

## 8. Hook recommendations (bytes = first 16 at the site)
**(a) "use plate P?"**: call site **233d40**, `e8 7b 26 fd ff | 48 85 c0 74 2d 48 8b 00 81 4b 60`. Only the call's
rel32; no RIP-relative data. At the call: `rcx = rsi = P`, `rbx = cmd`, `rdi = 0x140000000`, `ebp = 0`. Redirect
the rel32 to mod code: id = `*(u16*)*(P+0x58)`, level = byte +2, deck index = `*(s8*)(P+0x33)`, item = `P+0x60 &
0x800`, ready = `*(f32*)(P+0x64) >= 100`. Veto: return `rax = 0` (exactly the vanilla "not ready" path: nothing
happens). Allow: tail-call 2063c0.
Reached only when triangle was pressed this frame and the player can accept (section 4.3). Detouring 2063c0
itself is worse: its first instruction is RIP-relative (`f3 0f 10 05 ec 61 4e 00 0f 2f 41 64 77 2f 8b 41`).
Follow-up presses [236c92] and Attack / finisher never pass here.
**(b) No reload after a use**: call site **206a6e**, `e8 dd 7a fc ff | f3 0f 58 43 64 0f 2f 05 35 5b 4e` (the
comiss at 206a78 is RIP-relative; do not overwrite past the 5-byte call). `rcx = COMMAND*`, `xmm1 = factor`,
`xmm2 = dt`, `rbx = P`, `rdi = cmd`; result `xmm0` = percent added this frame. Return 100.0 = reload done,
0.0 = frozen (plate keeps the reloading look), anything else = custom speed. Not reached while 235140 returns 0
(style change; sibling site **206a4f** `e8 ec e6 02 00`, rcx = cmd, rdx = COMMAND*, xmm0 out) nor for items.
What the player sees with 100.0: the used plate runs the vanilla state-3 step (flag 2, 2060d0, cursor advance
231d00 -> all plates slide for 10 ticks), `P+0x64` becomes 100 in the same call, but flag 2 is only cleared and
the plate redrawn once its state is <= 1 again [206acb], i.e. after the slide; during the slide it keeps control
4 with the bar frame it had when 2061e0 ran (0). Pre-writing `P+0x64 = 100.0` in (a) right after an allowed use
makes that bar full instead. With no cursor advance there is no slide and the plate is ready in the same call:
set `cmd+0x60 |= 2` (see the flag table; tested by 231d00 and the auto-skip) or NOP **20698d**
(`e8 6e b3 02 00`, the call to 231d00).
**(c) Force non-item plates unavailable** (cosmetic + auto-skip; the real gate is the veto in (a)). Per frame,
e.g. from a detour of 206930 (`48 89 5c 24 08 57 48 83 ec 20 | 48 8b 3d ff 83 d9 10`: 10 position-independent
bytes, then a RIP-relative mov; rcx = P; called only from 204bbf in 204880 for type 2 with a live L2D), or by
walking `cmd+0x1e0[0 .. *(u16*)(cmd+0x220)-1]`. Consider only `P+0x30 == 2`, `!(P+0x60 & 0x800)`,
`!(P+0x60 & 0x80000000)` and **`P+0x31 < 2`** (a sliding plate is not redrawn at the end of its slide unless it
lands on the cursor row [2069d7], so wait until it is idle):
- block: if `!(P+0x60 & 2)`: `P+0x64 = x` (0..99, e.g. 0), `P+0x60 |= 2`, call 2060d0(P). Hold it with (b)
  returning 0.0 while the mod flag is set (plates that were already reloading freeze where they are).
  Effects: 2063c0 fails, the auto-skip treats the plate as reloading, PD+0x28 stores x on room change.
- restore: let (b) return 100.0 (or the vanilla value): 206930 itself sets `P+0x64 = 100`, clears 0x2 and
  0x40000000 and calls 2060d0 as soon as the state is <= 1. By hand: same three writes + 2060d0(P).
- look: 2060d0(P) is the game's refresh for any deck plate (control 4 + bar if flag 2, else 3). It also
  deselects; the cursor-row plate is re-selected by 233a00 -> 205f70(P) in the next cmd update (control 0 ready
  / 4 reloading), unless the deck is sealed or the D-Link list is open. To skip that one frame call 205f70(P)
  right after 2060d0(P) when `P+0x6c == 0`. Direct L2D alternative: SetControl(P+0x4c, 4) +
  SetNodeFrame(P+0x4c, 0x5d, pct, 0) for unavailable; SetControl 0 (cursor row) / 3 (others) for ready.
- do not use the seal (cmd+0x60 0x800000 / G 0x20000): it is recomputed every frame and blocks items too.
**(d) Forbid D-Link**: entry **2388a0**, `40 53 48 83 ec 30 | f7 05 98 65 d6 10 00 00 20 00` (6 relocatable
bytes, then a RIP-relative test). rcx = cmd; return eax: 1 = list opened (233a00 and the update then return),
0 = not. Mod: if the flag is set, `Show(*(int*)(cmd+0xc8), 0)` and return 0. Optional second gate: entry
**205ee0** (`40 53 48 83 ec 20 48 8b d9 8b 49 60 f6 c1 02 75`, position independent; rcx = entry plate; return
rax = 0 to refuse the start). Scripted links (238440) and illusions (237e20) do not pass either.
**Style-change prompt (later work)**: 2368f2 `66 39 ab 90 01 00 00 | 0f 84 83 01 00 00` (no RIP-relative data).
Taking the `je` gives the finisher instead; jumping to 236b49 with +0x64 bit 0x100 left set holds the gauge
full (231f20 does not decay while 0x100 is set) until the mod lets the vanilla branch run. Candidate id is
`*(u16*)(cmd+0x190)`.

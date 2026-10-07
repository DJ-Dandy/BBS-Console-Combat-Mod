# Gauge::CPlayerGauge (BBS FM PC/Steam) - reference for an MP-bar mod

All addresses are RVAs (image base 0x140000000). `g` = CPlayerGauge*, `h` = `g+0x8c` (L2D handle).
Everything below was read from the decompilation/disassembly; items marked (?) are inferred, not proven.

## 0. Quick facts / corrections to the brief
- vtable 0x6464f0: [0] 0x2080a0 scalar-deleting dtor, [1] 0x209f20 update, [2] 0xdbc70 nop,
  [3] 0x208380 **empty** (`ret`; drawing is done by the L2D manager, not by the gauge),
  [8] 0xdbc50 Kill (sets task flag 0x8000), [9] 0x111e30 CTreeTask exec (calls [1], then children).
- Object size 0xc8 (`operator new(0xc8)`). Base class chain: CTask -> CTreeTask -> CPlayerGauge.
- Singleton pointer: `DAT_150f9ec60` (rva 0x10f9ec60), written by ctor 0x207a40, cleared by dtor.
  Same object as `player+0x398`.
- The gauge layout is created with `FUN_1401a4fe0("gauge_01.l2d", 2, 0, 0)` (file name + layout
  INDEX 2 = p_gage), not 0x1a4ec0. Layout 3 `p_gage_alrt` is **never instantiated** (no other
  creator of gauge_01 layouts except "e_gage" by name in 0x207670/0x208d90; no "p_gage" string in exe).
- PlayerData (0x10f9eeb0) fields the gauge really uses: +0x1c base max HP (u16), +0x42 max-HP bonus
  (s16), +0x1e HP, **+0x22 focus (0..100)**, **+0x24 D-Link gauge (0..100)**. It never reads +0x20
  or +0x34. Max HP = min(+0x1c + +0x42, 180) (`FUN_1402854f0`).
- HP/focus/D-Link are **not polled** from PlayerData per frame. Init copies them in; afterwards the
  player pushes HP (`FUN_14020b9e0`) and the gauge itself is the live store for focus (`g+0x68`) and
  D-Link (`g+0x60`). They are written back to PlayerData by the dtor and by `FUN_140285640` (save sync).

## 1. Object layout
| off | type | meaning |
|---|---|---|
| 0x00 | ptr | vtable |
| 0x08 | int | CTask id in parent's list |
| 0x10 | ptr | **parent task = PL::CPlayer*** (set by `FUN_140111830(this, player)`); the only back-pointer. Gauge code itself uses `DAT_150f9ee40+0x118` instead |
| 0x18 | u16 | task flags: 0x8000 killed, 0x1 no-update, 0x2 no-draw, 0x4 run while paused, 0x8 real-time dt, 0x1000 tree task |
| 0x1a | u16 | bit0 = "update ran this frame" |
| 0x1c | int | task list id (-1) |
| 0x20 | float | **dt** of this frame (game-frame units), 0x24 carry |
| 0x28 | int | pause group (0) -> `FUN_1401ee230` |
| 0x30..0x57 | | CTaskList of children (vtbl, allocator 0x38, head 0x40, tail 0x48, count 0x50); empty |
| 0x58 | int | 0 (unused) ; 0x5c float 1.0 (unused) |
| 0x60 | float | D-Link gauge value 0..100 (displayed directly) |
| 0x64 | float | D-Link pending delta (added by `FUN_140208280`; negative while draining) |
| 0x68 | float | focus value 0..100 (displayed directly) |
| 0x6c | float | focus pending gain (added by `FUN_140208290`) |
| 0x70 | float | focus value saved when "consume mode" (shotlock) started |
| 0x74 | int | focus ghost-bar state: 0 none, 1 consuming, 2 ghost (node 0xdd) fading out |
| 0x78 | float | illusion: current time ; 0x7c float initial time |
| 0x80 | s16 | illusion: current count ; 0x82 s16 initial count |
| 0x84 | u32 | **flags** (see below) |
| 0x88 | int | **visibility state: 0 = created/not yet shown, 1 = shown, 3 = hidden by HUD-off** |
| 0x8c | int | **L2D instance handle of p_gage** (constant for the object's life) |
| 0x90 | float | appear delay (8.0, counts down by dt) |
| 0x94 | float | damage-bar hold timer (30.0 after an HP change) |
| 0x98 | int | sound handle of the looping low-HP alarm (SE 0xf), 0 = none |
| 0x9c | int | resource handle "gauge_01:1" (face texture slot; released in dtor by `FUN_1401a7270`) |
| 0xa0 | s16 | HP (target, green ring) |
| 0xa2 | s16 | displayed "damage" HP (red ring), chases 0xa0 |
| 0xa4 | s16 | max HP (clamped 0..0x230) |
| 0xa6 | s16 | D-Link panel: bitmask of slots already announced |
| 0xa8 | int | **second L2D handle**: D-Link panel ("dl_00.l2d"/"dl_03.l2d" layout 1), 0 = none |
| 0xac | float | D-Link elapsed time |
| 0xb0 | 8 bytes | D-Link command info (u16 id, byte 0xb3 slot mask) ; 0xb8, 0xc0: two 8-byte slot entries |

Flags `g+0x84`: 0x2 damage ring (node 0x16) visible; 0x4 low-HP alert active; 0x100 D-Link gauge
full/charged; 0x200 D-Link active (draining); 0x400 skip drain this frame (set by player action
states 0x245450, 0x246f10, 0x247420, 0x248650, 0x24dae0, 0x24e1a0, 0x2506e0); 0x800 D-Link disabled;
0x1000 D-Link forced full (command 0x173); 0x8000 illusion mode; 0x10000 focus MAX; 0x20000 focus
consume mode; 0x100000 focus disabled.

## 2. Lifecycle
- `new`: 0x21b4d3 (`mov ecx,0xc8; call operator new`), ctor call at **0x21b4e8** (`rcx`=mem,
  `rdx`=player), stored to `player+0x398` at 0x21b4f5. All inside `FUN_14021b450(player)`, guarded
  by `player+0x73f == 0` (player slot index; only the local player, slot 0, gets a gauge).
- `FUN_14021b450` is called only from `FUN_140220340` (0x2203cc / 0x2204fc) = slot 3 of the CPlayer
  FSM sub-object vtable (0x646ef8), i.e. once when a player entity starts, on every room load.
- ctor `FUN_140207a00`: CTreeTask ctor with parent=player, sets vtable, `DAT_150f9ec60 = this`,
  **calls `FUN_140209070(this)` at 0x207a4a (its only caller)**, then caches "bc01_00.l2d" rects and
  "num_01.l2d"/"num_01:0" handles into 0x10f9ec68/6c.
- `FUN_140209070` (init): zero flags/state; `0xa0=0xa2=PD.HP`; `0xa4=maxHP`; `0x60=PD[0x24]`;
  `0x68=PD[0x22]`; creates the instance (0x20917c, `eax`=handle stored at 0x209186) with
  `DAT_148f88028=0` => **created invisible**; `FUN_1401a7600(h,5,0)` (priority 5); sets ring frames;
  loads face texture by character (`table 0x818cb0[FUN_140219dc0(player,0)]`) into "gauge_01:1";
  applies D-Link-disabled (`DAT_150f9ee48 & 0x200000` or `player+0x324 & 0x80`) and focus-disabled
  (`DAT_150f9ee48 & 0x100000`) looks; `0x88=0`, `0x90=8.0`.
- Destruction: dtor 0x2080a0 runs when the parent player is deleted (CTreeTask dtor `FUN_140111a80`
  deletes children; `~CPlayer` 0x216030 also calls gauge->Kill). That happens on **every room
  change**: `FUN_1401ec110` (map exit) calls `FUN_1401a5900` (destroy ALL L2D instances) and then
  `FUN_1401127d0` (destroy all task lists). So at dtor time on a room change `h` is already dead
  (stale handles resolve to NULL, every `FUN_1401a****` wrapper is then a no-op returning 0).
  Dtor writes HP/maxHP/focus/D-Link back to PlayerData (only if HP > 0), destroys 0xa8 and 0x8c,
  stops the alarm SE, clears 0x10f9ec60/68/6c.
- `FUN_1401a5900` is also called by the death/continue sequences `FUN_14026cd80`/`FUN_14026d1f0`
  (CPlayerManager state 4/3), 0x2b05f0, CMissionFailed 0x3ab1d0, CMgRrManager 0x3c9720: the gauge
  object survives but its instance is gone. A mod's own instance dies there too.
- Only one CPlayerGauge can exist: one global, only slot-0 player creates it. Mirage Arena remote
  players (`FUN_14026ab60`, slots >0) get none. D-Link and Illusion forms reuse the same object
  (D-Link adds the 0xa8 panel; illusion re-skins node 0x1c). World map (mode 0x11) and character
  select create no CPlayerManager/player, hence no gauge. Command Board not checked (?).

## 3. Per-frame update `FUN_140209f20(g)` (vtable[1])
Called from CTask exec 0x111e30 only if `FUN_1401ee230(g+0x28)==0` (pause masks 0x10f9ebe0/ebf0/ec00,
group 0) and task flags & 0x8001 == 0. It reads no PlayerData.
1. **HUD-off** (`DAT_150f9ee48 & 0x2000`): if state != 3: state=3, `FUN_1401a5b30(h,0)`, same for
   0xa8, clear flag 0x4, stop alarm SE, hide CFriendGauge instances (0x10f9ec90). Return (every frame).
2. Leaving HUD-off (state==3): state=0, 0x90=0, `FUN_1401a5b30(h,1)` (+0xa8), `FUN_14020b5f0(0)`.
3. Damage ring: if 0xa2 != 0xa0: show node 0x16 (flag 0x2); if 0xa2 < 0xa0 snap up; else when timer
   0x94 <= 0: `0xa2 -= (0xa2-0xa0)/10 + 1` (per call, not dt-scaled); frame(0x16). When equal: hide
   0x16. Then `0x94 -= dt`.
4. Appear: if state==0 and `player+0x300 > 0` (player active): `0x90 -= dt`; at <= 0: state=1,
   `FUN_1401a5b30(h,1)`, then alert on (`FUN_14020ae00(g,1,h)`) if `0 < HP <= maxHP*0.25`, else alert off.
5. Focus (skipped if flag 0x100000), see section 5.
6. D-Link (see section 5), then the 0xa8 panel bookkeeping (destroyed when its out-animation ends).

**HP formula** (0x2091b2, 0x209217, 0x20a0ed, 0x20ba3f, 0x20bbca; consts 0x6468a4 = 470.0, 0x6ec5bc = 180.0):
`frame = (float)(int)(value * 470.0f / 180.0f)` with value = max HP -> node 0x15 (background ring),
HP -> node 0x17 (green), damage HP -> node 0x16 (red). 470 = layout length, 180 = HP cap.
Setters: `FUN_14020b9e0(g, hp, instant)` (HP; sets 0x94=30.0, re-evaluates alert),
`FUN_14020bb80(maxhp)` (uses the global), `FUN_14020b050(g, hp, dmgType)` = SetHP + window shake
(node 0x14 state 5/6/7, then back to 4 or 0). Player side: `FUN_14021fd80(player,hp,maxhp)`
(`player+0x4a0` HP, `+0x4a2` max HP, capped 0xb4).

**Low-HP alert**: `FUN_14020ae00(g,on,h)` only does `FUN_1401a7db0(h,0x14,4|0)` and
`FUN_1401a7db0(h,0x18,4|0)` (window + face animation state 4 = looping flash), sets/clears flag 0x4
and starts/stops alarm SE 0xf (handle in 0x98). **The instance is not destroyed or re-created and
`g+0x8c` never changes.** A second instance held by a mod needs no handle switch; it can read flag
0x4 if it wants to flash too.

## 4. Show / hide
Mechanisms, in order of how often they matter:
1. **`DAT_150f9ee48` bit 0x2000 (rva 0x10f9ee48) = "cockpit off"**. Checked by every gauge class
   (CPlayerGauge 0x209f28, CEnemyGauge 0x209606 (also bit 0x400000), CFriendGauge, CHitGauge 0x209ad3,
   CPointInfoGauge 0x20a824, CLockonMarker 0x213640, CPlayerCommand 0x234170/0x2371d0, minimap
   0x26b600). Setter: **`FUN_14026c4b0(int off)`** (20+ call sites; off=0 is ignored while
   `FUN_140353610()` != 0). Callers: event/cutscene script tag 0x50444767 in `FUN_1402a9d60`
   (call at 0x2aaa76, argument taken from the script), player cinematic action states 0x1c/0x20
   (`FUN_1402521d0`, `FUN_140252580`; restored in 0x251630/0x2516a0), Npc module
   0x302340..0x302d92, arena/net 0x34d030, 0x352420, 0x3540f0, 0x354280, 0x35df80, minigames
   0x3af960/0x3b3fb0 (CMgRbPlayer), 0x3ea650, 0x3ed170 (CFruitBallMain). Direct writers: death
   sequences 0x26cd80/0x26d1f0 (`|= 0x2000`, later `= 0x2000`), manager ctor/dtor (`&= 0x60000000`).
2. **Gauge state `g+0x88`**: 0 right after creation / after HUD-off ends (invisible until
   `player+0x300 > 0` and the 8.0 delay elapsed), 1 shown, 3 hidden. The actual switch is the L2D
   instance visible bit: `FUN_1401a5b30(h,flag)` -> CD2Ctrl byte +0x87 bit 0x04; read it with
   **`FUN_1401a6060(h)`** (returns 0 for a dead handle as well).
3. **Menus (CCamp 0x3f06e0, CTermMenu 0x436410, also CReport/CTrinity/CGameHelp restore paths)**
   do not touch the gauge. They pause task group 0 (`FUN_1401ee4d0(0xfffeefff,0)`, so 0x209f20 stops
   being called) and set L2D pause level `DAT_148f8802b` (rva 0x8f8802b) = 1, then
   `DAT_148f88020` (rva 0x8f88020) = 0 once the menu is up. `FUN_1401a4b10` (per frame) computes
   `DAT_148f88024[grp]` (rva 0x8f88024) = (dt[grp] != 0) || DAT_148f88020[grp]; `FUN_1401a4aa0` only
   updates/queues-for-draw instances whose group byte (CD2Ctrl+0x86) is enabled. The gauge is group 0,
   so it vanishes while a menu is open with its visible bit still set.
4. No gauge at all: `DAT_150f9ec60 == 0` (between rooms, world map, title, char select).
5. D-Link / illusion transformation does not hide the gauge by itself (only via 1. if a cutscene
   state sets the flag). Not found: any `FUN_1401a5ae0` call or node-level hide of the whole window.

**"Is the normal HUD visible now" test for a mod** (all four):
`g = *(0x10f9ec60) != 0` && `(*(u32*)0x10f9ee48 & 0x2000) == 0` && `*(int*)(g+0x88) == 1`
(or `FUN_1401a6060(*(int*)(g+0x8c)) != 0`) && `*(u8*)0x8f88024 != 0`.
If the mod's instance is created in L2D group 0 (set `DAT_148f8802a` (0x8f8802a) = 0 and
`DAT_148f88028` (0x8f88028) = initial visibility just before the create call; both are reset every
frame), condition 4 is applied by the engine automatically and the mod only has to mirror the
visible bit: `FUN_1401a5b30(modH, FUN_1401a6060(h))`. Use `FUN_1401a7600(modH,5,0)` for the same
draw priority/list as the gauge.

## 5. Focus and D-Link animation
L2D calls: `FUN_1401a7d30(h,node,float frame,0)` set frame; `FUN_1401a7db0(h,node,state)` select one
of the 8 animation states of the node's sequence; `FUN_1401a5b80(h,node,flag)` show/hide node;
`FUN_1401a6790(h,node)` get state; `FUN_1401a6740(h,node)` get frame (`FUN_1402088c0` = frame of 0x1b).

**Focus** (0xdc = frame art "012_focus_00", 0xde = fill "012_focus_01", 0xdd = ghost copy of the
fill, node id 0 = "012_focus_02" label):
- Normal: state 0 of 0xde, `frame = g+0x68` (0..100, no scaling). Gain: `FUN_140208290(g,int)` adds
  to 0x6c; each update moves `min(dt, pending)` from 0x6c to 0x68 and sets the frame (0x20a341..).
- MAX: when 0x68 >= 100 (const 0x6ec5b4): 0x68=100, 0x6c=0, flag 0x10000, `FUN_1401a7db0(h,0xde,4)`
  (state 4 = 800-frame loop starting at frame 90 = the MAX glow). Same in init and `FUN_140208820`.
- Consume mode (shotlock, flag 0x20000): `FUN_14020ab70(g,mode,ratio)`: mode>=1 first call saves
  0x70=focus, `FUN_14020ad00(g,0)` (state 0, clears 0x10000, sets 0x20000), 0x74=1, shows ghost 0xdd at
  frame 0x70 with colour 0xffffffff; later calls set `focus = ratio*0x70*0.01`; update sets
  frame(0xde)=focus while 0x74==1. mode 0: `focus = 0x70*ratio*0.01` then `FUN_14020ad00(g,focus)`;
  mode -1: `FUN_14020ad00(g,0x70)` (restore), hide 0xdd, 0x74=0. When 0x20000 clears, 0x74 goes 1->2
  and the ghost's RGBA is reduced by `dt*4.0` per frame (`FUN_1401a6600`/`FUN_1401a7c30`) until
  hidden.
- `FUN_14020ad00(g,v)` = SetFocus (v>=100 -> MAX state 4). `FUN_1402082b0(g)` = reset focus to 0.
- Disabled (flag 0x100000): 0xdc, 0xde and node 0 set to state 6, 0xdd hidden; update skips focus.

**D-Link** (0x1a = empty parent, 0x3de = base, 0x3df = blue base, 0x1b = fill "15_d_link_gage",
0x1c = label "016_d_link" seq id 0x1f5):
- Normal: `frame(0x1b) = g+0x60` (0..100). Gain: `FUN_140208280(g,float)` adds to 0x64; update: if
  pending < 1.0 add it all, else move dt per frame. At >= 100: 0x60=100, 0x64=0, flag 0x100,
  `FUN_1401a7db0(h,0x1b,4)` (130-frame loop = full glow). Every frame it also sets/clears bit
  0x200000 of `CPlayerCommand+0x60` (D-Link usable) from `0x60 >= 100`.
- Active D-Link (flag 0x200, set by `FUN_14020d510(g,cmdInfo)`, cleared by `FUN_140208390(g,now)`):
  per frame pending += `dt*-0.2` once 0xac >= 7200, else `min(dt*-0.01, dt*-0.0005*value)`; skipped
  for one frame if flag 0x400; value += pending; at <= 0 flag 0x100 cleared and value forced 0;
  frame(0x1b)=value. `FUN_14020ac70(g,v)` = SetDLink (state 0 + frame, or state 4 at 100).
- Disabled (flag 0x800): 0x3de, 0x3df, 0x1b in state 6; update skips D-Link (`flags & 0x8800`).
- Flag 0x1000: value pinned at 100, frame(0x1b)=100 every frame.
- **`FUN_14020bc50(g, on, float cur /*xmm2*/, s16 cnt /*r9w*/)` = illusion-form timer on the D-Link
  gauge.** on!=0: store 0x78=cur, 0x80=cnt; first time also 0x7c=cur, 0x82=cnt, swap node 0x1c's
  sequence to id 0x1f4 "illusion" (`FUN_1401a8080(h,0x1c,"gauge_01:0",0x1f4,curState)`), states of
  0x3de/0x3df/0x1b -> 0, flag 0x8000; then `frame(0x1b) = cnt*cur*100/(0x82*0x7c)`.
  on==0 (only if flag 0x8000): restore sequence 0x1f5, clear flag and 0x78..0x83, then state 6 on
  0x3de/0x3df/0x1b if D-Link disabled, else state 0 and state 4 / frame(0x60) for 0x1b.
  Callers: 0x2862ec (`FUN_140286210`, form reset, on=0), 0x289d6e, 0x28a3a3, 0x28a475 (function at
  0x289c00, not in the decompilation set).

## 6. Hook points for a mod
(a) **After the instance exists**: detour `FUN_140209070` (0x209070, `rcx`=g, single caller) and run
    mod code after calling the original; `*(int*)(g+0x8c)` is then valid, instance still invisible,
    PlayerData/`DAT_150f9ee40+0x118` (player) valid. First 15 bytes are position-independent
    (`48 89 5c 24 10 48 89 6c 24 18 48 89 74 24 20`). Mid-function alternative: 0x207a4f (return
    address in the ctor, `rdi`=g) or 0x209181 (`eax`=new handle, `rdi`=g).
(b) **Per frame**: entry of `FUN_140209f20` (0x209f20, `rcx`=g), or replace the vtable slot at rva
    0x6464f8. Runs every frame the gauge task runs, including while HUD-off (it returns early itself),
    but NOT while task group 0 is paused (camp menu etc.). Prologue is
    `40 53 55 56 48 83 ec 40` (8 bytes) followed by a RIP-relative `test [0x150f9ee48],0x2000`:
    a 5-byte rel jmp is fine, a 14-byte absolute jmp must relocate that instruction.
    Because `FUN_1401a5900` can kill instances behind the mod's back, re-validate the mod handle each
    frame (`FUN_1401a60e0(modH) == 8` means dead) and re-create if needed.
(c) **Before destruction**: entry of `FUN_1402080a0` (0x2080a0, `rcx`=g, `edx`=delete flag; prologue
    `40 57 48 83 ec 30 48 c7 44 24 20 fe ff ff ff`, position-independent). `g` fields are intact, but
    on room change the L2D instances were already destroyed; call `FUN_1401a57f0(modH)` anyway (safe
    on stale handles) and zero the mod handle. 0x207ce0 is an unreferenced duplicate dtor body.

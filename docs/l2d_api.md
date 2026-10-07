# BBS FM (PC/Steam) L2D runtime: API reference

All addresses are rva (image base 0x140000000). `[x]` = rva where the claim can be checked.
"D:" = verified in disassembly (float registers). Items marked **(unverified)** were not fully traced.
A second, independent copy of the same classes exists for the PC settings menu
(`CD2*@SettingMenu`, rva 0x554000..0x578800); ignore it.

## 1. Data model

Classes (RTTI): `CD2Data` > `CD2LayData` (a whole .l2d file) / `CD2SeqData` (one SQ2P block);
`CDrawable` > `CD2Ctrl` > `CD2LayCtrl` (layout instance) / `CD2SeqCtrl` (sequence instance, also used
for every node inside a layout instance); `CD2Obj` (one per animation of the active control).

### Handles
Both kinds of handle are slots of a fixed pool (`CFifoFpl`), created in init [1a6d70]:

| pool | manager ptr | entry buffer ptr | entries | list (head +0x10, tail +0x18, count +0x20) |
|---|---|---|---|---|
| data (files + SQ2P blocks) | [0x8f87fd8] | [0x8f87fe8] (0x1000 bytes) | 0x80 x 0x20 | [0x8f87ff8] |
| instances | [0x8f87fe0] | [0x8f87ff0] (0x8000 bytes) | 0x400 x 0x20 | [0x8f88000] |

Entry (0x20 bytes): `+0 u32 id` (bit31 = free), `+4 next free`, `+8 object*`, `+0x10 next in list`.
- Lookup [0e4ec0] (manager vfunc +0x18): `e = buf + (h & (cap-1))*0x20; valid iff h >= 0 && e->id == h`.
- Free [0e4fc0] does `id += cap; id |= 0x80000000`, so a recycled slot gets a new handle value:
  stale handles fail validation and every API call on them is a harmless no-op returning 0.
- Slot 0 of each pool is allocated at init with object = NULL [1a6e9a, 1a6eab], so **0 is never a valid
  handle**; creators return **-1** on failure. Max 1023 live instances, 127 data objects.
- (a) file handle = data-pool handle of a `CD2LayData` (type byte +0x18 == 0), name = file name
  without extension ("gauge_01"). (b) SQ2P handle = data-pool handle of a `CD2SeqData`
  (type == 1), name "gauge_01:N", N = one digit, max 10 blocks per file [1a4380].
  (c) instance handle = instance-pool handle of a `CD2LayCtrl` or `CD2SeqCtrl`.
- Name lookup [1a63e0]: copies <= 15 chars, strips ".ext" (4 chars from the dot), compares 16 bytes
  with data+8, returns data+0x20 (handle) or -1. So "gauge_01.l2d" = file, "gauge_01:0" = block 0.

### CD2Data (+0 vtbl)
`+0x08 char name[16]`, `+0x18 u8 type (0 file, 1 SQ2P)`, `+0x1c resource id`, `+0x20 handle`.
- CD2LayData (0xe8) [1a4380]: `+0x28 CD2SeqData* sq[10]`, `+0x78 s32 sqHandle[10]`, `+0xa0 nLayout`,
  `+0xa4 nSQ2P`, `+0xb8 layouts`, `+0xc0 nodes`, `+0xc8 fonts`, `+0xd0 strings`, `+0xd8 layout names`,
  `+0xe0 layout ids (u16[])`. All pointers go straight into the loaded file image.
- CD2SeqData (0xa8) [1ac1d0]: `+0x38 parts`, `+0x40 groups`, `+0x48 sprites`, `+0x50 nSeq`, `+0x60 seqs`,
  `+0x68 controls`, `+0x70 anims`, `+0x78 keys`, `+0x80 seq names`, `+0x88 seq ids (u16[])`,
  `+0x90 current TM2`, `+0x98 original TM2`, `+0xa0 substituted texture resource id`.

### CD2Ctrl (common part of every instance and every node) [ctor 1a1cd0]
| off | type | meaning |
|---|---|---|
| +0x08 | ptr | draw-queue link |
| +0x10 | s16 | draw priority (bucket) |
| +0x12 | s16 | draw queue: 6 = HUD queue (default), 2 = early queue |
| +0x18 | s32 | own instance handle (0 for nodes inside a layout) |
| +0x1c | s32 | data handle it was made from |
| +0x20 | f32[9] | world 3x3 matrix (computed) |
| +0x44,+0x48 | f32 | world x, y (computed; for a parentless layout this IS the position) |
| +0x4c | f32 | uniform scale |
| +0x50,+0x54 | f32 | local x, y (node offset / sequence-instance position) |
| +0x58 | f32 | rotation, radians |
| +0x5c | u32 | world colour (computed; for a parentless layout this is the stored colour) |
| +0x60 | u32 | local colour, default 0xff808080 |
| +0x68 | ptr | parent transform (= parent ctrl + 0x20) or 0 |
| +0x78 | f32 | current frame |
| +0x7c | f32 | pinned frame; < 0 (-1.0) = free running |
| +0x80 | s16 | end-action frame |
| +0x82 | s8 | current control (0..7) |
| +0x83 | s8 | end action (0 none, 1 set control, 2 destroy, 3 hide, 4 set control + pin frame) |
| +0x84 | s8 | end-action control |
| +0x85 | s8 | number of attached children |
| +0x86 | s8 | timer group 0..3 (copied from byte [0x8f8802a] at construction) |
| +0x87 | u8 | flags: bits0-1 type (0 layout, 1 sequence), 0x04 visible, 0x08 finished, 0x10 3D-anchored, 0x20 control-locked |

CD2LayCtrl (0xb0) adds [1a2310]: `+0x88 LY2 layout record`, `+0x90 LY2 node records`,
`+0x98 CD2SeqCtrl nodes[] (0xb8 each)`, `+0xa0 s32 max frame`, `+0xa4 f32 world z (3D anchor)`,
`+0xa8 s16 node count`.
CD2SeqCtrl (0xb8) adds [1ab240]: `+0x88 parent node (or 0)`, `+0x90 CD2SeqData*`, `+0x98 control[8]
of the sequence (file data, 0x10 each: maxFrame, returnFrame, animIdx, s16 loop, s16 nAnim)`,
`+0xa0 3D-anchor transform`, `+0xa8 {CD2Obj* objs; s16 n; s16 flags}*`, `+0xb0 s16 node ID`,
`+0xb2 s8 object count`, `+0xb3 s8 loop counter`.
Node IDs are looked up linearly by `+0xb0`; the first match wins (ID 0 is not unique).

### Globals
`[0x8f88008]` create "heap" arg; `[0x8f88010] f32 dt[4]` per timer group; `[0x8f88020] u8 drawEnable[4]`
(init 1,1,1,1; menu code clears them); `[0x8f88024] u8 active[4]`; `[0x8f88028] u8` visible flag given
to the next created instance; `[0x8f8802a] u8` timer group for the next created instance;
`[0x8f8802b] s8` number of paused groups (dt forced to 0 for groups 0..n-1); `[0x8f8802c] u8` global
draw enable; `[0x8f88030]/[0x8f88034]` frame/dt of the sequence being drawn.

## 2. Functions

Conventions: `h` = instance handle, `d` = data handle, `node` = u16 node ID, `ctl` = control 0..7,
`heap` = stored to [0x8f88008] (text objects only; every caller studied passes 0).
Unless noted, setters return 1, or 0 if the handle is invalid or of the wrong type.
"(layout)" = requires a layout instance, "(seq)" = requires a sequence instance. (n) = call sites.

### Creation / destruction
Every creator: instance is registered [1a4c60], takes a resource reference, gets
`visible = byte[0x8f88028]` and `timer group = byte[0x8f8802a]`, priority = file layer + 3 (= 3 for
everything in gauge_01), queue 6. Returns handle or -1. `ctl` is the **initial control**, and it also
fixes the object count and the sprites for the life of the instance [1ab240, 1aa490]; a control with
0 animations makes creation fail.
- 1a4ec0 `int CreateLayout(int dFile, u16 layoutId, int ctl, void* heap)` (119) type-checked.
- 1a4fe0 `int CreateLayoutByName(char* file, u16 layoutId, int ctl, void* heap)` (76). Loads nothing;
  -1 if the file is not registered. No type check: pass a file name, never "name:N".
- 1a4f60 `(char* file, char* layoutName, ctl, heap)` (17); 1a5100 `(char* file, int layoutIndex, ctl, heap)` (9);
  1a5080 `(int dFile, int layoutIndex, ctl, heap)` (0).
- 1a5180 `int CreateSeq(int dSq2p, u16 seqId, int ctl, void* heap)` (19) type-checked.
- 1a5350 `int CreateSeqByName(char* "name:N", u16 seqId, int ctl, void* heap)` (64). No type check.
- 1a52d0 `(char* "name:N", char* seqName, ctl, heap)` (10); 1a5220 `(int dFile, int sq2pIdx, u16 seqId, ctl, heap)` (2);
  1a53f0 `(char* file, int sq2pIdx, u16 seqId, ctl, heap)` (3); 1a55b0 `(char* file, int sq2pIdx, int seqIndex, ctl, heap)`;
  1a5630 `(char* "name:N", int seqIndex, ctl, heap)`; 1a54a0 / 1a5530 handle + index variants (0 callers).
- 1a57f0 `void Destroy(int h)` (523): unlink, free slot, release resource ref, destruct. No-op on a
  bad handle. Does **not** detach children or remove the object from a draw queue.
- 1a5900 `void DestroyAll()` (7): destroys every instance; called at every scene jump [1ec110].
- 1a4d80 `Attach(int hParent, int hChild)` (18); 1a4e10 `AttachToNode(int hParentLayout, u16 node, int hChild)` (153):
  child +0x68 = parent (node) transform; child then inherits matrix, position, colour [1a22b0, 1a2210, 1ab200].
- 1a5a50 `Detach(int hParent, int hChild)` (185) [1a2530]: child local += parent world, parent ptr cleared.

### Lookup / data
- 1a64f0 `int FindData(char* name)` (123): data handle or -1.
- 1a63b0 `CD2Data* DataPtr(int d)`; 1a6b30 `char* DataName(int d)`; 1a6ba0 `int Sq2pHandle(int dFile, int i)` (17);
  1a6bf0 `s16 Sq2pCount(int dFile)`; 1a65c0 `CD2LayCtrl* LayPtr(int h)`; 1a5fd0 `CD2SeqCtrl* SeqPtr(int h)`.
- 1a6a00 `int NodeDefaultPos(int dFile, u16 layoutId, u16 node, int out[2])` (9), 1a6ad0 same by file name (12):
  static screen position = layout XY + (240,136) + node XY summed over parents [1a41a0]; ignores key animation and scale.
- 1a6ca0 `int SpriteUVs(int dSq2p, u16 seqId, s16 out[][4])` (21): for control 0, anims 1..n-1: the UV rect (u0,v0,u1,v1) of the part used by the sprite's first group [1ac100].
- 1a84e0 `SetTexture(int dSq2p, CRsrcTM2* r)` (19), 1a8530 by name (22): swap the block's TM2 [1ac710];
  1a7270 `(int dSq2p)` (75) / 1a72c0 `(char*)` (43): restore original texture [1ac440].
- 1a6fb0 `int Register(void* l2dImage, char* name, int rsrcId)`: called only by the resource loader [11dac0].
- 1a5f20 `void Unregister(int dFile)`: called only by CRsrcL2D [11d960, 11da50, 11da90].

### Whole instance
- 1a5b30 `Show(int h, int on)` (420): flag 0x04. This is the show/hide call.
- 1a5ae0 `Set3DAnchor(int h, int on)` (25): flag 0x10; position is then a world point projected every frame.
- 1a73a0 `SetWorldPos(int h, float v[3])` (27): the point used by 3D-anchor mode (not a 2D position).
- 1a7600 `SetPriority(int h, s16 prio, int early)` (271) [1a1f40]: prio <= 0x17: priority = prio, queue = early ? 2 : 6;
  prio > 0x17: queue 2, priority prio-0x18.
- 1a7660 `SetPos(int h, float x /*xmm1*/, float y /*xmm2*/)` (164) D:. Layout without parent: writes world x,y
  (+0x44/+0x48) [1a3a00]; sequence instance or attached layout: writes local x,y (+0x50/+0x54) [1abc40].
- 1a71e0 `SetOffset(int h, float dx /*xmm1*/, float dy /*xmm2*/)` (21) D:. Layout: pos = layoutXY*scale + (dx,dy) + (240,136) [1a3280]; sequence: same as SetPos.
- 1a7730 `SetScale(int h, float s /*xmm1*/)` (13) D:. 1a76d0 `SetRot(int h, float rad /*xmm1*/)` (7) D:.
- 1a73f0 `SetColor(int h, u32 abgr, int raw)` (57): raw = 0 halves R,G,B first (alpha untouched) [1a35b0, 1abad0].
- 1a7520 `SetControl(int h, int ctl)` (544): layout: every unlocked node; sequence: itself. Resets frame to 0,
  unpins, clears finished and end action [1a3840, 1abb80].
- 1a74c0 `SetFrame(int h, float frame /*xmm1*/, int release)` (34) D:. Same semantics as 1a7d30 below; layout: applies to all nodes [1a3700].
- 1a5c70 `SetEndAction(int h, s8 action, s8 ctl, s16 frame)` (86) [1a1e10]. 1a7570 `(h, ctl, action, endCtl, frame)` (27) = SetControl + SetEndAction.
- 1a7310 `Restart(int h)` (6): frame 0, unpin, clear finished.
- 1a60e0 `int GetControl(int h)` (83): +0x82; returns 8 for a bad handle. 1a6060 `int IsVisible(int h)` (34).
  1a70a0 `int IsFinished(int h)` (231): flag 0x08. 1a60a0 `float GetFrame(int h)`. 1a61c0 `s16 GetPriority(int h)` (6).
  1a6180 `int GetType(int h)`: 0 / 1 / -1. 1a6240 `float GetMaxFrame(int h, int usePinned)` (4).
  1a6290 `GetPos(int h, float* x, float* y)` (11). 1a6010 `GetColor(int h, u32* out)` (2).
- 1a6200: do not call; vtable slot +0xc8 jumps to itself [1a1ec0].
- (seq) 1a7790 `ReplaceSeq(int h, int dSq2p /*0 = same block*/, u16 seqId, int ctl)` (22); 1a7b20 `SetText(int h, char* fmt, int textIdx, va_list)` (9);
  1a7450 `SetFontParam(int h, void*, int textIdx)` (15); 1a7be0 `CloneSprite(int h, int objIdx)`; 1a6350 `int GetSpriteData(int h, int objIdx, void** out)`; 1a6d20 `TM2* GetTexture(int h)`.

### One node of a layout instance (layout)
- 1a7d30 `SetNodeFrame(int h, u16 node, float frame /*xmm2*/, int release /*r9d*/)` (155) D: [1a3680 -> 1abb50]:
  release == 0: pin the node at `frame`. release != 0: unpin; if `frame < 0` playback continues from the pinned frame.
- 1a7db0 `SetNodeControl(int h, u16 node, int ctl)` (384) [1a37c0 -> 1abb80]: resets frame to 0 and **unpins**.
- 1a7e20 `(h, node, ctl, s8 action, s8 endCtl, s16 frame)` (7); 1a5cf0 `SetNodeEndAction(h, node, action, endCtl, frame)` (6) (action 2 refused).
- 1a5b80 `ShowNode(int h, u16 node, int on)` (593). 1a5bf0 `LockNode(int h, u16 node, int on, int allOthers)` (75): flag 0x20,
  node is then skipped by whole-instance SetControl; allOthers = 1 applies to every node except this one [1a3900].
- 1a7c30 `SetNodeColor(int h, u16 node, u32 abgr, int raw)` (147) [1a3530]. 1a6600 `GetNodeColor(h, node, u32* out)` (3).
- 1a7ec0 `SetNodePos(int h, u16 node, float x /*xmm2*/, float y /*xmm3*/)` (24) D:: node local offset.
  1a7f40 `SetNodeScale(int h, u16 node, float s /*xmm2*/)` D:. 1a6880 `GetNodePos(h, node, float* x, float* y)` (81).
- 1a7fb0 `ReplaceNodeSeq(int h, u16 node, int dSq2p /*0 = node's own block*/, u16 seqId, int ctl)` (104),
  1a8080 same with block name (44): rebuilds the node's objects, control = ctl, frame 0 [1ac5a0 -> 1a3b30].
- 1a6790 `int GetNodeControl(h, node)` (20): -1 if node missing. 1a6740 `float GetNodeFrame` (11). 1a6670 `int IsNodeVisible` (7).
  1a7130 `int IsNodeFinished` (24). 1a67e0 `int IsNodeLocked`. 1a7350 `RestartNode` (5).
- 1a8330 `SetNodeText(int h, u16 node, char* fmt, int textIdx, va_list args /*0 = no formatting*/)` (489).
  1a8170 `SetNodeMsg(h, node, int msgId, int textIdx, int mode, va_list)` (148). 1a7cb0 font parameters (26).
  1a6900 text size (16). 1a66c0 text object ptr (5). 1a7180 text state (2).
- 1a83f0 `CloneNodeSprite(int h, u16 node, int objIdx)` (108): private copy of that object's groups (0xc each)
  and parts (0x18 each) [1aaa70]. 1a6980 `int GetNodeSpriteData(h, node, objIdx, {group*,part*}** out)` (23): returns
  the group count, only after cloning [1aa5f0]. objIdx = animation index inside the control (0 = root).
- 1a8460 `(h, node, ptr)` (4): texture call on the node's TM2 **(unverified)**.

## 3. Positioning

- Units are PSP pixels, 480x272, origin top-left, y down. Evidence: scissor reset `(0,0,0x1e0,0x110)`
  [1ab112]; layout origin = layout XY + (240.0, 136.0) [1a2310, constants 6419e0/6373f4].
- Transform chain, evaluated by the root object (object 0) of each sequence while drawing [1a87b0]:
  `pos = keyPos * scale(+0x4c) + local(+0x50,+0x54)`, matrix scaled by +0x4c and rotated by +0x58, then
  multiplied by the parent's matrix and added to the parent's world x,y; the result is written back to
  +0x20/+0x44/+0x48/+0x5c and used by the other objects and by child nodes.
- Sequence instance: `SetPos(h, x, y)` is an absolute screen position of the sequence origin.
  `SetScale` and `SetRot` are independent of it. Default position is (0,0).
- Layout instance without parent: `SetPos` overwrites the world origin (default layoutXY + 240,136).
  `SetScale`/`SetRot` recompute it as `layoutXY*scale + (+0x50,+0x54)` [1a3a90] and so discard an earlier
  SetPos: call scale first, position last, or use `SetOffset`.
- One node: `SetNodePos` (offset relative to its parent, scaled by the parent) and `SetNodeScale`.
- Scale is uniform only; there is no separate x/y scale and no per-node rotation wrapper.
- 1a7600 is not positional. The table at 0x818300 is the HUD/command-deck placement table, 8-byte
  records `{u16 id, s16 x, s16 y, u8 priority, u8 flag}`; callers pass x,y to SetPos and the byte at +6
  to SetPriority, e.g. [204906..20492b] uses record 0x818318 = (id 22, x 5, y 249, priority 7).
  A second table at 0x8183c0 holds positions fetched with 1a6ad0 [207a00].

## 4. Frame advance

- Task 90000 [1a4b10] computes `dt[4]`: group 0 = game delta [86a400], groups 1..3 = raw delta
  [86a3fc]; both count 60 Hz ticks (integer vblank count, capped at 6) [0e3460]. All four are zeroed
  when byte [0x866da6] is set (a global pause flag, meaning inferred), and groups below `[0x8f8802b]` are zeroed. It also resets bytes [0x8f88028] and [0x8f8802a] to 0.
- Task 340000 [1a4aa0] walks the instance list and queues every instance whose group is active
  (`dt != 0 || drawEnable`) and whose flag 0x04 is set [1a2580, 1ab550]. Game tasks run earlier
  (130000..320000), rendering later (500000 [0faea0 -> 0fb140]).
- **Frames advance inside the draw call** [1aaea0 for a sequence/node, 1a2bb0 for the layout's own
  counter]. A hidden instance, a hidden node, or an inactive group does not advance.
- Per sequence, each draw: if pinned (+0x7c >= 0) frame = pinned value. Otherwise, if
  frame >= maxFrame and loop counter >= 0: counter 1 -> frame = maxFrame, counter = -1 (holds forever);
  counter 0 -> wrap to `returnFrame + fmod(frame - maxFrame, maxFrame - returnFrame)`, forever;
  counter N > 1 -> wrap and decrement. Objects are drawn, then `frame += dt[group]`; when it reaches
  maxFrame the finished flag is set and the end action runs [1ab12f..1ab1df].
  So file "Loop Number" 0 = loop forever, 1 = play once and hold, N = N plays, negative = never advances.
- Pinned frame: not clamped to the control's maxFrame. Each of the 11 key channels is evaluated
  separately [1a87b0 top loop]: before its first key it interpolates from the default value at t=0,
  after its last key it holds the last key. A pinned frame above the last key just shows the end
  state. Negative values cannot be pinned (they mean "release").
- Pinning stops automatic advance until release or until SetControl/SetNodeControl/Restart/ReplaceSeq,
  all of which unpin. Pin again after changing control.
- The real focus bar: node 0xde pinned to `gauge+0x68` (0..100) every frame [20a310], control 4 when full [20a398].

## 5. Draw order and visibility

- Flush order in the frame is queue 1, 2, 3, 5, then queue 6 last [0fb140]. Queue 6 (24 buckets
  [0fc540]) is where every L2D instance goes by default; what queues 1..5 contain was not traced.
- Buckets are drawn in ascending priority (higher = on top) [0f5140]; inside a bucket the order is the
  queueing order = instance creation order (older first, i.e. underneath) [0fc740, 1a4c60].
  Priority outside 0..23 is clamped to the last bucket.
- A layout draws its visible nodes in file order; node priority fields are ignored [1a30f7..1a3116].
- Visibility: `Show(h, 0/1)` (1a5b30), per node `ShowNode` (1a5b80). New instances take the byte at
  0x148f88028, which is 0 unless the caller just set it, so an instance is normally created hidden.
  Children attached with Attach do not inherit the parent's visible flag.
- The player gauge hides itself with `Show(h,0)` when `[0x150f9ee48] & 0x2000` is set and shows again
  when it clears [209f20 top]. `IsVisible(gauge handle)` therefore mirrors "normal HUD hidden".
  Gauge object: `CPlayerGauge* [0x150f9ec60]` (set in ctor [207a00], cleared in dtor [2080a0]);
  `+0x8c` = handle of layout 2 "p_gage" (priority 5 [209191]), `+0x9c` = data handle of "gauge_01:1",
  `+0x88` state (3 = hidden), `+0x68` focus 0..100. Update = vtable slot 1 = [209f20].

## 6. Loading / unloading

- .l2d files are registered by the resource system when their archive is loaded: CRsrcL2D load
  [11dac0] -> Register [1a6fb0] (under mutex [0x8f7dc18]; the thread it runs on was not traced) and
  unregistered by CRsrcL2D [11d960/11da50/11da90] -> [1a5f20].
- Unregister deletes the CD2LayData and its CD2SeqData blocks [1a3e10] and checks no instance.
  Instances keep raw pointers into the file image and into those objects, so a surviving instance
  would crash on its next draw. Two things prevent it: each instance/node holds a reference on the
  resource (count at rsrc+0x14) from registration to destruction [1ab910/1abcf0 -> 114e20/115fc0],
  and DestroyAll runs at every scene jump before resources go [1ec110].
  Whether an explicit group free ignores the reference count is **unverified**.
- gauge_01.l2d is in p00common.arc, loaded by [27bb90] into resource group 0x8fc when resource
  "PAtkData" is absent; [27bb90] is called from scene setup [1eb690, 1ed380]. Group 0x8fc is freed
  in the scene-jump code only when the next world byte [0x818150] is 0x10 and the current one
  [0x150f9eae0] is 0xf [1ec9da..1ec9e8], plus [3ac1b0]; other 0x8fc references (3bc9e0, 3c3f80,
  34f270) were not traced. In normal play it stays loaded across rooms.
- 1a4fe0 loads nothing; it only looks the name up.

## 7. Notes for the planned mod (second focus-style gauge)

Suggested calls (sequence instances, block "gauge_01:0"; ids 0x12c base, 0x12d fill, 0x191 delta bar):
```
*(u8*)0x148f88028 = 0;  *(u8*)0x148f8802a = 0;       // hidden, timer group 0; set before EACH create
base = CreateSeqByName("gauge_01:0", 0x12c, 0, 0);    // 1a5350
fill = CreateSeqByName("gauge_01:0", 0x12d, 0, 0);    // created second = drawn on top at equal priority
SetScale(h, 0.9f); SetPos(h, x, y); SetPriority(h, 5, 0);            // 1a7730, 1a7660, 1a7600
each frame: SetFrame(fill, pct_0_100, 0);                            // 1a74c0
            v = IsVisible(gaugeHandle); Show(base, v); Show(fill, v); // 1a6060, 1a5b30
end: Destroy(fill); Destroy(base);                                   // 1a57f0
```
1. **Tint cannot make it blue.** Colour is multiplicative at every level (key colour x local colour
   x parent colour, each /255 with R,G,B x2, so 0x80 = 1.0; alpha 0xff = 1.0) [1a8c24..]. The fill
   sprite (43) has vertex colours with B = 0 (0x8000ffff .. 0x800020ff, yellow to red) and the base
   sequence 0x12c carries a key colour of R 0x80, G 0, B 0 on its sprite. Any tint leaves blue at 0.
   For the fill, clone and edit: `CloneSprite(fill, objIdx)` then `GetSpriteData` (seq: 1a7be0/1a6350)
   and rewrite the four u32 colours at part+8. Sprite 43 is object 3 of sequence 0x12d (anim 875);
   check the index. The base's red is key data in the shared file image; changing it changes the
   real gauge too. Pink also needs blue, so it has the same limit; a tint can only scale the
   existing R and G. In part colours alpha 0x80 is opaque (alpha is doubled there [1a87b0 vertex loop]).
2. u32 colour byte order is R low, A high (default 0xff808080). `raw = 0` means "0..255 per channel,
   halved"; `raw = 1` means 0x80 = neutral.
3. The real bar is drawn at scale 0.9: its parent node 0x14 (p_wind_alart) has root scale keys 0.9.
   Its origin is (424,236) + (20,0) + 0.9*(2,-2) = (445.8, 234.2). Sprite 43 spans x -114..-22,
   y 2..23 from the origin before scaling.
4. All instances are destroyed at every scene jump [1ec110]. Stored handles go stale (safe, calls
   return 0). Recreate when the gauge is rebuilt [207a00 -> 209070] and treat "handle invalid" as normal.
5. Destroy only from game-task context (e.g. inside [209f20] or the gauge dtor [2080a0]). Between
   task 340000 and the render task a queued instance must not be destroyed: the queue keeps the
   pointer and the destructor leaves the CDrawable vtable [0x635ce8], whose draw slot is the stub 60eeda
   used for pure virtuals.
6. Sequence 0x12d masks its fill through the depth buffer: root anim z = 40 clears depth, the mask
   quad (sprite 53, z = 60) moves with the frame [1a87b0 end]. It is redone inside each sequence
   draw, so two instances should not disturb each other **(unverified beyond reading the state calls)**.
7. Alternative to free placement: `AttachToNode(gaugeHandle, 0xdc, mine)` makes the instance follow
   the HUD's slide-in, 0.9 scale and fades; SetPos then is an offset. Visibility is still separate,
   and it must be detached (1a5a50) or destroyed before the gauge instance is destroyed
   [2080a0], or its parent pointer dangles. A parent node that is hidden never updates its
   transform, so children of a hidden node are misplaced.
8. A second copy of layout 2 is a poor fit: every node hangs under node 0x14, and hiding that node
   freezes the transform its children use (see 7).
9. Byte [0x8f8802a] is not cleared by callers after use; a create later in the same frame inherits
   it. Timer group matters: menus clear `drawEnable[group]`, so group 0 hides with the rest of the HUD.
10. Creation failure leaks nothing visible but returns -1; the game tests `0 < handle`. Pool
    exhaustion (1023) leaks the object [1a4c60].

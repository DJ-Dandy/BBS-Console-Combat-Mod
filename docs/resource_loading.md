# BBS FM (PC/Steam) resource loading: where to hook to substitute inner files

All addresses are rvas (image base 0x140000000) in `[brackets]`. `(?)` = inferred, not verified.
Read from the decompilation/disassembly only; nothing here was run in the game.

**Local data caveat.** `arc/arc__system__CommonLua.arc` here is NOT vanilla: its Factory.lub
(10208 bytes, md5 a50471b0...) contains the marker "BBS_REVENGE" (file offset 0x7013); the vanilla
md5 in `prior/revenge_patch.py` is 09833f4c.... The vanilla size is not in the local material.

## 0. Summary

- `.bin` and `.lub` inner files become plain `CRsrcData` objects (no type-specific code). The object
  only holds name, pointer INTO the archive image, and length. Consumers look it up by name and read
  `rsrc+0x70` (pointer) / `rsrc+0x80` (length).
- Generic hook: entry of **[115b20]** `RsrcFinishLoad(CRsrcData* rcx)`. Every resource object
  (archive, inner file, standalone file) passes it exactly once per creation, with state 4, before
  it becomes visible to lookups (they require state 7, which 115b20 sets on return).
- Same-size files (PAtkData, PCam*, BCam*): in that hook, when state == 4, type == 2, the name
  matches and `[rcx+0x80]` is the expected length (35560 / 112), memcpy the replacement over
  `[rcx+0x70]`. That survives relocation and cache revival of the archive, and is redone
  automatically after a real re-read.
- Factory.lub (other length): do not rely on replacing `rsrc+0x70/+0x80` (relocation/revival resets
  the pointer but not the length, see 5). Retarget the `call luaL_loadbuffer` at **[2c5962]** to a
  stub that swaps (pointer, length) when `dword [rax+0x4c] == 0x348b2765`.
- Nothing checks archive contents after the read (6).

## 1. Classes and object layout

RTTI vtables: `CRsrcData` [637b08], `CRsrcArchive` [6381b8], 35 `CRsrcXXX` classes
[6377a8..6397b8] (one per typed extension, e.g. CRsrcL2D [638aa8]), `CRsrcContainerManager`
[639e48], `CRsrcList` [639e10]. All share CRsrcData's 21-slot vtable shape; the typed classes
override mostly slots 0, 1, 3, 4, 6.

Extension -> type id: table [639ec0], 0x29 entries of `{u32 id; char* ext}`, searched by
[11d0e0] (extension from [0e88f0], lower-cased, up to 7 chars): arc 1, **bin 2**, tm2 3, pmo 4,
pam 5, pmp 6, pvd 7, bcd 8, fep 9, frr 0xa, ead 0xb, ese 0xc, **lub 0xd**, lad 0xe, l2d 0xf,
pst 0x10, ... edp 0x29 (table order = id order); unknown = 0.
Type id -> constructor: table [8f7dac0] (0x2a pointers, filled by [117cf0]). Types 0, 2 (bin),
0xd (lub), 0x10, 0x27, 0x28 -> [116d70] = plain `CRsrcData` (new 0x90). Type 1 -> [1169d0] =
`CRsrcArchive` (new 0xa8). Factory: [117bf0] `(name, group, size, type=-1 -> from extension, dir)`.

`CRsrcData` (0x90 bytes; ctor body [113fb0]):

| off | type | meaning |
|---|---|---|
| +0x00 | ptr | vtable |
| +0x08 | ptr | next in the per-type list (heads at [8f7e328 + type*0x20]: head, tail, count) |
| +0x10 | u8 | type id (table above) |
| +0x11 | u8 | state: 0 none/failed, 1 size query pending, 2 size known, 3 read in flight, 4 data present but not initialised, 5 revived from cache, 6 relocating, 7 ready |
| +0x14 | s16 | reference count held by users (L2D instances...); non-zero pins the address in the layout pass [11b9a0] |
| +0x16 | s16 | request (use) count; group free decrements it [1157d0] |
| +0x18 | char[0x20] | directory ("arc/pc"); empty for inner files |
| +0x38 | char[0x10] | file name WITH extension ("PAtkData.bin", "Factory.lub", "p00common.arc") |
| +0x48 | u32 | directory code / hash |
| +0x4c | u32 | name hash = standard CRC-32 of the name up to the first '.' ([0e8db0], table [6354a0]); "Factory" = 0x348b2765, "PAtkData" = 0x80acd8a8 |
| +0x50 | s32 | group id (0x44c system, 0x6a4 map, 0x8fc player, ...); +10000 once released into the cache; negative groups are packed from the arena top |
| +0x54 | s32 | pending group (revival request) |
| +0x58 | u32 | resource id (container handle; what [11a8a0] takes) |
| +0x5c | u32 | id of the owning archive; 0 for top-level files |
| +0x60 | u32 | id of the pending file request |
| +0x68 | ptr | planned/new address (layout pass); for inner files = archive base + entry offset |
| +0x70 | ptr | **current data pointer** (what consumers read) |
| +0x78 | ptr | address at the last relocation fix-up (delta base) |
| +0x80 | u64 | **size**: exact entry length for inner files; file size rounded up to 0x400 (0x40) for top-level |
| +0x88 | u16 | flags: 4 = unloaded/dead, 2 = set by relocate, 8 copied from parent (?) |
| +0x8a | u16 | flags: 1 revive pending, 2 release pending, 4 released, 8 needs move, 0x10 needs (re)read, 0x40 "do not share", 0x100 archive without data |
| +0x8c..0x8e | u8 | remaster ("ReplaceItem") bookkeeping |

`CRsrcArchive` adds: +0x90 u32 directory code, +0x98 ptr `_aligned_malloc` copy of the entry table
(from the size query; freed in [115d60]/dtor [1160f0]), +0xa0 u16 inner entries seen, +0xa2 u8 "only
links, no data".

Vtable slots (CRsrcData / CRsrcArchive; names are mine):
0 dtor [113320 / 1160f0]; **1 +0x08 OnLoaded [114bc0 = `ret 0` / 114b40 creates inner objects]**;
2 +0x10 [112d60 / 114aa0]; **3 +0x18 OnRelocated(delta) [114b20 = `ret 0`]**; 4 +0x20 OnUnload
[114b30]; 6 +0x30 Unload [113530 / 113420]; **7 +0x38 Relocate(delta) [115340 / 1151e0]**;
8 +0x40 StartLoad [115de0 / 115d60]; 9 +0x48 Release [115790 / 1156b0]; 13 +0x68 ToCache
[114f20 / 114e60]; 15 +0x78 Revive [114cc0 / 114c20]; 17 +0x88 Destroy [113cc0 / 113c50].

Container: `[8f7dc10]` -> manager object at [8f7dc40]. List of nodes `{u32 id; CRsrcData* +8;
next +0x10}` head at mgr+0x40, sorted by group ([118460] inserts; registration wrapper [118400]).
Mutex "RsrcDB/DataAccess" (LwMutex around a Win32 mutex, so recursive) at mgr+0x58.

## 2. Loading an archive (question A)

Threads: game logic runs on the main thread in fibers (yield [110ba0] -> SwitchToFiber [52db20]).
Two worker threads: **"AsyncFileAccess"** (entry [0e7c80], created in [0e93a0]) and
**"RsrcBuilder"** (entry [118940], created in [11b280]).

1. Request (main thread): [11fd30] `(dirCode, name, group, flag)` builds "dir/name.arc" ([11c4f0],
   table [63a150]: 0x4350 "PC" arc/pc, 0x535953 "SYS" arc/system, 0x53534f42 "BOSS" arc/boss,
   0x50414d "MAP", ...; the same codes are the dirHash of link entries in an ARC) and calls
   [1203e0]. If the file is already known ([11a2b0]) only +0x16 is incremented; else [11ea40]
   creates the object (state 1) and [11ffa0] queues a size query: archives use op 5
   `AsyncExeGetArcInfo` (vtable [6352b0]; exec [0e8360] -> [0e8980]), others op 4 (file size).
   [0e8980] reads and inflates the WHOLE archive into a temporary `_aligned_malloc` block
   ([4bd160]), checks "ARC" + version 1, copies only header + entry table to a CBuf, frees the
   block. So every archive is read twice; this first read never reaches the resource system.
   Completion [11f740] (I/O thread): requests the linked files (entries with dirHash != 0) through
   [120720] -> [1203e0], keeps the table at arc+0x98, sets size (+0x80) and state 2.
2. Layout (main thread, loader task [11f2b0], started by [11f6a0]): [11c240] -> [11b9a0] assigns
   every top-level resource an address (+0x68) inside a static arena in the exe's .data:
   **[86a440 .. 8a6a440) = 0x8200000 bytes** (bounds at [80ed80]/[80ed78]; zone table [8a6a440],
   zone = [11c8c0](group)). Resources are packed in list order per zone; pinned ones (+0x14 != 0)
   keep their address. Then RsrcBuilder is resumed.
3. RsrcBuilder [118940]: under the RsrcDB mutex, [1185f0] per top-level resource: if the planned
   address differs from the current one, the data is moved (`[0e5030]`, a 4-byte memmove) and
   Relocate (+0x38) is called; new resources are queued. Then StartLoad (+0x40) per queued id:
   [115de0] -> [0e9b30]`(path, buf = rsrc+0x68, cb = 114420, user = rsrc, flags, remaster, size =
   rsrc+0x80)` (call at [115f4a]; by-hash variant [0e9cb0] at [115f7d]) = op 3 `AsyncExeLoad`.
   It then polls ([0e9450]) with the mutex released. (Archives holding only links, arc+0xa2,
   get their table memcpy'd from arc+0x98 in [115d60] instead; how they reach state 7 was not traced.)
4. I/O thread: [0e7a90] runs the executor: `AsyncExeLoad::exec` [0e8550] -> [0ea4c0] ->
   [0ea620]/[0e95a0] -> **[4bd080]** `(fileMan [10fb5620], path, dest, destSize, remasterFlag)` =
   the HD loader: [4bd230] finds the package entry (PackageFile::find [4bdb30], key = MD5 of the
   path [53fd60]), [4bdf10] seeks, reads, decrypts [4bcba0] and inflates [6072a0] straight into the
   arena buffer. Then `done` [0e7ec0] calls the callback on the same thread.
5. **Hand-over: [114420]** `(rcx = buffer or 0 on failure, rdx = bytes, r8 = CRsrcData*, r9d =
   flags)`. After remaster bookkeeping (Win32 mutex "ReplaceItemLock" [8f7dc18]) it does
   `state = 4; call 115b20` [1148b2..1148b9].
6. **[115b20]** `(rcx = rsrc)`: if state == 4: `call vtable[1]`, then state = 7.
   For an archive vtable[1] = **[114b40]**: walks the 0x20-byte entries of the image at arc+0x70
   (count at +6) and for each entry with dirHash == 0 calls **[115c50]** `(arc, entry)`:
   - looks the name up ([11a900]); if a ready resource of that name exists in the same group
     (and not flag 0x40) it is shared: its +0x16 is raised, entry+0xc = -1, no new object
     (exception hard-coded: "continue.ecm" in "p00common.arc");
   - else **[113b40]** `(arc, entry, 1)`: object = [117bf0](entry+0x10, arc.group, entry.length);
     registers it [118400]; writes the new id into **entry+0xc inside the archive image**;
     state = 4; parent id (+0x5c) = arc id; `+0x68 = +0x70 = +0x78 = arc.data + entry.offset`
     [113bce..113bd6]; then `call 115b20` [113bdd] -> vtable[1] of the inner object -> state 7.
   So the whole tree is built on the I/O thread inside the completion callback.
7. Second way an inner object is (re)built: archive Revive [114c20] -> [114ce0] -> [113b40](.., 0)
   (object without pointers, state 4), then archive Relocate [1151e0] -> **[115440]** `(arc, entry)`
   sets +0x68 and tail-jumps to 115b20 [115491]. This runs on RsrcBuilder.

Finding a resource:
- by id: **[11a8a0]** `(id)` (locks, -> [11aa00]); unlocked variant [11b0b0] (also scans the
  not-yet-registered list [80f0e0]).
- by name: **[11a9d0]** `(rcx = &key {char* name | u32 hash; u8 isHash}, edx = type id, r8d = group
  or 0, r9d = 1, [rsp+0x20] = flags)` -> [11aff0] (locks) -> [11aca0] hashes the name (CRC-32 up to
  '.') and walks the per-type list [11ab80] comparing `+0x4c`; filters: state == 7 [113a10], group
  [114150]. [11a900] `(name, group, 1)` is the same with the type taken from the extension.
- iterate a type: [11a3c0] `(type, group)` / next [11a640].

## 3. `.bin` files (question B)

Class: plain `CRsrcData` [637b08]; vtable[1] is a `ret`. Nothing copies the data at load.

**PAtkData.bin** (p00common.arc entry 0, offset 0x1d0, 35560 bytes = 889 records of 0x28):
- [27bb90] (scene setup; called at [1eb7d3] in [1eb690] and [1ed706] in [1ed380]) only tests
  whether "PAtkData" exists: `11a9d0({"PAtkData",0}, 2, 0, 1, 0)`; if not it requests
  `11fd30(0x4350, "p00common", 0x8fc, 0)`. So p00common is requested once and stays while group
  0x8fc lives (freed only in the cases listed in l2d_api.md section 6); a normal room change does
  not re-read it.
- The reader is **[26b950]** `(index)`: `if (PlayerMgr[0x128] == 0) PlayerMgr[0x128] =
  lookup("PAtkData")->+0x70; return PlayerMgr[0x128] + index*0x28`. PlayerMgr = `[10f9ee40]`
  (`PL::CPlayerManager`, 0x1e0 bytes). Its 16 call sites (21e6af .. 283d45) read the records
  directly from the archive image. No copy.
- The cached pointer is zeroed by the CPlayerManager ctor [26a260] (`this[0x25] = 0`); the manager
  is created in scene setup ([26b8b0], call at [1ebb08]) and destroyed at scene end
  ([26a540]/[26a730]). So the pointer is re-resolved from `rsrc+0x70` once per scene.
- Consequence: patching the 35560 bytes in the archive image when the archive is loaded is enough.

**Camera files** (112 bytes = 14 qwords). There is no "%c%03d" format; names are built from
templates:
- table [8185a0] = {"PCamV000", "PCamA000", "PCamT000"}, index `(charId-1) % 3`
  (charId = [26b7d0] = `[10f9ee4c]`), sprintf'ed into the static buffer [8185b8]; if
  `(charId-1)/3` is 2 or 3 byte 5 becomes 'R' ("PCamVR00"). Variants written into cam+0x218:
  "PCamX50n" [22f2d0], "PCamX0nn" [22eb90], "PCamX099" [22f560].
- boss camera name buffer [818640] "BCam0000": bytes 4..7 are set either by [22e840] (two-letter
  world code from [1f2ea0] + two-digit room number, e.g. "BCamsb02"; called from the player camera
  creation [26ad10]) or by [22e9c0] (bytes 1..4 of a string argument, e.g. "b01cd.." ->
  "BCam01cd"; this is the Lua C function **"SetBossCamera"** [2c7ba0], pointer at [652588]).
  Both set camera flag 0x100000 = "a boss camera exists".
- The one reader is **[22f1b0]** `(cam, name, mode)`: `r = 11a9d0({name,0}, 2, 0, 1, 0)`;
  mode 0: **copies 112 bytes from `r->+0x70` into the static block [8185d0]** and sets
  `cam+0x1e8 = &[8185d0]` (also when not found: the static block keeps its last content);
  mode != 0: keeps the raw pointer in cam+0x1f0 (used for "PCam0001" in the camera ctor [22a740]).
  [22e840] and [22e9c0] contain the same inlined copy.
- When: camera ctor [22a740] (per scene, via [26ad10]); target/reset [22db10] (via [220c90]) and
  the same code in the second half of [22f560] (from 22f650): BCam name first if flag 0x100000,
  else/fallback PCam; mode switches [22f2d0] (13 call sites, 25167b..283cca), [22eb90], [22f560];
  restore [22b6c0]. 22f1b0 has 15 call sites (22a85a .. 22f731).
- Consequence: the copy is refreshed from the archive image many times; patch the archive image
  in place at load. Patching [8185d0] directly would be overwritten at the next call.
- PCamV000/A000/T000 are in p01init/p02init/p03init (offsets 0x5d0 / 0x4080 / 0x48c0 locally),
  pulled in by the character's "pNNbase" archive (table [818bb0], requested by [27bb90], group
  0x8fc) (?: that link was not checked). Boss archives are not extracted locally; not traced.

## 4. `.lub` files (question C)

Class: plain `CRsrcData`. CommonLua.arc is requested with group 0x44c by [1da8a0] (and the twin
sequences at [1da6f7], [1db67d]) together with commonGame and areainfo: boot/title, system group.

Lua 5.1 (header `1b 4c 75 61 51 00 01 04 04 04 04 00`: little endian, int 4, **size_t 4,
Instruction 4, lua_Number 4-byte float**; a replacement chunk must be built the same way).
State `L` = `[10f9f100]`, script set = `[10f9f108]` (`CPoolList<LuaManager::LUA_SCRIPT_LIST>`,
64 slots), both created by [2c5600] ([5bc800], lua_newstate (?), with allocator [2c6900] = CRT
`_aligned_malloc/realloc`, so a bigger chunk is no problem) and destroyed by [2c5570].

- [2c41d0] (twin [2c5740]) loads the 14 names of table [651be0] in this order: AtkKind,
  CommandKind, GimmickKind, Collision, CmnEneParam, EnemyParam, World, common, FSM, EntityMgr,
  **Factory** ([651c30]), stack, MotTrigger, 2DDef; then every other ready resource of type 0xd
  ([11a3c0](0xd, 0) / [11a640]). Each goes through [2c6a80] `(set, hash, name, flag)`, which
  skips hashes already in the set and otherwise calls the loader stored at set+0x60.
- **The loader is [2c5920]** (not in the Ghidra output; registered at [2c5628]):
  ```
  2c5920  (ecx = name hash, rdx = name or 0, r8d = flag)
  2c5943  call 11a9d0            ; key {hash, isHash=1}, type 0xd, r9d = 1, flags 0 -> rax = CRsrcData*
  2c594d  mov r8,[rax+0x80]      ; size
  2c5954  xor r9d,r9d            ; chunk name = NULL
  2c5957  mov rdx,[rax+0x70]     ; data
  2c595b  mov rcx,[rip+..]       ; L = [10f9f100]
  2c5962  call 5bb9f0            ; luaL_loadbuffer(L, buf, size, name)   bytes: e8 89 60 2f 00
  2c5967  test eax,eax / jne fail
  2c5979  call 5b9cd0            ; lua_pcall(L, 0, LUA_MULTRET, 0)
  ```
  [5bb9f0] = luaL_loadbuffer (wraps lua_load [5b9b10] with reader [5bac00]); its only other caller
  is [5bd22f] (Lua library code).
- When: the Lua state is destroyed at every scene jump ([2c5570] called at [1ec1e5] in [1ec110])
  and recreated in scene setup ([2c5600] at [1ed4c5] in [1ed380]); [2c41d0] then runs at
  [1eb970] (scene setup [1eb690]). Also [1ece44/1ece70] ([1ecd30]) and [1ed029/1ed02e] ([1ecee0])
  (destroy + create + load) and [3edaab] ([3eda70], no-op for already loaded hashes).
  So **Factory.lub is loaded and run once per scene setup**, on the main thread, from whatever
  `rsrc+0x70/+0x80` hold at that moment. The archive itself is not re-read.
- **Best hook for Factory: retarget the call at [2c5962]** (E8 rel32, next instruction 2c5967).
  At that point: `rcx = L`, `rdx = data`, `r8 = size`, `r9 = 0`, and `rax = CRsrcData*` still
  valid (name at rax+0x38 = "Factory.lub", hash at rax+0x4c = 0x348b2765). The stub swaps rdx/r8
  when the hash matches and jumps to [5bb9f0]. rax is not a C argument, so either write the stub
  in assembly or, in C, identify the chunk by `buf == lookup(hash 0x348b2765)->+0x70`.
  The chunk name is NULL, so luaL_loadbuffer itself cannot tell which script it gets.
  Alternative: hook the entry of [2c5920] (`48 83 ec 48 | ba 0d 00 00 00 | ...`, no relative
  operands in the first 9 bytes; ecx = hash) and do load + pcall yourself for hash 0x348b2765,
  returning 1 on success, 0 on failure.
  Standard Lua 5.1 undump copies code, constants and strings, so the replacement buffer is only
  needed during the call (?: the undump code was not read).

## 5. Generic hook, thread, memory, lifetime (questions D, E)

**Hook: entry of [115b20]**, `void RsrcFinishLoad(CRsrcData* this)`, Microsoft x64, `rcx = this`,
no other arguments, returns nothing. Act only when `byte [rcx+0x11] == 4` (all three callers
guarantee it; the function itself tests it). Then:
`name = rcx+0x38`, `type = byte [rcx+0x10]`, `data = [rcx+0x70]`, `length = [rcx+0x80]`,
`group = [rcx+0x50]`, `owning archive id = [rcx+0x5c]` (0 = top-level; the archive object is
[11b0b0](id), its name at +0x38).

First 16 bytes: `40 56 | 48 83 EC 20 | 80 79 11 04 | 48 8B F1 | 0F 85 0F ...`
(push rsi; sub rsp,0x20; cmp byte [rcx+0x11],4; mov rsi,rcx; jne +0x10f). Instruction boundaries
at 2, 6, 10, 13. A 5-byte jmp must take 6 bytes (two whole instructions); the first 13 bytes have
no RIP-relative operand and no relative branch. Do not take more than 13: bytes 13..18 are
`jne rel32` (target [115c42]).

Callers: [1148b9] (completion callback, top-level files), [113bdd] (inner files, first load),
[115491] (a `jmp`, inner files rebuilt after revival). Because one of them is a tail jump, hook
the entry, not the call sites.

Data-only alternative for `.bin`/`.lub`: `CRsrcData::vftable[1]` at **[637b10]** holds [114bc0]
(`ret 0`). Replacing that pointer (.rdata, needs VirtualProtect) gives a callback `(rcx = rsrc)`
for every plain CRsrcData object at the same moment, with no code bytes changed. It does not see
archives or the other CRsrcXXX types.

Per-inner-file alternative: entry of [113b40] `(rcx = archive, rdx = entry, r8b = set-pointers)`:
name = rdx+0x10, data = `[rcx+0x70] + dword [rdx+4]`, length = `dword [rdx+8]`; bytes
`48 89 5C 24 10 | 48 89 74 24 18 | 48 89 7C 24 20 | 41 56` (position independent, first
instruction exactly 5 bytes). In-place patches only: the offset is a u32 relative to the archive.

Thread: 115b20 runs on **AsyncFileAccess** (I/O worker) for first loads, on **RsrcBuilder** for
the revival path; never on the main thread. No game mutex is held at the entry on the I/O path
(the request-queue mutex "File/ReqQueueAccess" [8f64810] is released before the executor runs;
[11a900]/[118400] take the RsrcDB mutex only around their own work); on the RsrcBuilder path the
RsrcDB mutex is held. The main thread cannot see the object yet: lookups require state 7. The
hook must not block and must not call lookups that expect state 7 on the object itself.

Memory: the archive image is NOT malloc'ed. It lies in the static arena [86a440..8a6a440) inside
the exe's `.data` section (read/write). Writing into it is safe; the game itself writes there
(entry+0xc ids; lower-casing names inside "e.pst" files in 115b20). `free`/`realloc` on it is not
possible, and the entry table cannot be grown.

Late injection: for archives already loaded when the hook goes in, apply the same patch through
[11a9d0] or the per-type lists (`[8f7e328 + type*0x20]`, next = rsrc+0x08).

**Lifetime.**
- Release: group free [119120] `(group, mode)` -> Release (+0x48) -> flag 2; next layout pass
  [11b9a0] -> [11c360] -> ToCache (+0x68): OnUnload, `group += 10000`. The image stays in the arena.
- Eviction: when space is needed the cached object is destroyed ([119dd0] -> Destroy +0x88; an
  archive first destroys its inner objects [113e20]). A later request reads the file again and all
  objects pass 115b20 again (state 4) -> in-place patches are re-applied by the same hook.
- Revival: a request for a cached archive ([11a2b0] -> [115610] sets +0x54) -> Revive (+0x78)
  -> state 5 -> archive Relocate [1151e0] -> per inner [115440] -> inner Relocate [115340].
  No re-read; the image still has the patched bytes.
- Relocation: [1185f0] memmoves an archive when the layout gives it a new address (anything
  earlier in the same zone changed and the archive is not pinned by +0x14), then [1151e0] ->
  [115440] sets inner `+0x68 = newBase + offset`, and [115340] does `+0x70 = +0x68`, calls
  OnRelocated(delta) (+0x18), `+0x78 = +0x70`. Consumers look written for this (?): PAtkData
  pointer re-resolved per scene, camera data copied, Lua loaded by lookup.
  Whether p00common / pNNinit / CommonLua / boss archives actually move in normal play was not
  determined (?). Zones: groups < 0x8fc zone 0 (0x44c system comes before 0x6a4 map in list order,
  so map changes should not move CommonLua (?)), 0x8fc..0x95f zone 1 (player).
- **Therefore a replaced `rsrc+0x70` does not stay valid**: revival and relocation overwrite
  +0x68/+0x70/+0x78 with the archive address without passing 115b20, while a replaced +0x80 would
  be kept, leaving the original pointer with the new length. If pointer replacement is wanted
  anyway, re-apply it in OnRelocated: `CRsrcData::vftable[3]` at [637b20] (holds [114b20], `ret 0`;
  called from [115340] after +0x70 was reset, only while group < 10000), or hook [115440].
  For inner objects +0x80 is used by consumers only (layout skips objects with +0x5c != 0).
- Replacement data: PAtkData's pointer is cached in CPlayerManager for a whole scene, and camera
  mode-1 pointers are kept in the camera object, so a replacement buffer must never be freed.
  Camera mode-0 data and Lua chunks are consumed during the call.

## 6. Verification of contents (question F)

None found after the read.
- [4bd080]: only `destSize >= entry size` (else -6) and `bytes read == size from the package
  entry` (else -2). MD5 [53fd60] is used on the path string to find the .hed entry, not on data.
- [0e8980] (size query): "ARC" magic and version == 1.
- Remaster bookkeeping ([528c30] -> [528180], [5279e0], [1148f0]; for names containing .pmo .txa
  .fep .tm2 .pmp .l2d .frr .arc) tracks pointers into the image for HD texture replacement; it
  does not hash anything and is not affected by changing .bin/.lub bytes.
- An in-memory change of an inner file's bytes, or of `rsrc+0x70/+0x80`, trips nothing. Do not
  change entry offsets/lengths or the entry count in the archive header: [114b40], [1151e0],
  [1156b0], [1157f0], [1136e0], [1137b0], [114c20] re-walk the table from arc+0x70 on every
  release/relocation, and entry+0xc holds live resource ids.

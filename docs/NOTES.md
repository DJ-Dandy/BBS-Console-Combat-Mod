# BBS KH2-style mod: working notes

Target: KINGDOM HEARTS Birth by Sleep FINAL MIX.exe, Steam (HD 1.5+2.5), sha256 375a8112... (unmodified) or
874bc1f2... (with the Combo Flow exe patch).  Image base 0x140000000; all addresses below are offsets from it.

Delivery: `dbghelp.dll` next to the exe (the exe imports dbghelp!MiniDumpWriteDump; every export of the system
DLL is forwarded lazily, see tools/genproxy.py).  The DLL does nothing in the other games of the collection.
Files: bbskh2.ini (settings), bbskh2_log.txt (log), bbskh2_off.txt (present = mod off),
bbskh2_debug.txt (present = debug channel: commands written to bbskh2_cmd.txt are answered in the log).

Reference notes (static analysis): l2d_api.md (2D layout runtime), player_gauge.md (HUD gauge object),
player_command.md (command deck, plates, input, styles, D-Link).

## Piece 1: MP instead of reloads (src/mp.c)
- 233d40 call 2063c0 (use the selected deck plate) -> use_hook: refuse while in MP burn, charge MP on success.
- 206a6e call 1ce550 (reload gain per frame) -> reload_hook: 100 (ready at once), or the recharge progress
  while in burn so every plate's own bar shows it.
- plates_block: while in burn, idle non-item plates get flag 0x2 + refresh 2060d0.
- cmd+0x60 |= 2 each frame: no cursor advance / auto-skip after a use (ini CursorAdvance).
- 2388a0 / 205ee0: no new D-Link while in burn.  2624e0: items that restore Focus restore MP.  285780: reset.
- Per-frame work runs from the player gauge's update (209f20), which also drives the bar.
- Max MP = baseline + 10 per deck slot the character has (PlayerData +0x36).  Baseline 60, or 20 on Critical
  (difficulty byte 150fa0881 == 3; ini Baseline / BaselineCritical): 90..140, on Critical 50..100.
- MP bar: one instance of sequence 0x12d of "gauge_01:0" (the Focus fill); its fill sprite (object 3) and
  mask (object 2) are cloned per instance (1a7be0) and rewritten every frame (1a6350): mask emptied, quads
  0..3 = black frame, empty part, fill (kept at the right end, as in KH2), "MP" plate.
- Place and size (`[Bar] Right`, `Top`, `Height`, `BarLength`): right end of the plate's solid part at x 425, top at
  y 254.6, 4.5 high, 57.6 long at 100 MP (the plate adds 2.6 x height = 11.7; 69.5 in all, the first bar's length,
  so the left end is at x 355.9 as before).  The Focus bar's outline ends at about
  y 253.9 (measured on a 1440p screenshot), so the bar hangs 0.7 under it.  The first size was top 255.5, height
  7.5, length 50; the tests' quad numbers are still for that size (test_bar_geom).
- **Pixel grid** (`[Bar] ExactSize`, default on).  CD2SeqCtrl::Draw (1401aaea0) hands every object's draw
  (CD2Obj vtable slot 1, 1401a87b0) a "scaled" flag, and 140526100 picks the quad builder by it: unscaled ->
  14052c550, which moves every corner to a whole unit of the 480 x 272 screen (`ceil(x - 0.5)`, texture position
  corrected, texel-centre rounding) as the PS2 drew sprites, filter mode 3; scaled -> 14052cbc0, corners and
  texture rectangle as given, mode 4.  A stand-alone sequence is "scaled" when its root record has keyn[8] ==
  keyn[9] == 1 and the first two keys of the root object's key table have t == 0 and a value != 1.0 (a layout
  node: the same test on the top parent, 1401aad80) - the "0.88" roots of the command plates.  The scale set
  with 1401a7730 does not count.  The bar's sequence (root record without keys) was therefore drawn on the grid:
  7.5 high with 0.75 borders became 8 with borders of 1, and textured pieces (plate, round end) sat up to half a
  unit away from the plain rectangles.  mp.c root_scale gives the instance's root object a record of its own
  (the game's + one scaleX and one scaleY key) on a two-key table of its own, every frame, and leaves 1401a7730
  at 1; the Command Style timer bar gets the same.  Test `t_draw` runs the game's draw code with the renderer
  stubbed and reads the vertices: on the grid without it, exact with it.
- Round left end (`[Bar] RoundEnd`, default on): all seven quads of that sprite are used - 0 frame's round end
  (8 art pixels), 1 frame's straight part, 2 / 3 the same for the empty inside (round end 6 pixels, from x 2), 4 the
  part of the fill that reaches into the round end, 5 the straight fill, 6 the plate.  The round pieces are drawn
  by tools/genmpart.py (a rounded rectangle's left end, radius 5 for the frame and 3 for the inside, in black,
  blue gradient and grey gradient, with 2 units of margin) into the second row of the art block, which is now
  192x108 pixels (texel units 104,184 + 96x54).  Texture positions are whole units = art pixels, so inside the
  round end the fill's edge moves by whole art pixels.
- The bar's art is KH2's own (the "MP" plate and gradients of field2d/us/zz0field.2dd, texture 1).  src/tex.c
  copies it into an unused area of the gauge sheet (texture 2 of p00common.arc, 1024x512; area 208,368 +
  192x108 = texel units 104,184 of the 512x256 sheet) when the game creates the texture: hook on the call
  4e1ee4 -> 4e1630 (pixels from libpng, BGRA).  The sheet is recognised by 24 check pixels; if it is not, the
  bar falls back to a plain one made from the Focus bar's cross-section (uv x 140, y 70..112).
  tools/genmpart.py builds src/mpart_gen.h from the two PNGs.  Before a public release: the header holds a
  small piece of KH2's texture; read it from the user's own KH2 files instead, or ask.
- The confirm-button icon on the plate under the cursor is a text node (node 0x5d of the deck plate layout,
  node 1 of the D-Link plate layout, default string "\xf5g"); the menu keeps it empty.
- Offline test: test/run.sh (plates on the real game code with the L2D calls stubbed; the bar on the real L2D
  runtime with BBS_GAUGE_L2D=<gauge_01.l2d extracted from arc/pc/p00common.arc>).

## Command Style offer (src/style.c)
- Hook on the compare at 2368f2 (cmd update, gauge full, level < 2): with a style candidate (+0x190 != 0x151) the game
  is held there (jump to 236b49 with +0x64 bit 0x100 left set) while a prompt with a timer is up; the old deck button
  (pad test 272c20) lets the game's own change run, timer out jumps to the finisher branch 236a82.
- Prompt = bc01_00 layout 7 with seq 0x197 on node 0x5a (control 0 steady, 4 flash), name on node 0, button icon =
  text of node 0x5a (f5 'g' = deck button, f5 'd' = confirm).  Timer bar: hud_timer_bar in mp.c.
  Draw priority 11 (both): under the Magic / Items / D-Link lists and their headers (12, row under the cursor 13),
  which reach up to it when long - reported drawn over them at 12, where the later-made offer won the bucket - and
  over the command menu (5 / 7).
- The menu no longer draws the game's style attack plates (type 4) and finisher plate (type 3); its Attack entry
  shows the active one's name.  236c92 (extra presses of multi-press commands) also accepts the confirm button.
- Finishers are offered the same way: 236d67 (call to 233f70) runs the timer, 234115 (its confirm test) is the old
  deck button, 236d98 (jne that skips Attack while a finisher is available) is removed and the Attack plates are
  un-covered (205890 / 205910).  Timer out = 233f70's own "finisher over" branch (flag 0x4000 set, 0x8000 clear).
- test/run.sh t_style drives the real hook with landing pads on its three exits, and 233f70 for the finisher.
- **An offer that runs out does not end the style you are in (`[Style] KeepStyle`).**  Reported: in a style, the
  next style / finisher comes up, you leave it alone, and you are back in the normal style.  Cause: the chain
  ends in 233f70's "finisher over" branch, and that branch has two ends.  With cmd+0x64 bit 0x20 (set for good in
  the constructor [22fe30]) and no D-Link / illusion (0x400080) it reverts: `2371d0(cmd, 0)`, next = 0x151,
  `237f20(cmd, 4)`.  Otherwise it keeps the level: `2371d0(cmd, level)`, which only clamps the gauge to
  level x 100 - the value a style begins with - and readies the style's plate.  Both then clear the affinity
  counters, the candidate and the "full" flag.  For a finisher that was only offered, at level 1 or 2, the mod
  takes bit 0x20 away for that one call and puts it back, so the game's own keep-the-level end runs.  A finisher
  that is used still ends the style, and in the normal style an offer that runs out still empties the gauge.
  Test in `t_style`, both ends on the game's function (KeepStyle = 0 reproduces the report).  Not seen in the game
  by me.

## Later pieces (not built yet)
- Attack / Magic / Items / D-Link menu (entry names: `[Menu] MagicText`, `ItemText`, `LinkText`); split deck editor (CCampDeck* classes); link list like KH2 summons.
- Style change as an optional prompt: patch point 2368f2 (see player_command.md section 8).

## Bundled earlier mods (src/bundle.c, generated src/bundle_gen.h, tools/genbundle.py)
- Combo Flow code: five byte ranges (2291bf, 26825f, 268286, routines at 62f080 and 631290) written in memory;
  skipped if the old exe patch is present.  Only used with `[Speed] Enabled=0`: speed.c has the walk-out now and
  hooks the same three places (an installed old exe patch is taken out of them in memory first).  Its PAtkData.bin bytes and the KH2 Camera files are set in place
  when the resource object is created: CRsrcData::vftable[1] at 637b10 (was a bare ret, 114bc0).
- Revenge Value: the call to luaL_loadbuffer at 2c5962 swaps the 638-byte original Factory.lub for our build
  (bundle_src/Factory_revenge.lub, made by tools/genrevenge.py from bundle_src/revenge/*.lua).  Since
  2026-10-04 the copy here goes beyond the old standalone mod: Zack, Hades, Master Xehanort and No Heart were
  added (the old mod's folder is untouched).  2026-10-09, version 2: the core rules are KH2's own, verified in
  the KH2 exe and data (its atkp table holds a per-attack "revenge damage" byte, x10 fixed point, normal hit
  10; the engine sums it into the victim at +0xd48, compares against a per-boss cap at +0xd4c - default 100 -
  every update when flag +0x6c8 bit 4 is set, fires the AI event named "revenge" at the cap, and drains the
  gauge 1.0 a frame at 60 fps while the enemy is not being hit [14040e337..6b, 1403db730, 1403db440]).
  Mapped to BBS Lua: weights 1 / finisher 3 / magic 1.5 capped at 4 per cast (45-frame window) / shotlock 0.3
  / +0.5 launch; drain 0.2 per 30 fps frame after a 15-frame grace; limit re-rolled +-1 after each revenge.
  - Why Zack / Peter Pan / Hades / Maleficent could be looped: the engine calls OnDamageBefore on (nearly)
    every hit [2d5e30: skipped when the reaction controller's type at +0x14 is 4 or >= 6] but OnDamage only
    when no damage reaction is running [2d60f0: +0xac's +0x10 == 0] - the first hit of a combo.  A break-out
    that lives only in OnDamage (Zack, Hades, Maleficent) can never fire mid-combo, and Peter Pan's script
    has no break-out at all (his OnDamage returns nothing and his recovery is GotoState("Idling")).
  - The guarantee, v2: every such boss has a `counter` built from its own states (Zack: Climbhazard /
    Hakougeki / BackJump as his script rolls them; Hades: BladesCrossing / NoRiaFingernailofFire; Maleficent:
    warp or StaffAttack; Peter Pan: Attack3; Hook: Guard / Evade into JumpCutting), fired from OnDamageBefore
    at the limit and, when hits land in a state where the engine mutes the callbacks, from a watchdog in
    OnUpdate (RV.watchdog = 6 frames at the limit).  An HP watch in OnUpdate counts hits the callbacks never
    saw (1.0 each).  A break-out can arm cfg.iframes frames of EnableNoDamageReaction (Peter Pan 24, Zack /
    Hades / Hook / Maleficent 18) so the revenge cannot be stuffed - KH2 revenge actions behave so.
  - Config keys: `counter`, `armored`, `fired`, `quiet = "real"`, and new in v2: `grace` / `drain` (per-boss
    gauge decay; No Heart 120 / 0.03 for his vanilla 10 s memory), `vary`, `iframes`, `recover` (run after
    OnReturnDamage: Peter Pan goes to BeforeAttackIdling - his approach-then-attack - instead of idling),
    `haste` (scales idle times read through entity methods such as GetIdlingChangeTime; timers baked into a
    script as constants are out of reach).  `decay` (v1 hard clear) was replaced by grace / drain.
  - v2.1 (2026-10-09, after the first in-game report: Aqua's final Vanitas could still be comboed forever).
    The v2 build read every time constant as 30ths of a second; Entity.GetFrameRate is the entity dt in
    60ths [1402b8570 -> entity+0x20], so the gauge waited only 0.25 s and drained 12 a second - more than a
    real combo (a hit every 0.3 .. 0.9 s, +1 each) could build.  No boss could ever reach its limit in play;
    the offline test missed it by hitting every 3 frames.  Recalibrated: grace 60, drain 0.1 (KH2's 6 a
    second), magic window 90, watchdog 12, armour 40-48, No Heart 120 / 0.03; the loop test now hits every
    35 frames.  The HP watch is gated to hit strings (a counted hit within 60 frames, or a second hidden
    drop within 30) so poison / burn ticks cannot build revenge.  And every remaining dice-driven boss got a
    `counter` built from its own moves, so the watchdog guarantee now covers all of them: Vanitas (all,
    Cartwheel), Remnant (DarkSplicer2 / WarpAttack2), Eraqus (Guard + Kagerou), Armor (Guard), Braig
    (Escape), Mysterious Figure (WarpMove_Counter), Experiment 221 (Flee string), the wielders and
    Terra-Xehanort (EvasionAction; his own-AI form has no such state and stays covered by pre()).
  - Build needs Lua 5.1 in the game's bytecode format: github.com/lua/lua tag v5.1.1 with LUA_NUMBER float
    (luaconf.h: number type, "%.7g" / "%f", strtof), the string length dumped and loaded as unsigned int
    (ldump.c DumpString, lundump.c LoadString, header byte 4), and lua_dump stripping when LUA_STRIP is set.
  - The engine calls callbacks through EntityManager:CallFunctionArgN (CommonLua EntityMgr.lub), a plain
    `entity[name]` lookup, and decides which callbacks exist with EntityManager:CheckFunctions after the
    factory's Create: functions the mod sets on the instance are found.  OnDamageBefore ~= 0: hit negated;
    OnDamage ~= 0: no flinch [2d5e30, 2d60f0].
  - No Heart (b85vs00) has no small-flinch animations (only 220-223, the launch set) and his OnDamage always
    returns 1: he cannot be made to stagger.  His own answer to a string of hits is a burst on a timer
    (132 + 0..180 frames after the first hit); the mod drives that timer from the revenge value instead.
  - test/revenge: the boss scripts run against a stand-in engine (which hit breaks the combo, with and
    without the mod).  v2 adds test/revenge/loop.sh: 80-hit unbroken combos (OnDamageBefore only, as the
    engine behaves mid-combo) against every configured boss, one script per process (the stand-in's globals
    are shared and scripts contaminate each other); every boss must break out repeatedly, with forced
    revenges, and never let more than 16 hits go unanswered.  b52ex00 cannot run in the stand-in (its script
    trips over uninitialised fields there); its own-AI break-out is pre()-saturated and its shared-AI form is
    covered through the wielder script.  Zack's arena / Ventus-dream modes read their CounterNum from enemy
    params at setup, which the stand-in cannot supply - another reason their own counters looked dead there.
- The old standalone mods were uninstalled from the game files on 2026-10-04 (their folders are untouched;
  their stale backup records were moved to _to_delete in the game folder).
- See resource_loading.md.

## Pace: ending lag, air weight, speed, cast times (src/speed.c)
Research notes: player_actions.md (state machine, end tests, air physics), player_speed.md (animation speed
field), cast_timing.md (KH2 vs BBS cast data; scripts in /home/claude/bbs/speed).
- An action (player states 0x10 attack, 0x11 magic, 0x12 item, 0x13 friend, 0x14 finisher) ends only when its
  animation has played out; the stick is not read.  Each state has one "animation finished?" test; hook_ctx on
  each (2291bf, 2246c0, 224476, 2253ff, 262056, 2599ab, 2562bc; magic 26825f and 26828a) returns the state's own
  "end" address when the stick is tilted and the action is done.
- "Done" is worked out at run time from the game's collision controllers (entity+0x88 of the player and of the
  weapon pl+0x378: +0x20 count, +0x28 entries of 0x90; entry +0x28 / +0x2a state 0 not yet / 1 on / 2 on to the
  end / 3 over, +0x48 description with s16 on / off at +0x24 / +0x26 and +0x30 / +0x32): all windows over, then
  max(last off + FollowThrough, frMoveEnd + 1, frMarkEnd + 1, triggers + 2).  No windows at all: frChangeEnable.
- While a press can still continue the action (normal combo hit that is not a finisher and frame < frComboEnable;
  commands 5c / 6f / 75 that are pressed again) the stick must have changed since the attack began.  Finisher
  records: list in speed.c, from the combo tables.
- Air: 21cb00 keeps an aerial action (pl+0x318 bit 23) from sinking before frMoveEnd and then uses 16 % gravity.
  The hook at 21cb55 clears the bit in the register copy once the end test of this frame said "done, nothing can
  follow" (g_fall, cleared every frame in the tick).  2633ff: the 6-frame vy hold at the start of the fall loop
  is skipped when the previous state was an action.  26407b: button lock after an aerial action 30 -> 6 ticks.
- Speed: pl+0x1a8 (and the weapons') set every frame from the call at 220b1d, only over 1.0 or our own value;
  never over the game's 0 / 0.5 / 1.2 / 2.0.  Lunge speed divisor (21cf52, was 60.0) = 60 / factor.
- Casts: wind-up factor = t1 / 30 / KH2 release time (never below 1); recovery lock per family = KH2's ticks from
  release to the combo window (Fire 38, Blizzard 20, Thunder 24; BBS has 40 for everything).  Only for the plain
  cast path (commands 83-87, 89, 8a-8c, 8e-91, 92-95).
- Not tested in the game by me; the debug channel logs every walk-out with the window count it saw.

## MP cost in the command descriptions (src/mp.c, `[Menu] DescriptionCost`)

* The camp menu's help line is node 0x65 of the camp layout (`camp+0xd0`, camp = [150fb51f0]):
  `140417710(camp, msgId, args)` -> `SetNodeMsg 1401a8170(h, 0x65, msgId, 0, mode 1, args)`,
  `140417770(camp, char*, args)` for plain strings.  A command's description is message
  `0x320000 + id` (`14041e030(COMMAND*)`; deck edit 400800 / 400040 / 3fd180, shop 4074c0).  The texts are in
  `message/<lang>/system/CT00100.ctd` (FileID 0x320000; plain bytes, `\n` between lines, 1 to 3 lines, longest
  line 54 characters).
* SetNodeMsg looks the text up with `1401b3750(id, char **text, layout **rec, u16*, char)` (call at 1a81bf) and
  the text object copies it (`140125650`).  The mod hooks that call and, for ids 0x5b..0xe3 that are not items,
  returns its own buffer: the text + " MP n" ("MP All" for Cure while CureUsesAllMP is on), on a new line when
  the last line would pass 54 characters and there are fewer than three lines.
* Text encoding (decoder `140549180` -> `140128360`): bytes below 0x81 are one character, otherwise two bytes
  form one 16-bit code, Shift-JIS style.  Codes handled by the layout pass `140125b70`: colour style per
  character, f950 = 1, f951 = 2, f959 = 3, f066 = 4, f941 / f958 = 0; f961 + three digits = x position; f5xx /
  f1ae.. / f9ad.. = icons.  Style colours (`140127800`, ABGR, 80 = full): 0 the text's own, 1 ff000080 red,
  2 ff008000 green, 3 ff008080 yellow, 4 ff006080 orange, each multiplied by the node colour (`140122750`).
* Test: `t_desc`.
* The four items that restore MP as well as Focus (`EtherRestoresMP`, item_effect_hook: Ether 0xbf 50 %, Mega-Ether
  0xc0, Elixir 0xc2, Megalixir 0xc3 100 %; Item Boost raises it as it does the heal) get descriptions that say so,
  through the same lookup hook: messages 0x3200bf / c0 / c2 / c3, replaced only when the game's text starts as the
  English one does.  Lines kept to 54 characters, at most three.  Test in `t_desc` against CT00100.ctd.

## MP row in the menu's character panel (src/status.c, tex.c; `[Menu] StatusMP`, `StatusMPColor`)

* The panel is `CCampTopStatus` (vtable 140677440, object at CCampTop+0xe0, 0xa8 bytes): layout id 0x67
  "chara_plate" of camp.l2d ("camp" data handle at +0x1c, instance handle at +0x20).  Base class CMenuWnd:
  slot 1 = update / state machine `14042ab80` (state byte +0x9b; state 1 calls slot 5 "open"), slot 5 =
  `140417f00` here (base `14042b340` creates the layout with control 1 or restarts control 1, then the texts are
  written), slot 9 / 12 start control 2 (close), slot 11 `14042b2c0` and the destructor `14042a790` destroy the
  layout (also `14042b010` / `14042b090`).
* Nodes (id: sequence, position under the plate): 0x1d plate "player_flame" (-85,-87 in the layout; root
  animation of controls 1 / 2 = slide by 80 and fade over 16 frames; object 5 = sprite 179 = the four lines,
  3 high at y 0 / 11 / 22 / 41 from (-2, 25), vertex colours yellow / green / orange / white), 0x1c LV (0,18),
  0x19 HP (0,30), 0x1a FP (0,41), 0x1b NEXT LEVEL (0,51): label sprites of two quads (shadow, glyph) tinted by
  a colour key; 0x1e name, 0x1f / 0x22 / 0x24 / 0x26 the value texts at (69, 17 / 28 / 39 / 58), sequence
  "font_02" (id 0xb0), text table entries 13..16 (colour, pitch 8, size index 4, alignment index 6, kind 1).
* Label art: grey (128) pixel glyphs in the menu sheet (remastered texture of camp.l2d block 0, 1024x1024,
  uv = texels, 2 texels per screen unit): LV (8,646), HP (34,646), FP (62,646), NEXT LEVEL (90,646).  Four
  copies of the sheet are loaded (US_camp_arc6 / 10 / 11 / 12.png), identical in that area.  tex.c recognises
  the sheet by a hash of the HP / FP alpha shapes and writes an "MP" cell at (300,676) 26x16: an M drawn in the
  same style (tools/genstatart.py) and the sheet's own P.  The area is transparent in every copy and outside
  every sprite rectangle of the file.
* status.c, every frame after the panel's update (the class's vtable slot 1 is replaced): FP / NEXT LEVEL
  nodes and the plate's last two lines move down 11 (SetNodePos 1a7ec0; CloneNodeSprite 1a83f0 +
  GetNodeSpriteData 1a6980 on node 0x1d object 5); two sequence instances of "camp:0" are attached to the
  plate node (AttachToNode 1a4e10): id 0x184 ("charge_tab_grd", one sprite of three quads without a colour
  key) rewritten into line + label shadow + label, and id 0xb0 with SetFontParam (1a7450; struct of 1aa820:
  +0 text, +8 colour, +0x14 flags 1 text / 2 colour / 4 pitch +0x17 / 8 alignment +0x18 / 0x40 size +0x19 /
  0x80 kind +0x1c) and SetText (1a7b20).  They copy the panel's priority, queue and timer group and follow its
  visible flag.  Vertex colour = wanted colour (texel 128 x 0x80 = 1.0); text colour = half of it (a text
  colour counts double: FP's table colour ff / 44 / 1a is shown as ff / 88 / 34, its label key is ff8434).
* Lifetime: an attached instance points into its parent, so `Destroy` (1401a57f0) is hooked: when the
  panel's layout goes, ours are detached (1a5a50) and destroyed first.
* Test: `t_status` (texture patch on the real sheet; layout part on the real 2D runtime with camp.l2d and
  stand-in font tables [0x8f86968 + i*8]: +0x100 glyph count, +0x10c glyphs per row, +0x10e / +0x110 metrics,
  +0x118 glyph table).

## Minimum damage (EXP Zero's damage floor without the ability, on Critical; src/mp.c, `[Combat] DamageFloor`)

* `1400d2960(int *dmg, attackId, enemyMaxHP, enemyName)`, called from the enemy damage function `1402d24b0`
  (attack id `hit+0x9c`, enemy max HP `enemy+0x23a`, name `enemy+0xf0`): only when the player has ability 0x1c9
  (EXP Zero; test `140221900`) and the table is loaded.  The floor is applied last, after the enemy's own
  reductions, and only when the computed damage is lower.
* Table `Exp0/Exp0Param.edp` ("@EDP", version 2), requested with the system files by `1401da420` in every mode
  and every difficulty, loader `1400d2af0`: a class nibble per attack id 1..0x12d (0 basic attacks, 1 most
  commands / magic / finishers, 2 the big commands 5d 5e 7c 80 86 a6-a9 ac ad af b0, 15 none), then rows of 13
  ints (two unused, max-HP range, and per class: divisor, addend, percent/10).
  Normal enemies (max HP 15..132): (HP/30 + 3) x 1 / 2 / 3.  Bosses (name starts with 'b', max HP 200..1500):
  HP/200 (at least 3), (HP/200 + 3) x 1.5, (HP/200 + 3) x 2.5.  Outside the ranges: no floor.
* The mod hooks the ability test's call (d29a5 -> `140221900`): the answer is also "yes" when the difficulty
  byte `150fa0881` is 3 (0 Beginner, 1 Standard, 2 Proud, 3 Critical; the same byte the menus use to offer EXP
  Zero), or on every difficulty with `DamageFloor=2`.  Untouched and still tied to the ability: the cap on
  damage taken (`1400d2c00`, from `14023e4e0`: non-boss hits at most (maxHP-1)/2), the EXP take-back
  (`140284800`), the "active" test `1400d2cc0`.
* Test: `t_floor` runs the game's function on a table with these values, without the ability.

## Context prompts on triangle (src/menu.c, style.c; `[Menu] ReactionButton`)

* A context prompt (Talk, Open, Save ...; `cmd+0x60` bit 0x40000, plate type 5 at `cmd+0x298`: layout 7 with
  node 0x5a on sequence 0x19d, whose first text is the button picture, f564 in the layout) is answered in the
  command update by the confirm test `140272860` at 236b97; while it is up only that test, the action commands
  (232df0) and the deck input (233a00) run - the Attack part is skipped.
* The call is replaced: for every prompt but the counter prompts (command category 6) the answer is the triangle
  test `140272c20`.  The plate's picture is set to f567 (f564 for counters) after each plate update.
* The attack button then: off Attack it is the menu's as usual (the "swallow" rule no longer stands back for such
  a prompt); on Attack, menu_input does what the skipped Attack part would [236d9e..236e8e]: the COMMAND of the
  attack plate of the current style level to `cmd+0x80` and `+0x1c8`, `pl+0x570 = dt`, under the same conditions
  (no 0x1000 / 0x4000 / 0x20000, `pl+0x318 & 0xc0`, not action 0x18e at level 0) but without the plate's
  "pressed" state (the game covers the attack plates while a prompt is up, 1402376b0).
* A Command Style offer does not take a triangle press while such a prompt is up (style_decide).
* Placement: the offer always sits right above the gauge window (prompt place - 3, its timer bar under it); a
  context prompt is at the prompt place, or 16 above the offer (`PromptAboveOffer`) while an offer's plate is on
  screen (style_prompt_shown: offer, finisher offer, or the "accepted" flash).

## Lists over the menu, with a header (src/menu.c list_y / hdr_frame, tex.c hd_patch; `[Menu] ListX`, `ListHeader*`, `ListDims*`)

* Place, as in KH1: x = `ListX` (14; the menu is at 4.5).  The first row of a list of n rows opened from entry e
  (1 Magic, 2 Items, 3 D-Link) is on row min(e, 3 - (n - 1)) of the menu's rows (0..3 at 198 / 213 / 228 / 243;
  negative rows lie above the menu, 15 apart): the list starts on its entry's row and runs down, and once it would
  pass the last row it ends there and grows upwards.  The D-Link list (the game's own plates, type 6) follows the
  same rule and gets the lists' draw priority (12 / 13: over the entries at 5 / 7 and the gauge window at 8).
* The four entries are darkened while a list is open (L2D SetColor on our instances, `ListDimLevel` percent).
* Header = an instance of bc01_00 layout 5 with node 0x46 (gauge) hidden; node 0x47 is the label: sequence 0x25a
  in battle (plate efaf03, letters ffff00), 0x25d in the field (005ff0 / 00ffff) - the game swaps them the same
  way for its window [2320e0], it does not use control 5 of 0x25a.  Object 1 = sprite 34 (plate, three quads:
  -2..5, stretch 5..76, 76..90), object 2 = sprite 35 (letters, one quad (0,-1)-(82,9) with uv 8,294..172,314 =
  a 20-unit strip at half size).  Private copies of both: the plate's stretch is made 24 longer (as long as a
  row), the letters' quad is pointed at one of our words.  Placed at the first row + (-1.6, -9.1).
* **Language files.**  The game loads `arc_<lang>/pc/p00common.arc` (en, fr, de, it, es in bbs_first; the plain
  `arc/pc/p00common.arc` is the Japanese one).  bc01_00.l2d differs between them only in sprite 35's quad (JP
  73 wide, uv to 154: "COMMAND"; the others 82, to 172: "COMMANDS" etc.), and the command sheet - the remastered
  texture of bc01_00, 1024 x 1024 - differs in the three words at px 17..367, 529..706 (and JP in a few icons).
  Everything in l2d/ and tex/ before this was the Japanese set; en/ has the English and French ones.
* The words MAGIC, ITEMS, D-LINK are our own drawings (tools/hdfont.py: rectangles on a 31 x 24 box, slant 0.4,
  heavy left stem 12.5, thin strokes 5.6, bars 5.3, black shadow at +3,+3, measured from the game's label; no
  pixels of the game are used).  tools/genhdart.py writes them to src/hdart_gen.h with 24 check pixels taken where
  the EN, JP and FR sheets agree; tex.c adds them at px 16,740 (236 x 132; units 8,370), an area all three leave
  empty, when the game creates the texture.  No recognised sheet: no header, the lists still work.
* Tests: `t_listhdr` (rows; the three sheets recognised and patched; header on the English bc01_00.l2d).

## D-pad right opens a list (src/menu.c menu_input; `[Menu] RightOpensList`)

* On the main menu, with the cursor on Magic, Items or D-Link, a new press of d-pad right (`140272cd0`) does what
  the confirm button does there: opens the Magic / Items list, or the game's D-Link list through `1402388a0`.
  Nothing on Attack or inside a list.  It does not depend on who owns the confirm button, so it also works while a
  counter prompt is up.  Test: in `t_menu`.

## Jump button closes an open list (src/menu.c; `[Menu] JumpClosesList`)

* The player's input gather `14021f9f0` asks the pad object "jump pressed" with `140272f10` (call at 21fb99; the
  button is the menus' cancel button, mask `1408221c4`, circle by default; the answer becomes flag 1 of
  `pl+0x31c`; `140272670` at 21fbb4 is "held", flag 0x400).  The call is replaced: with the Magic / Item list
  open the press closes the list and the answer is no; with the game's D-Link list open (`cmd+0x60` bit
  0x10000000) the answer is no and the game's own "close the list" pad test (`140272d30`, d-pad left, call at
  2337e9 inside 1402334f0) says yes once.  Test: in `t_menu`.

## Shortcuts (src/shortcut.c, sccamp.c, menu.c; `[Shortcuts]`, bindings in bbskh2_shortcuts.ini)

* A shortcut is "face button -> deck slot 1..8", per character (`150fa0880`, 0..2), default slots 1..4.  Rows in
  KH2's order: circle, triangle, square, cross.
* Pad: bit words in the PS2 layout (0x10 up, 0x20 right, 0x40 down, 0x80 left, 0x400 L1, 0x800 R1, 0x1000
  triangle, 0x2000 circle, 0x4000 cross, 0x8000 square; the left stick repeats the directions at << 16):
  `148f64930` held, `..34` newly pressed, `..38` changed, `..3c` with auto-repeat, `..9c` input off; history ring
  index `..54`, arrays `..58` / `..78`.  All written by `1400ed040` (called at e3716); keyboard and mouse
  bindings arrive in the same words.  Readers: `1400ecd90` new presses, `ecd40` repeat, `ecca0` held.
* shortcut.c, after that call: while L1 is held without R1, the command menu is on screen (menu.c tells), no
  D-Link list is open and the player's pad object (`mgr+0x58`: flags +0xa0 bit 0, lock timer +0x54) is live,
  the four face bits are removed from all of those words and a new press is kept for `BufferFrames`.  menu.c
  uses it through the same gate as a confirm in the list (can_use_now, mp_use, queue at cmd+0x80..).
* Vanilla L1 in control type 0: camera behind the character (pad test `1402729c0`, call at 22b5df) and next
  lock-on target (`140272b80`, call at 2650d4).  Both calls are replaced: true for one frame when L1 is let go
  within `TapFrames` and nothing was done with it.  Other control types keep the game's camera test.
* The L1 list (menu.c sc_frame): four more instances of the deck plate (bc01_00 layout 3), body node 0x5d always
  on sequence 0xb, layout control 0 = the game's look for the plate under its cursor: sprite 17, a plate 10
  longer whose left end is a solid tab (x 0..20 of 127), and a text object centred at (11, 8) in the tab that
  holds a button picture (text table entry 16: font index 3, pitch 8, alignment index 4 = centre / middle; the
  alignment value is low nibble 0 / 1 / 2 = left / centre / right, 0x10 / 0x20 = middle / bottom, `140123d80`).
  In control 0 the name node's text is at x 27 and the item count at 111.
  Grey: all plate colours are colour keys on grey texels and colours only multiply (key x local x parent), so
  the key has to change.  An object's animation record and key table are pointers of the object (`CD2Obj +0x30`
  record, `+0x38` key table; set by `1401abb80 -> 1401aa7f0` on every control change, read at draw time by
  `1401a87b0`), so the seven objects of that node get records of our own: record = 0x18 bytes {s32 maxf, s16
  sprite, u16 first key, u8 keys-per-kind[11] (status, baseX, baseY, offX, offY, rotX, rotY, rotZ, scaleX,
  scaleY, colour), kind (0 root, 1 sprite, 2 text; 0x80 = not drawn), blend, flag, scissor, z}, key = 0xc bytes
  {float frame, value (float or R G B A), interpolation}.  Put back every frame (a control change resets them).
  The inside is object 6 (sprite 2) with private quads: the game's gradient art (2,30..32,60).
  Button pictures in text: f564 confirm, f565 cancel, f566 square, f567 triangle, f568 L1, f569 R1, f56a L2,
  f56b R2, f57b circle, f57c cross (`140546c00` swaps in the current device's glyph).  A picture inside a line
  of text hangs below the letters; a text of pictures alone with "middle" alignment is centred.
* Menu (sccamp.c).  Command Decks = `CCampDeckTop` (vtable 140674c70; update 140402fa0 in slot 1; state +0x9b,
  2 = browsing; +0xea pane-pick mode; +0x11 pad lock of the screen's own confirm / cancel reading).  Children:
  +0xb8 the list `CCampDeckTopCmd` (vtable 140674d40, layout 0xcc: root node 0xb, entries nodes 1..5 at
  (-230, -75 + 17 i), sequence ids 4b / af / 113 / 177 / 242; cursor +0x8e; hand +0xe8 with offset (8, 10),
  light frame +0xf0 with offset (105, 0), both attached to node 0xb; positions +0xa8; input = vtable slot 8,
  140404490, which returns the "action taken" flag of 14042b450 and wraps the cursor 4 <-> 0 by itself),
  +0xc0 the Battle Commands pane `CCampDeckTopBtl` (vtable 140674e10, layout 0xc9: slot anchors nodes 1..8,
  18 apart; plate objects +0xc0 + 8 i with handle +8 and "locked" +0x40, layout 0x6f: node 3 name, 4 "LV",
  5 level; hand +0x100 attached to node 1, no offset).
* The entry: an instance of sequence 0x177 attached to node 0xb at (-230, 10), text through SetFontParam as
  text-table entry 24 (pitch 11 becomes 12 for font 0 on PC, `1401aa620`).  The text object of a sequence
  instance is the first object with one (`1401abb00`): object 1 here, the same as in the layout's own node.
* The list's input is wrapped: when the game's cursor wraps, ours takes over (mode ENTRY) and the game's entry
  is un-lit; from there up / down hand the cursor back to entry 4 / 0.  While in our modes the screen's pad lock
  is set so that its own confirm / cancel see nothing; cancel on the entry clears it and runs the game's input,
  so the screen closes as usual.  If the list's input stops being called while we hold the lock (the screen's
  update wrapper checks every frame), everything is let go.
* The screen's update (140402fa0) returns 1 when the screen is finished (state 4) and the menu then drops it;
  the wrapper has to return that value (a void wrapper closed the screen on its first frame).
* In the pane, a slot's buttons are shown by its level text node (5): anchor moved to (123, 5.3) (a button
  picture is drawn 2.2 below the middle of its text: measured in play, anchor 7.5 gave 5.1 .. 14.3), font
  parameters through `1401a7cb0` (node version of SetFontParam) = font index 3, pitch 8, alignment index 7
  (right / middle), no tint; put back to entry 65's (font index 4, alignment 0, the saved colour and text).
* The pane's plates come from `140402820`: per slot the 6-byte deck record of `14041dd30` (s16 inventory index,
  byte 3 = row inside a multi-slot command, byte 4 = slots the command takes); a plate object is made only where
  byte 3 is 0 (the first row's plate is made taller, `140410370`), so the second row of a two-slot command has
  no object at pane+0xc0 + 8 i.  The slot cursor skips slots without one.
* The fill `1404103c0(btn, COMMAND*, name 3, 2, level 5, 0, 0, LV 4, record, 0)` gives an item its count in the
  level node as "x3" with font parameters flags 0x29: alignment index 6 (right) and x offset 5 (text +0x2b4).
  The offset stays on the text object, so ours are set with offsets 0 (flag 0x20) and the old alignment, font
  and offsets are saved from the text object (+0x298, +0x2cc / +0x2cd, +0x2b4 / +0x2b5 / +0x2c8 / +0x2c9) and
  put back.
* Tests: `t_shortcut` (pad), `t_sclist` (L1 list on bc01_00.l2d), `t_sccamp` (menu on camp.l2d with the game's
  own list input).  Not covered offline: how any of it looks.

## Grey "COMMANDS" window while the shortcut list is shown (src/menu.c hd_frame; `[Menu] ShortcutHeader*`)

* The window is bc01_00 layout 5 (the game's instance at cmd+0x98): node 0x46 = sequence 0x258 in battle (7
  objects: root, sprite 15, 32 shadow, 37, 36 gauge fill, 37 gauge bar, 33 frame) or 0x259 in the field (6: no
  sprite 15); node 0x47 = sequence 0x25a (root, sprite 34 label plate, sprite 35 letters).  Colours are colour
  keys on grey texels: frame ffdc00 / 005ff0 (alpha ff, a0), plate efaf03 / 0056e6, letters ffff00 / 146efa.
* A Command Style's window and the D-Link's (dl_01) are other instances (cmd+0x9c) and are never touched.
* Grey = the object plays a twin of the record the game gave it: same record, key index into our table
  (g_sc_keys from entry 16, 40 per object), where the record's keys are copied and its colour keys (the last
  `keyn[10]` of them) set to grey with the game's alpha.  So appear / fade animations keep working in grey.
  Object +0x30 = record (set with 1401aa7f0), +0x38 = key table.
* Back to normal: the game's record (remembered per twin) and the node's key table (read from the root object,
  which is never changed) are put back.  A control change by the game (1401abb80) puts its own records back but
  not the key table; hd_node sees "game's record on our table" on its next pass and repairs it.  Until then such
  an object reads zeros or twin keys from our table: at worst one frame of a wrong colour, no bad read (the
  table has 0x10000 entries).
* The gauge (sprites 36 / 37) keeps its colours.  Test: `t_header`.

## D-Link list: cursor speed (src/menu.c link_scroll; `[Menu] LinkCursorTime`)

* The game's D-Link list is a wheel under a fixed cursor: a step calls 140205cf0(plate, direction) for every
  entry [233491] (new place plate+0x6e, state 2, time plate+0x68 = 10).  The plate's update steps the move with
  1402044c0 and, when it is over, makes the entry under the cursor (place == cmd+0x354, or the last) the selected
  one (flag 4, state 4, 140205a60 -> state 1), the others state 0 [204d5a].  140233200 takes no input until the
  entry under the cursor is ready (1402047f0), so a step costs 10 time units, a third of a second.
* The two calls of 205cf0 in 233200 (2332a1, 233491) go through link_scroll: time 0, the game's own step
  function ends the move at once, and what the plate's update would do next is done right there.  The list then
  moves at the pad's repeat rate, as the Magic and Items lists.  `LinkCursorTime` > 0 keeps the animation with
  that time (10 = the game's).  Test: in `t_menu`, on the game's own functions.

## D-Links during MP charge (`DLinkDuringCharge`)

* The first design refused a new D-Link while the bar recharged, in three places: the menu's D-Link entry
  (entry_usable), the game's "open the D-Link list" [2388a0] and "confirm an entry" [205ee0].  Reported as a bug: out
  of MP, no D-Link.  A D-Link costs no MP, so all three now go through `mp_blocks_link()`, which is only true with
  DLinkDuringCharge = 0.  The D-Link deck's commands still wait for the charge (use_hook), items do not.

* `DLinkRefillsMP` (asked for right after): starting a D-Link fills the bar and ends a running charge, as a Drive
  Form does in KH2.  Done in the hook on 205ee0, the game's confirm of a D-Link list entry (it takes the entry only
  if it is not used up and its command id is above 0x162): that is the player starting a link and nothing else - a
  link carried into another room does not come through it, so there is no refill per room.  Not when a link is
  already active.  Test in `t_use`, through the installed hook and the game's own function.

## Air combat: BBS against KH2, and air hops (src/speed.c hop_hit / hop_frame / h_gravity; `[Speed] AirHop*`, `AirLog`)
* BBS (player_actions.md 4.2, 4.3): g = -0.0049 per tick^2 (1/60 s), jump 0.135 per tick, height cap 1.75.  An
  aerial action starts with vy = 0, bit 0x800000 and 16 % gravity; 21cb00 forbids sinking before frMoveEnd and
  zeroes any rise from frMoveEnd on.  Every hit of an air combo starts from vy = 0 again, so a whole combo loses
  almost no height; an EXMOVE hit with the target 4 or more above starts at +44u, faster than a jump.  After the
  action: 6 frames of no gravity at the start of the fall, 30 ticks without buttons (the mod: AirWeight / AirLock).
* KH2 (its own files, read from the user's install): 03system.bin > pref > plyr, Sora's entry: AttackFirstV0 8,
  AttackComboV0 6, AttackFinishV0 12, AttackJV0 8 (forms differ, e.g. 12 / 9, 18 / 24); sstm FallMax 16; prty
  Sora JumpHeight 185 (Valor's high jump 235 / 310 in fmab).  obj/P_EX100.mset: the motions A330 / A331 (air
  combo hits, combo window from 22 / 20 of 60 a second) and A332 / A333 (finishers) have no "no gravity" range at
  all; the moves A341..A346 have one for part of the move (16-24 ... 16-100).  So KH2's air combo is a hop per
  hit with gravity on.  Its gravity is not in the data (a code constant).  Units: KH2 about cm per 1/60 s, BBS m
  per tick; a hop of 6 is about 3.6 m/s against BBS's jump of 8.1 m/s.  (Which motions are the air combo is
  read from their markers, not from a name table.)
* The mod (AirHop): for Attack-button hits in the air (state 0x10 part 5, record AERIAL without RISE / DOWN /
  FLOAT, local player only):
  - start of a hit (tick hook 220b1d, new record or the animation frame went back): vy = AirHopKeep x g x T / 2,
    T = frChangeEnable (0x0d, or cmb) x 2 / animation speed, clamped 8..80 ticks - the speed that is back at
    the start height when the next hit can start; x AirHopFirst on the first air hit since the attack state was
    entered; the finisher (is_finisher) gets AirHopFinisher x the jump speed (27.6 u) instead.  pl+0x508 = g.
    This replaces the +44u pull of 229fa0 for these hits.
  - every frame of such a hit (21cb55): xmm0 = g x AirHopGravity, on at 21cb7a (written to pl+0x508), bit 23
    cleared in the register copy (no no-sink clamp), xmm4 = 1e9 so 21cc00 never zeroes a rise.  pl+0x318 keeps
    bit 23, so the game still ends the attack into the fall state.
  - Ventus' first air hit (chg 15) at speed 1: vy 0.079, up 0.6, +0.1 when the next hit can start.
* `AirReach` (asked for, option 2 of four discussed): the lift is kept only as one rise to a target well above -
  KH2 has a separate rising attack for enemies about 0.5 .. 1.9 m above (its plyr U MinH / MaxH -50 / -190, Y
  up is negative there) within about 2.6 m.  At the start of a hop hit, if pl+0x600 > 0 and pl+0x618 (target
  height difference, positive = above; what 229fa0 itself uses: (dy x 10 + 4) x u) >= AirReachMin: vy = max(hop,
  sqrt(2 g min(dy, AirReachMax))), at most the jump speed.  No horizontal-distance test (no field for it found).
* Reported right after AirReach: on the ground, an enemy a little above, the player was pulled into the air.  The game
  plays air-combo records (AERIAL, which always sets bit 23) from the ground too; its own rise (229fa0) is only
  given when 264a10 says "in the air", so they stay down.  The hop and AirReach had no such test.  Now a hit hops
  only if it starts in the air - pl+0x318 bit 22, which the player update sets from 264a10 after the tick hook
  [220550 tail]; 264a10 itself is not called from the mod (it can call 292350, a landing effect) - and the
  gravity hook applies only to the record that started hopping (g_hop_live).  Test in `t_speed`.
* `JumpHang` (default 0, asked for): the 6-frame hold of the fall state (2633ff, the hook that already skipped it
  after actions) is skipped for every fall, so the top of a jump no longer hangs for 0.2 s.  JumpHang = 1 gives
  the game's hold back for jumps and ledges (still none after an action while AirWeight is on).
* AirLog = 1: a line per frame in the air (state, part, record, frame, vy, height summed from vy, gravity) and a
  summary on landing; "air: hop" lines say what each hop got.
* Test: in `t_speed`, on the game's integrator.  Not seen in the game by me: what the homing of [mks, mke]
  (21b670) does to the hop, and how it feels.  Next steps if it works: cap the pull towards targets above for
  other aerial moves, no hover at the top of a jump, finishers ending in a drop.

## Launch height (src/speed.c h_launch; `[Speed] LaunchHeight / LaunchMin / LaunchMax`)
* Asked for after comparing with KH2 (the user had first checked that the frequent launches happen in the unmodded
  game too).  BBS: the Attack button's record is chosen from the combo table (pl+0x5c0, PAO_<char>Cnn.bin, 20-byte
  entries: mode, count, PBA slot, then (type, value) conditions; type 1 = in the air, 4 = a bit of the mask below,
  6 = same as the entry above) by 2239b0, which builds the mask from the target [21b670]: pl+0x618 = target point
  height (a joint by default, 1d4920) - player height - PPM+0x1c (0.2 / 0.3 / 0.4), pl+0x614 horizontal distance,
  pl+0x620 angle.  Within 3.5: bit 1 (2) if dy > 0, bit 5 (0x20) if dy < -1.5; bit 3 (8) beyond 3.5; bit 2 (4)
  angle <= 45 deg, bit 4 (0x10) 45 .. 135 deg.  All three base tables: on the ground with bit 1 -> PBA slot 4,
  the rising attack (16 / 301 / 582); in the air bit 1 picks another air hit (19 / 304 / 584).
* KH2, first look (03system pref plyr, Sora: U MinH / MaxH -190 / -50, URange 260) gave 0.5 .. 1.9; the first build used
  that.  Asked to make Ventus match Sora exactly, KH2's code was searched (Ghidra, all 24789 functions): no code reads
  those plyr fields (the plyr entry is reached only through obj+0x150, and only +0x2c, +0x50..+0x70 are read).  The
  attack choice is 00battle.bin ptya (0x44-byte entries; Sora = pointer 1): 1404029f0 / 1404027f0 test, per entry,
  Near <= horizontal distance - target radius < Far, High < target y (player local, Y up negative) <= Low, angle.
  The target is the enemy's target collision (1403c9c00: bone + offset, radius +0x10, half height +0x12 = "span").
  Flag 2: High -= the jump height (prty, 185); flag 8 / 0x10 flip the span.  Sora's base rising attack (id 0, motion
  182, flags 0xa): -435 - span < y <= -110 - span, Far 260 -> the bottom of the target 110 .. 435 cm above Sora's feet
  within 260 cm of its edge.  Ordinary first hits (id 4 / 6, flags 0): -110 - span < y <= -20 + span -> the target
  reaches into 20 .. 110 cm.  Units: Sora's skeleton is 160 tall (cm), Ventus 1.59 (m); jump 185 / 1.86.
* The hook at 223b0d (ecx = mask, rbx = player, r13 = 1 in the air, pl+0x608 = target entity): on the ground,
  bit 1 = LaunchMin <= target phys+0x34 - player phys+0x34 <= LaunchMax and horizontal distance - target phys+0x190
  (radius, clamped 0 .. 1.5) <= LaunchReach (defaults 1.1 / 4.35 / 2.6).  BBS has no "bottom of the target volume";
  the entity's position (its feet / origin) stands in for it.  In the air nothing changes.  Command Style tables that
  test bit 1 on the ground follow the same rule.  Test in `t_speed` (the handler on a context with fake entities).

## D-Link entry icon (menu.c link_icon; `[Menu] DLinkIcon / DLinkIconX / DLinkIconY`)
* The entries' icons are sequences of bc01_00 on node 0x5a of the deck plate (layout 3): 0x20 keyblade, 0x21 hat,
  0x22 bottle (control 3 the still one), the D-Link entry had none.  The hearts are sequence 0x2e (the D-Link gauge's):
  control 0 the pink heart (sprite 61, texture 362,90..390,118, 14 x 14 at (0,0)), 1 / 3 the grey one (sprite 62),
  2 / 4 pink 10 to the left.  Its sprite sits at the node's origin where the other sequences put theirs around
  (104, 10), so the node is moved by (97, 2) (3 was too low, 1 too high) (1a7ec0 -> the node's +0x58 method), and node 0x5a is put back on
  control 0 every frame (the plate's set_control sets every node's control; 1 / 3 would be the grey heart).
  Asked for: the pink one always, also when the entry cannot be used (the other icons do not grey out either).

## Command Style in the air (style.c h_style_air; `[Style] AirChange`)
* State 0x19 (style change) starts in 28a950: for a style candidate (cmd+0x190) it asks 264a10 at 28ac1d; in the air it
  sets pl+0x318 bit 0x80000 and plays the fall loop (motion 10), and the update 286380 integrates gravity until
  landing, then plays motion 0x87 (0xd3 for categories 7 and 12) and goes on.  The hook at 28ac22 (rax = 264a10,
  rdi = player) makes the answer "on the ground" for category 6 (Command Styles) only: the change plays at once;
  1d3b90 right after zeroes the body's velocity and nothing in that state moves the player vertically, so it plays
  in the air.  The end (21a490(pl, -1)) asks 264a10 itself -> 264060, the fall state.  Not seen in the game by me.
  Test in `t_style` (the handler on a context).

## Speed per command (src/speed.c cmd_override; `[CommandSpeed]`)
* Asked for: Mega Flare a little slower, and a way to set every command by itself.  Mega Flare (0xac) was never in a
  cast-time family: it, and every spell but the four families, played at Actions (1.15) from start to end.
* `<id>=<speed>` in [CommandSpeed] (ids 1..0x23f, 0.25..4).  `wanted()` asks the list first: while the game says a
  command runs (`pl+0x5e0` != 0, the category the starters set) the speed of `pl+0x312`, the running command's
  id (21f9b0), if it is listed.  That covers every state a command runs in - 0x10 attack, 0x11 magic, 0x12 item,
  0x13 D-Link / friend, 0x14 finisher, 0x15 movement, 0x16 guard, 0x17 counter, 0x18 shotlock - through the same
  per-frame setter as the rest (so Haste / Slow / Stop still win and the lunge fix follows).  The list in the ini is
  generated from the command table (type / category) and the English names (CT00500.ctd).
* Not seen in the game by me: in particular that the Attack combo runs as id 1 (or the Command Style's own
  attack, 2..0x10 / 0x2e) in `pl+0x312`, and that movement, guard and shotlock animations look right at other
  speeds - parts of those are timed by the state's clock, not the animation.
* Test: in `t_speed`.

## MP Haste (mp.c charge_speed, haste_texts; `[MP] ChargeSeconds`, `MPHasteBonus`, `MPHasteRename`)

* KH2 (its 00battle.bin and the wiki): the MP charge takes 50 s / (1 + bonus); MP Haste 0.25, Hastera 0.5,
  Hastega 1.0 (0.75 in Final Mix).  Here: `ChargeSeconds` 25 (20 at first, 50 for one build, then 25 on request),
  and the game's own Magic Haste - ability
  0x1d0, which can be installed several times; `140221900(player, id)` gives the number in effect, the byte
  `player + 0x4a3 + slot`, slot = command table +7 (13) - adds `MPHasteBonus` 0.05 a copy (0.1 in that one build): 25 / 23.8 /
  22.7 / 21.7 / 20.8 / 20 s for 0..5 copies.  Attack Haste (0x1cf, slot 12) keeps the 0.05 a copy both abilities had before
  (`AttackHasteBonus`).  The two then do exactly the same, so on request Attack Haste is shown as MP Haste too:
  name message 0xfa01cf, description 0x32020c ("Shortens the reload time for all attack commands ...", 126
  bytes), same treatment as below.  An ability whose bonus is set to 0 keeps the game's texts.
* The name.  Command and ability names are file `message/<lang>/system/CT00500.ctd` (first id 0xfa0000, 498
  messages, message i = command i).  A message file: `@CTD`, +8 first id, +0xc u16 layouts, +0xe u16 messages,
  +0x10 offset of the message records (u32 id, u32 text offset, u32 layout), +0x14 / +0x18 the other tables; texts
  are plain bytes.  When a file is in memory the game calls slot 1 of CRsrcCTD's vtable (637910 -> 140112ed0):
  it sets +0x90 file, +0x98 records, +0xb0 / +0xb4 the id range, and for file 0xfa0000 copies every text's
  address into the command table (`140814908 + id * 0x18`).  Nothing else reads a name (no other use of
  0xfa0000 in the exe).
* The description.  An ability's is message `0x32003d + id` of CT00100.ctd (`14041d880`, through the plain
  lookup `1401b3d80`, which has 394 callers - not the SetNodeMsg lookup the MP-cost text hooks).  Magic Haste:
  0x32020d, "Shortens the reload time for all magic commands / installed in your deck. Multi-install the ability
  for even / quicker reloading." (125 bytes, lines up to 58).
* So the vtable slot is replaced: after the game's function, the two texts are rewritten in the loaded file
  itself, which every reader then sees.  The name goes there when it fits (MP Haste does; a longer one from the
  ini goes through the name pointer instead); the description is cut to the room the game's text has.  Only when
  the text found is the English one - other languages keep theirs - unless `MPHasteName` / `MPHasteHelp` are
  given.  The mod is loaded before the game reads any file, so the slot is in place for the first load, and a
  file loaded again is rewritten again.
* Test: `t_haste` (charge times on the game's own ability count; the texts through the patched slot on the real
  files, BBS_MSG_NAMES / BBS_MSG_HELP).  Not seen in the game by me.

## Berserker (mp.c berserk_factor, damage_hook; `[MP] BerserkerDamage`, `BerserkerRename`)

* Asked for: "command boost" renamed to Berserker, doing more damage during the MP charge and nothing else of
  KH2's Berserk Charge (there: Strength +1, +2 in Final Mix, per copy, and combos that never reach a finisher).
  The game has no Command Boost; the one Boost about commands is **Reload Boost** (0x1d9, slot 22, one copy:
  "Shortens the reload time for all commands installed in your deck whenever your HP falls below 25%"), which
  like the two Hastes has nothing left to do here (its only reader, the reload factor `140235140`, feeds a
  reload the mod replaces).  That one was taken.  The name is "Berserker" (the request spelt it "beserker").
* A percentage, not KH2's flat Strength: the test save is level 99 (Strength 53), where +2 would be about 4 %, and
  without the endless combo the damage is all the ability is.  20 % in the first build, 5 % on request.  The
  game truncates damage to a whole number, so 5 % as a multiplier added nothing to a hit below 20 - and basic
  hits are about 5 to 15 for most of the game ((Strength - Defence) x power: Strength 3-4 at level 1, 11-14 at
  20, 21-23 at 40, 49-53 at 99 from the level table 140649790; enemy Defence 3 to 15 by the wiki).  So, on
  request, the bonus is rounded UP: the game's own whole number + ceil(that x percent / 100), at least 1.
* Where damage is worked out: `1401f9180(attack, hit)`, called only from `1401f9790` [1f985a] when an attack
  registers on a target; the result is the hit record's +0xaa (s16), which the enemy's damage function
  `1402d24b0` then takes off its HP.
      crit (`1401f90e0`: attack+0x78 percent chance, +0x7a multiplier percent)
      x clamp((attack+0x82 stat - hit+0xac defence) x attack+0x84 power / 100; hit+0xb0 .. hit+0xae)
      x hit+0xb2.. resistance to element attack+0x80 (1..6) / 100 x hit+0xc4 x attack+0x88
  attack+0x7e & 0x7f is the kind: 0x22 gives stat + power (a cure: 2d24b0 negates it), 0x2a a percentage.
  attack+0x60 is the attacking object's entity id, +0x94 its owner's, +0x8c the command id.  The record is filled
  from the owner's callback in `1401f3d90` (stat, power, element, multiplier 1.0 by default).
* The hook on that call lets the game work the damage out (once: the critical hit is a dice roll in there), adds
  the rounded-up bonus to the result and holds it to 0x7fff; the attack record is not touched.  (The first two
  builds raised attack+0x88 for the length of the call instead.)  Conditions: the MP charge is
  running, the player has the ability (`140221900`), the kind is a damaging one, and the owner (`1401d45c0(id)`,
  the lookup 2d24b0 uses for the same question) is the player object, or its parent (+0x10) is - a spell in
  flight - or either is of type +0x28 == 1, the player class (2 = enemies [20cc70]).
* Texts: as MP Haste's, name message 0xfa01d9 (12 bytes of room) and description 0x320216 (97 bytes).
* Test: `t_berserk`, the game's formula through the hook with a stand-in entity table; texts in `t_haste`.  Not
  seen in the game by me: that the hit of a real attack carries the player as owner is read from the code, not
  observed.

## MP cost of the D-Link-only commands (mp.c base_cost; `DLinkCostByClass`)

* Reported by players: D-Link heals for 5 MP.  The fourteen commands only D-Link decks have (ids e4..f1, category
  8; decks: table at file offset 8161f8, 14 D-Links x 3 levels x 8 ids) all have reload 5 in the command table, so
  cost = reload made them 5 MP.  Two heal: e8 (Cinderella) and e9 (Doc), strength 100 / 125 / 150 by D-Link level
  against Curaga's 100; several attacks are stronger than 20 MP commands.
* The game's own rule for ordinary one-slot commands is by class (table byte +4, low nibble): class 1 reloads in
  10, class 2 in 15, class 3 in 20 (attack: 10/12 at 10, 7/11 at 15, 8/8 at 20; magic the same with the cures,
  status spells and Mega Flare as exceptions).  The D-Link commands now cost by that rule; the two heals are cures
  (`is_cure`): base 30, all MP under CureUsesAllMP.
  e4 Holy 15, e5 Wish Circle 10, e6 Enchanted Step 15, e7 Wish Shot 15, e8 heal, e9 Doc heal, ea Grumpy 15,
  eb Sneezy 15, ec Happy 10, ed Sleepy 15, ee Bashful 15, ef Dopey 20, f0 Dark Spiral 20, f1 Dark Splicer 20.
* A `[Cost]` entry now also wins over CureUsesAllMP (the ini always said it overrides everything; for the cures
  it did not).
* Test: in `t_ether`, against the real command tables.

## Two shortcut sets (shortcut.c, sccamp.c, menu.c sc_frame; `[Shortcuts] Sets`, `[Menu] ShortcutColor2`)
* Bindings are `g_bind[character][set][row]`; set 2 is saved beside set 1 in bbskh2_shortcuts.ini as Circle2,
  Triangle2, Square2, Cross2, deck slots 5..8 to begin with.  `sc_slot(row)` / `sc_assign(row, slot)` are the set
  shown in battle; `sc_slot_in` / `sc_assign_in` name the set.
* Battle: while the list is up the d-pad (0xf0 of the pad words: up 10, right 20, down 40, left 80) is taken from the
  game like the face buttons, by the same "counts where it went down" rule, and any new press flips the set (the
  menus' cursor sound, SE 1).  The set is remembered for the next time L1 is held.  A d-pad button still down
  when L1 is let go stays hidden from the game until it is let go.  The list's frame and tab (key 3 of the list's
  own key table) take ShortcutColor2 while set 2 is shown, and so does the "COMMANDS" window over it (hd_rgb: its
  frame, sprite 33, ShortcutColor2; its label plate, sprite 34, the same colour scaled by ShortcutHeaderPlate /
  ShortcutColor per channel, a87e38 by default; the twins' colour keys are rewritten on every grey pass).
* Menu (Command Decks > Shortcuts): while picking a slot, square (unless it is the confirm or cancel button)
  switches the set shown and edited; the plates show that set's buttons and the help line says which set it is.
  In "press a button" mode square is still a button to give.
* Tests: in `t_shortcut` and `t_sccamp`.

## Shortcuts: a face button counts where it went down (shortcut.c pad_frame)

* Reported: circle held, then L1 -> circle's shortcut was used.  The game's "pressed this frame" word is
  `~history[last frame] & held` [1400ed040], and the mod wipes the face buttons from that history entry while the
  list is shown (so nothing buffered sees them).  From the second frame of the list a held face button therefore
  read as pressed anew every frame: a button held before L1 fired at once, and a held one repeated.
* Presses are now counted by the mod itself from the raw held word, frame to frame (`g_face_prev`), list shown or
  not.  And the other direction, same cause: a face button still down when the list goes away stays hidden from the
  game until it is let go (`g_face_block`), or the game would take it for a fresh press and attack.
* Test: in `t_shortcut`.

## The lists' own colours (menu.c tint_node, row_colours, hdr_frame; `ListColors`)

* Asked for: Magic blue, Items green, D-Link "the D-Link blue", and none of them changing with battle.
* Where the game's colours are (bc01_00, dumped with tools/l2d.py): the deck plate's body is sequence 0xf in battle
  (frame sprite 0, key e1dc00) and 0xb in the field (sprite 0, 005ff0; reload glow sprite 2, 00c0ff additive); the
  label is 0x25a in battle (plate sprite 34 efaf03, letters sprite 35 ffff00) and 0x25d in the field (005ff0 /
  00ffff).  The D-Link plate (layout 0xa, node 1, sequence 0x2d) has no battle variant; its blue is in the corner
  colours of sprite 63's quads (0080ff; the right end 00def0), multiplied by a key 3c3c3c/a0 in controls 3 and 4 and
  by nothing under the cursor (control 0, with the gradient sprite 65 in 00b4c8).  The game's own D-LINK header
  (layout 0xb) is 0096fa.
* Rows of Magic and Items always get the field body (`row_body`: 14 objects = battle body -> ReplaceNodeSeq 0xb; the
  game re-skins only on entering / leaving battle [203ea0] and keeps its own note in plate flag 0x200, untouched),
  the header always sequence 0x25d.  Colours: `tint_node` gives an object a twin of the record the game would have
  it play, with other colour keys (alpha kept).  The game's record of object i is anims[control.first + i]
  [1abb80], so the pass is stateless: it sets what each object should play, twin or the game's own, every frame
  after the plate's update.  Magic's default 005ff0 is the field body's own colour: no twin at all.
* **A record's key index is signed 16-bit to the game** (`movsx esi, word [rec+6]` at 1a8871, then used unsigned):
  an index of 0x8000 or more reads far outside the table.  The twins' keys are at 0x4000.. in g_sc_keys (256 twins
  of up to 32 keys; the file has 3749 keys).  Found by t_colours drawing a tinted plate, before it reached the game.
* The row under the cursor.  The battle body's inside is three black quads of the frame sprite, which cursor_body()
  turns into the gradient.  The field body lays sprite 2 (object 6: black in control 3, the glow in 4) over that,
  so the gradient never showed in the field.  For a list row under the cursor `row_fill` gives that sprite's
  private quads the gradient's texels and the tint turns its black into the list's colour; back to black when the
  cursor leaves.  (The four main entries are as they were: gradient in battle, black inside in the field.)
* D-Link rows: the art's blue is kept; `DLinkRowsBright` replaces the 3c3c3c key by 808080 (rule with a `from`
  colour, so control 5's e1dc00 and the keyless control 0 are left alone).
* Test `t_colours`: records and key tables of every case, and the colour word the game's draw function sends
  (battle a100dfe5, Items a133cb00, Magic a1f46000: A B G R).  Mock-up: scratchpad col/run2.py.

## Crash log: which 2D instance (src/main.c l2d_crash_report)

* First crash seen in play (build 10574bd, 5.4 minutes into a session, in a fight): access violation at rva 4f0195
  (`cmp dword [rdx+0x10], 0x2f02eb5` in 1404f0180, the texture bind) with rdx = 74f111e420036048, called from
  14010ccb0 <- CD2SeqCtrl::Draw 1401aaea0 [1ab025] <- CD2LayCtrl::Draw 1401a2bb0 [1a311a] <- the draw list
  [0f5140].  So a node of some layout drew from a block (CD2SeqData) whose current texture (+0x90, a CRsrcTM2 the
  game can swap with 1a84e0 / 1a8530) had been freed.  The log could not say which layout: the registers were gone.
  The mod's own layouts only draw from bc01_00, whose texture every command plate of the game uses too, so it is
  probably a game instance; whether something the mod does lets it outlive its texture is open.
* Since then the exception filter searches the stack for controller objects (first word = vtable 6418b8 CD2LayCtrl or
  641dc0 CD2SeqCtrl) and logs each: handle, the mod's own handles for comparison (mod_own_handles), and per node the
  block name, current / original texture, substituted texture id.  The node whose texture's native pointer (+0x28)
  equals the faulting rdx is marked.
* Second time (same place, build with the report): the report itself died.  `IsBadReadPtr` works by faulting, the
  handler was entered again for that fault (it was reading past the end of the stack: only 0x5b0 bytes were left
  above rsp), six times over.  Now: reads are checked with VirtualQuery, the stack is read up to the thread's stack
  base only, and a fault during the report is not reported in turn.
* What the two crashes have in common: Terra, Castle of Dreams, the moment the first forced fight is won; the same
  call path; and the same "texture object" **74f111e420036048** although the game sat at another address each time.
  So it is not a freed pointer but data: the block's current texture (+0x90) pointed at memory that holds some
  other file's bytes by then (the resource arena is laid out the same way in the same situation).  It took the
  path at 10cd0d: image+0x40 non-zero and the float at +0x48 below 1.0, also not what a registered image has.
  No raw TIM2 header in the files at hand has that value at +0x28 (they look like 0000000224000000).
* Which block is still unknown.  Blocks that get a replacement texture (1401a84e0): the player gauge's face
  (gauge_01:1), the D-Link faces, the item / level-up window (itemget_02:1), the escort gauge (msn_00:1 / :2, layout
  1 of msn_00.l2d, CFriendGauge 14020ced0 - a child task of the gauge root that nothing deletes before the scene
  jump; it is hidden by the player gauge's update when the HUD goes off and shown again by 14020b5f0(0) when the HUD
  comes back).  The mod loads no files, swaps no textures and does not write the HUD-off flag.

## Texture guard (src/guard.c, `[Safety] TextureGuard`)

* **Off by default since the owner asked for it to be taken out** (the game felt slightly laggy; whether the guard
  is why is not measured - it is the only thing the crash fix added that runs for every 2D node every frame).  With
  it off the two draw vtable slots are not patched at all.  What stays is the fix itself (`menu_shutdown` from the
  gauge's destructor) and `l2d_live` in the modules' own "is it alive" tests.

* CD2SeqCtrl's draw (1401aaea0, vtable 641dc0 slot 3 - every node of every layout and every standalone sequence
  goes through this slot; a layout's draw 1401a2bb0 is vtable 6418b8 slot 2) is entered through `guard_seq`.
  Only when the game is about to bind (timer group drawn [8f88020 + group], drawing on [8f8802c], objects > 0): the
  block must be readable, and if its image has a size (+0x3d / +0x3e, the game's own test) the texture objects at
  +0x28 and +0x40 must be addresses that can be read (as far as 1404f0180 reads: the mark at +0x10 and, if it is
  02f02eb5, +0x70).  Readable = VirtualQuery, remembered per page in a small table that is emptied every 65536 draws.
* Not readable: if the block's own texture (+0x98) is sound it is put back in +0x90 (the replacement's resource id
  at +0xa0 stays, so the game's own restore still releases it) and the node is drawn; otherwise the node is not
  drawn that frame.  A draw the game could have done is never changed.
* The log then names: the layout (handle, file, node id) or the sequence, the mod's own handles, the block, the
  resource of the block's file and of the replacement texture (state, group, users, address, size), the 0x50 bytes
  now at the texture's address and which loaded file that address belongs to now (resource lists [8f7e328 + type *
  0x20], walked under the resource lock [8f7dc10]+0x58).  At most 24 reports, one per node and texture.
* Test `t_guard` (the decision on made-up blocks, with the value of the two crashes); `t_draw` runs the real draw
  through the patched slots.
* **Found** (third run, the guard's log): the nodes were the mod's own - the four menu entries (layouts of data
  handle 65 = bc01_00.l2d), a Command Style offer and its timer bar (sequence of gauge_01:0).  Their blocks were
  "not a data block any more", the files' resources (ids 2ff, 2fe) gone, and the textures' addresses belonged to
  a sound file (se1742.scd, group 6000) by then.  So at that moment the game destroys the player's gauge and
  command objects and frees their files (p00common's 2D files) **without** the "destroy every instance" that a
  scene jump does first, and without regard for the references instances hold on the file (the question left
  open in l2d_api.md section 6: a group free ignores them).  The gauge's destructor only removed the MP bar; the
  entries and the offer are looked after from the gauge's update (menu_frame), which no longer ran, so they stayed
  alive and visible over freed data.  The mod's bug, not the game's.
* Fix: the gauge's destructor hook calls `menu_shutdown` (menu.c: entries, shortcut rows, list header; style.c
  `style_shutdown`: offer plate and timer) while the files are still loaded.  Second line, guard.c: a layout or
  standalone sequence whose data handle (+0x1c) is no longer registered is not drawn (`data_gone`, one pool lookup
  a draw), and `l2d_live(&handle)` in every "is it alive" test of menu.c / style.c / mp.c destroys such an instance
  (the destroy path looks the data handle up before using it [1abcf0, 1a3cd0], so that is safe) and clears the
  handle, so the module makes a new one.  Test `t_gone`: the real runtime, file unregistered [1a5f20] under a live
  layout and sequence.
* Cost of the guard.  One launch of the fix build was "insanely laggy" from the first frame of play, the next
  launches were not, and the log showed no reports - so nothing was being destroyed and re-made, and the diff to
  the guard-only build is a few pool lookups.  Most likely (not measured on the PC) the readability test: it was
  VirtualQuery behind a 256-slot page table emptied every 65536 draws.  VirtualQuery also sizes the region around
  the address page by page, the images are in the game's resource arena (inside the exe image, one huge run of
  like pages), and whether two busy pages share a table slot - and so ask on every node - depends on where the
  heap is in that run.  Now: one byte read through ReadProcessMemory per unknown page (2048 slots, hashed, never
  emptied), and `g_seen` remembers per block the image and texture objects found sound, so an unchanged node costs
  three compares and no question at all.  `TextureGuard=0` takes the draw hooks out entirely; the fix above does
  not depend on them.
* Default history: on when written, off while the user tested the menu_shutdown fix alone (and a slight lag was
  being looked into), on again at the user's request as a safety net.
* Handles cannot be mistaken for one another: a freed slot's id grows by 0x400 (l2d_api.md), so a stale handle of
  ours never names a newer game instance.

## OpenKH Mod Manager package (modmanager/, tools/package.sh -> dist/BBS-KH2-Style.zip)
- The package is the same DLL under the name dll/bbskh2.dll plus dll/bbskh2.ini, two `copy` assets in mod.yml.
  The Mod Manager copies them to <CompiledModPath = .../mod/bbs>/dll/, and Panacea (the mod loader, dbghelp.dll
  in the game folder) loads every DLL in <mod_path>/<game>/dll/ at start-up [OpenKh.Research.Panacea
  Panacea.cpp LoadDLLs; the patcher's own special case is for "dlls/", which Panacea does not read].
- main.c already covers this: inert in the other games' exes, one copy only (BBSKH2_LOADED), settings from
  next to the game exe if there is a bbskh2.ini there, else from next to the DLL.
- Works only with the mod loader; a build that patches the pkg files would just store the DLL as a dead file.
- bundle.c also replaces the first Revenge Value build of Factory.lub (10208 bytes), so that the old mod left
  enabled in the Mod Manager does not hide the bosses added later.
- Not tested through the Mod Manager by me (no access to the user's OpenKH folder).

## Camera distance and tilt limits ([Camera])
- Player/boss camera files (PCam?000.bin, BCam????.bin, 112 bytes): header (FOV at 0x8), then two 0x30 chunks
  (Normal at 0x10, Extended at 0x40) with eye at +0x10 and aim at +0x20.  `Distance` moves the eye towards the aim
  point after the KH2 Camera bytes are applied (FOV and look-at unchanged); the result's crc is remembered so a
  reloaded resource is not scaled twice.  Default 0.9.
- BBS tilt clamp: FUN_14022d790 stores -30 deg (0xbf060a92) to camera+0x344 and +45 deg (0x3f490fdb) to +0x348
  (instructions at 22dae3 / 22daf0, immediates at 22dae9 / 22daf6); FUN_14022c0b0 clamps the stick tilt offset
  (+0x2b0) so base pitch + offset stays inside them.  Positive = camera above looking down.
- KH2 (verified in kh2.exe): the field camera reads the pad only for yaw (pad bits 0x14/0x15, +/-3 deg/frame in
  1403a9250 / 1403a70a0 -> 1403b7a80) and for movement direction (1403a8fa0).  The pitch component of its angle
  (+0x680) is only ever set by scripted/auto code, never from the stick.  The +/-45 deg found at gm::CAMERA+0xe2c is
  ACTION_NMGUN (a minigame camera's yaw limit, pitch +/-15 there).  So KH2 has no wider tilt range to copy;
  `PitchLow`/`PitchHigh` stay at the game's values unless changed.
- Tilting down on flat ground the camera's map ray (look point -> eye, FUN_1401ffa90 in FUN_14022b110) hits the
  floor at about -10 deg and the eye is pulled in to the hit point; past that, more tilt slides the camera along the
  floor towards the character.  The -30 limit stopped that ~1.2 m out, so PitchLow defaults to -60 (imm 0xbf860a92).

## Combo Master (src/combomaster.c; `[Combat] ComboMaster`, `ComboMasterName`, `ComboMasterHelp`)
- Asked for: KH's Combo Master as a new ability with a single entry like EXP Zero, unlocked by default.
- Abilities in the game: command ids 0x1c4..0x1e1, exactly 30 - the size of every table they live in: the save's
  u32 per ability at save+0x18cc (`14041d800`; read as save+0x11bc+id*4), the player's count per slot at pl+0x4a3
  (filled each frame by `1402218b0` from bits 0-2 through `14041d810`).  The u32: bits 0-2 in effect, 3-5 level,
  6-8 copies learned (`14041fb70`; max per id at `140811083 + id*0x1e`, EXP Zero 1), 9-13 copy on/off
  (`14041d2e0(id, copy, mode)`, mode <0 flips), 14-15 0 "???" / 1 new / 2 seen / 3 known (`419380` sets new,
  `41c5f0` turns seen into known on leaving the menu).  No free id inside the block, so a 31st cannot be stored there.
- The new one is id 0x1c3 (ABILITY_KIND_None: empty name and description in CT00500 / CT00100; the code that loads
  0x1c3 as an immediate uses it as a layout sequence number, not a command).  It exists only where an ability is
  seen and switched, the camp Abilities menu (CCampAbility):
  - list `3f1ce0` (callers 3f2979, 3f2f55, 3f39a6): entries of 0x10 at menu+0xa8, room for exactly 30: menu+0x288
    holds the 7 on-screen rows' positions (3f24d0 `lea rdx,[rbx+0x288]`), then the row objects at +0x2c0 / +0x2f8.
    The first build counted room for 41 and put a 31st entry there: the row positions overwrote it and moving the
    cursor onto it crashed (3f26ae, reading its u32).  EXP Zero is listed only on Critical (`140360150` = the
    difficulty byte 150fa0881 == 3), so the list has 29 entries elsewhere and 30 on Critical - no room there.
    The fix that followed makes room: the row positions (7 x 2 floats) are only ever reached through three
    `lea reg,[menu+0x288]` - 3f2f60 (setup loop, 1a6880 fills them), 3f2644 / 3f2657 (cursor, passed to 4283c0 /
    428800) - and nothing else in the menu (3f0000..3f4000), its base class (42a000..42c000) or those helpers touches
    +0x288..+0x2bf; the 0x1e constants there are the constructor's first count and 3f1ce0's loop over the 30 ids.
    hook_ctx at the three sites points them at a buffer of the mod's, so the list holds 31 (up to 33 would fit).
    The append only goes past 30 when all three hooks are in.
    count at +0x94; +0 u32*, +8 id, +0xa group (0 Prize 0x10, 1 Stats 0x0b, 2 Support 0x11),
    +0xb first of group, +0xc/+0xf level, +0xd learned, +0xe possible.  After each call the entry is appended to
    Support with its own u32 (0xc000 known | one copy learned | on bit), +0xd = +0xe = 1: one pip, like EXP Zero.
    Off until switched on, per save (below).
  - row name: `3f1b20` reads the command table's name pointer (140814908 + id*0x18) - set at install and again when
    the names file is loaded (mp.c's CRsrcCTD hook calls combomaster_texts after the game's).
  - description: `14041d880` answers only 0x1c4..0x1e1; its calls at 3f2711 / 3f3562 are hooked.
  - switching: the call of `14041d2e0` at 3f37f0 (cVar 3 in `3f32a0`, after the "learned" checks) is hooked: for
    0x1c3 it sets or clears the choice; the list is then rebuilt by the game (`3f2970` -> 3f1ce0 -> appended again).
  - where the choice is kept: in the save, bit 23 of EXP Zero's u32 (the save in memory is 150fa3d08 [1403600f0],
    so 150fa3d08 + 0x18cc + 5*4).  Every user of these u32s masks its own fields: bits 0-5 (422de0 recompute),
    6-8 / 18-20 (41fb70, 41e090), 9-13 (41d2e0), 14-17 (419380, 41c5f0; 41de10 returns only 14-17); the save
    conversions (1bdbc0 / 1c04a0) copy the whole u32.  So bit 23 is saved and loaded with the rest, is never cleared
    by the game, and a new game starts with it 0 (off).  The first builds kept one `ComboMasterOn` in the ini for all
    saves: switched on in one save, it showed on in every other (reported: a late Aqua save off, an early Terra save
    on when its menu was first opened).
  - detail page (confirm, 3f3659 `mov rax,[rbx+rax*8+0xa8]` before the `test [rax],0xc000`): `3f1550` and the
    300-byte table index by id-0x1c4, so for 0x1c3 the hook goes to the game's own buzzer path (3f3669).
- The effect: the normal combo's window.  The next hit can only be asked for in `14021c140`; for the Attack category
  that needs pl+0x320 bit 0x20 "connected" (cmd+0x60 & 0x1000 aside), set only by the weapon hit handler
  [293d9d / 293dee] or a fired bullet [229021].  Normal combo (sub-state 5): the window opens with 0x318 |= 0x40, valid
  until frComboEnable (+0xc); a queued command starts at frChangeEnable (+0xd).  The bit also ends the forward lunge
  (228f39 passes "not connected" to the movement call), so it is lent only for the window call at 22902e (rcx = pl,
  xmm1 = frame): ability on, category 1, sub-state 5, not connected, frame >= frMark End (+0x1d, where the swing
  lands; melee records have frTrigger 0) - set before the call, cleared after (21c140 writes 0x320 back from its own
  copy when it queues).  Other readers of the bit (magic states 2674d0 / 2687d0) never see it.
  Same as after a real hit: the open window also takes guard / dodge / other commands, so a whiffed normal-combo hit
  can be cancelled into them (vanilla: nothing until the animation ends).
- Test: `t_cm` (append on a list built like 3f1ce0 from the exe's tables, the switch / description / detail hooks,
  the name through the real names file, the window through the game's 21c140 at frames 3 / 8 / 26, real hit and deck
  attack untouched).  Not seen in the game by me: the menu itself (the list builder needs the save and deck).

## Cure and the big spells a little slower (speed.c; `[Speed] CureRelease`, `BigSpells`)
- Asked for: Cure and the bigger spells felt too quick; slow them a little, Cure still much snappier than vanilla.
- Cure family (Cure, Cura, Curaga, Esuna): CureRelease 0.30 (KH2) -> 0.40 s.  t1 20 / 18 / 22 frames (Ventus / Aqua /
  Terra) gave wind-up factors 2.22 / 2.00 / 2.44; now 1.67 / 1.50 / 1.83.  Vanilla release 0.67 / 0.60 / 0.73 s, so
  still 35-45 % sooner.  The lock after the release stays the game's 40 ticks.
- Big spells (no KH2 counterpart, played at Actions 1.15 before): 0xa6..0xb0 (Faith, Deep Freeze, Glacier, Ice
  Barrage, Firaga Burst, Raging Storm, Mega Flare, Quake, Tornado, Meteor, Transcendence) and 0x88 / 0x8d (Triple
  Firaga / Triple Blizzaga) play at BigSpells = 1.05 from start to end, about 9 % longer than before and still a
  little quicker than the game.  [CommandSpeed] for a single command still wins.  The ticks-based locks after the
  release (Mega Flare 120) are not animation and do not change.
- Test: t_speed (Cure factor and release time, every big spell 1.05, ordinary -aga / Aero / Stop spells not).

## Revenge Value v2.2: the gauge is not cleared (KH2), and armour windows hold
- Reported: Aqua's final Vanitas still easy to combo - he breaks out, but comes back right next to you and you are
  straight back in for another full string.  Asked to look at it for all bosses, not him alone.
- KH2 (kh2.exe): the gauge at +0xd48 is written only by the constructors (1403db840 / 1403db930: 0, cap +0xd4c
  100.0), the adds (1403db730 / 1403dbb90) and the drain (1403db440 / 1403dbbf0, clamped at 0).  The "revenge"
  event (14040e337: flag +0x6c8 bit 4, gauge >= cap -> 1403cac50 with "revenge") does not touch it.  So after a
  revenge the gauge stays at the cap and drains at 1.0 / frame (6 hits a second) while the boss is out of
  hit-stun - during its own revenge action.  Going straight back in, the next revenge comes after a few hits.
- v2.1 cleared the gauge after every break-out (done) and whenever the boss recovered from hit-stun
  (OnReturnDamage).  The second made every combo string start from 0: only one unbroken string of ~10 hits could
  ever bring a break-out, and after one the player always had a full gauge's worth of free hits.
- Now: done() keeps the value and sets idle to the grace (drain from the next frame); OnReturnDamage does the
  same.  Per boss `clear = true` keeps the old clearing - No Heart only, whose slow gauge (grace 120, drain 0.03)
  stands in for his own burst timer.
- The armour window after a forced revenge (iframes) is held every frame: the boss's own states switch "no damage
  reaction" off in their OnEndState (Vanitas' Cartwheel / WarpAttack2 ...), which cut it short before.
- Effect in the loop test (80 hits, one every 35 frames): 11 break-outs for every boss instead of 6 - 9.
- Vanitas (b63ex00) found in the investigation: his break-out at the limit is Warp2 - vanish, reappear 0.5 m behind
  the player (WarpToTargetBack scales the target's direction by -0.5), one slash (WarpAttack2), back to Idling.
  Unchanged for now; with the gauge kept he breaks out again after a few hits when you go straight back in.
- Grace before the drain: 60 -> 90 frames (1.5 s) on request - BBS bosses stand around between moves far more than
  KH2's, so the gauge has to outlast that.  (After a break-out or a recovery the drain still starts at once.)

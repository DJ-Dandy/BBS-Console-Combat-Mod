package.path = (os.getenv("RV_TEST") or "test/revenge") .. "/?.lua;" .. package.path
local M = require("emu")
math.randomseed(7)
M.common()
BBS_REVENGE.vary = 0           -- deterministic limits for these checks (variance has its own check below)
local fails = 0
local function check(c, msg) if not c then fails = fails + 1 io.write("  FAIL: ", msg, "\n") else io.write("  ok: ", msg, "\n") end end
local function hit(h, kind, cat)
  local c, r = EntityManager:CallFunctionArg5("OnDamageBefore", h, kind or ATK_KIND_DMG_SMALL, cat or COMMAND_CATEGORY_ATTACK, 0, 0)
  if r ~= nil and r ~= false and r ~= 0 then return "before" end
  c, r = EntityManager:CallFunctionArg5("OnDamage", h, kind or ATK_KIND_DMG_SMALL, cat or COMMAND_CATEGORY_ATTACK, 0, 0)
  if r ~= nil and r ~= false and r ~= 0 then return "damage" end
end
-- No Heart
M.script("b85vs00")
do
  local e, h = M.create("b85vs00")
  local st = BBS_REVENGE.ents.b85vs00
  rawset(e, "Update", function() end); rawset(e, "Debug", function() end)
  pcall(e.GotoState, e, "Idling")
  for i = 1, 13 do hit(h) end
  check(st.rv == 13 and e.burstTimer > 90000 and e.burstStartFlag == 1, "No Heart: 13 hits, value 13, his own timer held off (" .. tostring(e.burstTimer) .. ")")
  for i = 1, 200 do e:OnUpdate() end
  check(e:GetState() == "Idling" and st.rv > 10.5 and st.rv < 10.7, "200 frames of update between hits: no burst, value drains at his slow rate (state " .. e:GetState() .. ", value " .. st.rv .. ")")
  local armed = false
  for i = 1, 20 do hit(h) if st.firing then armed = true break end end
  check(armed and e.burstTimer < 0, "more hits: burst armed at his limit")
  e:OnUpdate()
  check(e:GetState() == "BurstAttack", "next update: BurstAttack (" .. e:GetState() .. ")")
  check(st.rv == 0 and st.firing == false and st.count == 1, "value cleared after the burst (" .. st.rv .. ")")
  check(hit(h) == "before" and st.rv == 0, "hits during the burst are ignored by him and do not count")
  pcall(e.GotoState, e, "Idling")
  for i = 1, 5 do hit(h) end
  check(st.rv == 5, "new string: value 5")
  for i = 1, 320 do e:OnUpdate() end
  check(st.rv == 0 and e.burstStartFlag == 0, "10 s without a hit: value and his timer cleared (" .. st.rv .. ", flag " .. tostring(e.burstStartFlag) .. ")")
  check(e:GetState() == "Idling", "... and no burst came out of it")
end
-- Master Xehanort
M.script("b40ex00")
do
  local e, h = M.create("b40ex00")
  local st = BBS_REVENGE.ents.b40ex00
  pcall(e.GotoState, e, "Idling")
  local r
  for i = 1, 8 do r = hit(h) end
  check(r == nil and st.rv == 8 and e:GetState() == "Idling", "Xehanort: 8 hits land, he stays in the combo")
  r = hit(h)
  check(r == "before" and e:GetState() == "Warp" and st.rv == 0 and e.defenseTimer > 0, "9th hit: quick warp (state " .. e:GetState() .. "), hit negated, value cleared")
  check(hit(h) == nil and e:GetState() == "Warp", "while he is warping nothing more is triggered")
  pcall(e.GotoState, e, "Idling"); st.rv = 0
  for i = 1, 2 do hit(h, ATK_KIND_DMG_SMALL, COMMAND_CATEGORY_FINISH) end
  check(st.rv == 6 and e:GetState() == "Idling", "finishers count 3 each (value " .. st.rv .. ")")
  check(hit(h, ATK_KIND_DMG_SMALL, COMMAND_CATEGORY_FINISH) == "before" and st.rv == 0, "... break-out at the third")
end
-- Zack
M.script("b40he00")
do
  local e, h = M.create("b60vs00")
  local st = BBS_REVENGE.ents.b60vs00
  for i = 1, 8 do hit(h) end
  check(st.rv == 8, "Zack: 8 hits")
  pcall(e.GotoState, e, "Bushinhazan")
  check(hit(h) == nil and st.firing == true, "limit reached during a move he cannot leave: no counter yet")
  pcall(e.GotoState, e, "Move")
  local r = hit(h)
  check(r == "before" and st.rv == 0, "next hit: his counter, now per hit (" .. tostring(r) .. ", into " .. e:GetState() .. ")")
  check(st.armor > 0, "the counter got its armour window")
  for i = 1, 60 do e:OnUpdate() end
  check(st.armor == 0, "... and the armour ran out")
  for i = 1, 4 do hit(h) end
  EntityManager:CallFunctionNoArg("OnReturnDamage", h)
  check(st.rv == 0, "recovering from hit-stun clears the value")
end
-- Hades
M.script("b01he00")
do
  local e, h = M.create("b80vs00")
  local st = BBS_REVENGE.ents.b80vs00
  for i = 1, 5 do hit(h) end
  check(st.rv == 5, "Hades: 5 hits")
  M.num["Effect.IsAlive"] = true
  check(hit(h) == "before" and st.rv == 0, "burning red: the hit is ignored and the value clears")
  M.num["Effect.IsAlive"] = false
  local r
  local n0 = st.count
  for i = 1, 9 do r = hit(h) end
  check((r == "damage" or r == "before") and st.rv == 0 and st.count > n0, "9 hits: counter (" .. tostring(r) .. ", into " .. e:GetState() .. ")")
end
-- Peter Pan (added in v2: vanilla has no break-out at all)
M.script("b20pp00")
do
  local e, h = M.create("b20pp00")
  local st = BBS_REVENGE.ents.b20pp00
  rawset(e, "Update", function() end); rawset(e, "Debug", function() end)
  pcall(e.GotoState, e, "Idling")
  local r
  for i = 1, 9 do r = hit(h) end
  check(r == nil and st.rv == 9, "Peter Pan: 9 hits land, no vanilla escape fires")
  r = hit(h)
  check(r == "before" and st.rv == 0 and e:GetState() == "Attack3", "10th hit: his own attack as the revenge (" .. tostring(e:GetState()) .. ")")
  check(st.armor > 0, "the revenge has its armour window")
  for i = 1, 70 do e:OnUpdate() end
  check(st.armor == 0, "... which runs out")
  EntityManager:CallFunctionNoArg("OnReturnDamage", h)
  check(e:GetState() == "BeforeAttackIdling", "after a combo he goes back on the offensive (" .. e:GetState() .. ")")
  -- a juggle the engine hides from the callbacks: the HP watch and the watchdog still break it
  local hp = 100
  M.num["Enemy.GetHp"] = function() return hp end
  pcall(e.GotoState, e, "Idling")
  st.rv, st.firing, st.hp = 0, false, nil
  local n0 = st.count
  for i = 1, 60 do hp = hp - 1 e:OnUpdate() if st.count > n0 then break end end
  check(st.count > n0, "hits invisible to the callbacks: the watchdog forced the revenge (value " .. st.rv .. ")")
  -- poison / burn: HP loss with no hit string behind it must not build revenge
  pcall(e.GotoState, e, "Idling")
  st.rv, st.firing, st.armor = 0, 0 == 1, 0
  st.seen, st.drop = -1e9, -1e9
  n0 = st.count
  for i = 1, 10 do
    hp = hp - 1
    for k = 1, 70 do e:OnUpdate() end        -- a tick a little over once a second
  end
  check(st.rv == 0 and st.count == n0, "damage over time (ticks over 1 s apart) builds no revenge (value " .. st.rv .. ")")
  M.num["Enemy.GetHp"] = 100
end
-- the limit varies after each revenge (KH2 FM re-rolls the cap)
M.script("b20pp00")
do
  local e, h = M.create("b20pp00")
  local st = BBS_REVENGE.ents.b20pp00
  rawset(e, "Update", function() end); rawset(e, "Debug", function() end)
  BBS_REVENGE.vary = 1
  local lo, hi = 99, 0
  for i = 1, 12 do
    pcall(e.GotoState, e, "Idling")
    st.rv, st.firing, st.armor = st.limit - 0.5, false, 0
    hit(h)
    if st.limit < lo then lo = st.limit end
    if st.limit > hi then hi = st.limit end
  end
  BBS_REVENGE.vary = 0
  check(lo >= 9 and hi <= 11 and hi > lo, string.format("the limit re-rolls in 10 +- 1 after each revenge (%.2f .. %.2f)", lo, hi))
end
io.write(fails == 0 and "ALL OK\n" or ("FAILED " .. fails .. "\n"))
os.exit(fails == 0 and 0 or 1)

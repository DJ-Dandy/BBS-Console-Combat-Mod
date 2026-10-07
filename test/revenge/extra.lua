package.path = (os.getenv("RV_TEST") or "test/revenge") .. "/?.lua;" .. package.path
local M = require("emu")
math.randomseed(7)
M.common()
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
  check(e:GetState() == "Idling" and st.rv == 13, "200 frames of update between hits: no burst, value kept (state " .. e:GetState() .. ", value " .. st.rv .. ")")
  hit(h)
  check(st.firing == true and e.burstTimer < 0, "14th hit: burst armed")
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
  check(hit(h) == "damage" and st.rv == 0, "next hit: counter")
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
  for i = 1, 9 do r = hit(h) end
  check(r == "damage" and (e:GetState() == "BladesCrossing" or e:GetState() == "NoRiaFingernailofFire"), "9 hits: counter (" .. e:GetState() .. ")")
end
io.write(fails == 0 and "ALL OK\n" or ("FAILED " .. fails .. "\n"))
os.exit(fails == 0 and 0 or 1)

-- usage: lua run.lua <script> <entity> [runs]     (RV_FACTORY = which Factory.lub to load)
package.path = (os.getenv("RV_TEST") or "test/revenge") .. "/?.lua;" .. package.path
local M = require("emu")
local script, name, runs = arg[1], arg[2], tonumber(arg[3] or "200")
local mode = arg[4] or "hits"
math.randomseed(12345)
M.common()
M.script(script)
local errs = {}
local function hit(e, kind, cat)
  -- what the engine does for one hit: OnDamageBefore (1 = hit negated), then OnDamage (1 = no flinch)
  local ok, r = pcall(function()
    local called, r = EntityManager:CallFunctionArg5("OnDamageBefore", e.myHandle, kind, cat, 0, 0)
    if called and r ~= nil and r ~= false and r ~= 0 then return "before" end
    called, r = EntityManager:CallFunctionArg5("OnDamage", e.myHandle, kind, cat, 0, 0)
    if called and r ~= nil and r ~= false and r ~= 0 then return "damage" end
    return nil
  end)
  if not ok then errs[tostring(r)] = (errs[tostring(r)] or 0) + 1 return "error" end
  return r
end
local hist, never = {}, 0
local states = {}
for run = 1, runs do
  local e, h = M.create(name)
  assert(e, "no entity")
  e.myHandle = e.myHandle or h
  if arg[7] then pcall(e.GotoState, e, arg[7]) end       -- start from this state
  local s0 = e.GetState and e:GetState()
  local out
  for i = 1, 40 do
    local kind, cat = ATK_KIND_DMG_SMALL, COMMAND_CATEGORY_ATTACK
    if mode == "finish" and i % 4 == 0 then cat = COMMAND_CATEGORY_FINISH end
    if mode == "magic" then cat = COMMAND_CATEGORY_MACIG end
    local r = hit(e, kind, cat)
    local s = e.GetState and e:GetState()
    if arg[5] == "burst" then
      -- No Heart: the break-out starts from his update
      if e.burstStartFlag == 1 and e.burstTimer < 0 then out = i; states["burstTimer<0"] = (states["burstTimer<0"] or 0) + 1 break end
    elseif r == "before" or r == "damage" then
      if arg[5] ~= "armored" then out = i; states[tostring(s)] = (states[tostring(s)] or 0) + 1 break end
    elseif r == "error" then out = -i break end
    -- between hits: a few frames pass
    if rawget(e, "OnUpdate") and arg[6] == "upd" then pcall(e.OnUpdate, e) end
  end
  if out == nil then never = never + 1 else hist[out] = (hist[out] or 0) + 1 end
end
local keys = {}
for k in pairs(hist) do keys[#keys + 1] = k end
table.sort(keys)
io.write(string.format("%-8s %-8s %-7s runs %d  broke out at hit:", script, name, mode, runs))
for _, k in ipairs(keys) do io.write(string.format(" %d:%d", k, hist[k])) end
io.write("  never:", never, "\n   into:")
for k, v in pairs(states) do io.write(" ", k, "=", v) end
io.write("\n")
for k, v in pairs(errs) do io.write("   ERROR x", v, ": ", k, "\n") end

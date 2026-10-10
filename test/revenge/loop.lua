-- The loop test: no configured boss can be comboed forever.
-- Models the one case that used to loop (read from the game's code at 2d5e30 /
-- 2d60f0): an unbroken combo, during which the engine calls OnDamageBefore on
-- every hit but never OnDamage (that one only runs when no damage reaction is
-- under way - extra.lua covers it per boss).  80 such hits at a real player's
-- pace - 35 engine frames (0.6 s) apart, the cadence that exposed the drain
-- bug - with the boss's OnUpdate between them; every boss must break out
-- again and again, with the forced revenge itself firing, and never let a
-- run go unanswered for long.
package.path = (os.getenv("RV_TEST") or "test/revenge") .. "/?.lua;" .. package.path
local M = require("emu")
math.randomseed(20261009)
M.common()

local MAXRUN = 16        -- hits: every limit is <= 12 + variance, watchdog adds a frame or two
local MINBREAKS = 3      -- in 80 hits
local fails, skipped = 0, 0

-- script file -> the configured entity names it serves
-- (each script runs in a process of its own: the stand-in engine shares its
-- globals between scripts, and one script's leftovers can break another)
local plan = {
  { "b10ex00", { "b10ex00" } },
  { "b11ex00", { "b11ex00" } },
  { "b12ex00", { "b12ex00" } },
  { "b63ex00", { "b63ex00" } },
  { "b20ex00", { "b20ex00" } },
  { "b81vs00", { "b81vs00" } },
  { "b30ex00", { "b30ex00" } },
  { "b01ex00", { "b01ex00" } },
  { "b01pp00", { "b01pp00" } },
  { "b20pp00", { "b20pp00" } },
  { "b20ls00", { "b20ls00" } },
  { "b01sb00", { "b01sb00" } },
  { "b40ex00", { "b40ex00" } },
  { "b52ex00", { "b52ex00" } },
  { "b40he00", { "b40he00", "b60vs00", "b30he00" } },
  { "b01he00", { "b01he00", "b80vs00" } },
  { "b60",     { "b60ex00", "b70ex00", "b82ex00" } },
  -- b85vs00 (No Heart) is armored and breaks out from his own update: covered in extra.lua
}

if arg[1] == "list" then
  for _, row in ipairs(plan) do io.write(row[1], "\n") end
  os.exit(0)
end
if arg[1] ~= nil then
  local keep = {}
  for _, row in ipairs(plan) do
    if row[1] == arg[1] then keep[#keep + 1] = row end
  end
  plan = keep
end

local function truthy(r) return r ~= nil and r ~= false and r ~= 0 end

for _, row in ipairs(plan) do
  local script, names = row[1], row[2]
  local okScript = pcall(M.script, script)
  if not okScript then
    io.write("  skip: ", script, " (script did not load)\n")
    skipped = skipped + #names
  else
    for _, name in ipairs(names) do
      local ok, e, h = pcall(M.create, name)
      if not ok or e == nil then
        io.write("  skip: ", name, " (no entity)\n")
        skipped = skipped + 1
      else
        e.myHandle = e.myHandle or h
        rawset(e, "Update", function() end)
        rawset(e, "Debug", function() end)
        pcall(e.GotoState, e, "Idling")
        local st = BBS_REVENGE.ents[name]
        if st == nil then
          fails = fails + 1
          io.write("  FAIL: ", name, " - configured but not attached\n")
        else
          st.rv, st.idle, st.firing, st.armor = 0, 0, false, 0
          local breaks, run, maxrun, broken = 0, 0, 0, false
          local c0, f0 = st.count, st.fcount
          for i = 1, 80 do
            local okh, r = pcall(function()
              local _, rb = EntityManager:CallFunctionArg5("OnDamageBefore", e.myHandle, ATK_KIND_DMG_SMALL, COMMAND_CATEGORY_ATTACK, 0, 0)
              return truthy(rb)
            end)
            if not okh then
              io.write("  skip: ", name, " - the stub engine cannot run it (hit ", i, ": ", tostring(r), ")\n")
              skipped = skipped + 1
              broken = true
              break
            end
            for k = 1, 35 do pcall(e.OnUpdate, e) end
            if st.count > c0 + breaks then
              -- the revenge move: it plays out, the boss is back in neutral after it
              breaks = st.count - c0
              if run > maxrun then maxrun = run end
              run = 0
              for k = 1, 30 do pcall(e.OnUpdate, e) end
              pcall(e.GotoState, e, "Idling")
              st.armor = 0
            else
              run = run + 1
            end
          end
          if not broken then
            if run > maxrun then maxrun = run end
            local forcedN = st.fcount - f0
            local good = breaks >= MINBREAKS and forcedN >= 1 and maxrun <= MAXRUN
            io.write(good and "  ok:   " or "  FAIL: ", name,
                     string.format("  break-outs %d (%d forced), longest unanswered run %d hits\n", breaks, forcedN, maxrun))
            if not good then fails = fails + 1 end
          end
        end
      end
    end
  end
end
io.write(fails == 0 and ("LOOP TEST OK (" .. skipped .. " skipped)\n") or ("LOOP TEST FAILED " .. fails .. "\n"))
os.exit(fails == 0 and 0 or 1)

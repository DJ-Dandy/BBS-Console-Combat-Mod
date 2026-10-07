-- Minimal stand-in for the game's script engine: enough to load the AI scripts and run their damage callbacks.
-- RV_LUB: folder with the game's scripts: common__<name>.lub from arc/system/CommonLua.arc, lua__<name>.lub from lua/
local LUB = (os.getenv("RV_LUB") or "/home/claude/bbs/rv/lub") .. "/"
local FACTORY = os.getenv("RV_FACTORY") or "bundle_src/Factory_revenge.lub"
local M = {}
local U = {}                                   -- universal stub value
local umt = {}
umt.__index = function() return U end
umt.__call = function() return U end
umt.__newindex = function() end
for _, k in ipairs{"__add","__sub","__mul","__div","__unm","__mod","__pow"} do umt[k] = function() return 0 end end
umt.__concat = function(a, b) return "?" end
umt.__lt = function() return false end
umt.__le = function() return false end
setmetatable(U, umt)
M.U = U
M.rand = math.random
M.names = {}                                   -- handle -> entity name
M.num = {}                                     -- "Ns.Fn" -> number or function
local function ns(name)
  local t = {}
  setmetatable(t, { __index = function(_, k)
    local key = name .. "." .. k
    return function(...)
      local v = M.num[key]
      if type(v) == "function" then return v(...) end
      if v ~= nil then return v end
      if k:find("^Is") or k:find("^Has") or k:find("^Check") then return false end
      return U
    end
  end })
  return t
end
for _, n in ipairs{"Entity","Enemy","Player","Effect","Sound","Bullet","Camera","Mission","Collision","Gimmick","Event","Network","Debug","Mot","Motion","Stage","Item","Command","Trophy","Screen","Fade","System","Timer","Voice","Bgm","Menu","Friend","Net","Area","Cockpit","Message","Navi"} do
  _G[n] = ns(n)
end
Script = ns("Script")
rawset(Script, "Random", function() return M.rand() end)
local function n(x) if type(x) == "number" then return x end return 0 end
Math = setmetatable({ Sin = function(a) return math.sin(n(a)) end, Cos = function(a) return math.cos(n(a)) end,
  Sqrt = function(a) return math.sqrt(math.abs(n(a))) end, Abs = function(a) return math.abs(n(a)) end,
  Atan2 = function(a, b) return math.atan2(n(a), n(b)) end }, { __index = function() return function() return 0 end end })
PI = math.pi
M.num["Entity.GetFrameRate"] = 1
M.num["Entity.GetName"] = function(h) return M.names[h] or "?" end
M.num["Enemy.IsNoDamageReaction"] = false
M.num["Enemy.IsInvincible"] = false
M.num["Entity.CalcDistanceSq"] = 4
M.num["Entity.CalcDistance"] = 2
M.num["Entity.CalcDistanceXZ"] = 2
M.num["Entity.GetMotionNowFrame"] = 0
M.num["Entity.IsMotionEnd"] = false
M.num["Entity.IsTimeOver"] = false
M.num["Entity.GetDamagePoint"] = 1
M.num["Entity.GetDamageCategory"] = 1
M.num["Entity.GetUserShootLocked"] = 0
M.num["Entity.HasNetGameHandle"] = false
M.num["Effect.IsAlive"] = false
M.num["Enemy.GetExtraParam"] = 8
M.num["Enemy.GetHp"] = 100
M.num["Enemy.GetHpMax"] = 100
M.num["Enemy.IsAllEnemyWaiting"] = false
M.num["Player.GetPlayerHP"] = 100
M.num["Script.IsDebugCheckAttack"] = false
M.num["Enemy.IsNetworkMaster"] = true
M.num["Enemy.IsNetworkMasterinteger"] = 1
M.num["Sound.IsInvalidateSeCall"] = 0
print = function() end
setmetatable(_G, { __index = function(_, k) if type(k) == "string" and k:sub(1, 2) == "__" then return nil end return U end })

local function load(file)
  local f = assert(loadfile(LUB .. file))
  f()
end
function M.common()
  for _, n in ipairs{"common","stack","FSM","AtkKind","CommandKind","EnemyParam","CmnEneParam","GimmickKind","World","Collision","MotTrigger","2DDef","EntityMgr"} do
    load("common__" .. n .. ".lub")
  end
  assert(loadfile(FACTORY))()
  local ge, extra = EntityManager.GetEntity, {}
  EntityManager.GetEntity = function(self, h)        -- helper objects the engine would have created
    local e = self.entities[h]
    if e == nil then e = extra[h] or setmetatable({}, { __index = function() return U end }); extra[h] = e end
    return e
  end
  M.entity = function(h) return EntityManager.entities[h] end
end
function M.script(name) load("lua__" .. name .. ".lub") end
local nextHandle = 100
function M.create(name)
  nextHandle = nextHandle + 1
  local h = nextHandle
  M.names[h] = name
  local ok, err = pcall(EntityFactory.Create, EntityFactory, name, h)     -- (OnInit may stop early in this stand-in)
  M.initError = not ok and err or nil
  return M.entity(h), h, ok
end
return M

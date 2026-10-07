-- =====================================================================
--  BBS Revenge Value  (KH2-style revenge system for Birth by Sleep FM)
--
--  Loaded as a prologue of arc/system/CommonLua.arc -> Factory.lub.
--  The original Factory.lub code runs first (unchanged, embedded), then
--  this file layers a revenge counter on top of selected bosses.
--
--  How BBS works (vanilla): every boss AI has OnDamageBefore/OnDamage
--  callbacks. Humanoid bosses flinch normally, but those callbacks roll
--  dice on *every hit* ("15% cartwheel away", "dmgCount * k chance to
--  guard/teleport") so a combo can be broken by its first hit.
--
--  What this does: for the bosses listed in RV.cfg the dice are taken
--  away.  Each hit that staggers the boss adds a Revenge Value that
--  depends on the kind of attack.  Below the boss's limit the boss can
--  not break out; once the limit is reached the boss performs its own
--  (vanilla) break-out / counter move and the counter starts over.
--  The counter also resets when the boss recovers from hit-stun.
--  Hits that land while the boss has super armour are not counted.
--  Bosses that are not listed (all the large ones) are untouched.
-- =====================================================================
__FACTORY_ORIGINAL__()

-- Everything below is additive.  It runs inside pcall so that, whatever
-- happens, the original factory above is already in place and working.
pcall(function()
local RV = { version = "1.0", cfg = {}, ents = {} }
BBS_REVENGE = RV

-- Revenge value added per hit, by command category of the attack.
RV.weight = {
  default   = 1.0,   -- normal combo hits, attack commands
  magic     = 1.5,
  finish    = 3.0,   -- combo finishers / finish commands
  shootlock = 0.3,   -- per shotlock hit
  launch    = 0.5,   -- extra for hits that launch / knock away
}
RV.decay = 60        -- frames (30/s) without a landed hit before the counter clears

-- ---------------------------------------------------------------------
-- Controlled dice.  While one of our wrapped callbacks runs, the boss
-- script sees the values we choose instead of real random numbers.
-- ---------------------------------------------------------------------
-- The dice state lives in one global table so that it survives the game
-- re-running the common scripts (every generation shares it).
local S = __BBS_REVENGE_DICE
if type(S) ~= "table" then
  S = { real = nil, def = nil, seq = nil, idx = 0 }
  __BBS_REVENGE_DICE = S
end

local HIGH = 0.999   -- "never happens" roll (stays inside the engine's 0..1 range)

local function coin()
  if S.real ~= nil and S.real() < 0.5 then return 0 end
  return HIGH
end
RV.coin = coin

local function controlledRandom()
  if S.def == nil then
    return S.real()
  end
  local v = nil
  if S.seq ~= nil then
    S.idx = S.idx + 1
    v = S.seq[S.idx]
  end
  if v == nil then v = S.def end
  if v == "coin" then v = coin() end
  return v
end

local function installRandom()
  if Script == nil or type(Script.Random) ~= "function" then return false end
  if S.real == nil then
    S.real = Script.Random          -- the engine's own function, captured once
    S.hook = controlledRandom
    Script.Random = controlledRandom
  end
  -- If something else wrapped Script.Random later we leave it alone: it
  -- still ends up in our hook, and S.real must never point at a wrapper.
  return true
end

-- State changes made by the boss script run with the real dice again, so
-- the move the boss breaks out into still plays out with its normal variety.
local function unforced(f, self, a, b, c)
  local pd, ps, pi = S.def, S.seq, S.idx
  S.def, S.seq = nil, nil
  local ok, r = pcall(f, self, a, b, c)
  S.def, S.seq, S.idx = pd, ps, pi
  if not ok then error(r, 0) end
  return r
end

local function forced(def, seq, f, self, a, b, c, d)
  if f == nil then return 0 end
  local pd, ps, pi = S.def, S.seq, S.idx
  local go = self.GotoState
  local own = rawget(self, "GotoState")
  if type(go) == "function" then
    rawset(self, "GotoState", function(s, x, y, z) return unforced(go, s, x, y, z) end)
  end
  S.def, S.seq, S.idx = def, seq, 0
  local ok, r = pcall(f, self, a, b, c, d)
  S.def, S.seq, S.idx = pd, ps, pi
  if type(go) == "function" then rawset(self, "GotoState", own) end
  if not ok then error(r, 0) end
  return r
end

local function truthy(r)
  return r ~= nil and r ~= false and r ~= 0
end

local function hitValue(kind, cat)
  local w = RV.weight
  local v = w.default
  if cat == COMMAND_CATEGORY_FINISH then
    v = w.finish
  elseif cat == COMMAND_CATEGORY_SHOOTLOCK then
    v = w.shootlock
  elseif cat == COMMAND_CATEGORY_MACIG then
    v = w.magic
  end
  if cat ~= COMMAND_CATEGORY_FINISH and cat ~= COMMAND_CATEGORY_SHOOTLOCK then
    if kind == ATK_KIND_DMG_BLOW or kind == ATK_KIND_DMG_TOSS or kind == ATK_KIND_DMG_BEAT or kind == ATK_KIND_DMG_FLICK then
      v = v + w.launch
    end
  end
  return v
end

-- Everything that hits the boss builds revenge; healing "hits" do not.
local function countsAsHit(kind)
  return kind ~= ATK_KIND_RECOVER
end

local function shielded(h)
  if Enemy.IsNoDamageReaction(h) then return true end
  local inv = Enemy.IsInvincible
  if inv ~= nil and inv(h) == true then return true end
  return false
end

local function attach(ent, name, handle)
  local c = RV.cfg[name]
  if c == nil or type(ent) ~= "table" then return end
  local odb, od, ord, upd = ent.OnDamageBefore, ent.OnDamage, ent.OnReturnDamage, ent.OnUpdate
  if odb == nil and od == nil then return end     -- nothing to steer (helper object, clone, ...)
  if not installRandom() then return end

  local st = rawget(ent, "__rv")
  if st ~= nil then                         -- same object re-used: just clear
    st.rv, st.idle, st.firing = 0, 0, false
    return
  end
  st = { rv = 0, idle = 0, firing = false, count = 0, name = name, cfg = c }
  rawset(ent, "__rv", st)
  RV.ents[name] = st

  -- dice value(s) for this boss; a config entry may be a function(self)
  local function pick(v, self, default)
    if type(v) == "function" then v = v(self) end
    if v == nil then return default end
    return v
  end
  local function runQuiet(f, self, kind, cat, attr, x)
    if c.pre ~= nil then c.pre(self, false) end
    local dice = pick(c.quiet, self, HIGH)
    if dice == "real" then           -- nothing to take away: pre() keeps the break-out off
      if f == nil then return 0 end
      return f(self, kind, cat, attr, x)
    end
    return forced(dice, nil, f, self, kind, cat, attr, x)
  end
  local function runFire(f, self, kind, cat, attr, x)
    if c.pre ~= nil then c.pre(self, true) end
    local dice = pick(c.fire, self, 0)
    local r
    if dice == "real" then           -- pre() already guarantees the break-out
      if f == nil then r = 0 else r = f(self, kind, cat, attr, x) end
    else
      r = forced(dice, pick(c.fireSeq, self, nil), f, self, kind, cat, attr, x)
    end
    if c.post ~= nil then c.post(self, truthy(r)) end
    return r
  end
  -- hits the boss script answers unconditionally keep their vanilla dice
  local function vanilla(self, kind, cat)
    return c.vanilla ~= nil and c.vanilla(self, kind, cat) == true
  end

  local function active(self)
    return c.active == nil or c.active(self)
  end

  local function done()
    st.rv, st.idle, st.firing = 0, 0, false
    st.count = st.count + 1
  end

  rawset(ent, "OnDamageBefore", function(self, kind, cat, attr, x)
    if not active(self) then
      if odb == nil then return 0 end
      return odb(self, kind, cat, attr, x)
    end
    if vanilla(self, kind, cat) then
      local r = 0
      if odb ~= nil then r = odb(self, kind, cat, attr, x) end
      if truthy(r) then done() end
      return r
    end
    local h = self.myHandle or handle
    if shielded(h) or not countsAsHit(kind) then
      return runQuiet(odb, self, kind, cat, attr, x)
    end
    local v = hitValue(kind, cat)
    if st.rv + v >= c.limit then
      st.rv = st.rv + v
      st.idle = 0
      st.firing = true
      local r = runFire(odb, self, kind, cat, attr, x)
      if not truthy(r) and c.counter ~= nil then
        -- the script has no break-out of its own on a hit: the config brings one
        local ok, r2 = pcall(c.counter, self, kind, cat, attr, x)
        if ok then r = r2 end
      end
      if truthy(r) then done() end
      return r
    end
    local r = runQuiet(odb, self, kind, cat, attr, x)
    if truthy(r) then
      done()                       -- the boss acted on its own: start over
    else
      st.rv = st.rv + v            -- the hit lands: build revenge
      st.idle = 0
    end
    return r
  end)

  if od ~= nil then
    rawset(ent, "OnDamage", function(self, kind, cat, attr, x)
      if not active(self) then return od(self, kind, cat, attr, x) end
      local r
      if vanilla(self, kind, cat) then
        r = od(self, kind, cat, attr, x)
      elseif st.firing and not shielded(self.myHandle or handle) then
        r = runFire(od, self, kind, cat, attr, x)
      else
        r = runQuiet(od, self, kind, cat, attr, x)
      end
      -- reaction cancelled: the boss broke out (a boss that never flinches cancels every reaction)
      if truthy(r) and not c.armored then done() end
      return r
    end)
  end

  if ord ~= nil then
    rawset(ent, "OnReturnDamage", function(self, a, b, c2, d)
      st.rv, st.idle, st.firing = 0, 0, false
      return ord(self, a, b, c2, d)
    end)
  end

  if upd ~= nil then
    rawset(ent, "OnUpdate", function(self, a, b, c2, d)
      if st.rv > 0 then
        local dt = Entity.GetFrameRate(self.myHandle or handle)
        if type(dt) ~= "number" then dt = 1 end
        st.idle = st.idle + dt
        if st.idle > (c.decay or RV.decay) then st.rv, st.idle, st.firing = 0, 0, false end
      end
      local r = upd(self, a, b, c2, d)
      -- break-outs that start from the boss's own update instead of from a hit
      if st.firing and c.fired ~= nil and c.fired(self) == true then done() end
      return r
    end)
  end
end
RV.attach = attach

-- Hook entity construction: wrap the per-entity constructor the first
-- time a configured boss is created.
local factoryCreate = EntityFactory.Create
EntityFactory.Create = function(self, name, handle)
  local rec = self[name]
  if type(rec) == "table" and RV.cfg[name] ~= nil and not rec.__rv then
    local make = rec.Create
    rec.__rv = true
    rec.Create = function(h)
      local ent = make(h)
      if ent ~= nil then
        local ok = pcall(attach, ent, name, h)
      end
      return ent
    end
  end
  return factoryCreate(self, name, handle)
end

__BOSS_CONFIG__
end)

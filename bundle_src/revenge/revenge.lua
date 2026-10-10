-- =====================================================================
--  BBS Revenge Value  (KH2-style revenge system for Birth by Sleep FM)
--
--  Loaded as a prologue of arc/system/CommonLua.arc -> Factory.lub.
--  The original Factory.lub code runs first (unchanged, embedded), then
--  this file layers a revenge counter on top of selected bosses.
--
--  How BBS works (vanilla): every boss AI has OnDamageBefore/OnDamage
--  callbacks.  OnDamageBefore runs on (nearly) every hit and can negate
--  it; OnDamage runs only when no damage reaction is under way - the
--  first hit of a combo - and can refuse the flinch.  Humanoid bosses
--  roll dice in them on every hit, so a combo can be broken by its
--  first hit; bosses whose only break-out lives in OnDamage (Zack,
--  Hades) or nowhere (Peter Pan) can instead be comboed forever.
--
--  Version 2 copies the system KH2 itself uses (verified in its code
--  and data): every hit adds a weight to a gauge, the gauge drains
--  while the boss is not being hit, and at a per-boss limit the boss
--  performs its revenge action - guaranteed, even mid-combo:
--    - below the limit the dice are taken away: no random escapes;
--    - at the limit the boss breaks out through its own script, or
--      through a `counter` the config brings; a watchdog in the boss's
--      update forces it even when the engine mutes the hit callbacks
--      (some juggle reactions do), and an HP watch keeps counting the
--      hits the callbacks never see;
--    - the limit is re-rolled a little after each revenge (KH2 FM
--      varies the cap), and the break-out can get a short armour
--      window so it cannot be stuffed (KH2 revenge actions have one).
--  Hits that land while the boss has super armour are not counted.
--  Bosses that are not listed (all the large ones) are untouched.
-- =====================================================================
__FACTORY_ORIGINAL__()

-- Everything below is additive.  It runs inside pcall so that, whatever
-- happens, the original factory above is already in place and working.
pcall(function()
local RV = { version = "2.0", cfg = {}, ents = {} }
BBS_REVENGE = RV

-- Revenge value added per hit.  Scaled to KH2's own attack data
-- (00battle.bin atkp, "revenge damage", read from the game: a normal
-- hit is 1.0, finishers ~3, multi-hit spells small amounts per tick).
-- All times below are in the engine's own frames: Entity.GetFrameRate
-- advances 60 a second (verified in the game's code - the entity dt it
-- returns is in 60ths).  The first v2 build read them as 30ths, so the
-- gauge drained twice as fast as KH2 with half the intended grace and
-- could never fill at a real combo's pace.
RV.weight = {
  default   = 1.0,   -- normal combo hits, attack commands
  magic     = 1.5,
  magicCast = 4.0,   -- one cast adds at most this (KH2's heaviest magic)
  magicWin  = 90,    -- 1.5 s: magic hits this close count as one cast
  finish    = 3.0,   -- combo finishers / finish commands
  shootlock = 0.3,   -- per shotlock hit
  launch    = 0.5,   -- extra for hits that launch / knock away
}
RV.grace    = 60     -- 1 s after the last hit before the gauge drains: a combo's
                     -- own gaps (0.3 .. 0.9 s) never drain it, as in KH2, where
                     -- hit-stun blocks the drain outright
RV.drain    = 0.1    -- per frame once draining = KH2's 6 hit-units a second
RV.vary     = 1.0    -- next limit = boss's limit +- up to this, re-rolled per revenge
RV.watchdog = 12     -- 0.2 s at the limit without a break-out before `counter` is forced from the update
RV.iframes  = 0      -- armour frames after a forced break-out (0 = none; per boss: cfg.iframes)

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

-- KH2's weights, from its attack data.  Magic is capped per cast: hits
-- within magicWin frames of each other count as one cast of at most
-- magicCast, the way KH2 gives multi-hit spells small per-tick values.
local function hitValue(st, kind, cat)
  local w = RV.weight
  if cat == COMMAND_CATEGORY_FINISH then return w.finish end
  if cat == COMMAND_CATEGORY_SHOOTLOCK then return w.shootlock end
  if cat == COMMAND_CATEGORY_MACIG then
    if st.t - st.magT > w.magicWin then st.magSum = 0 end
    st.magT = st.t
    local v = w.magic
    if st.magSum + v > w.magicCast then v = w.magicCast - st.magSum end
    if v < 0 then v = 0 end
    st.magSum = st.magSum + v
    return v
  end
  local v = w.default
  if kind == ATK_KIND_DMG_BLOW or kind == ATK_KIND_DMG_TOSS or kind == ATK_KIND_DMG_BEAT or kind == ATK_KIND_DMG_FLICK then
    v = v + w.launch
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
    st.rv, st.idle, st.firing, st.fireT, st.armor = 0, 0, false, 0, 0
    st.limit = c.limit
    return
  end
  st = { rv = 0, idle = 0, firing = false, fireT = 0, armor = 0, count = 0, fcount = 0,
         t = 0, magT = -1e9, magSum = 0, hp = nil, sawHit = false, seen = -1e9, drop = -1e9,
         limit = c.limit, name = name, cfg = c }
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

  local h0 = handle
  local function myh(self) return self.myHandle or h0 end

  -- the next limit: the boss's number, re-rolled a little (KH2 FM varies the cap)
  local function rollLimit(self)
    local vary = c.vary
    if vary == nil then vary = RV.vary end
    local r = 0.5
    if S.real ~= nil then r = S.real() end
    local l = c.limit + (r * 2 - 1) * vary
    if l < 2 then l = 2 end
    st.limit = l
  end

  -- a break-out happened.  fired = it was our forced revenge: arm the
  -- armour window (KH2 revenge actions cannot be stuffed), if configured.
  -- KH2 never clears the gauge (only the boss's constructor and the drain
  -- bottoming out write 0 to +0xd48): it stays where it is and drains while
  -- the boss acts on its own, out of hit-stun.  So the drain starts at once,
  -- and a player who goes straight back in finds a boss that breaks out
  -- again after a few hits instead of a fresh full gauge.
  local function done(fired)
    local grace = c.grace
    if grace == nil then grace = RV.grace end
    st.idle, st.firing, st.fireT = grace, false, 0
    if c.clear then st.rv, st.idle = 0, 0 end     -- per boss: its own timer, not KH2's gauge
    st.count = st.count + 1
    if fired then st.fcount = st.fcount + 1 end
    rollLimit()
    if fired then
      local n = c.iframes or RV.iframes
      if n ~= nil and n ~= false and n > 0 and type(Enemy.EnableNoDamageReaction) == "function" then
        Enemy.EnableNoDamageReaction(myh(ent), 1)
        st.armor = n
      end
    end
  end

  -- the forced revenge of a hit (or of the watchdog): the boss's own
  -- callback first, the config's counter when the script has none that
  -- fires on a hit.  Returns the value for the engine.
  local function fire(f, self, kind, cat, attr, x)
    local r = runFire(f, self, kind, cat, attr, x)
    if not truthy(r) and c.counter ~= nil then
      local ok, r2 = pcall(c.counter, self, kind, cat, attr, x)
      if ok then r = r2 end
    end
    if truthy(r) then done(true) end
    return r
  end

  -- KH2 bosses press; a config with haste < 1 shortens the idle times a
  -- script asks its parameters for (only where the script reads them
  -- through a method on the entity - timers baked into a script as
  -- constants cannot be reached).
  if type(c.haste) == "number" and c.haste ~= 1 then
    local fns = c.hasteFns or { "GetIdlingChangeTime" }
    for i = 1, #fns do
      local fname = fns[i]
      local fn = ent[fname]
      if type(fn) == "function" then
        rawset(ent, fname, function(self, a, b)
          local v = fn(self, a, b)
          if type(v) == "number" then return v * c.haste end
          return v
        end)
      end
    end
  end

  rawset(ent, "OnDamageBefore", function(self, kind, cat, attr, x)
    if not active(self) then
      if odb == nil then return 0 end
      return odb(self, kind, cat, attr, x)
    end
    st.sawHit = true
    if vanilla(self, kind, cat) then
      local r = 0
      if odb ~= nil then r = odb(self, kind, cat, attr, x) end
      if truthy(r) then done(false) end
      return r
    end
    if shielded(myh(self)) or not countsAsHit(kind) then
      return runQuiet(odb, self, kind, cat, attr, x)
    end
    local v = hitValue(st, kind, cat)
    if st.rv + v >= st.limit then
      st.rv = st.rv + v
      st.idle = 0
      st.seen = st.t
      st.firing = true
      return fire(odb, self, kind, cat, attr, x)
    end
    local r = runQuiet(odb, self, kind, cat, attr, x)
    if truthy(r) then
      done(false)                  -- the boss acted on its own: start over
    else
      st.rv = st.rv + v            -- the hit lands: build revenge
      st.idle = 0
      st.seen = st.t
    end
    return r
  end)

  if od ~= nil then
    rawset(ent, "OnDamage", function(self, kind, cat, attr, x)
      if not active(self) then return od(self, kind, cat, attr, x) end
      local r
      if vanilla(self, kind, cat) then
        r = od(self, kind, cat, attr, x)
        if truthy(r) and not c.armored then done(false) end
        return r
      end
      st.sawHit = true
      if st.firing and not shielded(myh(self)) then
        r = runFire(od, self, kind, cat, attr, x)
        if truthy(r) and not c.armored then done(true) end
        return r
      end
      r = runQuiet(od, self, kind, cat, attr, x)
      -- reaction cancelled: the boss broke out (a boss that never flinches cancels every reaction)
      if truthy(r) and not c.armored then done(false) end
      return r
    end)
  end

  if ord ~= nil or c.recover ~= nil then
    rawset(ent, "OnReturnDamage", function(self, a, b, c2, d)
      -- out of hit-stun: KH2 starts draining here, it does not clear the gauge
      -- (clearing it made every combo string start from nothing, so only one
      -- unbroken string of ~10 hits could ever bring a break-out)
      local grace = c.grace
      if grace == nil then grace = RV.grace end
      if c.clear then st.rv, st.idle, st.firing, st.fireT = 0, 0, false, 0
      elseif st.idle < grace then st.idle = grace end
      local r
      if ord ~= nil then r = ord(self, a, b, c2, d) end
      -- KH2 bosses come back at you after a combo; the config's recover
      -- overrides a script that just stands up into idling.
      if c.recover ~= nil then pcall(c.recover, self) end
      return r
    end)
  end

  rawset(ent, "OnUpdate", function(self, a, b, c2, d)
    local dt = Entity.GetFrameRate(myh(self))
    if type(dt) ~= "number" or dt <= 0 or dt > 6 then dt = 1 end
    st.t = st.t + dt
    -- armour window after a forced revenge.  Held every frame: the boss's own
    -- states switch "no damage reaction" off when they end (a counter's
    -- OnEndState), which used to cut the window short.
    if st.armor > 0 then
      st.armor = st.armor - dt
      if type(Enemy.EnableNoDamageReaction) == "function" then
        Enemy.EnableNoDamageReaction(myh(self), st.armor > 0 and 1 or 0)
      end
      if st.armor <= 0 then st.armor = 0 end
    end
    -- hits the engine hides from the callbacks (some juggle reactions)
    -- still cost HP: count them so those combos cannot run forever.  Only
    -- HP loss inside a hit string counts - within 1 s of a counted hit, or
    -- a second hidden drop within 0.5 s of the first - so poison and burn
    -- ticks (spaced wider, with no hits landing) cannot build revenge.
    local hp = Enemy.GetHp(myh(self))
    if type(hp) == "number" then
      if type(st.hp) == "number" and hp < st.hp and not st.sawHit and active(self) then
        if st.t - st.seen <= 60 or st.t - st.drop <= 30 then
          st.rv = st.rv + RV.weight.default
          st.idle = 0
          st.seen = st.t
          if st.rv >= st.limit then st.firing = true end
        end
        st.drop = st.t
      end
      st.hp = hp
    end
    st.sawHit = false
    -- the gauge drains while the boss is left alone (KH2: out of hit-stun)
    if st.rv > 0 then
      local grace = c.grace
      if grace == nil then grace = RV.grace end
      st.idle = st.idle + dt
      if st.idle > grace then
        local dr = c.drain
        if dr == nil then dr = RV.drain end
        st.rv = st.rv - dr * dt
        if st.rv <= 0 then st.rv, st.firing, st.fireT = 0, false, 0 end
      end
    end
    -- the watchdog: at the limit, a break-out that no hit has managed to
    -- fire (muted callbacks, a script with no own break-out) is forced
    if st.firing then
      st.fireT = st.fireT + dt
      if st.fireT >= RV.watchdog and c.counter ~= nil and active(self) then
        local ok, r = pcall(c.counter, self)
        if ok and truthy(r) then done(true) else st.fireT = 0 end
      end
    end
    local r
    if upd ~= nil then r = upd(self, a, b, c2, d) end
    -- break-outs that start from the boss's own update instead of from a hit
    if st.firing and c.fired ~= nil and c.fired(self) == true then done(false) end
    return r
  end)
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

-- ---------------------------------------------------------------------
-- Boss table.  key = entity name used by the game.
--   limit    revenge value at which the boss breaks out
--   quiet    dice value fed to the boss script below the limit  (default 0.999;
--            "real" = leave the dice alone)
--   fire     dice value fed to the boss script at the limit     (default 0;
--            "real" = leave the dice alone, pre() forces the break-out)
--   fireSeq  first dice values at the limit ("coin" = low or high at random)
--            quiet / fire / fireSeq may also be function(self)
--   pre      function(self, firing) run before the boss's own callback
--   post     function(self, fired) run after it when the limit was reached
--   vanilla  function(self, kind, cat) -> true: this hit keeps vanilla dice
--   active   function(self) -> false to leave this instance alone
--   counter  function(self, kind, cat) -> 1 when it made the boss break out: used at the
--            limit when the boss script itself has no break-out on a hit
--   armored  true: the boss never flinches, so its OnDamage result is not a break-out
--   fired    function(self) -> true once a break-out that starts from the boss's own
--            update (not from a hit) is under way
--   grace    frames (60/s) after the last hit before the gauge starts to drain (default RV.grace)
--   drain    gauge drained per frame once draining (default RV.drain; KH2's pace)
--   vary     the limit is re-rolled +- this after each revenge (default RV.vary)
--   iframes  armour frames (60/s) after a forced break-out, so it cannot be stuffed (default none)
--   recover  function(self) run after the boss recovers from hit-stun: KH2 bosses come
--            back at you, several BBS scripts just stand up into idling
--   haste    < 1 shortens the idle times the script reads through entity methods
--            (hasteFns, default GetIdlingChangeTime); timers baked in as constants stay
-- Large bosses are deliberately absent: they keep their vanilla behaviour.
-- ---------------------------------------------------------------------
local cfg = RV.cfg

-- Many scripts roll "Random() < dmgCount * k".  Keep their own hit counter
-- at zero below the limit and saturate it when the limit is reached.
local function dmgCountPre(self, firing)
  if type(self.dmgCount) == "number" then
    if firing then self.dmgCount = 99 else self.dmgCount = 0 end
  end
end

-- Break-outs for the bosses whose own escapes live in dice or in OnDamage:
-- each is the boss's own move set, usable per hit and from the watchdog.
local function vanitasCounter(self)
  if type(self.GotoState) ~= "function" or type(self.GetState) ~= "function" or self.stack == nil then return 0 end
  local s = self:GetState()
  if s == "Dead" or s == "Appear" or s == "Cartwheel" then return 0 end
  self.stack:push("Idling")
  self:GotoState("Cartwheel")
  if type(self.dmgCount) == "number" then self.dmgCount = 0 end
  return 1
end
local function wielderCounter(self)
  if type(self.GotoState) ~= "function" or type(self.GetState) ~= "function" or self.stack == nil then return 0 end
  if type(self.GetGrandEvasionRate) ~= "function" then return 0 end     -- not the shared wielder script
  local s = self:GetState()
  if s == "EvasionAction" or s == "Dead" or s == "Appear" then return 0 end
  self.stack:push("BattleIdling")
  self.stack:push("EvasionAction")
  self:GotoState(self.stack:pop(1))
  if type(self.DamageCnt) == "number" then self.DamageCnt = 0 end
  return 1
end

-- Vanitas (story fights)
-- vanilla: 15% cartwheel on every hit + growing chance of a warp counter.
local vanitas = { limit = 10, fire = 0.5, pre = dmgCountPre, counter = vanitasCounter }
cfg.b10ex00 = vanitas
cfg.b10ex01 = vanitas
cfg.b10ex02 = vanitas
-- Aqua's final fight: at range the script can only cartwheel, so let it.
cfg.b63ex00 = {
  limit = 10,
  pre = dmgCountPre,
  counter = vanitasCounter,
  fire = function(self)
    local d = Entity.CalcDistanceSq(self.myHandle, self.targetHandle)
    if type(d) == "number" and d > 25 then return 0 end
    return 0.5
  end,
}

-- Vanitas (Ventus's final fight): growing chance of Dark Splicer per hit.
cfg.b11ex00 = { limit = 10, pre = dmgCountPre, counter = vanitasCounter }

-- Vanitas Remnant: growing chance of Dark Splicer / warp attack per hit.
-- (his answer to shotlocks is unconditional and keeps its own dice)
cfg.b12ex00 = {
  limit = 8,
  fireSeq = { 0, "coin" },
  pre = dmgCountPre,
  vanilla = function(self, kind, cat) return cat == COMMAND_CATEGORY_SHOOTLOCK end,
  counter = function(self)
    if type(self.GotoState) ~= "function" or type(self.GetState) ~= "function" or self.stack == nil then return 0 end
    local s = self:GetState()
    if s == "Dead" or s == "Appear" or s == "DarkSplicer2" or s == "WarpAttack2" then return 0 end
    self.stack:push("Idling")
    if Script.Random() > 0.66 then
      self.stack:push("DarkSplicer2")
    else
      self.stack:push("WarpAttack2")
    end
    if type(self.dmgCount) == "number" then self.dmgCount = 0 end
    self:GotoState(self.stack:pop(1))
    return 1
  end,
}

-- Master Eraqus: guard (+ counter) chance on every hit.
cfg.b20ex00 = {
  limit = 12,
  pre = dmgCountPre,
  counter = function(self)
    if type(self.GotoState) ~= "function" or type(self.GetState) ~= "function" or self.stack == nil then return 0 end
    local s = self:GetState()
    if s == "Dead" or s == "Appear" or s == "Guard" or s == "Kagerou" then return 0 end
    self.stack:push("Wander")
    self.stack:push("Idling")
    if Script.Random() < 0.5 then self.stack:push("Kagerou") end
    self:GotoState("Guard")
    if type(self.dmgCount) == "number" then self.dmgCount = 0 end
    return 1
  end,
}

-- Armor of the Master: 30% counter per hit in his own style, and only a
-- 30% chance to flinch at all while he copies Ventus / Terra / Aqua.
local armor = {
  limit = 9,
  quiet = function(self) if self.style == 2 then return 0.999 end return 0 end,
  fire  = function(self) if self.style == 2 then return 0 end return 0.5 end,
  counter = function(self)
    if type(self.GotoState) ~= "function" or type(self.GetState) ~= "function" or self.stack == nil then return 0 end
    local s = self:GetState()
    if s == "Dead" or s == "Appear" or s == "Guard" then return 0 end
    if s ~= nil then self.stack:push(s) end
    self:GotoState("Guard")
    return 1
  end,
}
cfg.b81vs00 = armor

-- Braig: 30% escape on every hit.
local braig = {
  limit = 9,
  counter = function(self)
    if type(self.GotoState) ~= "function" or type(self.GetState) ~= "function" or self.stack == nil then return 0 end
    local s = self:GetState()
    if s == "Dead" or s == "Appear" or s == "Escape" or s == "InvertedShoot" or s == "Sniper"
       or s == "ChargeShoot" or s == "ArutemaShoot" then return 0 end
    self.stack:clear()
    self.stack:push("Idling")
    self:GotoState("Escape")
    return 1
  end,
}
cfg.b30ex00 = braig
cfg.b32ex00 = braig

-- Mysterious Figure: guard / warp counter / time-slip rolls on every hit.
cfg.b01ex00 = {
  limit = 9,
  counter = function(self)
    if type(self.GotoState) ~= "function" or type(self.GetState) ~= "function" or self.stack == nil then return 0 end
    local s = self:GetState()
    if s == "Dead" or s == "DeadCopy" or s == "Appear" or s == "WarpMove_Counter" or s == "Guard" then return 0 end
    self.stack:push("BattleIdling")
    self.stack:push("WarpMove_Counter")
    if type(self.DamageCnt) == "number" then self.DamageCnt = 0 end
    self:GotoState(self.stack:pop(1))
    return 1
  end,
  -- one roll picks the move: < GuardRate guard, < GuardRate + WarpRate warp counter
  fire = function(self) if RV.coin() == 0 then return 0 end return 0.45 end,
  pre = function(self, firing)
    if firing and type(self.DamageCnt) == "number" then
      self.__rvCnt = self.DamageCnt
      self.DamageCnt = 99
    end
  end,
  post = function(self, fired)
    if type(self.__rvCnt) == "number" then
      if not fired and self.DamageCnt == 99 then self.DamageCnt = self.__rvCnt end
      self.__rvCnt = nil
    end
  end,
}

-- Captain Hook: guard / evade roll on every hit (his own recovery already
-- counter-attacks, so no recover is needed).  The counter is his own escape -
-- guard or evade, then his jump-cut counter - for hits his rolls do not cover.
cfg.b01pp00 = {
  limit = 10,
  iframes = 40,
  counter = function(self)
    if type(self.GotoState) ~= "function" or type(self.GetState) ~= "function" then return 0 end
    local s = self:GetState()
    if s == "Dead" or s == "Appear" or s == "Guard" or s == "Evade" or s == "JumpCutting"
       or s == "SinkSea" or s == "FlyUp" or s == "Blow" or s == "FireDamage" then return 0 end
    if self.stack == nil then return 0 end
    self.stack:push("Idling")
    self.stack:push("JumpCutting")
    if Script.Random() < 0.5 then
      self.stack:push("Guard")
    else
      self.stack:push("Evade")
    end
    self:GotoState(self.stack:pop(1))
    pcall(function() Entity.SetMovementCollKind(self.myHandle, COLL_KIND_ENEMY) end)
    return 1
  end,
}

-- Peter Pan.  vanilla: no break-out of any kind - he can be comboed forever - and
-- he recovers from hit-stun into idling.  At the limit he answers with one of his
-- own attacks; after any combo he goes straight back on the offensive.
cfg.b20pp00 = {
  limit = 10,
  iframes = 48,
  counter = function(self)
    if type(self.GotoState) ~= "function" or type(self.GetState) ~= "function" then return 0 end
    local s = self:GetState()
    if s == "Dead" or s == "Appear" or s == "BattleStartState" then return 0 end
    self.targetHandle = Enemy.SearchAttackEntity(self.myHandle, SEARCH_TYPE_NEAR)
    if self.stack ~= nil then
      pcall(function() self.stack:clear() self.stack:push("Idling") end)
    end
    pcall(self.ReSetRot, self)
    self:GotoState("Attack3")
    return 1
  end,
  recover = function(self)
    if type(self.GotoState) ~= "function" or type(self.GetState) ~= "function" then return end
    local s = self:GetState()
    if s == "Dead" or s == "Appear" then return end
    self.targetHandle = Enemy.SearchAttackEntity(self.myHandle, SEARCH_TYPE_NEAR)
    self:GotoState("BeforeAttackIdling")
  end,
}

-- Experiment 221: growing chance to flee per hit.
cfg.b20ls00 = {
  limit = 8,
  fireSeq = { 0, "coin" },
  pre = dmgCountPre,
  counter = function(self)
    if type(self.GotoState) ~= "function" or type(self.GetState) ~= "function" or self.stack == nil then return 0 end
    local s = self:GetState()
    if s == "Dead" or s == "Appear" or s == "Flee" or s == "StartAirMove" or s == "Electroshock" or s == "AirMove" then return 0 end
    self.stack:clear()
    self.stack:push("AirMove")
    self.stack:push("Flee")
    self.stack:push("StartAirMove")
    if Script.Random() < 0.5 then
      self.stack:push("Electroshock")
      Enemy.EnableNoDamageReaction(self.myHandle, 1)      -- as his own escape does
    end
    if type(self.dmgCount) == "number" then self.dmgCount = 0 end
    self:GotoState(self.stack:pop(1))
    return 1
  end,
}

-- Maleficent: vanilla teleports / staff-attacks after only 3 hits - but only
-- from OnDamage, which the engine asks on the first hit of a combo alone, so
-- an unbroken combo could loop her.  The counter below is her own pair of
-- moves, fired per hit / from the watchdog.
cfg.b01sb00 = {
  limit = 9,
  fire = "real",
  iframes = 40,
  counter = function(self)
    if type(self.GotoState) ~= "function" or type(self.GetState) ~= "function" then return 0 end
    local s = self:GetState()
    if s == "Dead" or s == "Appear" or s == "Teleport" or s == "StaffAttack" or s == "BeginBarrier"
       or s == "BuildingBarrier" or s == "CheckBarrierComplete" then return 0 end
    if self.stack ~= nil and type(self.stack.getn) == "function" and self.stack:getn() <= 0 then
      if s ~= nil then self.stack:push(s) else self.stack:push("IdlingA") end
    end
    if Script.Random() < 0.5 and type(self.ChangeWarpState) == "function" then
      self:ChangeWarpState()
    else
      self:GotoState("StaffAttack")
    end
    if type(self.dmgCount) == "number" then self.dmgCount = 0 end
    pcall(function() Enemy.SetFaceAnim(self.myHandle, 3, 2) end)
    return 1
  end,
  pre = function(self, firing)
    if type(self.dmgCount) == "number" then
      if firing then self.dmgCount = 99 else self.dmgCount = 0 end
    end
  end,
}

-- Keyblade wielders (shared AI): Ventus / Terra / Aqua bosses and
-- Terra-Xehanort.  vanilla: dodge-roll roll on every hit; Terra-Xehanort
-- warps into a special attack after ~5 hits.  Allies are left alone.
local function wielderActive(self) return self.friendFlag ~= 1 end
local function wielderPre(self, firing)
  if type(self.DamageCnt) ~= "number" then return end
  if firing then
    self.__rvCnt = self.DamageCnt
    self.DamageCnt = 99
    if type(self.saveDamage) == "number" then self.saveDamage = 2 end
  elseif type(self.saveDamage) == "number" then
    self.saveDamage = 0
  end
end
local function wielderPost(self, fired)
  if type(self.__rvCnt) == "number" then
    if not fired and self.DamageCnt == 99 then self.DamageCnt = self.__rvCnt end
    self.__rvCnt = nil
  end
end
-- (a wielder whose script has no dodge chance, e.g. in mid-air, still has none)
local wielder = { limit = 9, active = wielderActive, pre = wielderPre, post = wielderPost, counter = wielderCounter }
local wielders = { "b60ex00", "b62ex00", "b68ex00", "b69ex00", "b70ex00", "b72ex00", "b73ex00",
                   "b78ex00", "b79ex00", "b80ex00", "b82ex00", "b83ex00", "b88ex00", "b89ex00" }
for i = 1, #wielders do
  cfg[wielders[i]] = wielder
end
local xehanort = { limit = 8, active = wielderActive, pre = wielderPre, post = wielderPost, fireSeq = { 0.999, "coin" }, counter = wielderCounter }
cfg.b50ex00 = xehanort
cfg.b51ex00 = xehanort

-- Terra-Xehanort (final episode; own AI when it has SetDmgCount, shared
-- wielder AI otherwise).  vanilla: counter-attacks after 2-5 hits.
local function ownAI(self) return self.SetDmgCount ~= nil end
cfg.b52ex00 = {
  limit = 8,
  active = wielderActive,
  counter = wielderCounter,     -- the shared-AI form only; his own AI has no EvasionAction and is covered by pre()
  fire = function(self) if ownAI(self) then return "real" end return 0 end,
  fireSeq = function(self) if ownAI(self) then return nil end return xehanort.fireSeq end,
  pre = function(self, firing)
    if ownAI(self) then
      if type(self.nDmgCount) == "number" then
        if firing then
          self.nDmgCount = 0
          self.nDmgHamariCount = 1
        else
          self.nDmgCount = 100
        end
      end
    else
      wielderPre(self, firing)
    end
  end,
  post = function(self, fired)
    if not ownAI(self) then wielderPost(self, fired) end
  end,
}

-- Zack (all three versions share one script).  vanilla: one hit counter for the
-- whole fight, counter move at 8-10 hits - but it lives in OnDamage, which the
-- engine only asks on the first hit of a combo, so an unbroken combo could loop
-- him forever.  The counter below is his own move set, fired per hit / from the
-- watchdog with roughly his own odds.
local function zackCounter(self)
  if type(self.GotoState) ~= "function" or type(self.GetState) ~= "function" then return 0 end
  local s = self:GetState()
  if s == "Bushinhazan" or s == "Climbhazard" or s == "BackJump_Short" or s == "Hakougeki"
     or s == "BackJump_Short_Cancel" or s == "Dead" or s == "Appear" then return 0 end
  if self.stack == nil then return 0 end
  self.stack:clear()
  self.stack:push("Move")
  local r = Script.Random()
  if r < 0.4 then
    self.stack:push("Climbhazard")
    self.stack:push("BackJump_Short")
  elseif r < 0.7 then
    self.stack:push("Hakougeki")
    self.stack:push("BackJump_Short_Cancel")
  else
    self.stack:push("BackJump_Short")
  end
  self:GotoState(self.stack:pop(1))
  if type(self.damageCnt) == "number" then self.damageCnt = 0 end
  return 1
end
local zack = {
  limit = 9,
  fire = "real",
  iframes = 40,
  counter = zackCounter,
  pre = function(self, firing)
    if type(self.damageCnt) == "number" then
      if firing then self.damageCnt = 99 else self.damageCnt = 0 end
    end
  end,
}
cfg.b30he00 = zack
cfg.b40he00 = zack
cfg.b60vs00 = zack

-- Hades (story and Mirage Arena).  vanilla: counters at the 7th hit in a row on the
-- same spot.  (While he burns red he ignores hits; that stays.)
local function hadesCounter(self)
  if type(self.GotoState) ~= "function" or type(self.GetState) ~= "function" then return 0 end
  local s = self:GetState()
  if s == "Watching" or s == "Dead" or s == "Freeze" or s == "Appear" or s == "BladesCrossing"
     or s == "MegaFire" or s == "FingernailofFire" or s == "NoRiaFingernailofFire" then return 0 end
  if Script.Random() < 0.5 then
    self:GotoState("BladesCrossing")
  else
    self:GotoState("NoRiaFingernailofFire")
  end
  if type(self.sameDamageCont) == "number" then self.sameDamageCont = 0 end
  return 1
end
local hades = {
  limit = 9,
  fire = "real",
  iframes = 40,
  counter = hadesCounter,
  pre = function(self, firing)
    if type(self.sameDamageCont) ~= "number" then return end
    if firing then
      self.sameDamageCont = 99
      self.oldDamagePoint = Entity.GetDamagePoint(self.myHandle)
    else
      self.sameDamageCont = 0
    end
  end,
}
cfg.b01he00 = hades
cfg.b80vs00 = hades

-- Master Xehanort.  vanilla: never leaves a ground combo - his script's escape (a quick
-- warp, from idle into a warp attack) is still there but switched off at the first line
-- of OnDamageBefore.  At the limit he uses it.
cfg.b40ex00 = {
  limit = 9,
  counter = function(self)
    if type(self.QuickWarpStart) ~= "function" or type(self.GetState) ~= "function" then return 0 end
    local s = self:GetState()
    if s == "Warp" or s == "UltimateFreeze" or s == "Dead" or s == "Appear" or self.phase == 100 then return 0 end
    self:QuickWarpStart()
    self.damageCombo = 0
    return 1
  end,
}

-- No Heart (standing form).  He never flinches (he has no flinch animations), so there is
-- no combo to break out of.  vanilla: 4.4-10.4 s after the first hit of a string of hits
-- he answers with his burst.  Here the burst comes when the revenge value is reached.
cfg.b85vs00 = {
  limit = 14,
  grace = 120, drain = 0.03,    -- vanilla forgets a string of hits over ~10 s
  armored = true,
  quiet = "real",
  fire = "real",
  pre = function(self, firing)
    if type(self.burstTimer) ~= "number" then return end
    self.burstStartFlag = 1                       -- timer "running": the script does not roll a new one
    if firing then self.burstTimer = -1 else self.burstTimer = 100000 end
  end,
  fired = function(self) return self:GetState() == "BurstAttack" end,
}

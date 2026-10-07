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
--   decay    frames without a landed hit before the counter clears (default RV.decay)
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

-- Vanitas (story fights)
-- vanilla: 15% cartwheel on every hit + growing chance of a warp counter.
local vanitas = { limit = 10, fire = 0.5, pre = dmgCountPre }
cfg.b10ex00 = vanitas
cfg.b10ex01 = vanitas
cfg.b10ex02 = vanitas
-- Aqua's final fight: at range the script can only cartwheel, so let it.
cfg.b63ex00 = {
  limit = 10,
  pre = dmgCountPre,
  fire = function(self)
    local d = Entity.CalcDistanceSq(self.myHandle, self.targetHandle)
    if type(d) == "number" and d > 25 then return 0 end
    return 0.5
  end,
}

-- Vanitas (Ventus's final fight): growing chance of Dark Splicer per hit.
cfg.b11ex00 = { limit = 10, pre = dmgCountPre }

-- Vanitas Remnant: growing chance of Dark Splicer / warp attack per hit.
-- (his answer to shotlocks is unconditional and keeps its own dice)
cfg.b12ex00 = {
  limit = 8,
  fireSeq = { 0, "coin" },
  pre = dmgCountPre,
  vanilla = function(self, kind, cat) return cat == COMMAND_CATEGORY_SHOOTLOCK end,
}

-- Master Eraqus: guard (+ counter) chance on every hit.
cfg.b20ex00 = { limit = 12, pre = dmgCountPre }

-- Armor of the Master: 30% counter per hit in his own style, and only a
-- 30% chance to flinch at all while he copies Ventus / Terra / Aqua.
local armor = {
  limit = 9,
  quiet = function(self) if self.style == 2 then return 0.999 end return 0 end,
  fire  = function(self) if self.style == 2 then return 0 end return 0.5 end,
}
cfg.b81vs00 = armor

-- Braig: 30% escape on every hit.
local braig = { limit = 9 }
cfg.b30ex00 = braig
cfg.b32ex00 = braig

-- Mysterious Figure: guard / warp counter / time-slip rolls on every hit.
cfg.b01ex00 = {
  limit = 9,
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

-- Captain Hook: guard / evade roll on every hit.
cfg.b01pp00 = { limit = 10 }

-- Experiment 221: growing chance to flee per hit.
cfg.b20ls00 = { limit = 8, fireSeq = { 0, "coin" }, pre = dmgCountPre }

-- Maleficent: vanilla teleports / counters after only 3 hits.
cfg.b01sb00 = {
  limit = 9,
  fire = "real",
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
local wielder = { limit = 9, active = wielderActive, pre = wielderPre, post = wielderPost }
local wielders = { "b60ex00", "b62ex00", "b68ex00", "b69ex00", "b70ex00", "b72ex00", "b73ex00",
                   "b78ex00", "b79ex00", "b80ex00", "b82ex00", "b83ex00", "b88ex00", "b89ex00" }
for i = 1, #wielders do
  cfg[wielders[i]] = wielder
end
local xehanort = { limit = 8, active = wielderActive, pre = wielderPre, post = wielderPost, fireSeq = { 0.999, "coin" } }
cfg.b50ex00 = xehanort
cfg.b51ex00 = xehanort

-- Terra-Xehanort (final episode; own AI when it has SetDmgCount, shared
-- wielder AI otherwise).  vanilla: counter-attacks after 2-5 hits.
local function ownAI(self) return self.SetDmgCount ~= nil end
cfg.b52ex00 = {
  limit = 8,
  active = wielderActive,
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
-- whole fight, counter move at 8-10 hits; a roll on every launching hit.
local zack = {
  limit = 9,
  fire = "real",
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
local hades = {
  limit = 9,
  fire = "real",
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
  decay = 300,          -- vanilla forgets the string after 10 s without a hit
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

/* Pace of the player's actions: less ending lag, weight in the air, a little more speed, KH2's cast times.

   The player (PL::CPlayer, update 220550) is a state machine; an action is one of the states
       0x10 attacks (normal combo = sub-state 5, deck attack commands = sub-state 3 / 4)
       0x11 magic   0x12 items   0x13 friend / D-Link commands   0x14 finishers
   and every one of them ends only when its animation has played out (magic: when a fixed 40-tick lock after the
   release has run out).  The stick is not read at all in between.  docs/NOTES.md has the map; in short:

   1. Walk-out.  Each state has one place where it asks "has the animation finished?" and either ends the action
      (21c510) or carries on.  A hook on each of those tests ends the action early when the stick is tilted and the
      action is "done": every hit window of the body and the weapon has closed (read from the game's own collision
      controllers), the attack's own movement and its triggers are over, plus a few frames of follow-through.
      While the next hit of a combo can still be asked for, only a *change* of the stick counts (neutral <-> tilted
      or a turn of 1 rad), so that holding the stick towards the enemy never cuts a combo short.
          2291bf  normal attacks        end 2291cd      2246c0 / 224476  deck attacks   end 2264d3
          2253ff  dash-type commands    end 22540d      262056  items                    end 262064
          2599ab  friend commands       end 2599b9      2562bc  generic finishers        end 2562c6
          26825f / 26828a  magic recovery               end 26829a
   2. Air.  An action started in the air sets pl+0x318 bit 23: the integrator 21cb00 then forbids sinking until
      frMoveEnd and uses 16 % gravity for the rest of the action.  The hook at 21cb55 clears that bit (in the
      register copy the function tests) once the action is done and nothing can follow, so the character falls
      through the recovery.  After the action the fall state holds vy for the first 6 frames of the fall loop
      [2633ff]; that is skipped when the fall follows an action.  The button lock after an action that ended in
      the air (30 ticks, 26407b) is shortened.
   3. Speed.  entity+0x1a8 is the game's own animation speed factor (Haste 2.0, Slow 0.5, Stop 0; hit windows,
      triggers and the weapon follow it).  It is set once per frame from the call at 220b1d, only while it holds
      exactly 1.0 or our own value, and put back when the action is over.  Attack lunges are speed * time, so their
      speed is raised by the same factor [21cf52] to keep their reach.
   4. Cast times.  KH2 (Sora, ground): Fire goes off at 0.37 s and the next action can start at 1.00 s; Blizzard
      0.27 / 0.60; Thunder 0.30 / 0.70; Cure goes off at 0.30.  BBS: Fire 0.33-0.43, Blizzard 0.33-0.43, Thunder
      0.33-0.50, Cure 0.60-0.73, and every spell is locked for 40 ticks (0.67 s) after it goes off.  The wind-up
      is played faster so that the release frame (PAtk t1) comes at KH2's time, never slower than the original,
      and the lock after the release gets KH2's length per family.
   5. Per command ([CommandSpeed] in the ini, "<command id in hex>=<speed>").  A command listed there is played at
      that speed from start to end - whatever state the player is in for it (attack, magic, item, D-Link,
      finisher, shotlock, movement, guard, counter, reaction), as long as the game says a command is running
      (pl+0x5e0 != 0, its id in pl+0x312).  It stands in for every rule above for that command; anything not
      listed keeps them.  The ticks-based lock after a spell's release (FireFree ...) is not an animation and
      stays.  Haste / Slow / Stop still win, as they do over Actions. */
#include "core.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "steals_gen.h"
#include "mod.h"

extern char g_ini[MAX_PATH + 32];
extern int g_debug;

static int   c_on = 1, c_walk = 1, c_air = 1, c_lunge = 1;
static float c_actions = 1.15f;         /* animation speed of attacks, commands, items, spell recovery */
static float c_margin = 3.0f;           /* frames of follow-through after the last hit window */
static float c_airlock = 6.0f;          /* ticks without buttons after an action that ended in the air (game: 30) */
/* KH2's release times in seconds (0 = leave the spell alone) and ticks from release to "free" (game: 40) */
static float c_rel_fire = 0.37f, c_rel_blizzard = 0.27f, c_rel_thunder = 0.30f, c_rel_cure = 0.30f;
static float c_free_fire = 38.0f, c_free_blizzard = 20.0f, c_free_thunder = 24.0f;
static float c_cast_max = 2.5f;
#define CMD_IDS 0x240
static float g_cmd_speed[CMD_IDS];      /* [CommandSpeed]: speed of one command, 0 = not listed */
static float cmd_override(u8 *pl) {
    if (!*(u32*)(pl + 0x5e0)) return 0.0f;                      /* no command running */
    u16 id = *(u16*)(pl + 0x312);
    return id < CMD_IDS ? g_cmd_speed[id] : 0.0f;
}

#define STICK_MOVE 0.45f                /* the game's own "moving" threshold (0x646fb8) */
enum { K_NORMAL, K_DECK, K_ITEM, K_FRIEND, K_FINISH, K_MAGIC };

/* combo finishers of the Attack button: the PAtkData records the combo tables (PBA_*.bin, PAO_*.bin) use as their
   last hit (record type 2) */
static const u16 fin_idx[] = { 15, 20, 27, 32, 38, 44, 48, 53, 59, 65, 71, 77, 81, 86, 299, 306, 307, 312, 318, 323, 329,
    333, 339, 344, 348, 352, 363, 367, 372, 580, 581, 586, 591, 597, 602, 608, 613, 619, 624, 629, 633, 637, 644, 650,
    654, 660, 664, 864, 871, 878 };
static int is_finisher(const u8 *rec) {
    u8 *mgr = G(u8*, 0x10f9ee40); if (!mgr) return 0;
    u8 *base = *(u8**)(mgr + 0x128); if (!base || rec < base) return 0;
    size_t i = (size_t)(rec - base) / 0x28;
    for (unsigned k = 0; k < sizeof fin_idx / sizeof *fin_idx; k++) if (fin_idx[k] == i) return 1;
    return 0;
}

static float frame_of(u8 *pl) { u8 *mot = *(u8**)(pl + 0x78); return mot ? *(float*)(mot + 0x3c) : 0.0f; }
static int   finished(u8 *pl) { u8 *mot = *(u8**)(pl + 0x78); return !mot || (mot[8] & 1); }

/* Hit windows of one entity for the current animation.  Controller = *(entity+0x88): +0x20 count, +0x28 entries
   (0x90 each): +0x28 / +0x2a state of the first / second shape (0 not yet, 1 on, 2 on until the animation ends,
   3 over), +0x48 description: s16 +0x24 on, +0x26 off, +0x30 on of the second shape (-1 = none), +0x32 off. */
static void windows(u8 *ent, int *n, int *open, float *last) {
    if (!ent) return;
    u8 *ctl = *(u8**)(ent + 0x88); if (!ctl) return;
    int cnt = *(s32*)(ctl + 0x20); u8 *e = *(u8**)(ctl + 0x28);
    if (!e || cnt <= 0 || cnt > 64) return;
    for (int i = 0; i < cnt; i++, e += 0x90) {
        u8 *d = *(u8**)(e + 0x48); if (!d) continue;
        for (int k = 0; k < 2; k++) {
            if (k && *(s16*)(d + 0x30) < 0) break;
            s16 st = *(s16*)(e + 0x28 + 2 * k), off = *(s16*)(d + (k ? 0x32 : 0x26));
            (*n)++;
            if (st != 3) (*open)++;
            else if ((float)off > *last) *last = (float)off;
        }
    }
}
/* the frame from which the action is "done", or -1 while that cannot be said */
static int g_dbg_n; static float g_dbg_last;
static float done_frame(u8 *pl, const u8 *rec, int kind) {
    int n = 0, open = 0; float last = 0.0f;
    windows(pl, &n, &open, &last);
    windows(*(u8**)(pl + 0x378), &n, &open, &last);
    g_dbg_n = n; g_dbg_last = last;
    if (open) return -1.0f;
    float busy = last;
    if (rec[0x1f] > busy) busy = rec[0x1f];                      /* frMoveEnd */
    if (rec[0x1d] > busy) busy = rec[0x1d];                      /* frMarkEnd */
    for (int i = 0x14; i <= 0x17; i++) if (rec[i] && rec[i] + 1.0f > busy) busy = rec[i] + 1.0f;   /* triggers */
    float d;
    if (kind == K_ITEM) d = rec[0x14] + c_margin;                /* the item takes effect at t1 */
    else if (n) d = last + c_margin;
    else if (rec[0x0d]) d = rec[0x0d];                           /* nothing to hit with: frChangeEnable */
    else return -1.0f;
    return d > busy + 1.0f ? d : busy + 1.0f;
}

/* has the stick changed since this attack began? */
static struct { const u8 *rec; float last, yaw; u8 tilted, changed; } g_trk;
static void track(const u8 *rec, float fr, int tilted, float yaw) {
    if (rec != g_trk.rec || fr < 1.0f || fr < g_trk.last) {
        g_trk.rec = rec; g_trk.yaw = yaw; g_trk.tilted = (u8)tilted; g_trk.changed = 0;
    } else if (tilted != g_trk.tilted) g_trk.changed = 1;
    else if (tilted) {
        float d = yaw - g_trk.yaw;
        if (d > 3.14159265f) d -= 6.2831853f;
        if (d < -3.14159265f) d += 6.2831853f;
        if (d * d >= 1.0f) g_trk.changed = 1;
    }
    g_trk.last = fr;
}
/* can another press still continue this action (the next hit of a combo, or of a command that is pressed again)? */
static int can_follow(u8 *pl, const u8 *rec, int kind, float fr) {
    u8 cmb = rec[0x0c];
    if (kind == K_NORMAL) return !is_finisher(rec) && cmb && fr < cmb;
    if (kind == K_DECK) {
        u16 id = *(u16*)(pl + 0x312);
        if (id == 0x5c || id == 0x6f || id == 0x75) return !cmb || fr < cmb;
    }
    return 0;
}

/* Set by the hooks below when the action whose end test they sit on is done and nothing can follow; read by the
   gravity hook, which the same state function calls later in the frame; cleared once per frame (tick_hook). */
static u8 *g_fall;

/* the common part of the end tests: 1 = end the action now */
static int leave(u8 *pl, int kind) {
    const u8 *rec = *(u8**)(pl + 0x5b8);
    if (!rec || !*(u8**)(pl + 0x78)) return 0;
    float fr = frame_of(pl);
    int tilted = *(float*)(pl + 0x44c) >= STICK_MOVE;
    track(rec, fr, tilted, *(float*)(pl + 0x454));
    if (finished(pl)) return 0;
    if (*(u32*)(pl + 0x320) & 0x80) return 0;                    /* a command is queued */
    if (kind == K_FINISH && (*(u16*)(rec + 4) & 0x1000) && *(u16*)(pl + 0x422) == rec[8]) return 0;   /* a second animation follows */
    float d = done_frame(pl, rec, kind);
    if (d < 0.0f || fr < d) return 0;
    int follow = can_follow(pl, rec, kind, fr);
    if (!follow && !(*(u16*)(rec + 4) & 0x2000)) g_fall = pl;    /* (FLOAT attacks hover on purpose) */
    if (!c_walk || !tilted) return 0;
    if (*(u32*)(pl + 0x31c) & 0x2000) return 0;                  /* the game is ignoring the stick */
    if (follow && !g_trk.changed) return 0;
    if (g_debug) LOG("speed: walk-out, command %x record %d frame %.1f (done at %.0f; %d hit windows, last closes at %.0f; kind %d, %s)", *(u16*)(pl + 0x312),
                     (int)((rec - *(u8**)(G(u8*, 0x10f9ee40) + 0x128)) / 0x28), fr, d, g_dbg_n, g_dbg_last, kind, (*(u32*)(pl + 0x318) & 0xc00000) ? "air" : "ground");
    return 1;
}

static u64 MSABI h_attack(Ctx *c) { return leave((u8*)c->rbx, K_NORMAL) ? (u64)(g_base + 0x2291cd) : 0; }
static u64 MSABI h_deck(Ctx *c)   { return leave((u8*)c->rdi, K_DECK) ? (u64)(g_base + 0x2264d3) : 0; }
static u64 MSABI h_dash(Ctx *c)   { return leave((u8*)c->rdi, K_DECK) ? (u64)(g_base + 0x22540d) : 0; }
static u64 MSABI h_item(Ctx *c)   { return leave((u8*)c->rbx, K_ITEM) ? (u64)(g_base + 0x262064) : 0; }
static u64 MSABI h_friend(Ctx *c) { return leave((u8*)c->rbx, K_FRIEND) ? (u64)(g_base + 0x2599b9) : 0; }
static u64 MSABI h_finish(Ctx *c) { return leave((u8*)c->rbx, K_FINISH) ? (u64)(g_base + 0x2562c6) : 0; }

/* ---- magic ---- */
enum { F_NONE, F_FIRE, F_BLIZZARD, F_THUNDER, F_CURE };
static int family(u16 id) {
    if ((id >= 0x83 && id <= 0x87) || id == 0x89) return F_FIRE;
    if (id >= 0x8a && id <= 0x8c) return F_BLIZZARD;
    if (id >= 0x8e && id <= 0x91) return F_THUNDER;
    if (id >= 0x92 && id <= 0x95) return F_CURE;
    return F_NONE;
}
static int plain_spell(u8 *pl) {            /* an ordinary cast: magic command, not the Magnet family (its end resets the camera) */
    u16 m = *(u16*)(pl + 0x34c);
    return *(u32*)(pl + 0x5e0) == 2 && !(m >= 0x9c && m <= 0x9e);
}
/* 26825f: the cast animation has just finished, the lock is still running: the game would stand in the idle pose
   (or hang in the fall loop) until it is out */
static u64 MSABI h_mag_anim(Ctx *c) {
    u8 *pl = (u8*)c->rbx;
    if (!c_walk || !plain_spell(pl) || *(float*)(pl + 0x1a0) > 40.0f) return 0;
    return (u64)(g_base + 0x26829a);
}
/* 26828a: every frame of the recovery in which nothing is queued, just before "lock <= 0 ?" */
static u64 MSABI h_mag_lock(Ctx *c) {
    u8 *pl = (u8*)c->rbx; const u8 *rec = *(u8**)(pl + 0x5b8);
    float t = *(float*)(pl + 0x1a0);
    if (!rec || !plain_spell(pl) || t > 40.0f || t <= 0.0f) return 0;
    float fr = frame_of(pl);
    int late = rec[0x0d] && fr >= rec[0x0d];                     /* past frChangeEnable */
    if (*(u16*)(pl + 0x422) == 10 || (late && !(*(u32*)(pl + 0x320) & 0x80) && !(*(u16*)(rec + 4) & 0x2000))) g_fall = pl;
    if (c_on) {                                                  /* KH2's time from release to the next action */
        int f = family(*(u16*)(pl + 0x312));
        float fre = f == F_FIRE ? c_free_fire : f == F_BLIZZARD ? c_free_blizzard : f == F_THUNDER ? c_free_thunder : 40.0f;
        if (fre < 40.0f && 40.0f - t >= fre && !(*(u32*)(pl + 0x320) & 0x80)) return (u64)(g_base + 0x26829a);
    }
    if (!c_walk || finished(pl)) return 0;
    if (*(u32*)(pl + 0x320) & 0x80) return 0;
    if (*(u32*)(pl + 0x31c) & 0x2000) return 0;
    if (*(u16*)(pl + 0x422) == *(u16*)(pl + 0x428)) return 0;    /* already waiting in the idle pose */
    if (*(u16*)(rec + 4) & 0x2000) return 0;                     /* FLOAT */
    if (*(float*)(pl + 0x44c) < STICK_MOVE || !late) return 0;
    float d = done_frame(pl, rec, K_MAGIC);
    if (d < 0.0f || fr < d) return 0;
    if (g_debug) LOG("speed: walk-out of spell %x at frame %.1f", *(u16*)(pl + 0x312), fr);
    return (u64)(g_base + 0x26829a);
}

/* ---- air ---- */
/* 21cb55 in the vertical integrator 21cb00: rcx = player, eax = copy of pl+0x318 (bit 23 = aerial action: no sinking
   before frMoveEnd, 16 % gravity), r9d = mode (0 = plain gravity, 0x2000 = FLOAT record) */
static u64 MSABI h_gravity(Ctx *c) {
    u32 mode = (u32)c->r9;
    if (c_air && mode && !(mode & 0x2000) && (c->rax & 0x800000) && g_fall == (u8*)c->rcx) {
        static u8 *last_rec;                                     /* (debug: one line per action) */
        u8 *pl = (u8*)c->rcx;
        if (g_debug && *(u8**)(pl + 0x5b8) != last_rec) { last_rec = *(u8**)(pl + 0x5b8); LOG("speed: falling out of command %x at frame %.1f", *(u16*)(pl + 0x312), frame_of(pl)); }
        c->rax &= ~(u64)0x800000;
    }
    return (u64)(g_base + (mode ? 0x21cb5a : 0x21cb72));
}
/* 2633ff in the fall state: "fall loop frame >= 6 ?" (rdi = player); before that vy is held */
static u64 MSABI h_hover(Ctx *c) {
    u16 prev = *(u16*)((u8*)c->rdi + 0x308);
    return c_air && prev >= 0x10 && prev <= 0x14 ? (u64)(g_base + 0x26357c) : 0;
}

/* ---- speed ---- */
static float g_mine;                    /* the factor this module has put into pl+0x1a8 (0 = none) */
static float *g_lunge_div;              /* divisor of the lunge speed, 60 / factor */
static float wanted(u8 *pl) {
    float o = cmd_override(pl);
    if (o > 0.0f) return o;
    switch (*(u16*)(pl + 0x304)) {
    case 0x10: case 0x12: case 0x13: return c_actions;
    case 0x14: return *(u32*)(pl + 0x5e0) == 7 ? c_actions : 1.0f;
    case 0x11: {
        if (*(s16*)(pl + 0x310) == 4) return c_actions;          /* recovery */
        const u8 *rec = *(u8**)(pl + 0x5b8);
        int f = family(*(u16*)(pl + 0x312));
        float rel = f == F_FIRE ? c_rel_fire : f == F_BLIZZARD ? c_rel_blizzard : f == F_THUNDER ? c_rel_thunder : f == F_CURE ? c_rel_cure : -1.0f;
        if (rel < 0.0f || !rec) return c_actions;                /* no KH2 counterpart */
        if (rel == 0.0f || !rec[0x14]) return 1.0f;
        float k = rec[0x14] / 30.0f / rel;                       /* t1 is in frames of 1/30 s */
        return k < 1.0f ? 1.0f : k > c_cast_max ? c_cast_max : k;
    }
    }
    return 1.0f;
}
static void put(u8 *pl, float f) {       /* what the game's setter 21de30 does */
    *(float*)(pl + 0x1a8) = f;
    u8 *w = *(u8**)(pl + 0x378); if (w) *(float*)(w + 0x1a8) = f;
    w = *(u8**)(pl + 0x380);     if (w) *(float*)(w + 0x1a8) = f;
}
static float distinct(float f) { return f == 0.5f || f == 1.2f || f == 2.0f ? f + 0.002f : f; }   /* the game's own values */
static void speed_frame(u8 *pl) {
    float cur = *(float*)(pl + 0x1a8);
    if (g_mine != 0.0f && cur != g_mine) g_mine = 0.0f;          /* the game has set its own value meanwhile */
    float f = distinct(wanted(pl));
    if (f != 1.0f && (cur == 1.0f || g_mine != 0.0f)) {
        if (g_debug && f != g_mine) LOG("speed: x%.2f (state %x, command %x, part %d)", f, *(u16*)(pl + 0x304), *(u16*)(pl + 0x312), *(s16*)(pl + 0x310));
        put(pl, f); g_mine = f;
    }
    else if (f == 1.0f && g_mine != 0.0f) { put(pl, 1.0f); g_mine = 0.0f; }
    if (g_lunge_div) *g_lunge_div = 60.0f / (g_mine != 0.0f ? g_mine : 1.0f);
}
/* 220b1d: call 23b3a0 (status effects) in the player update, after this frame's state function and before the
   animation step */
static void MSABI tick_hook(u8 *pl) {
    if (c_on) speed_frame(pl);
    g_fall = NULL;
    FN(void, 0x23b3a0, u8*)(pl);
}

/* ---- install ---- */
static float ini_f(const char *key, float def) {
    char b[64], d[64];
    snprintf(d, sizeof d, "%g", def);
    GetPrivateProfileStringA("Speed", key, d, b, sizeof b, g_ini);
    return (float)atof(b);
}
int speed_enabled(void) { return c_on; }
static const Steal *const sites[] = { &S_atk_end, &S_mag_anim, &S_mag_lock, &S_deck_a, &S_deck_b, &S_deck_c, &S_item_end,
                                      &S_friend_end, &S_fin_end, &S_fall_hover, &S_gravity };
int speed_check(void) {
    c_on = (int)ini_f("Enabled", 1);
    c_walk = (int)ini_f("WalkOut", 1); c_air = (int)ini_f("AirWeight", 1); c_lunge = (int)ini_f("LungeFix", 1);
    c_actions = ini_f("Actions", c_actions); c_margin = ini_f("FollowThrough", c_margin); c_airlock = ini_f("AirLock", c_airlock);
    c_rel_fire = ini_f("FireRelease", c_rel_fire); c_rel_blizzard = ini_f("BlizzardRelease", c_rel_blizzard);
    c_rel_thunder = ini_f("ThunderRelease", c_rel_thunder); c_rel_cure = ini_f("CureRelease", c_rel_cure);
    c_free_fire = ini_f("FireFree", c_free_fire); c_free_blizzard = ini_f("BlizzardFree", c_free_blizzard);
    c_free_thunder = ini_f("ThunderFree", c_free_thunder);
    int listed = 0;
    for (int id = 1; id < CMD_IDS; id++) {
        char key[8], b[32]; snprintf(key, sizeof key, "%x", id); b[0] = 0;
        GetPrivateProfileStringA("CommandSpeed", key, "", b, sizeof b, g_ini);
        float v = b[0] ? (float)atof(b) : 0.0f;
        if (v > 0.0f && v < 0.25f) v = 0.25f;
        if (v > 4.0f) v = 4.0f;
        g_cmd_speed[id] = v > 0.0f ? v : 0.0f;
        if (g_cmd_speed[id] > 0.0f) listed++;
    }
    if (listed) LOG("speed: %d commands with a speed of their own", listed);
    if (c_actions < 0.5f) c_actions = 0.5f;
    if (c_actions > 2.0f) c_actions = 2.0f;
    if (c_airlock < 0.0f) c_airlock = 0.0f;
    if (!c_on) return 1;
    int bad = 0;
    for (unsigned i = 0; i < sizeof sites / sizeof *sites; i++) {
        const Steal *s = sites[i];
        if (memcmp(g_base + s->rva, s->bytes, s->nfix ? s->fix[0] : s->len) == 0) continue;
        if (bundle_combo_in_exe() && (s == &S_atk_end || s == &S_mag_anim || s == &S_mag_lock)) continue;   /* put back in speed_apply */
        LOG("speed: site %x does not match", s->rva); bad++;
    }
    u8 *p = g_base + 0x220b1d; s32 d; memcpy(&d, p + 1, 4);
    if (p[0] != 0xE8 || (u32)(0x220b1d + 5 + d) != 0x23b3a0) { LOG("speed: call site 220b1d does not match"); bad++; }
    if (G(u32, 0x26407b) != 0x41f00000) { LOG("speed: 26407b does not match"); bad++; }
    static const u8 lunge[] = { 0x41, 0x0f, 0x28, 0xf2, 0xf3, 0x0f, 0x5e, 0x35 };
    if (memcmp(g_base + 0x21cf4e, lunge, sizeof lunge)) { LOG("speed: 21cf4e does not match"); bad++; }
    return bad == 0;
}
void speed_apply(void) {
    if (!c_on) { LOG("speed: off"); return; }
    bundle_restore_sites();                                      /* the old Combo Flow exe patch sits on three of the sites */
    hook_ctx(&S_atk_end, h_attack, "attack end");
    hook_ctx(&S_deck_a, h_deck, "deck attack end");
    hook_ctx(&S_deck_b, h_deck, "deck attack end 2");
    hook_ctx(&S_deck_c, h_dash, "dash attack end");
    hook_ctx(&S_item_end, h_item, "item end");
    hook_ctx(&S_friend_end, h_friend, "friend command end");
    hook_ctx(&S_fin_end, h_finish, "finisher end");
    hook_ctx(&S_mag_anim, h_mag_anim, "magic animation end");
    hook_ctx(&S_mag_lock, h_mag_lock, "magic lock");
    hook_ctx(&S_gravity, h_gravity, "air gravity");
    hook_ctx(&S_fall_hover, h_hover, "fall hover");
    hook_call(0x220b1d, 0x23b3a0, tick_hook, "player tick");
    if (c_air && c_airlock != 30.0f) { u32 v; memcpy(&v, &c_airlock, 4); patch_u32(0x26407b, 0x41f00000, v, "air lock"); }
    if (c_lunge) {
        g_lunge_div = near_alloc(4);
        if (g_lunge_div) { *g_lunge_div = 60.0f; patch_riprel(0x21cf56, 0, 0x6ec5a4, g_lunge_div, "lunge speed"); }
    }
    LOG("speed: actions x%.2f, walk-out %d, air %d (lock %.0f), release fire %.2f blizzard %.2f thunder %.2f cure %.2f",
        c_actions, c_walk, c_air, c_airlock, c_rel_fire, c_rel_blizzard, c_rel_thunder, c_rel_cure);
}

#ifndef _WIN32      /* offline test access */
float test_speed_wanted(u8 *pl) { return wanted(pl); }
float *test_cmd_speed(void) { return g_cmd_speed; }
void  test_speed_frame(u8 *pl) { speed_frame(pl); }
float test_done_frame(u8 *pl, const u8 *rec, int kind) { return done_frame(pl, rec, kind); }
int   test_leave(u8 *pl, int kind) { return leave(pl, kind); }
u8   *test_fall(void) { return g_fall; }
void  test_fall_clear(void) { g_fall = NULL; }
u64   test_h_gravity(Ctx *c) { return h_gravity(c); }
u64   test_h_mag_lock(Ctx *c) { return h_mag_lock(c); }
u64   test_h_mag_anim(Ctx *c) { return h_mag_anim(c); }
u64   test_h_hover(Ctx *c) { return h_hover(c); }
float test_lunge_div(void) { return g_lunge_div ? *g_lunge_div : 0.0f; }
#endif

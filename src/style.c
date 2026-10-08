/* Command Styles as an offer instead of an automatic change.

   The game (cmd update 2363d0, style logic) decides at a full command gauge:
       2368e9  cmp eax,2 / jge 236a82            level 2 already: finisher
       2368f2  cmp word [rbx+0x190],bp(0x151)    next-style candidate
       2368f9  je 236a82                         none: finisher becomes available
       2368ff  ...                               else the style change starts at once
   rbx = cmd, eax = style level, ecx = cmd+0x64.  The hook sits on the compare at 2368f2.  With a candidate it holds
   the game at this point - jump to 236b49, the end of the style logic, with the "gauge full" flag (+0x64 bit 0x100)
   left set, which also stops the gauge from decaying - and shows a prompt with a timer:
     - the style button (the game's old deck-command button, pad test 272c20) -> fall through: the game's own change
     - timer out -> 236a82: the finisher becomes available, exactly as when there is no candidate
   Sections where the game forces a style (+0x64 bit 0x40000) are left as they are.

   Finishers get the same treatment.  When the game makes the finisher available (cmd+0x60 bit 0x20000; plate +0x1d0)
   it turns the Attack row into the finisher: 233f70 fires it on the confirm button [234115], and the Attack section
   of the update is skipped [236d91 test / 236d98 jne].  Here:
     - 236d98 is removed and the Attack plates are taken out of their "covered" state, so Attack keeps working
     - the confirm test at 234115 becomes the style button's test, so that button fires the finisher
     - the call to 233f70 at 236d67 runs a timer first; when it is out the finisher is dropped by letting the game's
       own "the finisher is over" branch of 233f70 run (flag 0x4000 set, 0x8000 clear).  That branch has two ends:
       with cmd+0x64 bit 0x20 ("revert after a finisher", set for good in the constructor) and no D-Link it goes
       back to the normal style and an empty gauge [2371d0(cmd, 0), 237f20(cmd, 4)]; without the bit it keeps the
       style level and puts the gauge at the bottom of that level [2371d0(cmd, level)], which is what the game
       does for a D-Link's finisher.  A finisher that was only offered and not taken must not cost the style the
       player is in, so in a style (level 1 or 2) the bit is taken away for that one call: the style stays, the
       gauge is where it was when the style began.  In the normal style nothing changes: the gauge empties.

   The prompt is an instance of the plate layout (bc01_00 layout 7) with the style plate's look (seq 0x197 on node
   0x5a: control 0 steady, 4 the "used" flash), the style's name (node 0) and the style button's icon (the text of
   node 0x5a: character code f5 'g'); under it a small bar that runs down.  A finisher offer uses the finisher plate's
   layout instead (layout 9, main node 1, steady in control 0). */
#include "core.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "steals_gen.h"
#include "mod.h"

extern char g_ini[MAX_PATH + 32];
extern int g_debug;

#define L2D_CreateLayout(file, id, ctl)   FN(int, 0x1a4ec0, int, u16, int, void*)(file, id, ctl, NULL)
#define L2D_Destroy(h)                    FN(void, 0x1a57f0, int)(h)
#define L2D_Show(h, on)                   FN(int, 0x1a5b30, int, int)(h, on)
#define L2D_GetControl(h)                 FN(int, 0x1a60e0, int)(h)          /* 8 = dead handle */
#define L2D_SetControl(h, c)              FN(int, 0x1a7520, int, int)(h, c)
#define L2D_SetPos(h, x, y)               FN(int, 0x1a7660, int, float, float)(h, x, y)
#define L2D_SetPriority(h, p)             FN(int, 0x1a7600, int, s16, int)(h, p, 0)
#define L2D_SetNodeText(h, n, s)          FN(int, 0x1a8330, int, u16, const char*, int, void*)(h, n, s, 0, NULL)
#define L2D_ReplaceNodeSeq(h, n, sq, id, c) FN(int, 0x1a7fb0, int, u16, int, u16, int)(h, n, sq, id, c)

/* draw priority of the offer and its timer bar: under the Magic / Items / D-Link lists and their headers (12, the row
   under the cursor 13), which can grow tall enough to reach it, and over the command menu (5 / 7) */
#define OFFER_PRIO 11
static int   c_on = 1;
static float c_seconds = 6.0f;          /* how long the offer stands */
static int   c_keep = 1;                /* an offer that runs out leaves the player in the style he is in */
static u32   c_bar_col = 0x8020a0ff;    /* timer bar tint: orange (style) */
static u32   c_fin_col = 0x80ffc020;    /* ... light blue (finisher) */

static int   g_active;                  /* an offer is up */
static u8   *g_cmd;                     /* ... on this command object */
static float g_left, g_total;           /* ticks (60 per second) */
static int   g_cand;                    /* the style offered (command id) */
static float g_flash;                   /* ticks left of the "accepted" flash */
static int   g_plate, g_plate_cand, g_plate_kind, g_bar;
static int   g_fin;                     /* a finisher offer is up */
static u8   *g_fin_cmd;
static float g_fin_left, g_fin_total;

static float ini_f(const char *key, float def) {
    char b[64], d[64]; snprintf(d, sizeof d, "%g", def);
    GetPrivateProfileStringA("Style", key, d, b, sizeof b, g_ini);
    return (float)atof(b);
}

int style_prompt_active(void) { return g_active || g_fin; }
int style_prompt_shown(void) { return c_on && (g_active || g_fin || g_flash > 0); }     /* the offer's plate is on screen */

static u64 MSABI style_decide(Ctx *c) {
    u8 *cmd = (u8*)c->rbx;
    if (!c_on || !cmd) return 0;
    int cand = *(u16*)(cmd + 0x190);
    if (cand == 0x151) return 0;                                /* no candidate: the game's finisher branch */
    if (*(u32*)(cmd + 0x64) & 0x40000) return 0;                /* forced-style section */
    if (!g_active || g_cmd != cmd) {
        g_active = 1; g_cmd = cmd; g_total = g_left = c_seconds * 60.0f; g_flash = 0;
        if (g_debug) LOG("style: %x offered", cand);
    }
    g_cand = cand;                                              /* the candidate can still change while the offer is up */
    u8 *mgr = *(u8**)(cmd + 0x68);
    /* accepted - unless a context prompt is up: triangle answers that one first (menu.c) */
    if (mgr && !menu_react_prompt(cmd) && FN(int, 0x272c20, u8*)(mgr + 0x58)) {
        g_active = 0; g_flash = 30.0f;
        if (g_debug) LOG("style: %x accepted", cand);
        return 0;
    }
    float dt = *(float*)(cmd + 0x20);
    if (!(dt >= 0) || dt > 6) dt = 1;
    g_left -= dt;
    if (g_left <= 0) {                                          /* declined */
        g_active = 0;
        if (g_debug) LOG("style: %x not taken", cand);
        return (u64)(g_base + 0x236a82);
    }
    return (u64)(g_base + 0x236b49);
}

/* 234115: the button that fires an available finisher */
static int MSABI finisher_pad(u8 *pad) { return FN(int, 0x272c20, u8*)(pad); }

/* 236d67: the call to 233f70 (finisher button and finisher end) in the input part of the cmd update */
static void MSABI finisher_hook(u8 *cmd) {
    u32 f60 = *(u32*)(cmd + 0x60);
    if ((f60 & 0x24000) == 0x20000 && *(u8**)(cmd + 0x1d0)) {       /* available, not fired */
        if (!g_fin || g_fin_cmd != cmd) {
            g_fin = 1; g_fin_cmd = cmd; g_fin_total = g_fin_left = c_seconds * 60.0f;
            /* the game "covered" the Attack plates for the finisher (2058c0 / 205a10): back to their ready state,
               as it does itself when a prompt closes (205890 at level 0, 205910 above) */
            int lv = *(int*)(cmd + 0x1a8);
            u8 *P = (lv >= 0 && lv <= 2) ? *(u8**)(cmd + 0x1b0 + lv * 8) : NULL;
            if (P && !(*(u32*)(P + 0x60) & 0x80000000u)) FN(void, lv == 0 ? 0x205890 : 0x205910, u8*)(P);
            if (g_debug) LOG("finisher: %x offered", *(u16*)*(u8**)(*(u8**)(cmd + 0x1d0) + 0x58));
        } else {
            float dt = *(float*)(cmd + 0x20);
            if (!(dt >= 0) || dt > 6) dt = 1;
            g_fin_left -= dt;
            if (g_fin_left <= 0 && *(s16*)(cmd + 0x80) == 0) {      /* not taken: the game's "finisher is over" branch */
                u32 keep = f60 & 0x8000;
                u32 f64 = *(u32*)(cmd + 0x64);
                int stay = c_keep && *(int*)(cmd + 0x1a8) > 0 && (f64 & 0x20);     /* in a style: it is not lost */
                if (stay) *(u32*)(cmd + 0x64) = f64 & ~0x20u;
                *(u32*)(cmd + 0x60) = (f60 | 0x4000) & ~0x8000u;
                FN(void, 0x233f70, u8*)(cmd);
                *(u32*)(cmd + 0x60) |= keep;
                if (stay) *(u32*)(cmd + 0x64) |= 0x20;
                g_fin = 0;
                if (g_debug) LOG("finisher: not taken%s", stay ? ", style kept" : "");
                return;
            }
        }
    } else if (g_fin && g_fin_cmd == cmd) {
        g_fin = 0;
    }
    FN(void, 0x233f70, u8*)(cmd);
    if (g_fin && (*(u32*)(cmd + 0x60) & 0x4000)) { g_fin = 0; if (g_debug) LOG("finisher: fired"); }
}

/* what the game writes on its finisher plate [206400]: the player's own name for the finisher (PlayerData+0x1b0) if
   there is one and the command is not 0x11, else the command's name */
static const char *finisher_name(u8 *cmd) {
    u8 *P = *(u8**)(cmd + 0x1d0);
    u16 *k = P ? *(u16**)(P + 0x58) : NULL;
    int id = k ? k[0] : 0;
    const char *own = (const char*)(g_base + 0x10f9eeb0 + 0x1b0);
    if (own[0] && id != 0x11 && !(P && (*(u32*)(P + 0x60) & 0x400))) return own;
    const char *g = (id > 0 && id < 0x23a) ? G(const char*, 0x814908 + (u32)id * 0x18) : NULL;
    return (g && g[0]) ? g : "Finish";
}

/* the game's name for the style if it has one, else the English one */
static const char *style_name(int id) {
    static const char *en[15] = { "Critical Impact", "Spellweaver", "Fever Pitch", "Firestorm", "Diamond Dust", "Thunderbolt", "Cyclone",
        "Bladecharge", "Sky Climber", "Rockbreaker", "Ghost Drive", "Rhythm Mixer", "Dark Impulse", "Wingblade", "Frozen Fortune" };
    const char *g = (id > 0 && id < 0x23a) ? G(const char*, 0x814908 + (u32)id * 0x18) : NULL;
    if (g && g[0]) return g;
    return (id >= 0x152 && id <= 0x160) ? en[id - 0x152] : "Style";
}
static void plate_destroy(void) { if (g_plate > 0) L2D_Destroy(g_plate); g_plate = 0; g_plate_cand = 0; }
static int plate_alive(void) { l2d_live(&g_plate); return g_plate > 0 && L2D_GetControl(g_plate) != 8; }
/* the offer plate and its timer go whatever the settings (the HUD is being torn down) */
void style_shutdown(void) { plate_destroy(); hud_timer_bar(&g_bar, 0, 0, 0, 0, -1, 0, 0, 0); g_active = 0; g_fin = 0; g_flash = 0; }

/* once per frame; (x, y) = where a prompt plate goes */
void style_frame(u8 *cmd, int hud, float x, float y) {
    if (!c_on) return;
    u32 f60 = cmd ? *(u32*)(cmd + 0x60) : 0, f64 = cmd ? *(u32*)(cmd + 0x64) : 0;
    /* an offer ends without its hook when the situation it was made in is gone (new room, D-Link, deck loss ...) */
    if (g_active && (!cmd || cmd != g_cmd || !(f64 & 0x100) || (f64 & 0x82) || (f60 & 0x20000) ||
                     *(int*)(cmd + 0x1a8) >= 2 || *(u16*)(cmd + 0x190) == 0x151)) g_active = 0;
    if (g_fin && (!cmd || cmd != g_fin_cmd || (f60 & 0x24000) != 0x20000 || !*(u8**)(cmd + 0x1d0))) g_fin = 0;
    if (g_flash > 0) {
        float dt = cmd ? *(float*)(cmd + 0x20) : 1;
        if (!(dt >= 0) || dt > 6) dt = 1;
        g_flash -= dt;
    }
    int kind = g_active ? 1 : g_fin ? 2 : g_flash > 0 ? 1 : 0;         /* 1 style, 2 finisher */
    if (!kind || !cmd) {
        plate_destroy();
        hud_timer_bar(&g_bar, 0, 0, 0, 0, -1, 0, 0, 0);
        return;
    }
    if (plate_alive() && g_plate_kind != kind) plate_destroy();
    if (!plate_alive()) {
        int file = G(int, 0x10f9ed48), sq = G(int, 0x10f9ed4c);     /* bc01_00.l2d and its sprite block */
        G(u8, 0x8f88028) = 0; G(u8, 0x8f8802a) = 0;                 /* created hidden, HUD group */
        g_plate = L2D_CreateLayout(file, kind == 1 ? 7 : 9, 0); g_plate_cand = 0; g_plate_kind = kind;
        if (g_plate <= 0) { g_plate = 0; return; }
        if (kind == 1) L2D_ReplaceNodeSeq(g_plate, 0x5a, sq, 0x197, 0);
        L2D_SetPriority(g_plate, OFFER_PRIO);
        L2D_SetNodeText(g_plate, kind == 1 ? 0x5a : 1, "\xf5g");    /* the style button's icon */
    }
    int h = g_plate;
    if (kind == 1) {
        if (g_plate_cand != g_cand) {
            g_plate_cand = g_cand;
            L2D_SetNodeText(h, 0, style_name(g_cand));
            if (g_debug) LOG("style: prompt shows %x '%s'", g_cand, style_name(g_cand));
        }
        if (!g_active) { if (L2D_GetControl(h) != 4) L2D_SetControl(h, 4); }       /* accepted: flash, then gone */
    } else {
        u16 *k = *(u16**)(*(u8**)(cmd + 0x1d0) + 0x58);
        int id = k ? k[0] : 0;
        if (g_plate_cand != id + 1) {
            g_plate_cand = id + 1;
            L2D_SetNodeText(h, 0, finisher_name(cmd));
            if (g_debug) LOG("finisher: prompt shows %x '%s'", id, finisher_name(cmd));
        }
    }
    L2D_SetPos(h, x, y);
    L2D_Show(h, hud);
    /* the plate's inside spans about x+16 .. x+100; the bar hangs just under the plate */
    float frac = g_active ? (g_total > 0 ? g_left / g_total : 0) : g_fin ? (g_fin_total > 0 ? g_fin_left / g_fin_total : 0) : -1;
    if (frac >= 0) hud_timer_bar(&g_bar, x + 16.0f, y + 16.6f, 84.0f, 2.6f, frac, kind == 1 ? c_bar_col : c_fin_col, OFFER_PRIO, hud);
    else hud_timer_bar(&g_bar, 0, 0, 0, 0, -1, 0, 0, 0);
}

static int call_ok(u32 rva, u32 target) {
    u8 *p = g_base + rva; s32 d; memcpy(&d, p + 1, 4);
    return p[0] == 0xE8 && (u32)(rva + 5 + d) == target;
}
static const u8 jne_attack[6] = { 0x0f, 0x85, 0xf2, 0x00, 0x00, 0x00 };     /* 236d98: jne 236e90 */
int style_check(void) {
    c_on = (int)ini_f("Prompt", (float)c_on);
    c_seconds = ini_f("Seconds", c_seconds);
    c_keep = (int)ini_f("KeepStyle", (float)c_keep);
    if (c_seconds < 0.5f) c_seconds = 0.5f;
    if (!c_on) return 1;
    if (memcmp(g_base + S_style_decide.rva, S_style_decide.bytes, S_style_decide.len) != 0) { LOG("style: site 2368f2 does not match"); return 0; }
    if (!call_ok(0x236d67, 0x233f70) || !call_ok(0x234115, 0x272860) || memcmp(g_base + 0x236d98, jne_attack, 6) != 0) {
        LOG("style: finisher sites do not match"); return 0;
    }
    return 1;
}
void style_apply(void) {
    if (!c_on) { LOG("style prompt: off"); return; }
    static const u8 nops[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    hook_ctx(&S_style_decide, style_decide, "style_decide");
    hook_call(0x236d67, 0x233f70, finisher_hook, "finisher");
    hook_call(0x234115, 0x272860, finisher_pad, "finisher pad");
    patch_bytes(0x236d98, jne_attack, nops, 6, "attack with a finisher available");
}

void style_own(char *out, int n) { own_add(out, n, "offer", g_plate); own_add(out, n, "offertimer", g_bar); }
#ifndef _WIN32      /* offline test access */
u64 test_style_decide(Ctx *c) { return style_decide(c); }
int *test_style_active(void) { return &g_active; }
float *test_style_left(void) { return &g_left; }
int *test_fin_active(void) { return &g_fin; }
float *test_fin_left(void) { return &g_fin_left; }
void test_finisher_hook(u8 *cmd) { finisher_hook(cmd); }
int *test_style_keep(void) { return &c_keep; }
#endif

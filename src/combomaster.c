/* Combo Master: a new ability of its own, after the other KH games' - the Attack combo goes on when its hits miss.

   The ability.  The game's abilities are command ids 0x1c4..0x1e1, exactly the 30 its tables hold (the save keeps a
   u32 per ability at save+0x18cc, the player a count per slot at pl+0x4a3; FUN_1402218b0 / 14041d810).  So this one
   is not one of them: it is id 0x1c3 (ABILITY_KIND_None, no name, no description, used by nothing as a command) and
   lives only in the camp Abilities menu (CCampAbility), the one place an ability is seen and switched:
     - 3f1ce0 builds the list: entries of 0x10 bytes at menu+0xa8, the count at +0x94.  The game has room for
       exactly 30 - on Critical, with EXP Zero, all of it: menu+0x288 holds the 7 on-screen rows' positions (2 floats
       each).  Those are only ever reached through three `lea reg,[menu+0x288]` [3f2f60 setup, 3f2644 / 3f2657 cursor]
       (nothing else in the menu, its base class or the helpers it calls touches +0x288..+0x2bf), so they are moved to
       a buffer of this file's and the list holds 31.  Per entry
       +0 the ability's u32 (bits 0-2 in effect, 3-5 level, 6-8 copies learned, 9-13 which copies are on,
       14-15: 0 "???", 1 new, 2 seen, 3 known), +8 id, +0xa group (0 Prize, 1 Stats, 2 Support), +0xb first of its group,
       +0xc level shown, +0xd copies learned, +0xe copies possible, +0xf level.  It is called from three places
       [3f2979, 3f2f55, 3f39a6]; after each, when there is room, Combo Master is added at the end of Support, like
       EXP Zero: one copy,
       learned from the start, off until switched on.  Its u32 is this file's own.
     - the name: the command table's pointer (140814908 + id * 0x18), which the names file fills when it is loaded
       (mp.c's hook on that calls combomaster_texts after it).
     - the description: 14041d880, which answers only for 0x1c4..0x1e1 [called at 3f2711 and 3f3562].
     - switching: 14041d2e0(id, copy, on) [3f37f0], again only for 0x1c4..0x1e1; here it switches this one and
       keeps the choice in the save itself, like every other ability's: bit 23 of EXP Zero's u32
       (save 150fa3d08 + 0x18cc + 5 * 4).  That u32's other users mask their own fields - bits 0-20, and 14041de10
       hands out only bits 14-17 - so the bit is never read or cleared by the game, it goes into the save file with
       the rest, and a new game starts with it clear: off.  Each save (each character) has its own.
     - the detail page (confirm on an entry, 3f3659): it reads 30-entry tables by id, so for Combo Master the game's
       own "nothing to show" buzzer is taken instead.

   What it does.  In BBS the next hit of the normal Attack combo can only be asked for in the window FUN_14021c140
   opens, and for the Attack category that window needs pl+0x320 bit 0x20, "the attack has connected", which only
   the weapon's hit handler [293d9d] or a fired bullet [229021] sets.  A combo that whiffs has no window, so it ends.
   The same bit also stops the attack's forward lunge (the movement call at 228f39 takes "not connected"), so it is
   not simply set: for the window call of the normal combo (sub-state 5, at 22902e) the attack counts as connected
   from the end of its homing (PAtk frMark End, +0x1d - where the swing lands; melee records have no trigger frame),
   and the bit is put back after the call.  The next hit still starts no sooner than frChangeEnable (+0xd), as after
   a real hit. */
#include "core.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mod.h"

extern char g_ini[MAX_PATH + 32];
extern int g_debug;

#define CM_ID        0x1c3
#define NAME_PTR(id) G(const char*, 0x814908 + (u32)(id) * 0x18)
#define LIST_FN      0x3f1ce0u
static const u32 LIST_CALLS[3] = { 0x3f2979, 0x3f2f55, 0x3f39a6 };
#define HELP_FN      0x41d880u
static const u32 HELP_CALLS[2] = { 0x3f2711, 0x3f3562 };
#define SWITCH_FN    0x41d2e0u
#define SWITCH_CALL  0x3f37f0u
#define WIN_FN       0x21c140u
#define WIN_CALL     0x22902eu
#define LIST_GAME    30                 /* the game's room: menu+0xa8 .. +0x288, where the 7 rows' positions start */
#define REC_SEEN     0xc000u          /* 0x4000 new, 0x8000 just seen (dim NEW badge), 0xc000 known [41c5f0] */
#define REC_LEARNED  (1u << 6)          /* one copy */
#define REC_ON       (1u << 9)          /* copy 0 switched on */

static int c_enabled = 1;               /* [Combat] ComboMaster: the ability exists at all */
#define SAVE_FLAG    G(u32, 0x10fa3d08 + 0x18cc + 5 * 4)   /* EXP Zero's u32 in the save in memory [1403600f0] */
#define FLAG_ON      (1u << 23)
static char c_name[48], c_help[256];
static u32 g_rec;                       /* the ability's u32, as the menu reads it */
static float g_rowpos[7][2];            /* the menu's row positions, moved out of menu+0x288 */
static int g_rows_moved;                /* all three sites redirected: the list may hold 31 */

/* the row positions: lea rdx,[rbx+0x288] (rbx = menu + row * 8) twice in 3f24d0, lea rdi,[rsi+0x288] in the setup */
static const Steal S_cm_row_a = { 0x3f2644, 7, 0, {0}, {0}, {0x48,0x8d,0x93,0x88,0x02,0x00,0x00} };
static const Steal S_cm_row_b = { 0x3f2657, 7, 0, {0}, {0}, {0x48,0x8d,0x93,0x88,0x02,0x00,0x00} };
static const Steal S_cm_row_init = { 0x3f2f60, 7, 0, {0}, {0}, {0x48,0x8d,0xbe,0x88,0x02,0x00,0x00} };
static u64 row_index(Ctx *c) { u64 k = (c->rbx - c->rdi) / 8; return k < 7 ? k : 0; }
static u64 MSABI row_a(Ctx *c) { c->rdx = (u64)g_rowpos[row_index(c)]; return (u64)(g_base + 0x3f264b); }
static u64 MSABI row_b(Ctx *c) { c->rdx = (u64)g_rowpos[row_index(c)]; return (u64)(g_base + 0x3f265e); }
static u64 MSABI row_init(Ctx *c) { c->rdi = (u64)g_rowpos[0]; return (u64)(g_base + 0x3f2f67); }

/* the detail page is refused for it (3f3659: mov rax,[rbx+rax*8+0xa8]; then the "seen" test) */
static const Steal S_cm_detail = { 0x3f3659, 8, 0, {0}, {0}, {0x48,0x8b,0x84,0xc3,0xa8,0x00,0x00,0x00} };

static int is_on(void) { return (SAVE_FLAG & FLAG_ON) != 0; }
static void rec_update(void) { g_rec = REC_SEEN | REC_LEARNED | (is_on() ? REC_ON : 0); }
static int ini_get(const char *key, int def) {
    char b[32], d[32]; snprintf(d, sizeof d, "%d", def);
    GetPrivateProfileStringA("Combat", key, d, b, sizeof b, g_ini);
    return atoi(b);
}
static int ini_str(const char *key, char *out, size_t n) {
    char b[256];
    GetPrivateProfileStringA("Combat", key, "", b, sizeof b, g_ini);
    if (!b[0]) return 0;
    size_t k = 0;
    for (const char *p = b; *p && k + 1 < n; p++) {     /* \n in the ini is a line break */
        if (p[0] == '\\' && p[1] == 'n') { out[k++] = '\n'; p++; } else out[k++] = *p;
    }
    out[k] = 0; return 1;
}

/* ---- the Abilities menu ---- */
static void cm_append(u8 *menu) {
    if (!c_enabled) return;
    s16 *count = (s16*)(menu + 0x94);
    if (*count < 1 || *count >= LIST_GAME + (g_rows_moved ? 1 : 0)) return;
    for (int i = 0; i < *count; i++) if (*(u16*)(menu + 0xa8 + i * 0x10 + 8) == CM_ID) return;
    u8 *e = menu + 0xa8 + *count * 0x10, *last = e - 0x10;
    rec_update();
    memset(e, 0, 0x10);
    *(u32**)e = &g_rec;
    *(u16*)(e + 8) = CM_ID;
    e[0xa] = 2;                                         /* Support */
    e[0xb] = last[0xa] != 2;                            /* heads the group only if Support had nothing */
    e[0xd] = 1; e[0xe] = 1;                             /* one copy, learned; one possible */
    (*count)++;
}
static void MSABI list_hook(u8 *menu) {
    FN(void, LIST_FN, u8*)(menu);
    cm_append(menu);
}
static const char *MSABI help_hook(u8 *cmd) {
    if (c_enabled && cmd && *(u16*)(cmd + 6) == CM_ID) return c_help;
    return FN(const char*, HELP_FN, u8*)(cmd);
}
static void MSABI switch_hook(u16 id, u8 copy, s8 on) {
    if (id != CM_ID) { FN(void, SWITCH_FN, u16, u8, s8)(id, copy, on); return; }
    if (copy != 0) return;
    int now = on < 0 ? !is_on() : on != 0;
    if (now) SAVE_FLAG |= FLAG_ON; else SAVE_FLAG &= ~FLAG_ON;
    rec_update();
    LOG("combo master: switched %s", now ? "on" : "off");
}
static u64 MSABI detail_hook(Ctx *c) {
    u8 *menu = (u8*)c->rbx;
    s16 i = *(s16*)(menu + 0x8e);
    if (c_enabled && *(u16*)(menu + 0xa8 + i * 0x10 + 8) == CM_ID) return (u64)(g_base + 0x3f3669);   /* buzzer */
    return 0;
}
/* the names file is in memory (mp.c's hook on its "ready" call): the name goes to the command table */
void combomaster_texts(u8 *self) {
    if (c_enabled && *(u32*)(self + 0xb0) == 0xfa0000u) NAME_PTR(CM_ID) = c_name;
}
int combomaster_on(void) { return c_enabled && is_on(); }

/* ---- the effect: the normal combo's window ---- */
static u64 MSABI window_hook(u8 *pl, float frame) {
    u32 *flags = (u32*)(pl + 0x320);
    int lend = 0;
    if (combomaster_on() && !(*flags & 0x20) && *(int*)(pl + 0x5e0) == 1 && *(u16*)(pl + 0x310) == 5) {
        u8 *rec = *(u8**)(pl + 0x5b8);
        if (rec && frame >= (float)rec[0x1d]) lend = 1;
    }
    if (lend) *flags |= 0x20;
    u64 r = FN(u64, WIN_FN, u8*, float)(pl, frame);
    if (lend) *flags &= ~0x20u;
    return r;
}

static int call_ok(u32 rva, u32 target) {
    u8 *p = g_base + rva; s32 d; memcpy(&d, p + 1, 4);
    return p[0] == 0xE8 && (u32)(rva + 5 + d) == target;
}
int combomaster_check(void) {
    c_enabled = ini_get("ComboMaster", 1);
    if (!ini_str("ComboMasterName", c_name, sizeof c_name)) snprintf(c_name, sizeof c_name, "Combo Master");
    if (!ini_str("ComboMasterHelp", c_help, sizeof c_help))
        snprintf(c_help, sizeof c_help, "Lets you keep your Attack combo going even when your\nattacks miss.");
    rec_update();
    if (!c_enabled) return 1;
    int bad = 0;
    for (int i = 0; i < 3; i++) if (!call_ok(LIST_CALLS[i], LIST_FN)) { LOG("combo master: list call %x does not match", LIST_CALLS[i]); bad++; }
    for (int i = 0; i < 2; i++) if (!call_ok(HELP_CALLS[i], HELP_FN)) { LOG("combo master: help call %x does not match", HELP_CALLS[i]); bad++; }
    if (!call_ok(SWITCH_CALL, SWITCH_FN)) { LOG("combo master: switch call does not match"); bad++; }
    if (!call_ok(WIN_CALL, WIN_FN)) { LOG("combo master: combo window call does not match"); bad++; }
    if (memcmp(g_base + S_cm_detail.rva, S_cm_detail.bytes, S_cm_detail.len)) { LOG("combo master: detail site does not match"); bad++; }
    const Steal *rows[3] = { &S_cm_row_a, &S_cm_row_b, &S_cm_row_init };
    for (int i = 0; i < 3; i++) if (memcmp(g_base + rows[i]->rva, rows[i]->bytes, rows[i]->len)) { LOG("combo master: row site %x does not match", rows[i]->rva); bad++; }
    if (*(u32*)(g_base + 0x3f3661) != 0xc00000f7u) { LOG("combo master: detail test does not match"); bad++; }
    return bad == 0;
}
void combomaster_apply(void) {
    if (!c_enabled) { LOG("combo master: off"); return; }
    for (int i = 0; i < 3; i++) hook_call(LIST_CALLS[i], LIST_FN, list_hook, "abilities list (Combo Master)");
    for (int i = 0; i < 2; i++) hook_call(HELP_CALLS[i], HELP_FN, help_hook, "ability help (Combo Master)");
    hook_call(SWITCH_CALL, SWITCH_FN, switch_hook, "ability switch (Combo Master)");
    hook_ctx(&S_cm_detail, detail_hook, "ability detail (Combo Master)");
    g_rows_moved = hook_ctx(&S_cm_row_a, row_a, "ability rows a (Combo Master)")
                 & hook_ctx(&S_cm_row_b, row_b, "ability rows b (Combo Master)")
                 & hook_ctx(&S_cm_row_init, row_init, "ability rows setup (Combo Master)");
    hook_call(WIN_CALL, WIN_FN, window_hook, "combo window (Combo Master)");
    NAME_PTR(CM_ID) = c_name;
    LOG("combo master: in the Abilities menu (on or off per save, off at first)");
}

#ifndef _WIN32
int test_cm_on(void) { return is_on(); }
void test_cm_set(int on) { if (on) SAVE_FLAG |= FLAG_ON; else SAVE_FLAG &= ~FLAG_ON; }
u32 *test_cm_rec(void) { return &g_rec; }
const char *test_cm_help(void) { return c_help; }
void test_cm_append(u8 *menu) { cm_append(menu); }
float *test_cm_rowpos(void) { return g_rowpos[0]; }
u64 test_cm_row(int which, u8 *menu, int row) { Ctx c; memset(&c, 0, sizeof c); c.rdi = (u64)menu; c.rbx = (u64)menu + row * 8; c.rsi = (u64)menu;
    u64 r = which == 0 ? row_a(&c) : which == 1 ? row_b(&c) : row_init(&c); return which == 2 ? c.rdi : (r ? c.rdx : 0); }
u64 test_cm_detail(u8 *menu) { Ctx c; memset(&c, 0, sizeof c); c.rbx = (u64)menu; return detail_hook(&c); }
#endif

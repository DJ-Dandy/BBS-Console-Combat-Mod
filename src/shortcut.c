/* Shortcuts, as in KH2: hold L1 and press a face button to use the command in a chosen deck slot.

   A shortcut belongs to a deck SLOT (1..8), not to a command: change what is in the slot and the shortcut uses
   the new command.  The four shortcuts are kept per character in bbskh2_shortcuts.ini next to the game exe and
   are edited in the menu (sccamp.c).  This file: the bindings, and the controller side.

   Pad.  The game keeps the pad as bit words in the PS2 layout (0x400 L1, 0x800 R1, 0x1000 triangle, 0x2000
   circle, 0x4000 cross, 0x8000 square): [148f64930] held, [..34] edge, [..38] changed, [..3c] repeat, written once
   a frame by 1400ed040 (called from 1400e3716) - keyboard and mouse bindings arrive in the same words.  After that
   call, while L1 is held (and R1 is not: L1 + R1 is the shotlock) and the command menu is on screen, the four
   face buttons are taken out of those words, so nothing else in the game sees them, and a press is kept as "use
   shortcut n" for a few frames until the command menu (menu.c) can carry it out.

   Two sets.  There are two sets of four shortcuts; while the list is up, a press of the d-pad (any direction)
   flips to the other set, and the d-pad is kept from the game meanwhile, as the face buttons are.  The set shown
   stays as it was left for the next time L1 is held.  Set 2's buttons are saved as Circle2 .. Cross2.

   L1 alone.  In the default control type a press of L1 turns the camera behind the character (pad test 1402729c0,
   called at 22b5df) and, while locked on, changes the target (140272b80, called at 2650d4).  Both now happen on a
   short tap, when L1 is let go without a shortcut having been used. */
#include "mod.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern char g_dir[MAX_PATH], g_ini[MAX_PATH + 32];
extern int g_debug;

#define PAD_HELD  G(u32, 0x8f64930)
#define PAD_EDGE  G(u32, 0x8f64934)
#define PAD_CHG   G(u32, 0x8f64938)
#define PAD_REP   G(u32, 0x8f6493c)
#define PAD_OFF   G(u32, 0x8f6499c)
#define B_L1   0x400u
#define B_R1   0x800u
#define B_FACE 0xf000u
#define B_DPAD 0x00f0u                  /* up, right, down, left */
#define CMD     G(u8*, 0x10f9ed40)
#define GFLAGS  G(u32, 0x10f9ee48)
#define CHARA   G(u8, 0x10fa0880)        /* 0..2: whose save this is (the menu's name message is 0x300 + this) */

/* rows in KH2's order: circle, triangle, square, cross */
static const u32 row_bit[SC_ROWS] = { 0x2000, 0x1000, 0x8000, 0x4000 };
/* text codes of the button pictures (the game swaps in the keyboard's or the pad's own): f57b circle, f567
   triangle, f566 square, f57c cross */
static const u8 row_icon[SC_ROWS] = { 0x7b, 0x67, 0x66, 0x7c };
static const char *const row_key[SC_SETS][SC_ROWS] = { { "Circle", "Triangle", "Square", "Cross" },
                                                        { "Circle2", "Triangle2", "Square2", "Cross2" } };

static int c_on = 1;
static int c_tap = 18;                  /* frames: a press of L1 shorter than this is a tap */
static int c_buffer = 12;               /* frames a shortcut press waits for the character to be free */
static int c_sets = 2;                  /* 1 = one set, as before (the d-pad is then left alone) */
static s8  g_bind[3][SC_SETS][SC_ROWS]; /* deck slot 0..7, -1 = none */
static int g_page;                      /* the set shown in battle */
static char g_file[MAX_PATH + 32];

static int chara(void) { int c = CHARA; return c < 3 ? c : 0; }
int sc_enabled(void) { return c_on; }
u32 sc_row_mask(int row) { return row_bit[row & 3]; }
const char *sc_row_icon(int row) { static char t[SC_ROWS][4]; t[row & 3][0] = (char)0xf5; t[row & 3][1] = (char)row_icon[row & 3]; t[row & 3][2] = 0; return t[row & 3]; }
int sc_sets(void) { return c_sets; }
int sc_page(void) { return c_sets > 1 ? g_page : 0; }
int sc_slot_in(int set, int row) { return g_bind[chara()][set & 1][row & 3]; }
int sc_slot(int row) { return sc_slot_in(sc_page(), row); }
static void save(int c) {
    static const char *const sec[3] = { "Character0", "Character1", "Character2" };
    for (int s = 0; s < SC_SETS; s++)
        for (int r = 0; r < SC_ROWS; r++) {
            char v[8]; snprintf(v, sizeof v, "%d", g_bind[c][s][r] + 1);
            WritePrivateProfileStringA(sec[c], row_key[s][r], v, g_file);
        }
}
/* one slot per button; slot < 0 clears.  Returns 1 if something changed. */
int sc_assign_in(int set, int row, int slot) {
    int c = chara(); row &= 3; set &= 1;
    if (slot > 7) return 0;
    if (slot < 0) slot = -1;
    if (g_bind[c][set][row] == slot) return 0;
    g_bind[c][set][row] = (s8)slot;
    save(c);
    return 1;
}
int sc_assign(int row, int slot) { return sc_assign_in(sc_page(), row, slot); }
static void load(void) {
    static const char *const sec[3] = { "Character0", "Character1", "Character2" };
    snprintf(g_file, sizeof g_file, "%sbbskh2_shortcuts.ini", g_dir);
    for (int c = 0; c < 3; c++)
        for (int s = 0; s < SC_SETS; s++)
            for (int r = 0; r < SC_ROWS; r++) {
                char b[16], d[8]; snprintf(d, sizeof d, "%d", s * SC_ROWS + r + 1);   /* default: slots 1..4, then 5..8 */
                GetPrivateProfileStringA(sec[c], row_key[s][r], d, b, sizeof b, g_file);
                int v = atoi(b);
                g_bind[c][s][r] = (s8)(v >= 1 && v <= 8 ? v - 1 : -1);
            }
}

/* ---------------- pad ---------------- */
static int g_hud;                       /* the command menu was on screen last frame (menu.c) */
static int g_active;                    /* L1 held, shortcuts shown, face buttons taken */
static int g_l1, g_l1_used, g_tap;
static int g_want = -1, g_want_t;       /* row whose shortcut was pressed, frames left to carry it out */
void sc_set_hud(int on) { g_hud = on; }
int sc_held(void) { return g_active; }
int sc_want(void) { return g_active && g_want_t > 0 ? g_want : -1; }
void sc_done(void) { g_want = -1; g_want_t = 0; }

static int can_show(void) {
    u8 *cmd = CMD;
    if (!cmd || !g_hud || (GFLAGS & 0x2000)) return 0;
    if (*(u32*)(cmd + 0x60) & 0x10000000) return 0;                    /* D-Link list open */
    u8 *mgr = *(u8**)(cmd + 0x68);
    if (!mgr) return 0;
    u8 *pad = mgr + 0x58;
    return (*(u32*)(pad + 0xa0) & 1) && *(float*)(pad + 0x54) <= 0.0f;  /* pad enabled, not locked */
}
/* A face button counts where it went down.  The game works out "pressed this frame" from last frame's entry of its
   pad history [1400ed040: ~history & held], and the face buttons are wiped from that entry while the list is
   shown - so to the game's own count a button that is simply being held looks pressed anew every frame.  Reading
   that count here made a button held BEFORE L1 use its shortcut the moment the list came up (and a held button
   repeat it).  Presses are counted here instead, from the buttons as the pad gives them, frame to frame.
   The other way round: a button still down when the list goes away would look freshly pressed to the game for the
   same reason, and attack; it stays hidden from the game until it is let go. */
static u32 g_face_prev;                 /* face buttons (and the d-pad) down last frame */
static u32 g_face_block;                /* down while the list was shown and not let go since: not the game's */
static void hide_face(u32 m) {
    PAD_HELD &= ~m; PAD_EDGE &= ~m; PAD_CHG &= ~m; PAD_REP &= ~m;
    u32 i = G(u32, 0x8f64954) & 7;                                     /* this frame's entries of the pad history */
    G(u32, 0x8f64958 + i * 4) &= ~m; G(u32, 0x8f64978 + i * 4) &= ~m;
}
static void pad_frame(void) {
    g_tap = 0;
    u32 held = PAD_OFF ? 0 : PAD_HELD;
    u32 mine = B_FACE | (c_sets > 1 ? B_DPAD : 0);                     /* the buttons the list takes while it is up */
    u32 face = PAD_HELD & mine, press = face & ~g_face_prev;
    g_face_prev = face;
    if (held & B_L1) {
        if (g_l1 < 100000) g_l1++;
        if (held & B_R1) g_l1_used = 1;
    } else {
        if (g_l1 > 0 && g_l1 <= c_tap && !g_l1_used) g_tap = 1;
        g_l1 = 0; g_l1_used = 0;
    }
    g_active = c_on && (held & B_L1) && !(held & B_R1) && can_show();
    if (!g_active) {
        sc_done();
        g_face_block &= face;                                           /* let go: the game's again */
        if (g_face_block) hide_face(g_face_block);
        return;
    }
    g_face_block = face;
    if (face) g_l1_used = 1;
    if (g_want_t > 0 && --g_want_t == 0) g_want = -1;
    if (press & B_DPAD) {                                               /* the other set */
        g_page ^= 1; sc_done();
        FN(u64, 0x1b2e40, int, float, u64, u32, u32)(1, G(float, 0x6420b8), 0, 4, 0);      /* the menus' cursor sound */
        if (g_debug) LOG("shortcuts: set %d", g_page + 1);
    }
    for (int r = 0; r < SC_ROWS; r++) if (press & row_bit[r]) { g_want = r; g_want_t = c_buffer; break; }
    hide_face(mine);
}
static void MSABI pad_hook(float dt) {
    FN(void, 0xed040, float)(dt);
    pad_frame();
}
/* 22b5df: "turn the camera behind the character" */
static int MSABI cam_reset_pad(u8 *pad) {
    u32 fl = *(u32*)(pad + 0xa0);
    if (((fl >> 1) & 3) != 0) return FN(int, 0x2729c0, u8*)(pad);      /* other control types: as the game */
    if (*(float*)(pad + 0x24) != 0.0f) return 0;                        /* R1 held */
    return g_tap && (fl & 1) && *(float*)(pad + 0x54) <= 0.0f;
}
/* 2650d4: "next lock-on target" */
static int MSABI target_pad(u8 *pad, u32 *a, u32 *b) {
    *a = 0; *b = 0;
    if ((*(u32*)(pad + 0xa0) & 0x400) || *(float*)(pad + 0x24) != 0.0f || !g_tap) return 0;
    *a = 0x40000000;                                                    /* 2.0f, as the game */
    *(u32*)(pad + 0x38) = 0;
    return 1;
}

static int call_ok(u32 rva, u32 target) {
    u8 *p = g_base + rva; s32 d; memcpy(&d, p + 1, 4);
    return p[0] == 0xE8 && (u32)(rva + 5 + d) == target;
}
int shortcut_check(void) {
    char b[32];
    GetPrivateProfileStringA("Shortcuts", "Enabled", "1", b, sizeof b, g_ini); c_on = atoi(b);
    GetPrivateProfileStringA("Shortcuts", "TapFrames", "18", b, sizeof b, g_ini); c_tap = atoi(b);
    GetPrivateProfileStringA("Shortcuts", "BufferFrames", "12", b, sizeof b, g_ini); c_buffer = atoi(b);
    GetPrivateProfileStringA("Shortcuts", "Sets", "2", b, sizeof b, g_ini); c_sets = atoi(b) == 1 ? 1 : 2;
    if (c_buffer < 1) c_buffer = 1;
    load();
    if (!c_on) return 1;
    if (!call_ok(0xe3716, 0xed040) || !call_ok(0x22b5df, 0x2729c0) || !call_ok(0x2650d4, 0x272b80)) { LOG("shortcuts: pad call sites do not match"); return 0; }
    return 1;
}
void shortcut_apply(void) {
    if (!c_on) return;
    hook_call(0xe3716, 0xed040, pad_hook, "pad state");
    hook_call(0x22b5df, 0x2729c0, cam_reset_pad, "camera reset pad");
    hook_call(0x2650d4, 0x272b80, target_pad, "target pad");
    LOG("shortcuts: L1 + face buttons (file %s)", g_file);
}

#ifndef _WIN32      /* offline test access */
void test_sc_pad_frame(void) { pad_frame(); }
int test_sc_tap(void) { return g_tap; }
void test_sc_reload(void) { load(); }
s8 *test_sc_bind(void) { return g_bind[0][0]; }
int *test_sc_page(void) { return &g_page; }
#endif

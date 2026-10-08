/* KH2-style command menu: Attack / Magic / Items / D-Link in place of the scrolling command deck.

   The game's own command objects keep doing the work (plates, use gate, D-Link list); this file replaces the
   deck's input handler and re-places what is drawn:
     - FUN_140233a00 (deck input: scroll, triangle = use, left = shortcut, right = D-Link list) is replaced by
       menu_input(): up/down move the cursor, the confirm button opens a list or uses the chosen command,
       left goes back.
     - the confirm button reaches the game's own Attack / finisher code only while the cursor is on Attack
       (FUN_140272860 answers "not pressed" otherwise).
     - every command plate's update (CCommandPlate vtable slot 1) is followed by plate_after(): deck plates are
       shown only inside the Magic / Item list, as a fixed list; D-Link entries as a fixed list while the
       game's D-Link list is open.
     - Magic / Item / Link are three extra instances of the deck-plate layout with their own text.
   Notes: docs/player_command.md, docs/l2d_api.md. */
#include "mod.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "steals_gen.h"
#include "hdart_gen.h"

extern char g_dir[MAX_PATH], g_ini[MAX_PATH + 32];
extern int g_debug;

#define CMD     G(u8*, 0x10f9ed40)
#define GFLAGS  G(u32, 0x10f9ee48)
#define CMD_CAT(id)  G(u8, 0x814900 + (u32)(id) * 0x18 + 1)

/* L2D runtime */
#define L2D_CreateLayout(file, id, ctl)   FN(int, 0x1a4ec0, int, u16, int, void*)(file, id, ctl, NULL)
#define L2D_Destroy(h)                    FN(void, 0x1a57f0, int)(h)
#define L2D_Show(h, on)                   FN(int, 0x1a5b30, int, int)(h, on)
#define L2D_IsVisible(h)                  FN(int, 0x1a6060, int)(h)
#define L2D_GetControl(h)                 FN(int, 0x1a60e0, int)(h)          /* 8 = dead handle */
#define L2D_SetControl(h, c)              FN(int, 0x1a7520, int, int)(h, c)
#define L2D_SetPos(h, x, y)               FN(int, 0x1a7660, int, float, float)(h, x, y)
#define L2D_SetPriority(h, p)             FN(int, 0x1a7600, int, s16, int)(h, p, 0)
#define L2D_SetColor(h, c)                FN(int, 0x1a73f0, int, u32, int)(h, c, 0)   /* 0..255 per channel, as the game calls it */
#define L2D_ShowNode(h, n, on)            FN(int, 0x1a5b80, int, u16, int)(h, n, on)
#define L2D_SetNodeFrame(h, n, f)         FN(int, 0x1a7d30, int, u16, float, int)(h, n, f, 0)
#define L2D_SetNodeControl(h, n, c)       FN(int, 0x1a7db0, int, u16, int)(h, n, c)
#define L2D_GetNodeControl(h, n)          FN(int, 0x1a6790, int, u16)(h, n)
#define L2D_SetNodeColor(h, n, c)         FN(int, 0x1a7c30, int, u16, u32, int)(h, n, c, 1)   /* raw: 0x80 = 1.0, alpha ff = 1.0 */
#define L2D_SetNodeText(h, n, s)          FN(int, 0x1a8330, int, u16, const char*, int, void*)(h, n, s, 0, NULL)
#define L2D_ReplaceNodeSeq(h, n, sq, id, c) FN(int, 0x1a7fb0, int, u16, int, u16, int)(h, n, sq, id, c)
#define SE(id)                            FN(u64, 0x1b2e40, int, float, u64, u32, u32)(id, 1.0f, 0, 0, 0)

/* ---------------- settings ---------------- */
static int   c_menu = 1;
static float c_main_x = 4.5f, c_row_y[4] = { 198, 213, 228, 243 };   /* Attack, Magic, Item, Link */
static float c_fill = 100;             /* plate under the cursor: strength of the colour gradient inside it, percent (0 = none) */
static int   c_noshadow = 1;           /* no drop shadow under the gauge window */
static float c_bump = 6;               /* the entry under the cursor sits this far to the right, as in KH2 */
/* The Magic / Items / D-Link list opens over the menu, a little to its right, as in KH1: its first row takes the row
   of the entry it was opened from and the rest follow downwards; a list too long for the rows left below it ends on
   the menu's last row and grows upwards instead.  A header with the list's name sits on the row above the first. */
static float c_list_x = 14, c_pitch = 15;
static int   c_hdr = 1;                /* the header: the game's "COMMANDS" label plate with MAGIC / ITEMS / D-LINK on it */
static float c_hdr_dx = -1.6f, c_hdr_dy = -9.1f;   /* its place from the first row's (left edges in line, plate on the row above) */
static float c_hdr_wide = 24;          /* the label plate is made this much longer (sprite units), to the rows' length */
static int   c_list_dim = 1;           /* the four entries behind an open list are darkened, as in KH1 ... */
static float c_list_dim_level = 50;    /* ... to this much of their brightness, percent */
static int   c_list_col = 1;           /* each list has its own colour, the same in battle and out of it (0 = the game's battle / field colours) */
static u32   c_col_plate[3] = { 0x005ff0, 0x00c832, 0x0096fa };    /* Magic, Items, D-Link: the header's plate and the rows' frames ... */
static u32   c_col_title[3] = { 0x00ffff, 0xc8ff64, 0xffffff };    /* ... and the letters on the header.  RRGGBB, 80 = the texture's own grey */
static int   c_link_bright = 1;        /* D-Link rows not under the cursor in full colour, as the other lists' rows (the game draws them dark) */
static float c_head_dy = -4.5f;        /* the "COMMANDS" gauge window: moved by this from the game's place (table row 1) */
static float head_y(void) { return (float)G(s16, 0x818308 + 4) + c_head_dy; }
static float c_prompt_x = 2, c_prompt_y = 160;     /* context prompt (Save, Talk, Open ...): above the window, as KH2's reaction command */
static float c_prompt_raise = 8;       /* ... and this much higher while a Command Style or a D-Link is active: their windows are taller */
static float prompt_y(u8 *cmd) {
    int tall = *(int*)(cmd + 0x1a8) > 0 || (*(u32*)(cmd + 0x64) & 0x80);      /* style level 1 / 2, or linked */
    return c_prompt_y - (tall ? c_prompt_raise : 0.0f);
}
/* A Command Style / finisher offer sits right above the window (3 higher than a context prompt alone would: its
   timer bar hangs under it).  A context prompt that is up at the same time goes above the offer. */
static float c_prompt_stack = 16;      /* height of an offer's plate: how far above the offer the prompt sits */
static float offer_y(u8 *cmd) { return prompt_y(cmd) - 3.0f; }
static float context_y(u8 *cmd) { return style_prompt_shown() ? offer_y(cmd) - c_prompt_stack : prompt_y(cmd); }
static char  c_text[3][24] = { "Magic", "Items", "D-Link" };     /* "D-Link" is the game's own word for it */
/* shortcut list (hold L1): the game's large "under the cursor" plate, which has room for a button's picture on its
   left, in grey.  Positions are in the plate's own units (it is drawn at 0.88) */
static float c_sc_icon_x = 11, c_sc_icon_y = 8;    /* centre of the button's picture: the game's own place for it */
static float c_sc_pad = 0;                          /* extra width of the room for the picture */
static u32   c_sc_col = 0x808080;                   /* frame and tab, RRGGBB, 80 = the texture's own grey */
static u32   c_sc_col2 = 0xc09040;                  /* ... while the second set is shown: gold */
static u32   c_sc_fill = 0x606060;                  /* strength of the gradient inside */
static float c_sc_item_dx = 11;                     /* an item's bottle icon: this far right of its place on the plain plate */
static u32   c_sc_dim = 0x404040;                   /* name of a command that cannot be used now */
static int   c_hd = 1;                              /* the "COMMANDS" window is grey as well while the list is shown */
static u32   c_hd_plate = 0x707070, c_hd_text = 0xffffff;   /* ... its label plate and the letters on it (its frame: c_sc_col) */
static float c_link_time = 0;          /* time the D-Link list takes to move its cursor, in 1/30 s (the game: 10); 0 = at once, as the other lists */
static int   c_react = 1;              /* context prompts (Talk, Open, Save ...) are answered with triangle, as KH2's reaction commands */
static int   c_right_open = 1;         /* d-pad right on Magic, Items or D-Link opens its list, as the confirm button does */
static int   c_jump_back = 1;          /* the jump button closes an open list (Magic, Item, D-Link) and does not jump, as in KH2 */
static int   c_warn = 1;               /* a command that would empty the MP bar has its name in another colour */
static u32   c_warn_col = 0xff107080;  /* ... AABBGGRR, 80 = full: yellow */

static float ini_f(const char *key, float def) {
    char p[MAX_PATH + 32], b[64], d[64];
    snprintf(p, sizeof p, "%s", g_ini); snprintf(d, sizeof d, "%g", def);
    GetPrivateProfileStringA("Menu", key, d, b, sizeof b, p);
    return (float)atof(b);
}
static void ini_s(const char *key, char *out, int n) {
    char p[MAX_PATH + 32], d[64];
    snprintf(p, sizeof p, "%s", g_ini); snprintf(d, sizeof d, "%s", out);
    GetPrivateProfileStringA("Menu", key, d, out, n, p);
}
static void load_ini(void) {
    c_menu = (int)ini_f("Enabled", (float)c_menu);
    c_main_x = ini_f("MainX", c_main_x);
    c_row_y[0] = ini_f("AttackY", c_row_y[0]); c_row_y[1] = ini_f("MagicY", c_row_y[1]);
    c_row_y[2] = ini_f("ItemY", c_row_y[2]);   c_row_y[3] = ini_f("LinkY", c_row_y[3]);
    c_bump = ini_f("CursorShift", c_bump);
    c_fill = ini_f("CursorFill", c_fill);
    c_list_x = ini_f("ListX", c_list_x); c_pitch = ini_f("ListPitch", c_pitch);
    c_hdr = (int)ini_f("ListHeader", (float)c_hdr); c_hdr_dx = ini_f("ListHeaderX", c_hdr_dx); c_hdr_dy = ini_f("ListHeaderY", c_hdr_dy);
    c_hdr_wide = ini_f("ListHeaderWiden", c_hdr_wide); if (c_hdr_wide < 0) c_hdr_wide = 0; if (c_hdr_wide > 60) c_hdr_wide = 60;
    c_list_dim = (int)ini_f("ListDimsMenu", (float)c_list_dim); c_list_dim_level = ini_f("ListDimLevel", c_list_dim_level);
    if (c_list_dim_level < 0) c_list_dim_level = 0; if (c_list_dim_level > 100) c_list_dim_level = 100;
    c_list_col = (int)ini_f("ListColors", (float)c_list_col);
    { static const char *kp[3] = { "MagicColor", "ItemsColor", "DLinkColor" }, *kt[3] = { "MagicTitleColor", "ItemsTitleColor", "DLinkTitleColor" };
      for (int i = 0; i < 3; i++) { char b[32];
        snprintf(b, sizeof b, "%06x", c_col_plate[i]); ini_s(kp[i], b, sizeof b); c_col_plate[i] = (u32)strtoul(b, NULL, 16) & 0xffffff;
        snprintf(b, sizeof b, "%06x", c_col_title[i]); ini_s(kt[i], b, sizeof b); c_col_title[i] = (u32)strtoul(b, NULL, 16) & 0xffffff; } }
    c_link_bright = (int)ini_f("DLinkRowsBright", (float)c_link_bright);
    c_head_dy = ini_f("HeaderShift", c_head_dy);
    c_noshadow = (int)ini_f("HeaderShadow", 0) == 0;
    c_prompt_x = ini_f("PromptX", c_prompt_x); c_prompt_y = ini_f("PromptY", c_prompt_y);
    c_prompt_raise = ini_f("PromptStyleRaise", c_prompt_raise);
    c_prompt_stack = ini_f("PromptAboveOffer", c_prompt_stack);
    c_warn = (int)ini_f("BurnWarning", (float)c_warn);
    c_jump_back = (int)ini_f("JumpClosesList", (float)c_jump_back);
    c_right_open = (int)ini_f("RightOpensList", (float)c_right_open);
    c_react = (int)ini_f("ReactionButton", (float)c_react);
    c_sc_icon_x = ini_f("ShortcutIconX", c_sc_icon_x); c_sc_icon_y = ini_f("ShortcutIconY", c_sc_icon_y);
    c_sc_item_dx = ini_f("ShortcutItemIconShift", c_sc_item_dx);
    c_sc_pad = ini_f("ShortcutPad", c_sc_pad); if (c_sc_pad < 0) c_sc_pad = 0; if (c_sc_pad > 30) c_sc_pad = 30;
    { char b[32];
      snprintf(b, sizeof b, "%06x", c_sc_col); ini_s("ShortcutColor", b, sizeof b); c_sc_col = (u32)strtoul(b, NULL, 16) & 0xffffff;
      snprintf(b, sizeof b, "%06x", c_sc_col2); ini_s("ShortcutColor2", b, sizeof b); c_sc_col2 = (u32)strtoul(b, NULL, 16) & 0xffffff;
      snprintf(b, sizeof b, "%06x", c_sc_fill); ini_s("ShortcutFill", b, sizeof b); c_sc_fill = (u32)strtoul(b, NULL, 16) & 0xffffff;
      snprintf(b, sizeof b, "%06x", c_sc_dim); ini_s("ShortcutDimText", b, sizeof b); c_sc_dim = (u32)strtoul(b, NULL, 16) & 0xffffff; }
    c_hd = (int)ini_f("ShortcutHeader", (float)c_hd);
    { char b[32];
      snprintf(b, sizeof b, "%06x", c_hd_plate); ini_s("ShortcutHeaderPlate", b, sizeof b); c_hd_plate = (u32)strtoul(b, NULL, 16) & 0xffffff;
      snprintf(b, sizeof b, "%06x", c_hd_text); ini_s("ShortcutHeaderText", b, sizeof b); c_hd_text = (u32)strtoul(b, NULL, 16) & 0xffffff; }
    c_link_time = ini_f("LinkCursorTime", c_link_time); if (c_link_time < 0) c_link_time = 0;
    { char b[32]; snprintf(b, sizeof b, "%08x", c_warn_col); ini_s("BurnWarningColor", b, sizeof b); c_warn_col = (u32)strtoul(b, NULL, 16); }
    ini_s("MagicText", c_text[0], sizeof c_text[0]); ini_s("ItemText", c_text[1], sizeof c_text[1]); ini_s("LinkText", c_text[2], sizeof c_text[2]);
}

/* ---------------- state ---------------- */
enum { E_ATTACK, E_MAGIC, E_ITEM, E_LINK };
enum { S_MAIN, S_MAGIC, S_ITEM };
static int g_state = S_MAIN;
static int g_cur = E_ATTACK;            /* cursor on the main menu */
static int g_sel[2];                    /* cursor inside the Magic / Item list (index among its entries) */
static int g_entry[4];                  /* L2D instances of Attack, Magic, Item, Link */
static int g_attack_id = -1;            /* command whose name the Attack entry shows */
static int g_entry_battle = -1;
static int g_link_req;                  /* the Link entry was confirmed: answer the game's "open D-Link list" pad test */
static int g_link_was_open;
static int g_link_close;                /* the jump button was pressed with the D-Link list open: close it */
static int g_in_cmd, g_swallow;         /* inside the command object's update / keep the confirm button from the game */
static u8 *g_cmd_seen;

static int plate_count(u8 *cmd) { int n = *(u16*)(cmd + 0x220); return n > 8 ? 8 : n; }
static u8 *plate_at(u8 *cmd, int i) { return *(u8**)(cmd + 0x1e0 + i * 8); }
static int plate_is_item(u8 *P) { u16 *k = *(u16**)(P + 0x58); return k && CMD_CAT(k[0]) == 3; }
static int plate_valid(u8 *P) { u16 *k; return P && P[0x30] == 2 && (k = *(u16**)(P + 0x58)) != NULL && k[0] != 0; }
/* number of entries of a list (kind: 0 magic, 1 item); *out = the idx-th one */
static int list_count(u8 *cmd, int kind, int idx, u8 **out) {
    int n = 0;
    for (int i = 0; i < plate_count(cmd); i++) {
        u8 *P = plate_at(cmd, i);
        if (!plate_valid(P) || plate_is_item(P) != kind) continue;
        if (n == idx && out) *out = P;
        n++;
    }
    return n;
}
static int list_index_of(u8 *cmd, u8 *P, int kind) {
    int n = 0;
    for (int i = 0; i < plate_count(cmd); i++) {
        u8 *Q = plate_at(cmd, i);
        if (!plate_valid(Q) || plate_is_item(Q) != kind) continue;
        if (Q == P) return n;
        n++;
    }
    return -1;
}
static int deck_sealed(u8 *cmd) { return (*(u32*)(cmd + 0x60) & 0x800000) != 0; }      /* the game's own "commands sealed" */
static int link_open(u8 *cmd) { return (*(u32*)(cmd + 0x60) & 0x10000000) != 0; }
static int link_active(u8 *cmd) { return (*(u32*)(cmd + 0x64) & 0x80) != 0; }
/* A context prompt is up (cmd+0x60 bit 0x40000; its plate, type 5, at +0x298) and it is one that triangle answers:
   everything but the counter prompts (command category 6), which stay on the attack button. */
static int react_prompt(u8 *cmd) {
    if (!c_react || !(*(u32*)(cmd + 0x60) & 0x40000)) return 0;
    u8 *P = *(u8**)(cmd + 0x298);
    u16 *k = P ? *(u16**)(P + 0x58) : NULL;
    return !(k && CMD_CAT(k[0]) == 6);
}
int menu_react_prompt(u8 *cmd) { return c_menu && cmd && react_prompt(cmd); }
/* the confirm button belongs to the menu (not to the game's own tests inside the command update): off Attack, unless
   a prompt that is answered with that button is up */
static int swallow(u8 *cmd) {
    return (g_state != S_MAIN || g_cur != E_ATTACK) && (!(*(u32*)(cmd + 0x60) & 0x40000) || react_prompt(cmd));
}
static int entry_usable(u8 *cmd, int e) {
    switch (e) {
    case E_MAGIC: return !mp_in_burn() && !deck_sealed(cmd) && list_count(cmd, 0, -1, NULL) > 0;
    case E_ITEM:  return !deck_sealed(cmd) && list_count(cmd, 1, -1, NULL) > 0;
    case E_LINK:  return cmd[0x350] > 0 && !(GFLAGS & 0x200000) && !deck_sealed(cmd) && (link_active(cmd) || !mp_blocks_link());
    }
    return 1;
}

/* ---------------- input ---------------- */
static int (MSABI *o_confirm)(u8 *pad);
/* FUN_140272860: confirm button pressed.  The game's Attack / finisher code only sees it while the cursor is on
   Attack (a context prompt such as Talk or a counter always gets it). */
static int MSABI confirm_hook(u8 *pad) {
    if (g_in_cmd && g_swallow) return 0;
    return o_confirm(pad);
}
/* The game refills its placement table {u16 id; s16 x; s16 y; u8 prio; u8 flag} at 818300 from the layout file
   whenever the command object is built (22fe30, 235870).  Rows 2..7 are the Attack plate and what stacks on it
   (y 247 / 249): keep them moved to the top row of the menu.  Runs before the plates create their graphics. */
static void fix_table(void) {
    s16 want = (s16)c_row_y[0], cur = G(s16, 0x818318 + 4);
    if (cur == want) return;
    for (int r = 2; r <= 7; r++) G(s16, 0x818300 + r * 8 + 4) += (s16)(want - cur);
}
/* Parts of the old deck that have no place in the menu: the dark strip behind the old Attack row (+0xac), the scroll
   arrows (+0xb0), the three shadow plates of the old deck rows (+0xb4) and the "D-Link available" prompt (+0xc8).
   The game shows some of them again whenever it finds them hidden, so they are also moved off the screen; done
   after the command object's update and again from the gauge's. */
static void hide_old_parts(u8 *cmd) {
    static const u16 off[4] = { 0xac, 0xb0, 0xb4, 0xc8 };
    for (int i = 0; i < 4; i++) {
        int h = *(int*)(cmd + off[i]);
        if (h <= 0) continue;
        L2D_SetPos(h, -2000.0f, -2000.0f);
        if (off[i] != 0xb4) L2D_Show(h, 0);
    }
}
static void (MSABI *o_cmd_update)(u8 *cmd);
static void MSABI cmd_update_hook(u8 *cmd) {
    fix_table();
    if (g_cmd_seen != cmd) { g_cmd_seen = cmd; g_state = S_MAIN; g_cur = E_ATTACK; g_link_was_open = 0; }   /* new room */
    *(float*)(cmd + 0x124) = head_y();      /* where new gauge windows (style changes) are put */
    g_in_cmd = 1;
    g_swallow = swallow(cmd);
    o_cmd_update(cmd);
    g_in_cmd = 0;
    if (c_menu) hide_old_parts(cmd);
    int open = link_open(cmd);
    if (!open) g_link_close = 0;
    if (g_link_was_open && !open && link_active(cmd)) g_cur = E_ATTACK;     /* a link was chosen */
    g_link_was_open = open;
}
/* 238902: the pad test "open the D-Link list" (d-pad right in the game) */
static int MSABI link_open_pad(u8 *pad) { (void)pad; int r = g_link_req; g_link_req = 0; return r; }
/* 2333f1 and 2332fe: the pad tests "confirm the D-Link entry" and, while linked, "end the link" (Revert)
   (triangle in the game) -> the confirm button */
static int MSABI link_confirm_pad(u8 *pad) { return o_confirm(pad); }
/* 236c92: the pad test for the extra presses of multi-press commands (Ars Arcanum, Sonic Blade ...: triangle in the
   game).  The command was cast with the confirm button here, so that button continues it too. */
static int MSABI followup_pad(u8 *pad) { return FN(int, 0x272c20, u8*)(pad) || o_confirm(pad); }

/* 236b97: the pad test that answers a context prompt (the confirm button in the game).  Talk, Open, Save and the
   like are answered with triangle (140272c20), as KH2's reaction commands; counter prompts as in the game. */
static int MSABI prompt_pad(u8 *pad) {
    u8 *cmd = CMD;
    if (cmd && react_prompt(cmd)) return FN(int, 0x272c20, u8*)(pad);
    return FN(int, 0x272860, u8*)(pad);
}
/* While a prompt is up the game does not run the Attack part of its update (the prompt has taken Attack's place and
   button).  With the prompt on triangle the attack button attacks again: what that part does [236d9e..236e8e] -
   the COMMAND of the attack plate of the current style level goes to cmd+0x80 - without the plate's own
   "pressed" state, which belongs to the prompt for now. */
static void prompt_attack(u8 *cmd, u8 *pl) {
    u32 fl = *(u32*)(cmd + 0x60);
    if ((fl & (0x1000 | 0x4000 | 0x20000)) || *(s16*)(cmd + 0x80) != 0) return;
    if (!(pl[0x318] & 0xc0)) return;
    int lv = *(int*)(cmd + 0x1a8);
    if (lv < 0 || lv > 2) return;
    u8 *P = *(u8**)(cmd + 0x1b0 + lv * 8);
    u64 *c = P ? *(u64**)(P + 0x58) : NULL;
    if (!c || (lv == 0 && *(s16*)(pl + 0x354) == 0x18e)) return;
    *(u64*)(cmd + 0x1c8) = *c;
    *(u64*)(cmd + 0x80) = *c;
    *(u32*)(pl + 0x570) = *(u32*)(cmd + 0x20);
}
/* 21fb99: the player's "jump pressed" (140272f10: the cancel button of the menus, circle by default; the answer
   becomes input flag 1 at pl+0x31c).  With a list of the command menu open the press goes to the menu: the Magic /
   Item list closes here, the D-Link list through the game's own "close" test below - and there is no jump.  With
   the main menu showing the button jumps as always. */
static int MSABI jump_pad(u8 *pad) {
    int r = FN(int, 0x272f10, u8*)(pad);
    u8 *cmd = CMD;
    if (!r || !c_jump_back || !cmd) return r;
    if (link_open(cmd)) { g_link_close = 1; return 0; }
    if (g_state != S_MAIN) { g_state = S_MAIN; SE(1); return 0; }
    return r;
}
/* 2337e9: the pad test "close the D-Link list" (d-pad left in the game) */
static int MSABI link_close_pad(u8 *pad) {
    int r = FN(int, 0x272d30, u8*)(pad);
    if (g_link_close) { g_link_close = 0; r = 1; }
    return r;
}

/* 2332a1, 233491: the D-Link list moves its cursor (140205cf0(plate, direction), for every entry: the game's list
   is a wheel that turns under a fixed cursor).  The move is an animation of 10 time units (plate+0x68; plate state
   2), and the list takes no input until the entry that arrives under the cursor has finished it - a third of a
   second per step, where the Magic and Item lists here move as fast as the pad repeats.  The menu draws the entries
   as a fixed list anyway, so the move is ended at once: with the time at 0 the game's own step function
   (1402044c0) finishes it, and what the plate's update does next [204d5a] is done here too - the entry under the
   cursor (position cmd+0x354, or the last one) becomes the selected one (flag 4, state 4, 140205a60), the others
   go to rest (state 0).  So there is no frame without a selected entry. */
static u64 MSABI link_scroll(u8 *P, int dir) {
    u64 r = FN(u64, 0x205cf0, u8*, int)(P, dir);
    u8 *cmd = CMD;
    if (c_link_time > 0 || !cmd || P[0x30] != 6 || P[0x31] != 2) { if (c_link_time > 0) *(float*)(P + 0x68) = c_link_time; return r; }
    *(float*)(P + 0x68) = 0;
    if (FN(int, 0x2044c0, u8*)(P)) {
        int cur = (s8)cmd[0x354] < 0 ? cmd[0x350] - 1 : (s8)cmd[0x354];
        if ((s8)P[0x6c] == cur) { *(u32*)(P + 0x60) |= 4; P[0x31] = 4; FN(void, 0x205a60, u8*)(P); }
        else P[0x31] = 0;
    }
    return r;
}

static int can_use_now(u8 *cmd, u8 *pl) {       /* the game's own conditions for the deck button */
    if (*(s16*)(cmd + 0x188) != *(s16*)(pl + 0x354)) return 0;                                  /* style changing */
    if ((*(u32*)(cmd + 0x60) & 0x40000) && *(u8**)(cmd + 0x298)) {
        u16 *k = *(u16**)(*(u8**)(cmd + 0x298) + 0x58);
        if (k && CMD_CAT(k[0]) == 6) return 0;                                                  /* counter prompt up */
    }
    if ((*(u32*)(pl + 0x318) & 0xc0) != 0x40) return 0;
    if (FN(int, 0x240a80, u8*)(pl)) return 0;
    if (*(s16*)(cmd + 0x80) != 0) return 0;
    return 1;
}
/* The deck plate that holds deck slot `slot` (0..7).  Plates are packed: plate i shows working-deck entry i
   (140818 7b0[i]), whose slot number is the byte 1408187a8[i] [235b50].  While linked the deck is the D-Link's own
   and the plate index is the slot. */
static u8 *slot_plate(u8 *cmd, int slot) {
    int linked = link_active(cmd);
    for (int i = 0; i < plate_count(cmd); i++) {
        u8 *P = plate_at(cmd, i);
        if (!plate_valid(P)) continue;
        int s = i;
        if (!linked) {
            long w = (long)(*(u8**)(P + 0x58) - (g_base + 0x8187b0)) / 8;
            if (w >= 0 && w < 8 && G(u8, 0x8187a8 + w) < 8) s = G(u8, 0x8187a8 + w);
        }
        if (s == slot) return P;
    }
    return NULL;
}
/* replaces FUN_140233a00; returns 1 when the caller should stop (D-Link list just opened) */
static int MSABI menu_input(u8 *cmd) {
    if (link_open(cmd)) return 0;
    u8 *mgr = *(u8**)(cmd + 0x68), *pl = *(u8**)(cmd + 0x70);
    if (!mgr || !pl) return 0;
    u8 *pad = mgr + 0x58;
    if (sc_held()) {                                /* L1 held: the face buttons are shortcuts (shortcut.c) */
        int row = sc_want();
        if (row >= 0) {
            int slot = sc_slot(row);
            u8 *P = slot >= 0 ? slot_plate(cmd, slot) : NULL;
            if (!P || deck_sealed(cmd)) sc_done();                      /* nothing in that slot */
            else if (can_use_now(cmd, pl)) {
                u64 *c = (u64*)mp_use(P);                               /* NULL: reloading, or commands off in MP charge */
                if (c) {
                    *(u64*)(cmd + 0x80) = *c; *(u64*)(cmd + 0x88) = *c;
                    *(u8**)(cmd + 0x90) = P;
                    g_state = S_MAIN; g_cur = E_ATTACK;
                }
                sc_done();
            }                                                           /* else: busy; the press waits a few frames */
        }
        return 0;
    }
    int dir = FN(int, 0x272580, u8*)(pad);          /* +1 up, -1 down, with repeat */
    int left = FN(int, 0x272d30, u8*)(pad);
    int ok = g_swallow ? o_confirm(pad) : 0;        /* on Attack the game handles the button itself */

    if (g_state != S_MAIN) {
        int kind = g_state == S_ITEM;
        int n = list_count(cmd, kind, -1, NULL);
        if (n == 0 || deck_sealed(cmd) || (!kind && mp_in_burn())) { g_state = S_MAIN; return 0; }
        if (g_sel[kind] >= n) g_sel[kind] = n - 1;
        if (g_sel[kind] < 0) g_sel[kind] = 0;
        if (dir) { g_sel[kind] = (g_sel[kind] - dir + n) % n; SE(1); }
        if (left) { g_state = S_MAIN; SE(1); return 0; }
        if (ok && can_use_now(cmd, pl)) {
            u8 *P = NULL; list_count(cmd, kind, g_sel[kind], &P);
            u64 *c = P ? (u64*)mp_use(P) : NULL;
            if (c) {
                *(u64*)(cmd + 0x80) = *c; *(u64*)(cmd + 0x88) = *c;
                *(u8**)(cmd + 0x90) = P;
                g_state = S_MAIN; g_cur = E_ATTACK;         /* back to the main menu, as in KH2 */
            }
        }
        return 0;
    }
    if (dir) { g_cur = (g_cur - dir + 4) % 4; SE(1); }
    if (g_cur == E_ATTACK && react_prompt(cmd) && o_confirm(pad)) { prompt_attack(cmd, pl); return 0; }
    /* d-pad right (140272cd0, a new press) opens the entry's list as well: left closes a list, right opens one.
       It is the menu's own button, so it works with a counter prompt up too, when confirm belongs to the prompt. */
    if (c_right_open && g_cur != E_ATTACK && FN(int, 0x272cd0, u8*)(pad)) ok = 1;
    if (ok && g_cur != E_ATTACK && entry_usable(cmd, g_cur)) {
        if (g_cur == E_LINK) {
            g_link_req = 1;
            int r = FN(int, 0x2388a0, u8*)(cmd);            /* the game's own "open the D-Link list" */
            g_link_req = 0;
            if (r) return 1;
        } else { g_state = g_cur == E_MAGIC ? S_MAGIC : S_ITEM; SE(1); }
    }
    return 0;
}

/* ---------------- drawing ---------------- */
/* y of row idx of a list of n rows opened from main entry e (1 Magic, 2 Items, 3 D-Link).  Rows count from the
   menu's: 0..3 are Attack..D-Link, negative ones lie above the menu. */
static float row_y(int row) { return row >= 0 ? c_row_y[row > 3 ? 3 : row] : c_row_y[0] + (float)row * c_pitch; }
static int list_first_row(int e, int n) { int first = e; if (first + n - 1 > 3) first = 3 - (n - 1); return first; }
static float list_y(int e, int idx, int n) { return row_y(list_first_row(e, n) + idx); }
static void set_control(int h, int ctl) { if (L2D_GetControl(h) != ctl) L2D_SetControl(h, ctl); }
static void set_node_control(int h, u16 node, int ctl) { if (L2D_GetNodeControl(h, node) != ctl) L2D_SetNodeControl(h, node, ctl); }
/* An animation record and a key of a 2D sequence, as in the file.  Every object of a node plays one record (CD2Obj
   +0x30) whose keys are looked up in the object's key table (+0x38); a record's keys follow each other by kind,
   the colour keys last. */
typedef struct { s32 maxf; s16 spr; u16 key; u8 keyn[11]; u8 kind, blend, flag, sciss, z; } Anim;     /* 0x18 bytes, as in the file */
typedef struct { float t; union { float f; u8 c[4]; } v; u32 interp; } Key;                           /* 0xc bytes */
enum { K_STATUS, K_BASEX, K_BASEY, K_SCALEX = 8, K_SCALEY, K_COLOR };
/* A key table as large as a record can index: should the game ever put one of its own records back on an object
   that still has this table, it reads zeros and not somebody else's memory. */
static Key  g_sc_keys[0x10000];

/* ---- colours of our own ----
   Every colour of a plate or of the header comes from the colour keys of its animations (yellow in battle, blue
   in the field) laid over grey texels, so a tint cannot change it: colours only multiply.  An object can be given
   another record though.  tint_node() gives the objects of a node a twin of the record the game would have them
   play - the same record with the same keys, copied into our key table with the colour keys changed - and puts
   the game's own record back where no rule applies.  The game's record of object i in the node's current control
   is known from the file (control record +8 = first record of the control [1401abb80]), so nothing has to be
   remembered per object: the pass says what each object should play and sets it if it plays something else.  A
   control change by the game (which puts its records back but leaves our key table on the objects) is put right
   by the next pass, which runs after the plate's own update and before anything is drawn.
   Twins are made once per record and colour and kept; their keys sit above any key index of the file (bc01_00
   has 3749 keys), so a game record read through our table by mistake finds zeros. */
typedef struct { s16 spr; u32 from, to; } Tint;         /* objects showing this sprite: colour keys equal to `from` (ANY: all of them) become `to` */
#define ANY 0xffffffffu
#define TW_SLOTS 256
#define TW_KEYS  32
#define TW_KEY0  0x4000                 /* a record's key index is a signed 16-bit number to the game [1401a8871]: all of ours stay below 0x8000 */
static Anim g_tw_anim[TW_SLOTS];
static struct { Anim *src; u32 from, to; } g_tw_id[TW_SLOTS];
static int g_tw_n;
static Anim *twin_for(Anim *src, Key *gk, const Tint *r) {
    for (int i = 0; i < g_tw_n; i++) if (g_tw_id[i].src == src && g_tw_id[i].to == r->to && g_tw_id[i].from == r->from) return &g_tw_anim[i];
    int nk = 0, nc = src->keyn[K_COLOR], hit = 0;
    for (int k = 0; k < 11; k++) nk += src->keyn[k];
    if (!nc || nk > TW_KEYS) return NULL;
    for (int k = nk - nc; k < nk; k++) {
        const u8 *c = gk[src->key + k].v.c; u32 rgb = (u32)c[0] << 16 | (u32)c[1] << 8 | c[2];
        if ((r->from == ANY || rgb == r->from) && rgb != r->to) hit = 1;
    }
    if (!hit) return NULL;                              /* the game's record already has that colour */
    if (g_tw_n >= TW_SLOTS) g_tw_n = 0;                 /* full (the file was loaded anew many times): start over; every pass sets what it wants again */
    int slot = g_tw_n++;
    Key *dst = &g_sc_keys[TW_KEY0 + slot * TW_KEYS];
    memcpy(dst, gk + src->key, (size_t)nk * sizeof(Key));
    for (int k = nk - nc; k < nk; k++) {
        u8 *c = dst[k].v.c; u32 rgb = (u32)c[0] << 16 | (u32)c[1] << 8 | c[2];
        if (r->from != ANY && rgb != r->from) continue;
        c[0] = (u8)(r->to >> 16); c[1] = (u8)(r->to >> 8); c[2] = (u8)r->to;          /* alpha stays the game's */
    }
    g_tw_anim[slot] = *src; g_tw_anim[slot].key = (u16)(TW_KEY0 + slot * TW_KEYS);
    g_tw_id[slot].src = src; g_tw_id[slot].from = r->from; g_tw_id[slot].to = r->to;
    return &g_tw_anim[slot];
}
static u8 *node_ctl(int h, u16 node) {                  /* the CD2SeqCtrl of a node of a layout instance */
    u8 *lc = FN(u8*, 0x1a65c0, int)(h);
    if (!lc) return NULL;
    u8 *nodes = *(u8**)(lc + 0x98);
    for (int i = 0; i < *(s16*)(lc + 0xa8); i++) if (*(s16*)(nodes + i * 0xb8 + 0xb0) == (s16)node) return nodes + i * 0xb8;
    return NULL;
}
static void tint_node(int h, u16 node, const Tint *rules, int nr) {
    u8 *nd = node_ctl(h, node);
    if (!nd) return;
    u8 *sd = *(u8**)(nd + 0x90), **op = *(u8***)(nd + 0xa8), *cr = *(u8**)(nd + 0x98);
    int ctl = (s8)nd[0x82];
    if (!sd || !op || !*op || !cr || ctl < 0 || ctl > 7) return;
    Anim *anims = *(Anim**)(sd + 0x70); Key *gk = *(Key**)(sd + 0x78);
    if (!anims || !gk) return;
    cr += ctl * 0x10;
    int n = (s8)nd[0xb2];
    if (*(s16*)(cr + 0xe) < n) n = *(s16*)(cr + 0xe);
    Anim *rec = anims + *(s32*)(cr + 8);
    for (int i = 1; i < n; i++) {
        u8 *ob = *op + i * 0x50;
        Anim *g = rec + i, *cur = *(Anim**)(ob + 0x30), *want = g;
        if (cur != g && !(cur >= g_tw_anim && cur < g_tw_anim + TW_SLOTS)) continue;      /* not the game's and not a twin: not ours to change */
        if ((g->kind & 0x7f) == 1 && !(g->kind & 0x80))
            for (int k = 0; k < nr; k++) if (rules[k].spr == g->spr) { Anim *t = twin_for(g, gk, &rules[k]); if (t) { want = t; break; } }
        Key *wk = want == g ? gk : g_sc_keys;
        if (cur == want && *(Key**)(ob + 0x38) == wk) continue;
        *(Key**)(ob + 0x38) = wk;
        FN(void, 0x1aa7f0, u8*, Anim*)(ob, want);
    }
}
/* the game's relation between a plate's colour and the glow it reloads with (005ff0 and 00c0ff): twice as bright */
static u32 glow_of(u32 c) {
    if (c == 0x005ff0) return 0x00c0ff;
    u32 r = (c >> 16 & 0xff) * 2, g = (c >> 8 & 0xff) * 2, b = (c & 0xff) * 2;
    return (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}
/* quads of a sprite: geometry + texture rectangle and corner colours (colour bytes R,G,B,A; 0x80 = 1.0) */
typedef struct { s16 x0, y0, x1, y1; u16 part, attr; } Grp;
typedef struct { s16 u0, v0, u1, v1; u32 col[4]; } Part;
typedef struct { Grp *g; Part *p; } Pair;
#define L2D_CloneNodeSprite(h, n, obj)    FN(int, 0x1a83f0, int, u16, int)(h, n, obj)
static int node_quads(int h, u16 node, int obj, Pair **q) { *q = NULL; return FN(int, 0x1a6980, int, u16, int, Pair**)(h, node, obj, q); }
static void quad(Pair *q, int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, u32 col) {
    q->g->x0 = (s16)x0; q->g->y0 = (s16)y0; q->g->x1 = (s16)x1; q->g->y1 = (s16)y1;
    q->p->u0 = (s16)u0; q->p->v0 = (s16)v0; q->p->u1 = (s16)u1; q->p->v1 = (s16)v1;
    q->p->col[0] = q->p->col[1] = q->p->col[2] = q->p->col[3] = col;
}
/* A row of the Magic (kind 0) or Items (1) list: one of the game's deck plates.  The game gives its plates the
   battle body (sequence 0xf, yellow) on entering battle and the field body (0xb, blue) on leaving it [140203ea0,
   on the change only; its own note of which one a plate has is the plate's flag 0x200, which is not touched].
   A row always has the field body, in its list's colour.

   The two bodies differ in one more thing.  The battle body's inside is part of its frame sprite (the three black
   quads cursor_body() turns into the gradient for the row under the cursor); the field body lays a sprite of its
   own over that - sprite 2 (object 6), black in the ready control, the glow in the reloading one - so a gradient
   put in the frame sprite does not show there.  For the row under the cursor (mode 2) that sprite itself becomes
   the gradient: its quads take the gradient's texels and its black the list's colour. */
static void row_body(int h) {
    u8 *nd = node_ctl(h, 0x5d);
    if (nd && (s8)nd[0xb2] == 14) L2D_ReplaceNodeSeq(h, 0x5d, G(int, 0x10f9ed4c), 0xb, L2D_GetControl(h));     /* 14 objects: the battle body */
}
static void row_fill(int h, int on) {
    Pair *q;
    int n = node_quads(h, 0x5d, 6, &q);
    if (n <= 0 || !q) {
        if (!on) return;                                /* nothing was changed here */
        L2D_CloneNodeSprite(h, 0x5d, 6);
        n = node_quads(h, 0x5d, 6, &q);
    }
    if (n != 3 || !q || !q[0].g || !q[2].p) return;
    int grad = q[0].p->u0 == 4 && q[0].p->v0 == 32, plain = q[0].p->u0 == 32 && q[0].p->v0 == 30;
    if (!grad && !plain) return;                        /* not the sprite we know */
    if (on && c_fill > 0) {
        int v = (int)(1.28f * c_fill + 0.5f); if (v > 255) v = 255;
        u32 col = 0xff000000u | (u32)v * 0x010101u;
        quad(&q[0], 2, 0, 9, 15,    4, 32, 16, 58, col);            /* the sprite sits at (8, 2): the same place as cursor_body()'s */
        quad(&q[1], 9, 0, 99, 15,  16, 32, 16, 58, col);
        quad(&q[2], 99, 0, 107, 15, 16, 32, 29, 58, col);
    } else if (grad) {
        quad(&q[0], 2, 0, 9, 15,   32, 30, 46, 60, 0x80808080);
        quad(&q[1], 9, 0, 98, 15,  46, 30, 46, 60, 0x80808080);
        quad(&q[2], 98, 0, 106, 15, 44, 30, 62, 60, 0x80808080);
    }
}
static void row_colours(int h, int kind, int mode) {
    u8 *nd = node_ctl(h, 0x5d);
    if (!nd || (s8)nd[0xb2] != 7) return;               /* not the field body's seven objects */
    u32 c = c_col_plate[kind];
    /* body, cursor body, reload glow; and under the cursor the black inside (it only exists in the ready control) */
    Tint r[4] = { { 0, ANY, c }, { 17, ANY, c }, { 2, 0x00c0ff, glow_of(c) }, { 2, 0x000000, c } };
    int fill = mode == 2 && c_fill > 0;
    tint_node(h, 0x5d, r, fill ? 4 : 3);
    row_fill(h, fill);
}
/* The plate under the cursor, as KH1 marks it: a stronger colour, not a brighter one.
   The deck plate's body (sprite 0 of node 0x5d, six quads) is three quads of black inside - texture 136,138..172,168 -
   and three of frame - 76,132..134,170 - drawn in the plate's colour at alpha a0.  For the cursor row this instance's
   private copy of those quads is changed: the frame gets full alpha, and the inside takes the gradient the game
   itself fills its selected plate with (texture 2,30..32,60: black at the top, the plate's colour at the bottom;
   its solid part is 4,32..29,58).  Text and icon are separate nodes and stay as they are.
   mode 0 = plain, 1 = solid frame only (cursor on an unavailable entry), 2 = frame and gradient. */
static void cursor_body(int h, u16 node, int mode) {
    for (int obj = 1; obj <= 3; obj++) {
        Pair *q;
        int n = node_quads(h, node, obj, &q);
        if (n <= 0 || !q) {
            if (!mode) continue;                            /* nothing was changed here */
            L2D_CloneNodeSprite(h, node, obj);
            n = node_quads(h, node, obj, &q);
        }
        if (n != 6 || !q || !q[0].g || !q[5].p) continue;
        if (q[3].p->u0 != 76 || q[3].p->v0 != 132 || q[5].p->u1 != 134 || q[5].p->v1 != 170) continue;    /* not the body */
        if (q[0].g->x1 != 20 && q[0].g->x1 != 17) continue;     /* sprite 82 (same art, 19 here): the reload backing */
        if (mode == 2 && c_fill > 0) {
            int v = (int)(1.28f * c_fill + 0.5f); if (v > 255) v = 255;
            u32 col = 0xff000000u | (u32)v * 0x010101u;
            quad(&q[0], 10, 2, 17, 17,   4, 32, 16, 58, col);
            quad(&q[1], 17, 2, 107, 17, 16, 32, 16, 58, col);
            quad(&q[2], 107, 2, 115, 17, 16, 32, 29, 58, col);
        } else {
            quad(&q[0], 10, 2, 20, 17,  136, 138, 156, 168, 0x80808080);
            quad(&q[1], 20, 2, 106, 17, 152, 138, 152, 168, 0x80808080);
            quad(&q[2], 106, 2, 115, 17, 154, 138, 172, 168, 0x80808080);
        }
        u32 fc = mode ? 0xff808080u : 0x80808080u;
        for (int i = 3; i < 6; i++) q[i].p->col[0] = q[i].p->col[1] = q[i].p->col[2] = q[i].p->col[3] = fc;
        return;
    }
}
/* The D-Link plate under the cursor.  The game's look (control 0 of node 1) is the plain frame in full colour, a
   longer frame laid over it (sprite 64: object 9, five quads, first one -10..-1 with texture 120,48..138,86) and a
   colour gradient inside the longer frame (sprite 65: object 10, three quads from x 1, texture 2,30..).  Here the
   longer frame is emptied and the gradient moved 8 to the left, into the plain frame. */
static void link_cursor_body(int h) {
    Pair *q;
    int n = node_quads(h, 1, 9, &q);
    if (n <= 0 || !q) { L2D_CloneNodeSprite(h, 1, 9); n = node_quads(h, 1, 9, &q); }
    if (n == 5 && q && q[0].p->u0 == 120 && q[0].p->v0 == 48 && q[0].g->x0 == -10)
        for (int i = 0; i < n; i++) q[i].g->x0 = q[i].g->x1 = q[i].g->y0 = q[i].g->y1 = 0;
    /* object 5 (sprite 66, three quads from x 1, texture 32,30..): the red backing that the gradient covered at its
       old place; it would show as a sliver at the left now */
    n = node_quads(h, 1, 5, &q);
    if (n <= 0 || !q) { L2D_CloneNodeSprite(h, 1, 5); n = node_quads(h, 1, 5, &q); }
    if (n == 3 && q && q[0].p->u0 == 32 && q[0].p->v0 == 30 && q[0].g->x0 == 1)
        for (int i = 0; i < n; i++) q[i].g->x0 = q[i].g->x1 = q[i].g->y0 = q[i].g->y1 = 0;
    n = node_quads(h, 1, 10, &q);
    if (n <= 0 || !q) { L2D_CloneNodeSprite(h, 1, 10); n = node_quads(h, 1, 10, &q); }
    if (n == 3 && q && q[0].p->u0 == 2 && q[0].p->v0 == 30 && q[0].g->x0 == 1)
        for (int i = 0; i < n; i++) { q[i].g->x0 -= 8; q[i].g->x1 -= 8; }
}
/* A ready plate.  The game's look for the plate under the cursor (control 0) is a larger plate with room for the
   button icon on its left.  The menu keeps the plain plate (control 3) and marks the cursor row by its shift to the
   right and its stronger frame and fill.  The name is white on every ready row, under the cursor or not: the name
   node (seq 0xd / 0x71) in control 1 = white, at the same place as the dim control 3 the game gives the other rows.
   Only a row that cannot be used keeps the dim name (control 4 of the plate). */
static void ready_look(int h, int selected, u16 name_node) {
    (void)selected;
    set_control(h, 3);
    set_node_control(h, name_node, 1);
}

/* The plate under the cursor carries the confirm button's icon (a text node holding the icon's character code).
   The menu does not show it: that text is kept empty (cheap: a lookup and a short string copy). */
/* after every command plate's own update */
static void (MSABI *o_plate_update)(u8 *P);
static void MSABI plate_update_hook(u8 *P) {
    o_plate_update(P);
    u8 *cmd = CMD;
    if (!cmd) return;
    u32 fl = *(u32*)(P + 0x60); int h = *(int*)(P + 0x4c);
    if ((fl & 0x80000000u) || h <= 0) return;                   /* L2D not created yet */
    int type = P[0x30];
    u8 *atk = *(u8**)(cmd + 0x1b0);
    int hud = atk && !(*(u32*)(atk + 0x60) & 0x80000000u) && L2D_IsVisible(*(int*)(atk + 0x4c));
    if (type == 2) {
        int kind = plate_is_item(P);
        int open = hud && plate_valid(P) && ((g_state == S_MAGIC && !kind) || (g_state == S_ITEM && kind)) && !link_open(cmd) && !sc_held();
        if (!open) { L2D_Show(h, 0); return; }
        int n = list_count(cmd, kind, -1, NULL), idx = list_index_of(cmd, P, kind);
        int selected = idx == g_sel[kind];
        if (selected) L2D_SetNodeText(h, 0x5d, "");
        L2D_SetPos(h, c_list_x + (selected ? c_bump : 0), list_y(kind ? E_ITEM : E_MAGIC, idx, n));
        L2D_SetPriority(h, selected ? 13 : 12);
        L2D_SetColor(h, 0xffffffff);
        if (c_list_col) row_body(h);
        if (fl & 2) { set_control(h, 4); L2D_SetNodeFrame(h, 0x5d, *(float*)(P + 0x64)); }
        else ready_look(h, selected, 0);
        int cm = !selected ? 0 : (fl & 2) ? 1 : 2;
        if (c_list_col) row_colours(h, kind, cm);
        /* the name (node 0; the text takes the node's colour): yellow when using the command now would end in
           MP charge - Cure always, anything else when MP is too low for it */
        { u16 *k = *(u16**)(P + 0x58);
          int warn = c_warn && !kind && !(fl & 2) && k && mp_would_burn(k[0]);
          L2D_SetNodeColor(h, 0, warn ? c_warn_col : 0xff808080u); }
        cursor_body(h, 0x5d, cm);
        L2D_Show(h, 1);
    } else if (type == 6) {
        if (!link_open(cmd)) return;
        int n = cmd[0x350], idx = (s8)P[0x33];
        if (idx < 0 || idx >= n) return;
        L2D_SetNodeText(h, 1, "");
        /* the game puts the whole plate in control 0 when the cursor reaches it (flag 4) */
        int selected = (fl & 4) != 0;
        if (selected && L2D_GetControl(h) == 0) {
            link_cursor_body(h);
            set_node_control(h, 2, 1);                      /* name: bright, at the plain plate's place */
            /* the two affinity hearts (nodes 3, 4): controls 0 / 1 = full / empty placed for the large plate,
               2 / 3 = the same placed for the plain one */
            for (u16 nd = 3; nd <= 4; nd++) { int c = L2D_GetNodeControl(h, nd); if (c == 0 || c == 1) L2D_SetNodeControl(h, nd, c + 2); }
        }
        else if (!selected && L2D_GetControl(h) == 3) set_node_control(h, 2, 1);   /* ready, not under the cursor: white as well */
        /* The D-Link plate's blue is in its art (the corners of sprite 63's quads are 0080ff); the game multiplies
           it with 3c3c3c for every row but the one under the cursor.  Here those rows are in full colour, like the
           rows of the other lists (their alpha, a0, stays). */
        { static const Tint bright[1] = { { 63, 0x3c3c3c, 0x808080 } };
          tint_node(h, 1, bright, c_list_col && c_link_bright ? 1 : 0); }
        L2D_SetPos(h, c_list_x + (selected ? c_bump : 0), list_y(E_LINK, idx, n));
        L2D_SetPriority(h, selected ? 13 : 12);         /* over the menu's entries and the gauge window, as the other lists */
        L2D_SetColor(h, 0xffffffff);
        L2D_Show(h, hud);
    } else if (type == 5) {
        /* the game stacks the prompt on the Attack row, one step further right per style level (table rows 4.. =
           x 65, 130, 195); here it always sits at the same place above the gauge window */
        L2D_SetPos(h, c_prompt_x, context_y(cmd));
        /* its button picture (text of node 0x5a; the layout's is f564, the confirm button): the one that answers it */
        if (c_react) { u16 *k = *(u16**)(P + 0x58); L2D_SetNodeText(h, 0x5a, k && CMD_CAT(k[0]) == 6 ? "\xf5\x64" : "\xf5\x67"); }
    } else if (type == 1 || type == 3 || type == 4) {
        /* the red Attack plate and what the game stacks beside it (style attack plates, the finisher plate): kept
           alive but not drawn; the menu's own Attack entry carries the current attack's name, and style.c offers
           the finisher as a prompt */
        L2D_SetColor(h, 0x00ffffff);
    }
}

/* The list's header: an instance of the gauge window's layout (bc01_00 layout 5) with the gauge node (0x46) off,
   which leaves node 0x47 - the label plate (sprite 34) and the letters on it (sprite 35, one quad).  The game has
   two sequences for it, 0x25a in battle (yellow) and 0x25d in the field (blue), and swaps them for its window
   [2320e0]; the header always plays the field one, with the plate and the letters in the colours of its list
   (ListColors = 0: the game's two looks, as its window).
   The letters' quad of this instance is pointed at one of the words tex.c adds to the command sheet; they are
   drawn like the game's (a 20-unit high strip at half size).  No words in the texture (sheet not recognised): no
   header. */
static int g_hdr, g_hdr_kind = -1, g_hdr_battle = -1;
static void hdr_destroy(void) { if (g_hdr > 0) L2D_Destroy(g_hdr); g_hdr = 0; g_hdr_kind = -1; }
static void hdr_frame(int kind, int battle, float x, float y) {        /* kind: 0 MAGIC, 1 ITEMS, 2 D-LINK, < 0 none */
    static const s16 word[3][4] = {
        { HDART_MAGIC_U, HDART_MAGIC_V, HDART_MAGIC_W, HDART_MAGIC_H }, { HDART_ITEMS_U, HDART_ITEMS_V, HDART_ITEMS_W, HDART_ITEMS_H },
        { HDART_DLINK_U, HDART_DLINK_V, HDART_DLINK_W, HDART_DLINK_H } };
    l2d_live(&g_hdr);
    int alive = g_hdr > 0 && L2D_GetControl(g_hdr) != 8;
    if (kind < 0 || kind > 2 || !c_hdr || !tex_hd_art_ready()) { if (alive) L2D_Show(g_hdr, 0); return; }
    if (c_list_col) battle = 0;                                     /* one look, whatever goes on: the field sequence, in the list's colours */
    if (alive && g_hdr_battle != battle) { hdr_destroy(); alive = 0; }
    if (!alive) {
        int file = G(int, 0x10f9ed48), sq = G(int, 0x10f9ed4c);
        G(u8, 0x8f88028) = 0; G(u8, 0x8f8802a) = 0;                 /* created hidden, HUD group */
        g_hdr = L2D_CreateLayout(file, 5, 0);
        if (g_hdr <= 0) { g_hdr = 0; return; }
        g_hdr_battle = battle; g_hdr_kind = -1;
        L2D_ShowNode(g_hdr, 0x46, 0);
        if (!battle) L2D_ReplaceNodeSeq(g_hdr, 0x47, sq, 0x25d, 0);
    }
    int h = g_hdr;
    Pair *q;
    int n = node_quads(h, 0x47, 2, &q);
    if (n <= 0 || !q) { L2D_CloneNodeSprite(h, 0x47, 2); n = node_quads(h, 0x47, 2, &q); g_hdr_kind = -1; }
    if (n != 1 || !q || !q[0].g || !q[0].p) { L2D_Show(h, 0); return; }                 /* not the label we know */
    if (g_hdr_kind != kind) {
        const s16 *w = word[kind];
        quad(&q[0], 0, -1, w[2] / 2, -1 + w[3] / 2, w[0], w[1], w[0] + w[2], w[1] + w[3], 0x80808080);
        g_hdr_kind = kind;
    }
    /* the plate (object 1, sprite 34: left end -2..5, a stretch 5..76 of one texture column, right end 76..90):
       longer, so the header is as long as the rows under it */
    n = node_quads(h, 0x47, 1, &q);
    if (n <= 0 || !q) { L2D_CloneNodeSprite(h, 0x47, 1); n = node_quads(h, 0x47, 1, &q); }
    int wide = (int)(c_hdr_wide + 0.5f);
    if (wide > 0 && n == 3 && q && q[1].g && q[2].g && q[1].g->x0 == 5 && q[1].g->x1 == 76 && q[2].g->x0 == 76 && q[1].p->u0 == q[1].p->u1) {
        q[1].g->x1 = (s16)(76 + wide); q[2].g->x0 = (s16)(76 + wide); q[2].g->x1 = (s16)(q[2].g->x1 + wide);
    }
    if (c_list_col) {
        Tint r[2] = { { 34, ANY, c_col_plate[kind] }, { 35, ANY, c_col_title[kind] } };     /* the plate, the letters */
        tint_node(h, 0x47, r, 2);
    }
    L2D_SetPos(h, x, y);
    L2D_SetPriority(h, 12);
    L2D_SetColor(h, 0xffffffff);
    L2D_Show(h, 1);
}

static int g_sc_entry[SC_ROWS], g_sc_key[SC_ROWS];
void menu_own(char *out, int n) {
    static const char *en[4] = { "attack", "magic", "items", "dlink" };
    for (int i = 0; i < 4; i++) own_add(out, n, en[i], g_entry[i]);
    for (int i = 0; i < SC_ROWS; i++) { char nm[8]; snprintf(nm, sizeof nm, "sc%d", i); own_add(out, n, nm, g_sc_entry[i]); }
    own_add(out, n, "listhdr", g_hdr);
}
static void entries_destroy(void) {
    hdr_destroy();
    for (int i = 0; i < 4; i++) { if (g_entry[i] > 0) L2D_Destroy(g_entry[i]); g_entry[i] = 0; }
    for (int i = 0; i < SC_ROWS; i++) { if (g_sc_entry[i] > 0) L2D_Destroy(g_sc_entry[i]); g_sc_entry[i] = 0; }
    g_attack_id = -1;
}
/* Everything this file has on screen goes: called when the player's gauge object is destroyed, because nothing in
   here runs once it is gone (menu_frame is driven by its update) and the game may unload bc01_00.l2d right after -
   it does at the end of Terra's first forced fight in Castle of Dreams, with no "destroy every instance" first.
   The four entries and a Command Style offer were left behind there, alive and visible over freed data: the crash
   in the texture bind seen twice. */
void menu_shutdown(void) {
    entries_destroy(); sc_set_hud(0);
    g_tw_n = 0;                                 /* the twins belong to a file that may be gone next: made anew when wanted */
    style_shutdown();
}
static int entry_alive(int i) { l2d_live(&g_entry[i]); return g_entry[i] > 0 && L2D_GetControl(g_entry[i]) != 8; }
static void entry_create(int e, int battle) {
    static const u16 icon[4] = { 0x20, 0x21, 0x22, 0 };         /* keyblade, magic hat, item bottle, none */
    int file = G(int, 0x10f9ed48), sq = G(int, 0x10f9ed4c);     /* bc01_00.l2d and its sprite block */
    G(u8, 0x8f88028) = 0; G(u8, 0x8f8802a) = 0;                 /* created hidden, HUD group */
    int h = L2D_CreateLayout(file, 3, 3);                       /* layout 3 = one deck plate */
    if (h <= 0) { g_entry[e] = 0; return; }
    g_entry[e] = h;
    if (!battle) L2D_ReplaceNodeSeq(h, 0x5d, sq, 0xb, 3);       /* field colours, as the game does for its plates */
    if (icon[e]) L2D_ReplaceNodeSeq(h, 0x5a, sq, icon[e], 3); else L2D_ShowNode(h, 0x5a, 0);
    L2D_ShowNode(h, 0x5c, 0); L2D_ShowNode(h, 0x5b, 0);         /* item count */
    L2D_SetNodeText(h, 0x5d, "");                               /* confirm button icon */
    if (e != E_ATTACK) L2D_SetNodeText(h, 0, c_text[e - 1]);
    else g_attack_id = -1;
}
/* The shortcut list, shown in place of Attack / Magic / Item / Link while L1 is held: one row per button, with the
   button's picture and the name of the command in that button's deck slot.

   A row is the game's own look for the deck plate under its cursor - sequence 0xb of the plate body (node 0x5d) in
   control 0: sprite 17, a longer plate whose left end is a solid tab, and a text object in the middle of that tab,
   (11, 8), which holds the confirm button's picture (text table entry 16, centred both ways).  Here the text is the
   row's button instead.
   Grey.  Every colour of the plate comes from the colour keys of its animations (blue in the field, yellow in
   battle) on grey texels, and colours only multiply, so a tint cannot take the colour out.  The animation each object
   plays is a pointer of the object though (CD2Obj +0x30, with the key table at +0x38; set from the control's records
   by 1401abb80 -> 1401aa7f0, read while drawing by 1401a87b0): the seven objects of this instance's body get records
   of our own, with grey keys.  The same in the field and in battle.
   Objects of sequence 0xb: 0 root (scale 0.88), 1 sprite 0 (the plain body), 2 sprite 17, 3 sprite 11, 4 the text,
   5 sprite 16 (reload fill), 6 sprite 2 (three quads; here the gradient the game fills its selected plate with). */
#define SC_OBJS 7
static Anim g_sc_anim[SC_OBJS];
static int  g_sc_ready;
static void key_f(int i, float v) { g_sc_keys[i].t = 0; g_sc_keys[i].v.f = v; g_sc_keys[i].interp = 0; }
static void key_c(int i, u32 rgb) { g_sc_keys[i].t = 0; g_sc_keys[i].v.c[0] = (u8)(rgb >> 16); g_sc_keys[i].v.c[1] = (u8)(rgb >> 8); g_sc_keys[i].v.c[2] = (u8)rgb; g_sc_keys[i].v.c[3] = 0xff; g_sc_keys[i].interp = 0; }
static void sc_anims(void) {
    static const s16 spr[SC_OBJS] = { -1, 0, 17, 11, -1, 16, 2 };
    memset(g_sc_anim, 0, sizeof g_sc_anim);
    for (int i = 0; i < SC_OBJS; i++) { g_sc_anim[i].spr = spr[i]; g_sc_anim[i].maxf = -1; g_sc_anim[i].kind = 0x81; }     /* not drawn */
    Anim *a;
    key_f(0, 0.88f); key_f(1, 0.88f);
    a = &g_sc_anim[0]; a->maxf = 0; a->kind = 0; a->key = 0; a->keyn[K_SCALEX] = 1; a->keyn[K_SCALEY] = 1;
    key_f(2, 10.0f); key_c(3, c_sc_col);
    a = &g_sc_anim[2]; a->maxf = 0; a->kind = 1; a->key = 2; a->keyn[K_BASEX] = 1; a->keyn[K_COLOR] = 1;
    key_f(4, c_sc_icon_x + c_sc_pad * 0.5f); key_f(5, c_sc_icon_y);
    a = &g_sc_anim[4]; a->maxf = 0; a->kind = 2; a->key = 4; a->keyn[K_BASEX] = 1; a->keyn[K_BASEY] = 1;
    key_f(6, 18.0f); key_f(7, 2.0f); key_c(8, c_sc_fill);
    a = &g_sc_anim[6]; a->maxf = 0; a->kind = 1; a->key = 6; a->keyn[K_BASEX] = 1; a->keyn[K_BASEY] = 1; a->keyn[K_COLOR] = 1;
    g_sc_ready = 1;
}
/* the body node's objects of a deck plate instance, if they are the seven of sequence 0xb */
static u8 *sc_objs(int h) {
    u8 *lc = FN(u8*, 0x1a65c0, int)(h);
    if (!lc) return NULL;
    u8 *nodes = *(u8**)(lc + 0x98);
    for (int i = 0; i < *(s16*)(lc + 0xa8); i++) {
        u8 *nd = nodes + i * 0xb8;
        if (*(s16*)(nd + 0xb0) != 0x5d) continue;
        if ((s8)nd[0xb2] != SC_OBJS || !*(u8***)(nd + 0xa8)) return NULL;
        u8 *o = **(u8***)(nd + 0xa8);
        if (!o || o[1 * 0x50 + 0x48] != 6 || o[2 * 0x50 + 0x48] != 5 || o[6 * 0x50 + 0x48] != 3 || !*(u8**)(o + 4 * 0x50 + 0x10)) return NULL;
        return o;
    }
    return NULL;
}
/* our records on the body, and the two private sprites; every frame (a control change puts the game's back) */
static int sc_body(int h) {
    u8 *o = sc_objs(h);
    if (!o) return 0;
    if (!g_sc_ready) sc_anims();
    for (int i = 0; i < SC_OBJS; i++, o += 0x50) {
        if (*(Anim**)(o + 0x30) == &g_sc_anim[i] && *(Key**)(o + 0x38) == g_sc_keys) continue;
        *(Key**)(o + 0x38) = g_sc_keys;
        FN(void, 0x1aa7f0, u8*, Anim*)(o, &g_sc_anim[i]);       /* the record and, from its kind, "drawn or not" */
    }
    Pair *q;
    int pad = (int)(c_sc_pad + 0.5f);
    /* inside: the gradient (texture 2,30..32,60; black at the top), from where the tab ends */
    int n = node_quads(h, 0x5d, 6, &q);
    if (n <= 0 || !q) { L2D_CloneNodeSprite(h, 0x5d, 6); n = node_quads(h, 0x5d, 6, &q); }
    if (n == 3 && q && q[0].g && q[2].p && q[0].p->u0 != 2) {
        quad(&q[0], 1 + pad, 0, 8 + pad, 15,   2, 30, 16, 60, 0x80808080);
        quad(&q[1], 8 + pad, 0, 99, 15,       16, 30, 16, 60, 0x80808080);
        quad(&q[2], 99, 0, 107, 15,           16, 30, 32, 60, 0x80808080);
    }
    /* a wider tab: the solid column of the long plate (quad 1, -1..8) grows, the stretch after it shrinks */
    if (pad > 0) {
        n = node_quads(h, 0x5d, 2, &q);
        if (n <= 0 || !q) { L2D_CloneNodeSprite(h, 0x5d, 2); n = node_quads(h, 0x5d, 2, &q); }
        if (n == 5 && q && q[1].g && q[1].g->x0 == -1 && q[1].g->x1 == 8 && q[3].g->x0 == 17) {
            q[1].g->x1 = (s16)(8 + pad); q[2].g->x0 = (s16)(8 + pad); q[2].g->x1 = (s16)(17 + pad); q[3].g->x0 = (s16)(17 + pad);
        }
    }
    return 1;
}
/* The "COMMANDS" window above the list is grey too while the list is shown (not a Command Style's or a D-Link's
   window: those are other instances and are left alone).  It is the game's own instance (cmd+0x98: node 0x46 with
   the gauge and its frame, sprite 33; node 0x47 with the label plate, sprite 34, and the letters, sprite 35), so
   nothing of it is replaced for good: the three objects play a twin of the record the game gave them - the same
   record with the same keys, copied into our key table with the colour keys (the last ones of a record) in grey;
   alpha, positions and fades stay the game's.  When the list goes the game's record and key table are put back.
   A control change by the game puts its record back by itself but leaves our key table on the object: that is seen
   here on the next pass (the root object, never touched, has the node's own table). */
#define HD_SLOTS 3
#define HD_KEYS  40
#define HD_KEY0  16                     /* first key of the twins in g_sc_keys (the list's rows use 0..8) */
static Anim  g_hd_anim[HD_SLOTS];
static Anim *g_hd_src[HD_SLOTS];
static u8 *node_objs(int h, u16 node, int *count) {
    u8 *lc = FN(u8*, 0x1a65c0, int)(h);
    if (!lc) return NULL;
    u8 *nodes = *(u8**)(lc + 0x98);
    for (int i = 0; i < *(s16*)(lc + 0xa8); i++) {
        u8 *nd = nodes + i * 0xb8;
        if (*(s16*)(nd + 0xb0) != (s16)node) continue;
        if (!*(u8***)(nd + 0xa8)) return NULL;
        *count = (s8)nd[0xb2];
        return **(u8***)(nd + 0xa8);
    }
    return NULL;
}
/* the twin's colours: the frame takes the list's frame colour of the set shown (gold for set 2), the label plate a
   shade darker as with set 1 (ShortcutHeaderPlate against ShortcutColor), the letters their own */
static u32 hd_rgb(int slot) {
    if (slot == 2) return c_hd_text;
    if (!sc_page()) return slot == 0 ? c_sc_col : c_hd_plate;
    if (slot == 0) return c_sc_col2;
    u32 out = 0;                                /* set 1's plate / frame ratio, per channel, applied to set 2's colour */
    for (int sh = 0; sh <= 16; sh += 8) {
        u32 f = (c_sc_col >> sh) & 0xff, p = (c_hd_plate >> sh) & 0xff, c = (c_sc_col2 >> sh) & 0xff;
        u32 v = f ? c * p / f : p; if (v > 0xff) v = 0xff;
        out |= v << sh;
    }
    return out;
}
static void hd_recolour(int slot) {
    Anim *a = &g_hd_anim[slot];
    int nk = 0, nc = a->keyn[K_COLOR];
    for (int k = 0; k < 11; k++) nk += a->keyn[k];
    if (!nc || nk > HD_KEYS) return;
    Key *dst = &g_sc_keys[HD_KEY0 + slot * HD_KEYS];
    u32 rgb = hd_rgb(slot);
    for (int k = nk - nc; k < nk; k++) { dst[k].v.c[0] = (u8)(rgb >> 16); dst[k].v.c[1] = (u8)(rgb >> 8); dst[k].v.c[2] = (u8)rgb; }
}
static void hd_node(int h, u16 node, int grey) {
    int n = 0;
    u8 *o = node_objs(h, node, &n);
    if (!o || n < 2) return;
    Key *gk = *(Key**)(o + 0x38);
    if (!gk || gk == g_sc_keys) return;
    for (int i = 1; i < n; i++) {
        u8 *ob = o + i * 0x50;
        Anim *cur = *(Anim**)(ob + 0x30);
        if (!cur) continue;
        if (cur >= g_hd_anim && cur < g_hd_anim + HD_SLOTS) {           /* our twin */
            if (grey) { hd_recolour((int)(cur - g_hd_anim)); continue; }          /* the set shown may have changed */
            *(Key**)(ob + 0x38) = gk;
            FN(void, 0x1aa7f0, u8*, Anim*)(ob, g_hd_src[cur - g_hd_anim]);
            continue;
        }
        if (*(Key**)(ob + 0x38) == g_sc_keys) *(Key**)(ob + 0x38) = gk;  /* the game's record again, our table still on it */
        if (!grey) continue;
        int slot = cur->spr == 33 ? 0 : cur->spr == 34 ? 1 : cur->spr == 35 ? 2 : -1;
        if (slot < 0 || (cur->kind & 0x7f) != 1 || (cur->kind & 0x80)) continue;
        int nk = 0, nc = cur->keyn[K_COLOR];
        for (int k = 0; k < 11; k++) nk += cur->keyn[k];
        if (!nc || nk > HD_KEYS) continue;
        Key *dst = &g_sc_keys[HD_KEY0 + slot * HD_KEYS];
        memcpy(dst, gk + cur->key, (size_t)nk * sizeof(Key));
        u32 rgb = hd_rgb(slot);
        for (int k = nk - nc; k < nk; k++) { dst[k].v.c[0] = (u8)(rgb >> 16); dst[k].v.c[1] = (u8)(rgb >> 8); dst[k].v.c[2] = (u8)rgb; }
        g_hd_anim[slot] = *cur; g_hd_anim[slot].key = (u16)(HD_KEY0 + slot * HD_KEYS);
        g_hd_src[slot] = cur;
        *(Key**)(ob + 0x38) = g_sc_keys;
        FN(void, 0x1aa7f0, u8*, Anim*)(ob, &g_hd_anim[slot]);
    }
}
static void hd_frame(u8 *cmd, int grey) {
    int h = *(int*)(cmd + 0x98);
    if (h <= 0 || L2D_GetControl(h) == 8) return;
    grey = grey && c_hd;
    hd_node(h, 0x46, grey); hd_node(h, 0x47, grey);
}
static void sc_frame(u8 *cmd, int show, int battle) {
    (void)battle;
    if (show) {                             /* the frame's colour tells the two sets apart */
        if (!g_sc_ready) sc_anims();
        key_c(3, sc_page() ? c_sc_col2 : c_sc_col);
    }
    for (int r = 0; r < SC_ROWS; r++) {
        l2d_live(&g_sc_entry[r]);
        int h = g_sc_entry[r];
        int alive = h > 0 && L2D_GetControl(h) != 8;
        if (!show) { if (alive) L2D_Show(h, 0); continue; }
        if (!alive) {
            int file = G(int, 0x10f9ed48), sq = G(int, 0x10f9ed4c);
            G(u8, 0x8f88028) = 0; G(u8, 0x8f8802a) = 0;
            h = L2D_CreateLayout(file, 3, 0);
            if (h <= 0) { g_sc_entry[r] = 0; continue; }
            g_sc_entry[r] = h; g_sc_key[r] = -2;
            L2D_ReplaceNodeSeq(h, 0x5d, sq, 0xb, 0);                    /* always the field body: seven objects */
            L2D_ShowNode(h, 0x5a, 0);                                   /* no category icon */
            L2D_SetNodeText(h, 0x5d, sc_row_icon(r));                   /* the button, where the game shows its confirm button */
            FN(int, 0x1a7ec0, int, u16, float, float)(h, 0, -1.0f + c_sc_pad, 0.0f);     /* name: after the tab */
        }
        int slot = sc_slot(r);
        u8 *P = slot >= 0 ? slot_plate(cmd, slot) : NULL;
        u16 *k = P ? *(u16**)(P + 0x58) : NULL;
        int id = k ? k[0] : 0, item = P && plate_is_item(P), count = item ? ((u8*)k)[2] : 0;
        int key = id | count << 16 | item << 24;
        if (key != g_sc_key[r]) {
            g_sc_key[r] = key;
            const char *name = id > 0 && id < 0x23a ? G(const char*, 0x814908 + (u32)id * 0x18) : NULL;
            L2D_SetNodeText(h, 0, name ? name : "---");
            L2D_ShowNode(h, 0x5c, item); L2D_ShowNode(h, 0x5b, item);  /* item count, as on the game's plates */
            if (item) { char n[8]; snprintf(n, sizeof n, "%d", count); L2D_SetNodeText(h, 0x5b, n); }
            /* an item also gets its category icon, the bottle of the menu's Item rows (sequence 0x22 in control 3:
               the small icon, drawn at x 103 for the plain plate; this plate is longer, and its item count is at
               111 where the plain plate's is at 100).  Attack and magic commands get no icon here. */
            if (item) {
                L2D_ReplaceNodeSeq(h, 0x5a, G(int, 0x10f9ed4c), 0x22, 3);
                FN(int, 0x1a7ec0, int, u16, float, float)(h, 0x5a, c_sc_item_dx, 0.0f);
            }
            L2D_ShowNode(h, 0x5a, item);
        }
        u32 fl = P ? *(u32*)(P + 0x60) : 2;
        int usable = P && !(fl & 2) && !deck_sealed(cmd) && (item || !mp_in_burn());
        L2D_SetPos(h, c_main_x, c_row_y[r]);
        L2D_SetPriority(h, 5);
        set_control(h, 0);                                              /* the long plate: name and item count at its places */
        if (item) set_node_control(h, 0x5a, 3);                         /* the still icon, not the cursor row's pulsing one */
        int grey = sc_body(h);
        if (!grey) { set_control(h, 4); L2D_SetNodeFrame(h, 0x5d, 0.0f); }     /* not the plate we know: the plain "not available" look */
        /* name: white, dim when the command cannot be used now, yellow when it would end in MP charge */
        u32 nc = !usable ? (0xff000000u | (c_sc_dim & 0xff0000) >> 16 | (c_sc_dim & 0xff00) | (c_sc_dim & 0xff) << 16)
               : c_warn && !item && mp_would_burn(id) ? c_warn_col : 0xff808080u;
        if (!grey) set_node_control(h, 0, 1);
        L2D_SetNodeColor(h, 0, nc);
        L2D_SetColor(h, 0xffffffff);
        L2D_Show(h, 1);
    }
}
/* The gauge window's bar has a drop shadow: sprite 32 (first quad 8,1..80,19, uv 224,2..224,38) drawn black at
   alpha 0x50.  It reaches down into the Attack entry, so it is emptied.  It is object 2 of node 0x46 in the battle
   skin (seq 0x258) and object 1 in the field skin (0x259); the object's quads are a private copy of this instance
   (1a83f0), made again whenever the game swaps the skin or the control. */
static void strip_gauge_shadow(int h) {
    for (int obj = 1; obj <= 2; obj++) {
        Pair *q = NULL;
        int n = FN(int, 0x1a6980, int, u16, int, Pair**)(h, 0x46, obj, &q);
        if (n <= 0 || !q) {
            FN(int, 0x1a83f0, int, u16, int)(h, 0x46, obj);
            q = NULL; n = FN(int, 0x1a6980, int, u16, int, Pair**)(h, 0x46, obj, &q);
        }
        if (n < 3 || !q || !q[0].g || !q[0].p) continue;
        if (q[0].p->u0 != 224 || q[0].p->v0 != 2 || q[0].p->u1 != 224 || q[0].p->v1 != 38) continue;      /* not the bar shape */
        if (q[0].g->x0 != 8 || q[0].g->y0 != 1 || q[0].g->x1 != 80) continue;                             /* not the shadow (or done) */
        for (int i = 0; i < n; i++) q[i].g->x0 = q[i].g->x1 = q[i].g->y0 = q[i].g->y1 = 0;
    }
}
/* once per frame, from the HP gauge's update */
void menu_frame(u8 *gauge) {
    (void)gauge;
    if (!c_menu) {          /* the game's own deck: only the Command Style offer is drawn, above the gauge window */
        u8 *c = CMD, *a = c ? *(u8**)(c + 0x1b0) : NULL;
        int vis = a && !(*(u32*)(a + 0x60) & 0x80000000u) && L2D_IsVisible(*(int*)(a + 0x4c));
        style_frame(c, vis, c_prompt_x, (float)G(s16, 0x818308 + 4) - 22.0f);
        return;
    }
    u8 *cmd = CMD;
    if (!cmd) { entries_destroy(); sc_set_hud(0); style_frame(NULL, 0, 0, 0); return; }
    int battle = (GFLAGS & 1) != 0;
    if (battle != g_entry_battle) { entries_destroy(); g_entry_battle = battle; }
    u8 *atk = *(u8**)(cmd + 0x1b0);
    int hud = atk && !(*(u32*)(atk + 0x60) & 0x80000000u) && L2D_IsVisible(*(int*)(atk + 0x4c));
    sc_set_hud(hud && !link_open(cmd));
    int sc = hud && sc_held();
    sc_frame(cmd, sc, battle);
    hd_frame(cmd, sc);
    /* an open list: its header, and the menu behind it darker */
    int open_e = !hud || sc ? 0 : link_open(cmd) ? E_LINK : g_state == S_MAGIC ? E_MAGIC : g_state == S_ITEM ? E_ITEM : 0;
    int open_n = open_e == E_LINK ? cmd[0x350] : open_e ? list_count(cmd, open_e == E_ITEM, -1, NULL) : 0;
    if (open_n < 1) open_e = 0;
    hdr_frame(open_e ? open_e - 1 : -1, battle, c_list_x + c_hdr_dx, list_y(open_e, 0, open_n) + c_hdr_dy);
    u32 entry_col = 0xffffffffu;
    if (open_e && c_list_dim) { u32 v = (u32)(c_list_dim_level * 2.55f + 0.5f); entry_col = 0xff000000u | v * 0x010101u; }
    for (int e = 0; e < 4; e++) {
        if (!entry_alive(e)) { entry_create(e, battle); if (!g_entry[e]) continue; }
        int h = g_entry[e];
        int usable = e == E_ATTACK ? !(atk && (*(u32*)(atk + 0x60) & 0x2000)) : entry_usable(cmd, e);
        int selected = g_cur == e && !(e != E_LINK && link_open(cmd));
        if (e == E_ATTACK && atk) {
            /* the name of the attack of the current style level (plates +0x1b0, +0x1b8, +0x1c0) */
            int lv = *(int*)(cmd + 0x1a8); if (lv < 0 || lv > 2) lv = 0;
            u8 *ap = *(u8**)(cmd + 0x1b0 + lv * 8);
            if (!ap) ap = atk;
            u16 *k = *(u16**)(ap + 0x58); int id = k ? k[0] : 0;
            if (id != g_attack_id && id > 0 && id < 0x23a) { g_attack_id = id; L2D_SetNodeText(h, 0, G(const char*, 0x814908 + (u32)id * 0x18)); }
        }
        L2D_SetPos(h, c_main_x + (selected ? c_bump : 0), c_row_y[e]);
        L2D_SetPriority(h, selected ? 7 : 5);
        if (!usable) {
            set_control(h, 4);
            L2D_SetNodeFrame(h, 0x5d, 0.0f);                    /* plain "unavailable" look; the MP bar shows the recharge */
        } else ready_look(h, selected, 0);
        cursor_body(h, 0x5d, !selected ? 0 : !usable ? 1 : 2);
        L2D_SetColor(h, entry_col);
        L2D_Show(h, hud && !sc);
    }
    /* the gauge window(s): normal, current (a style's), outgoing, fading - a little higher than in the game */
    int seen[4] = {0};
    for (int i = 0; i < 4; i++) {
        int h = *(int*)(cmd + 0x98 + i * 4), dup = 0;
        for (int j = 0; j < i; j++) if (seen[j] == h) dup = 1;
        seen[i] = h;
        if (h > 0 && !dup && L2D_GetControl(h) != 8) L2D_SetPos(h, *(float*)(cmd + 0x120), head_y());
    }
    if (c_noshadow && seen[0] > 0 && L2D_GetControl(seen[0]) != 8) strip_gauge_shadow(seen[0]);     /* the game's own window (+0x98) */
    /* While the D-Link list is open the game dims the gauge window and lays its "D-LINK" header (+0xc4, which
       also carries three shadow plates for the old list rows) over it.  The menu shows neither: the header is
       kept off the screen (its state still drives the game's list logic) and the gauge window stays as it is. */
    if (*(int*)(cmd + 0xc4) > 0) L2D_SetPos(*(int*)(cmd + 0xc4), -2000.0f, -2000.0f);
    if (link_open(cmd) && *(int*)(cmd + 0x9c) > 0) L2D_SetColor(*(int*)(cmd + 0x9c), 0xffffffff);
    hide_old_parts(cmd);
    /* the Command Style offer: above the gauge window, and above a context prompt if one is up as well */
    style_frame(cmd, hud, c_prompt_x, offer_y(cmd));
}

/* ---------------- install ---------------- */
#define VT_PLATE_UPDATE 0x646018        /* PL::CCommandPlate vftable[1] -> 204880 */
#define VT_CMD_UPDATE   0x647b88        /* PL::CPlayerCommand vftable[1] -> 2363d0 */
static int call_ok(u32 rva, u32 target) {
    u8 *p = g_base + rva; s32 d; memcpy(&d, p + 1, 4);
    return p[0] == 0xE8 && (u32)(rva + 5 + d) == target;
}
int menu_check(void) {
    load_ini();
    if (!c_menu) return 1;
    int bad = 0;
    if (memcmp(g_base + S_deck_input.rva, S_deck_input.bytes, S_deck_input.len)) { LOG("menu: deck input site does not match"); bad++; }
    if (memcmp(g_base + S_confirm.rva, S_confirm.bytes, S_confirm.len)) { LOG("menu: confirm test site does not match"); bad++; }
    if (G(u64, VT_PLATE_UPDATE) != (u64)(g_base + 0x204880) || G(u64, VT_CMD_UPDATE) != (u64)(g_base + 0x2363d0)) { LOG("menu: vtable slots do not match"); bad++; }
    if (!call_ok(0x238902, 0x272cd0) || !call_ok(0x2333f1, 0x272c20) || !call_ok(0x2332fe, 0x272c20) || !call_ok(0x236c92, 0x272c20)) { LOG("menu: D-Link pad call sites do not match"); bad++; }
    if (!call_ok(0x236b97, 0x272860)) { LOG("menu: prompt pad call site does not match"); bad++; }
    if (!call_ok(0x21fb99, 0x272f10) || !call_ok(0x2337e9, 0x272d30)) { LOG("menu: jump / list close pad call sites do not match"); bad++; }
    if (!call_ok(0x2332a1, 0x205cf0) || !call_ok(0x233491, 0x205cf0)) { LOG("menu: D-Link list move call sites do not match"); bad++; }
    /* placement table rows 2..7: the Attack plate and what stacks on it, y 247 / 249 */
    if (G(s16, 0x818310 + 4) != 247 || G(s16, 0x818318 + 4) != 249) { LOG("menu: placement table does not match"); bad++; }
    return bad == 0;
}
void menu_apply(void) {
    if (!c_menu) { LOG("menu: off"); return; }
    hook_fn(&S_deck_input, menu_input, "deck input");
    o_confirm = hook_fn(&S_confirm, confirm_hook, "confirm");
    o_plate_update = (void*)(g_base + 0x204880); o_cmd_update = (void*)(g_base + 0x2363d0);
    void *f; u64 old;
    f = (void*)plate_update_hook; old = (u64)(g_base + 0x204880); patch_bytes(VT_PLATE_UPDATE, (u8*)&old, (u8*)&f, 8, "plate vtable");
    f = (void*)cmd_update_hook;   old = (u64)(g_base + 0x2363d0); patch_bytes(VT_CMD_UPDATE, (u8*)&old, (u8*)&f, 8, "cmd vtable");
    hook_call(0x238902, 0x272cd0, link_open_pad, "link open pad");
    hook_call(0x2333f1, 0x272c20, link_confirm_pad, "link confirm pad");
    hook_call(0x2332fe, 0x272c20, link_confirm_pad, "link end pad");
    hook_call(0x236c92, 0x272c20, followup_pad, "follow-up pad");
    hook_call(0x236b97, 0x272860, prompt_pad, "prompt pad");
    hook_call(0x21fb99, 0x272f10, jump_pad, "jump pad");
    hook_call(0x2337e9, 0x272d30, link_close_pad, "link close pad");
    hook_call(0x2332a1, 0x205cf0, link_scroll, "link list move");
    hook_call(0x233491, 0x205cf0, link_scroll, "link list move");
    LOG("menu: on");
}

void menu_dbg(const char *cmd_, char *args) {
    (void)cmd_;
    char sub[16] = {0}; float a = 0, b = 0, c = 0, d = 0; int n = sscanf(args, "%15s %f %f %f %f", sub, &a, &b, &c, &d);
    if (!strcmp(sub, "main") && n >= 5) { c_main_x = a; c_row_y[1] = b; c_row_y[2] = c; c_row_y[3] = d; }
    else if (!strcmp(sub, "list") && n >= 3) { c_list_x = a; c_pitch = b; if (n >= 5) { c_hdr_dx = c; c_hdr_dy = d; } }
    else if (!strcmp(sub, "attack") && n >= 2) c_row_y[0] = a;      /* y of the Attack row */
    else if (!strcmp(sub, "bump") && n >= 2) c_bump = a;
    else if (!strcmp(sub, "fill") && n >= 2) c_fill = a;
    else if (!strcmp(sub, "header") && n >= 2) c_head_dy = a;
    else if (!strcmp(sub, "prompt") && n >= 3) { c_prompt_x = a; c_prompt_y = b; if (n >= 4) c_prompt_raise = c; }
    u8 *cmd = CMD;
    LOG("  header y %.1f (table %d, shift %.1f)  prompt %.1f %.1f", head_y(), G(s16, 0x818308 + 4), c_head_dy, c_prompt_x, c_prompt_y);
    LOG("  menu: state %d cursor %d sel %d/%d entries %d %d %d %d  main x %.0f y %.0f %.0f %.0f %.0f  list x %.0f bottom %.0f pitch %.0f",
        g_state, g_cur, g_sel[0], g_sel[1], g_entry[0], g_entry[1], g_entry[2], g_entry[3], c_main_x, c_row_y[0], c_row_y[1], c_row_y[2], c_row_y[3], c_list_x, row_y(3), c_pitch);
    if (cmd) LOG("  cmd flags %08x %08x  magic %d items %d links %d  attack plate visible %d", *(u32*)(cmd + 0x60), *(u32*)(cmd + 0x64),
                 list_count(cmd, 0, -1, NULL), list_count(cmd, 1, -1, NULL), cmd[0x350],
                 *(u8**)(cmd + 0x1b0) ? L2D_IsVisible(*(int*)(*(u8**)(cmd + 0x1b0) + 0x4c)) : -1);
}

#ifndef _WIN32
u8 *test_slot_plate(u8 *cmd, int slot) { return slot_plate(cmd, slot); }
int *test_sc_entries(void) { return g_sc_entry; }
void test_sc_frame(u8 *cmd, int show, int battle) { sc_frame(cmd, show, battle); }
int test_prompt_pad(u8 *pad) { return prompt_pad(pad); }
int test_jump_pad(u8 *pad) { return jump_pad(pad); }
int test_link_close_pad(u8 *pad) { return link_close_pad(pad); }
int *test_link_close(void) { return &g_link_close; }
float test_context_y(u8 *cmd) { return context_y(cmd); }
float test_offer_y(u8 *cmd) { return offer_y(cmd); }
void *test_sc_anims(void) { return g_sc_anim; }
void *test_sc_keys(void) { return g_sc_keys; }
void test_strip_gauge_shadow(int h) { strip_gauge_shadow(h); }
void test_cursor_body(int h, int mode) { cursor_body(h, 0x5d, mode); }
void test_link_cursor_body(int h) { link_cursor_body(h); }
int *test_menu_state(void) { return &g_state; }
int *test_menu_cur(void) { return &g_cur; }
int *test_menu_sel(void) { return g_sel; }
int test_menu_input(u8 *cmd) { return menu_input(cmd); }
void test_cmd_update(u8 *cmd) { cmd_update_hook(cmd); }
void test_hd_frame(u8 *cmd, int grey) { hd_frame(cmd, grey); }
void test_hdr_frame(int kind, int battle, float x, float y) { hdr_frame(kind, battle, x, y); }
int *test_hdr(void) { return &g_hdr; }
float test_list_y(int e, int idx, int n) { return list_y(e, idx, n); }
float *test_list_x(void) { return &c_list_x; }
u64 test_link_scroll(u8 *P, int dir) { return link_scroll(P, dir); }
float *test_link_time(void) { return &c_link_time; }
#endif
#ifndef _WIN32
int test_menu_step(u8 *cmd) {       /* what the command update does around the deck input handler */
    g_in_cmd = 1; g_swallow = swallow(cmd);
    int r = menu_input(cmd); g_in_cmd = 0; return r;
}
int test_confirm(u8 *pad, u8 *cmd) { g_in_cmd = 1; g_swallow = swallow(cmd); int r = FN(int, 0x272860, u8*)(pad); g_in_cmd = 0; return r; }
void test_plate_update(u8 *P) { plate_update_hook(P); }
void test_row_colours(int h, int kind) { row_body(h); row_colours(h, kind, 0); }
void test_row_colours_m(int h, int kind, int mode) { row_body(h); row_colours(h, kind, mode); cursor_body(h, 0x5d, mode); }
void test_tint_node(int h, u16 node, s16 spr, u32 from, u32 to) { Tint r = { spr, from, to }; tint_node(h, node, &r, to == 0xfffffffeu ? 0 : 1); }
void *test_obj_anim(int h, u16 node, int obj, void **keys) { u8 *nd = node_ctl(h, node); if (!nd) return NULL; u8 *ob = **(u8***)(nd + 0xa8) + obj * 0x50; if (keys) *keys = *(void**)(ob + 0x38); return *(void**)(ob + 0x30); }
int test_node_objs(int h, u16 node) { u8 *nd = node_ctl(h, node); return nd ? (s8)nd[0xb2] : -1; }
u32 *test_col_plate(void) { return c_col_plate; }
u32 *test_col_title(void) { return c_col_title; }
int *test_list_col(void) { return &c_list_col; }
int test_twins(void) { return g_tw_n; }
#endif

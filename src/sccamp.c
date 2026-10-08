/* The "Shortcuts" entry of the menu's Command Decks screen: where the L1 shortcuts (shortcut.c) are set.

   The screen is CCampDeckTop (vtable 140674c70, update 140402fa0, state byte +0x9b: 2 = browsing).  Its children:
   +0xb8 the list on the left, CCampDeckTopCmd (vtable 140674d40; layout 0xcc of camp.l2d; cursor index +0x8e, 0..4
   = Edit Deck, Meld Commands, Command List, Choose Deck, Finish Commands; hand cursor +0xe8, light frame +0xf0;
   plate positions +0xa8), and +0xc0 the Battle Commands pane, CCampDeckTopBtl (layout 0xc9: eight slot anchors,
   nodes 1..8, 18 apart; one plate object per slot at +0xc0.., layout 0x6f: node 3 name, 4 "LV", 5 level; its own
   hand cursor +0x100, attached to node 1).

   Added here, without a screen of its own:
     - a sixth plate under the list ("Shortcuts"): an instance of the fourth plate's sequence attached to the
       list's root node, so it slides in and out with the list.  The list's input function (vtable slot 8,
       140404490) is wrapped: moving past either end of the game's five entries lands on the new one.  While the
       cursor is there the screen's own confirm / cancel handling is switched off (its pad lock, +0x11).
     - confirming it moves the hand to the Battle Commands pane, which already shows every deck slot.  Up / down
       pick a slot, confirm asks for a button: the next face button pressed becomes that slot's shortcut (its own
       button again clears it), any direction backs out.  Each plate shows the buttons of its slot where the
       command's level normally is.  Cancel goes back to the list.
     - two sets of shortcuts (shortcut.c): while picking a slot, square shows and sets the other set, and square
       again goes back; the help line says which set is shown. */
#include "mod.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern char g_ini[MAX_PATH + 32];
extern int g_debug;

#define CAMP        G(u8*, 0x10fb51f0)
#define VT_TOP_UPDATE   (0x674c70 + 0x08)
#define FN_TOP_UPDATE   0x402fa0
#define VT_LIST_INPUT   (0x674d40 + 0x40)
#define FN_LIST_INPUT   0x404490
#define SEQ_PLATE   0x177           /* "menu_act_plate": the fourth entry's plate, with its text */
#define N_ROOT      0xb
#define ITEM_X      (-230.0f)
#define ITEM_Y      10.0f           /* the game's five are at -75 .. -7, 17 apart */
#define SLOT_PITCH  18.0f
#define N_LV        4               /* nodes of a slot plate (layout 0x6f) */
#define N_LEVEL     5

#define L2D_Alive(h)            ((h) > 0 && FN(int, 0x1a60e0, int)(h) != 8)
#define L2D_Show(h, on)         FN(int, 0x1a5b30, int, int)(h, on)
#define L2D_IsVisible(h)        FN(int, 0x1a6060, int)(h)
#define L2D_GetControl(h)       FN(int, 0x1a60e0, int)(h)
#define L2D_SetControl(h, c)    FN(int, 0x1a7520, int, int)(h, c)
#define L2D_SetPos(h, x, y)     FN(int, 0x1a7660, int, float, float)(h, x, y)
#define L2D_ShowNode(h, n, on)  FN(int, 0x1a5b80, int, u16, int)(h, n, on)
#define L2D_IsNodeVisible(h, n) FN(int, 0x1a6670, int, u16)(h, n)
#define L2D_GetNodeControl(h, n) FN(int, 0x1a6790, int, u16)(h, n)
#define L2D_SetNodeControl(h, n, c) FN(int, 0x1a7db0, int, u16, int)(h, n, c)
#define L2D_SetNodeText(h, n, s) FN(int, 0x1a8330, int, u16, const char*, int, void*)(h, n, s, 0, NULL)
#define SE(id)                  FN(u64, 0x1b2e40, int, float, u64, u32, u32)(id, G(float, 0x6420b8), 0, 4, 0)
#define HELP_TEXT(s)            FN(void, 0x417770, u8*, const char*, void*)(CAMP, s, NULL)
#define HELP_MSG(id)            FN(void, 0x417710, u8*, int, void*)(CAMP, id, NULL)

static int  c_on = 1;
static char c_title[32] = "Shortcuts";

enum { M_OFF, M_ENTRY, M_SLOTS, M_ASSIGN };     /* cursor elsewhere / on our entry / picking a slot / waiting for a button */
static int g_mode, g_slot;
static int g_set;                               /* the set being shown and set: 0 or 1 */
static int g_list_h, g_item;                    /* the list's layout instance and our plate */
static u8 *g_top;                               /* the screen, during its update */
static int g_seen;                              /* the list's input ran during this update */
static int (MSABI *o_list_input)(u8 *self);

static void plates_forget(void);
static void item_drop(void) {
    if (L2D_Alive(g_item)) { FN(int, 0x1a5a50, int, int)(g_list_h, g_item); l2d_destroy(g_item); }
    g_item = 0; g_list_h = 0; g_mode = M_OFF;
    plates_forget();
}
void sccamp_on_destroy(int h) { if (h == g_list_h && h > 0) item_drop(); }

static u8 *pane(void) { return g_top ? *(u8**)(g_top + 0xc0) : NULL; }
static int slots(void) { int n = FN(s16, 0x41dd20, void)(); return n < 1 ? 1 : n > 8 ? 8 : n; }   /* deck slots the character has */
/* The plates of the Battle Commands pane while we are in it: where a plate has "LV" and the command's level, it
   shows the buttons of the slot's shortcuts.  The level's text node (5; text table entry 65: small yellow digits
   from the top left of (116, 3)) is given the settings of the button text of the game's command plates (font index
   3, pitch 8) with its anchor at the right end and the middle of the plate - a text of pictures alone, centred on
   its own height, as the game places its confirm button.  ours = 0 puts everything back. */
/* What the level text of a plate was, to put it back: the game's fill (1404103c0) leaves a command's level in the
   text table's settings, but gives an item its count ("x3") right-aligned and moved 5 to the right. */
typedef struct { u8 saved, lv, level, font, bold; s8 off[4]; u32 colour, align; float x, y; char text[24]; } PlateSave;
static PlateSave g_plate[8];
/* The plate is 126 x 15.  A button picture is drawn 2.2 below the middle of its text (measured in the game: with
   the anchor at 7.5 its picture spanned 5.1 .. 14.3), so the anchor sits that much above the plate's middle. */
static float c_icon_x = 123.0f, c_icon_y = 5.3f;
#define ICON_X c_icon_x
#define ICON_Y c_icon_y
static void plates_forget(void) { memset(g_plate, 0, sizeof g_plate); }
/* font parameters of the level node (1401a7cb0 -> 1401aa820): text, colour, pitch 8, alignment index (into 0 16 32 1
   17 33 2 18 34: low nibble left / centre / right, 0x10 middle, 0x20 bottom), font index (into 0 2 3 1 4 3), and
   the text's own offsets (x, y at text +0x2b4 / +0x2b5, and +0x2c8 / +0x2c9) */
static void level_text(int h, const char *text, u32 colour, int align, int size, const s8 *off) {
    FontParam fp; memset(&fp, 0, sizeof fp);
    fp.text = text; fp.colour = colour; fp.pitch = 8; fp.align = (u8)align; fp.size = (u8)size; fp.kind = 1;
    if (off) memcpy(fp.space, off, 4);
    fp.flags = 0x01 | 0x02 | 0x04 | 0x08 | 0x20 | 0x40;
    FN(int, 0x1a7cb0, int, u16, FontParam*, int)(h, N_LEVEL, &fp, 0);
}
static int align_index(u32 raw) {
    static const u8 tab[9] = { 0, 16, 32, 1, 17, 33, 2, 18, 34 };
    for (int i = 0; i < 9; i++) if (tab[i] == (raw & 0xff)) return i | ((raw & 0x100) ? 0x80 : 0);
    return 0;
}
static int font_index(int font, int bold) {
    static const u8 tab[6] = { 0, 2, 3, 1, 4, 3 };
    for (int i = 0; i < 6; i++) if (tab[i] == font && (i == 2) == (bold != 0)) return i;
    for (int i = 0; i < 6; i++) if (tab[i] == font) return i;
    return 4;
}
/* the plate object of a deck slot, if the cursor can go there: slots the character does not have yet are marked
   (+0x40), and the second row of a command that takes two slots has no plate of its own (140402820 makes one only
   where the slot record's "row inside the command" byte is 0; the first row's plate is made taller) */
static u8 *slot_btn(int i) {
    u8 *p = pane();
    u8 *b = p && i >= 0 && i < 8 ? *(u8**)(p + 0xc0 + i * 8) : NULL;
    return b && !b[0x40] ? b : NULL;
}
static void plates(int ours) {
    u8 *p = pane();
    if (!p || !L2D_Alive(*(int*)(p + 0x20))) { memset(g_plate, 0, sizeof g_plate); return; }
    for (int i = 0; i < 8; i++) {
        u8 *b = slot_btn(i);
        PlateSave *sv = &g_plate[i];
        if (!b) continue;
        int h = *(int*)(b + 8);
        if (!L2D_Alive(h)) { sv->saved = 0; continue; }
        if (ours) {
            if (!sv->saved) {
                u8 *to = NULL;
                if (!FN(int, 0x1a66c0, int, u16, u8**, int)(h, N_LEVEL, &to, 0) || !to) continue;
                const char *t = *(const char**)(to + 0x30);
                snprintf(sv->text, sizeof sv->text, "%s", t ? t : "");
                sv->colour = *(u32*)(to + 0x2a8); sv->align = *(u32*)(to + 0x298);
                sv->font = to[0x2cc]; sv->bold = to[0x2cd];
                sv->off[0] = (s8)to[0x2b4]; sv->off[1] = (s8)to[0x2b5]; sv->off[2] = (s8)to[0x2c8]; sv->off[3] = (s8)to[0x2c9];
                sv->x = 116.0f; sv->y = 3.0f;
                FN(int, 0x1a6880, int, u16, float*, float*)(h, N_LEVEL, &sv->x, &sv->y);
                sv->lv = L2D_IsNodeVisible(h, N_LV) != 0; sv->level = L2D_IsNodeVisible(h, N_LEVEL) != 0;
                sv->saved = 1;
            }
            char t[16]; int n = 0;
            for (int r = 0; r < SC_ROWS; r++) if (sc_slot_in(g_set, r) == i) { const char *ic = sc_row_icon(r); t[n++] = ic[0]; t[n++] = ic[1]; }
            t[n] = 0;
            L2D_ShowNode(h, N_LV, 0); L2D_ShowNode(h, N_LEVEL, 1);
            FN(int, 0x1a7ec0, int, u16, float, float)(h, N_LEVEL, ICON_X, ICON_Y);
            level_text(h, t, 0xff808080, 7, 3, NULL);                   /* right / middle, the button font, no offsets */
        } else if (sv->saved) {
            FN(int, 0x1a7ec0, int, u16, float, float)(h, N_LEVEL, sv->x, sv->y);
            level_text(h, sv->text, sv->colour, align_index(sv->align), font_index(sv->font, sv->bold), sv->off);
            L2D_ShowNode(h, N_LV, sv->lv); L2D_ShowNode(h, N_LEVEL, sv->level);
            sv->saved = 0;
        }
    }
}
/* the next slot the cursor can be on, from `from` in direction dir (+1 / -1), wrapping; -1 if there is none */
static int slot_next(int from, int dir) {
    for (int k = 1; k <= 8; k++) {
        int i = ((from + dir * k) % 8 + 8) % 8;
        if (i < slots() && slot_btn(i)) return i;
    }
    return -1;
}
static void slot_cursor(void) {
    u8 *p = pane(), *c = p ? *(u8**)(p + 0x100) : NULL;
    if (!c) return;
    float pos[2] = { 0.0f, (float)g_slot * SLOT_PITCH + 10.0f };        /* as the Edit Deck screen places its hand */
    FN(void, 0x428360, u8*, int)(c, 1);
    FN(void, 0x4283c0, u8*, float*)(c, pos);
}
/* the help line at the bottom.  Button pictures: f564 confirm, f568 L1 (the game shows the keyboard's or the pad's own) */
static void help(void) {
    char t[240];
    switch (g_mode) {
    case M_ENTRY:
        snprintf(t, sizeof t, "Set the shortcuts: commands used with a single button.\nHold %c%c in the field to bring them up.", 0xf5, 0x68);
        HELP_TEXT(t); break;
    case M_SLOTS:
        if (sc_sets() > 1)
            snprintf(t, sizeof t, "Set %d: pick a deck slot and press %c%c to give it a button.\nPress %s for set %d. In battle the d-pad switches sets.",
                     g_set + 1, 0xf5, 0x64, sc_row_icon(2), 2 - g_set);
        else
            snprintf(t, sizeof t, "Select a deck slot and press %c%c to give it a button.\nShortcuts follow the slot, not the command in it.", 0xf5, 0x64);
        HELP_TEXT(t); break;
    case M_ASSIGN:
        if (sc_sets() > 1)
            snprintf(t, sizeof t, "Set %d: press %s %s %s or %s for this slot.\nThe slot's own button removes it. Move to go back.", g_set + 1, sc_row_icon(1), sc_row_icon(0), sc_row_icon(2), sc_row_icon(3));
        else
            snprintf(t, sizeof t, "Press %s %s %s or %s for this slot.\nThe slot's own button removes it. Move to go back.", sc_row_icon(1), sc_row_icon(0), sc_row_icon(2), sc_row_icon(3));
        HELP_TEXT(t); break;
    }
}
/* the list cursor (hand and light frame) to our plate, or back to the game's entry idx */
static void list_cursor(u8 *self, int idx) {
    static float mine[2] = { ITEM_X, ITEM_Y };
    float *pos = idx < 0 ? mine : (float*)(self + 0xa8 + idx * 8);
    if (*(u8**)(self + 0xe8)) FN(void, 0x4283c0, u8*, float*)(*(u8**)(self + 0xe8), pos);
    if (*(u8**)(self + 0xf0)) FN(void, 0x428800, u8*, float*, char)(*(u8**)(self + 0xf0), pos, 0);
}
static void enter_entry(u8 *self) {            /* the game has just wrapped its cursor to the entry in +0x8e */
    L2D_SetNodeControl(*(int*)(self + 0x20), (u16)(*(s16*)(self + 0x8e) + 1), 0);
    g_mode = M_ENTRY;
    list_cursor(self, -1);
    help();
}
static void leave_entry(u8 *self, int idx) {
    g_mode = M_OFF;
    *(s16*)(self + 0x90) = *(s16*)(self + 0x8e); *(s16*)(self + 0x8e) = (s16)idx;
    L2D_SetNodeControl(*(int*)(self + 0x20), (u16)(idx + 1), 4);
    list_cursor(self, idx);
    HELP_MSG(0x34370100 + idx);
    if (g_top) g_top[0x11] = 0;
}
static void enter_slots(u8 *self) {
    u8 *p = pane();
    if (!p) return;
    g_mode = M_SLOTS;
    if (g_slot >= slots() || !slot_btn(g_slot)) g_slot = slot_next(-1, 1);
    if (g_slot < 0) { g_slot = 0; g_mode = M_ENTRY; return; }           /* no slot to point at */
    if (*(u8**)(self + 0xe8)) FN(void, 0x428590, u8*, int)(*(u8**)(self + 0xe8), 1);   /* list hand: resting, as when the game picks a pane */
    FN(u64, 0x42b250, u8*, u16, int)(p, G(u16, 0x81e160), 4);                           /* pane frame: selected */
    plates(1); slot_cursor(); help();
}
static void leave_slots(u8 *self) {
    u8 *p = pane();
    plates(0);
    if (p) {
        FN(u64, 0x42b250, u8*, u16, int)(p, G(u16, 0x81e160), 0);
        if (*(u8**)(p + 0x100)) FN(void, 0x428360, u8*, int)(*(u8**)(p + 0x100), 0);
    }
    if (*(u8**)(self + 0xe8)) FN(void, 0x428590, u8*, int)(*(u8**)(self + 0xe8), 0);
    g_mode = M_ENTRY;
    help();
}

/* vtable slot 8 of the list: its input, once a frame while the list has the focus */
static int MSABI list_input_hook(u8 *self) {
    u8 *top = g_top;
    g_seen = 1;
    int browsing = c_on && top && top[0x9b] == 2 && !top[0xea] && !self[0x100] && (s8)self[0x101] < 0 && (s8)self[0x102] < 0
                   && *(int*)(self + 0x20) == g_list_h && L2D_Alive(g_item);
    if (!browsing) {
        if (g_mode != M_OFF && top) { if (g_mode > M_ENTRY) leave_slots(self); leave_entry(self, 4); }
        return o_list_input(self);
    }
    if (g_mode == M_OFF) {
        s16 before = *(s16*)(self + 0x8e);
        int r = o_list_input(self);
        s16 after = *(s16*)(self + 0x8e), dir = *(s16*)(self + 0x8c);
        if (dir && ((before == 4 && after == 0) || (before == 0 && after == 4))) { enter_entry(self); top[0x11] = 1; }
        return r;
    }
    /* ours: the screen sees no buttons; read the pad as the game's menus do (newly pressed; up / down with repeat) */
    top[0x11] = 1;
    *(s16*)(self + 0x8c) = 0; self[0x9e] = 0;
    u32 press = (self[0x11] || G(u8, 0x10fb5364)) ? 0 : FN(u32, 0xecd90, u32)(0xffffffffu);
    u32 up = *(u32*)(self + 0x44), down = *(u32*)(self + 0x48);
    u32 move = FN(u32, 0x429960, u8*, u32)(self, up | down);
    u32 ok = G(u16, 0x8221c0), cancel = G(u16, 0x8221c4);
    if (g_mode == M_ENTRY) {
        /* onto the game's last or first entry; like the game there, no auto-repeat past an end of the list */
        if (move & up) { leave_entry(self, 4); self[0x10] = 1; *(u32*)(self + 0xc) = up; SE(1); }
        else if (move & down) { leave_entry(self, 0); self[0x10] = 1; *(u32*)(self + 0xc) = down; SE(1); }
        else if (press & ok) { enter_slots(self); SE(2); }
        else if (press & cancel) { leave_entry(self, 4); return o_list_input(self); }      /* the screen closes as usual */
    } else if (g_mode == M_SLOTS) {
        int to = (move & up) ? slot_next(g_slot, -1) : (move & down) ? slot_next(g_slot, 1) : -1;
        if (to >= 0) { if (to != g_slot) { g_slot = to; slot_cursor(); } SE(1); }
        else if (sc_sets() > 1 && (press & sc_row_mask(2)) && !(press & (ok | cancel))) { g_set ^= 1; plates(1); help(); SE(1); }   /* square: the other set */
        else if (press & ok) { g_mode = M_ASSIGN; help(); SE(2); }
        else if (press & cancel) { leave_slots(self); SE(4); }
    } else {
        u32 face = press & 0xf000u;
        if (face) {
            for (int r = 0; r < SC_ROWS; r++) if (face & sc_row_mask(r)) { sc_assign_in(g_set, r, sc_slot_in(g_set, r) == g_slot ? -1 : g_slot); break; }
            plates(1);
            g_mode = M_SLOTS; help(); SE(2);
        } else if (press & 0x00f000f0u) { g_mode = M_SLOTS; help(); SE(4); }                     /* a direction: never mind */
    }
    return 0;
}

/* our plate, once a frame after the screen's update */
static void item_frame(u8 *top) {
    u8 *list = *(u8**)(top + 0xb8);
    int lh = list ? *(int*)(list + 0x20) : 0;
    if (!L2D_Alive(lh)) { if (g_list_h) item_drop(); return; }
    if (lh != g_list_h) { item_drop(); g_list_h = lh; }
    if (!L2D_Alive(g_item)) {
        u8 *pc = FN(u8*, 0x1a65c0, int)(lh);
        g_item = pc ? l2d_attach_seq(lh, pc, N_ROOT, SEQ_PLATE) : 0;
        if (!g_item) return;
        /* as the list's own entries (text table 21..24: pitch 11, which the PC version makes 12 for this font - 1401aa620) */
        FontParam fp; memset(&fp, 0, sizeof fp);
        fp.text = c_title; fp.colour = 0xff808080; fp.pitch = 0x0c; fp.align = 0; fp.size = 0; fp.kind = 1;
        fp.flags = 0x01 | 0x02 | 0x04 | 0x08 | 0x40 | 0x80;
        FN(int, 0x1a7450, int, FontParam*, int)(g_item, &fp, 0);
        L2D_SetPos(g_item, ITEM_X, ITEM_Y);
    }
    /* lit while the cursor is on it; otherwise the look of one of the game's entries the cursor is not on */
    int idle = L2D_GetNodeControl(lh, (u16)(*(s16*)(list + 0x8e) == 0 ? 2 : 1));
    if (idle < 0 || idle > 7 || idle == 4) idle = 0;
    int want = g_mode != M_OFF ? 4 : idle;
    if (L2D_GetControl(g_item) != want) L2D_SetControl(g_item, want);
    L2D_Show(g_item, L2D_IsVisible(lh) != 0);
}
/* The update returns "this screen is finished" (the menu then goes back to its top screen): passed on as it is. */
static u64 MSABI top_update_hook(u8 *self) {
    g_top = self; g_seen = 0;
    u64 r = FN(u64, FN_TOP_UPDATE, u8*)(self);
    /* the list did not ask for input while we held the screen's buttons (something else took over): let go */
    if (g_mode != M_OFF && !g_seen) { if (g_mode > M_ENTRY) plates(0); g_mode = M_OFF; self[0x11] = 0; }
    item_frame(self);
    g_top = NULL;
    return r;
}

int sccamp_check(void) {
    char b[32];
    GetPrivateProfileStringA("Shortcuts", "Enabled", "1", b, sizeof b, g_ini); c_on = atoi(b);
    GetPrivateProfileStringA("Shortcuts", "MenuText", "Shortcuts", c_title, sizeof c_title, g_ini);
    GetPrivateProfileStringA("Shortcuts", "MenuIconX", "123", b, sizeof b, g_ini); c_icon_x = (float)atof(b);
    GetPrivateProfileStringA("Shortcuts", "MenuIconY", "5.3", b, sizeof b, g_ini); c_icon_y = (float)atof(b);
    if (!c_on) return 1;
    int bad = 0;
    if (G(u64, VT_TOP_UPDATE) != (u64)(g_base + FN_TOP_UPDATE) || G(u64, VT_LIST_INPUT) != (u64)(g_base + FN_LIST_INPUT)) { LOG("shortcuts: menu vtable slots do not match"); bad++; }
    if (!l2d_destroy_site_ok()) { LOG("shortcuts: Destroy does not match"); bad++; }
    return bad == 0;
}
void sccamp_apply(void) {
    if (!c_on || !l2d_destroy_hook()) return;
    o_list_input = (void*)(g_base + FN_LIST_INPUT);
    void *f; u64 old;
    f = (void*)top_update_hook;  old = (u64)(g_base + FN_TOP_UPDATE); patch_bytes(VT_TOP_UPDATE, (u8*)&old, (u8*)&f, 8, "deck screen update");
    f = (void*)list_input_hook;  old = (u64)(g_base + FN_LIST_INPUT); patch_bytes(VT_LIST_INPUT, (u8*)&old, (u8*)&f, 8, "deck list input");
    LOG("shortcuts: \"%s\" entry in the Command Decks menu", c_title);
}

void sccamp_own(char *out, int n) { own_add(out, n, "scplate", g_item); }
#ifndef _WIN32      /* offline test access */
int *test_sccamp_state(void) { static int s[4]; s[0] = g_mode; s[1] = g_slot; s[2] = g_item; s[3] = g_list_h; return s; }
int *test_sccamp_set(void) { return &g_set; }
void test_sccamp_top(u8 *top) { g_top = top; }
void test_sccamp_frame(u8 *top) { item_frame(top); }
void *test_sccamp_update_hook(void) { return (void*)top_update_hook; }
int test_sccamp_input(u8 *self) { return list_input_hook(self); }
#endif

/* The character panel of the menu (name, LV, HP, FP, NEXT LEVEL) gets an MP row between HP and FP.

   The panel is the class CCampTopStatus (vtable 140677440): layout 0x67 "chara_plate" of camp.l2d, instance handle
   at +0x20.  Its nodes (ids): 0x1d the plate (parent of the rest; its sprite object 5 holds the four coloured
   lines), 0x1c / 0x19 / 0x1a / 0x1b the labels LV / HP / FP / NEXT LEVEL (small sprites, tinted), 0x1f / 0x22 /
   0x24 / 0x26 their values (text; filled by the "open" function 140417f00), 0x1e the name.

   Every frame, after the panel's own update (vtable slot 1 = 14042ab80, replaced in this class's table):
     - FP and NEXT LEVEL (label, value, line) are moved down by one row (11 units);
     - two instances of sequences of the same file are attached to the plate node, so they slide and fade with
       it: one whose three quads are rewritten into the row's line and the "MP" label with its shadow (the label
       is art added to the menu texture by tex.c), and one text for "current/maximum" set up like the FP value's.
   An attached instance keeps a pointer into its parent, so ours are detached and destroyed before the panel's
   layout is: Destroy (1401a57f0) is hooked for that. */
#include "core.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "steals_gen.h"
#include "mod.h"
#include "statart_gen.h"

extern char g_ini[MAX_PATH + 32];
extern int g_debug;

#define VT_STATUS    0x677440
#define VT_UPDATE    (VT_STATUS + 8)
#define FN_UPDATE    0x42ab80
#define CAMP_BLOCK   "camp:0"
#define SEQ_QUADS    0x184          /* "charge_tab_grd": one sprite of three quads, no colour of its own */
#define SEQ_TEXT     0xb0           /* "font_02": the sequence of the value texts */
#define N_PLATE 0x1d
#define N_FP_LABEL 0x1a
#define N_FP_VALUE 0x24
#define N_NEXT_LABEL 0x1b
#define N_NEXT_VALUE 0x26
#define ROW 11

typedef struct { s16 x0, y0, x1, y1; u16 part, attr; } Grp;
typedef struct { s16 u0, v0, u1, v1; u32 col[4]; } Part;
typedef struct { Grp *g; Part *p; } Pair;

static int c_on = 1;
static u32 c_col = 0xfff37e01;      /* AABBGGRR as shown on screen: the top colour of KH2's MP bar, (1, 126, 243) */
static int g_parent, g_quads, g_text;
static u64 (MSABI *o_destroy)(int h);

static int alive(int h) { return h > 0 && FN(int, 0x1a60e0, int)(h) != 8; }
static void drop(void) {
    int own[2] = { g_quads, g_text };
    g_quads = g_text = 0;
    for (int i = 0; i < 2; i++) {
        if (!alive(own[i])) continue;
        FN(int, 0x1a5a50, int, int)(g_parent, own[i]);         /* Detach; nothing happens if the parent is gone */
        if (o_destroy) o_destroy(own[i]); else FN(void, 0x1a57f0, int)(own[i]);
    }
    g_parent = 0;
}
static u64 MSABI destroy_hook(int h) {
    if (h > 0 && h == g_parent) drop();
    if (h > 0) sccamp_on_destroy(h);
    return o_destroy(h);
}
/* the Destroy hook serves this file and sccamp.c */
int l2d_destroy_hook(void) {
    if (!o_destroy) o_destroy = hook_fn(&S_l2d_destroy, destroy_hook, "l2d destroy");
    return o_destroy != NULL;
}
void l2d_destroy(int h) { if (o_destroy) o_destroy(h); else FN(void, 0x1a57f0, int)(h); }
int l2d_destroy_site_ok(void) { return memcmp(g_base + S_l2d_destroy.rva, S_l2d_destroy.bytes, S_l2d_destroy.len) == 0; }

static void node_to(int h, u16 node, float x0, float y0, float x1, float y1) {
    float x = -1000, y = -1000;
    if (!FN(int, 0x1a6880, int, u16, float*, float*)(h, node, &x, &y)) return;
    if (x == x0 && y == y0) FN(int, 0x1a7ec0, int, u16, float, float)(h, node, x1, y1);
}
static void set_quad(Pair *q, int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, u32 c) {
    q->g->x0 = (s16)x0; q->g->x1 = (s16)x1; q->g->y0 = (s16)y0; q->g->y1 = (s16)y1;
    q->p->u0 = (s16)u0; q->p->u1 = (s16)u1; q->p->v0 = (s16)v0; q->p->v1 = (s16)v1;
    q->p->col[0] = q->p->col[1] = q->p->col[2] = q->p->col[3] = c;
}
/* a new instance of a sequence of the menu's file, attached to a node of a layout instance (pc = its control
   object); it takes the parent's draw priority, queue and timer group.  0 on failure */
int l2d_attach_seq(int parent, u8 *pc, u16 node, u16 seq) {
    u8 vis = G(u8, 0x8f88028), grp = G(u8, 0x8f8802a);
    G(u8, 0x8f88028) = 0;                       /* created hidden */
    G(u8, 0x8f8802a) = pc[0x86];                /* the panel's timer / draw group */
    int h = FN(int, 0x1a5350, const char*, u16, int, void*)(CAMP_BLOCK, seq, 0, NULL);
    G(u8, 0x8f88028) = vis; G(u8, 0x8f8802a) = grp;
    if (h <= 0) return 0;
    u8 *c = FN(u8*, 0x1a5fd0, int)(h);
    if (!c || !FN(int, 0x1a4e10, int, u16, int)(parent, node, h)) { FN(void, 0x1a57f0, int)(h); return 0; }
    *(s16*)(c + 0x10) = *(s16*)(pc + 0x10);     /* same draw priority and queue; made later = drawn on top of the plate */
    *(s16*)(c + 0x12) = *(s16*)(pc + 0x12);
    return h;
}

static void frame(u8 *self) {
    int h = *(int*)(self + 0x20);
    if (!alive(h)) { if (g_parent) drop(); return; }
    u8 *pc = FN(u8*, 0x1a65c0, int)(h);
    if (!pc) return;
    if (h != g_parent) { drop(); g_parent = h; }

    /* FP and NEXT LEVEL one row down (positions are those of the layout; anything else is left alone) */
    node_to(h, N_FP_LABEL, 0, 41, 0, 41 + ROW);
    node_to(h, N_FP_VALUE, 69, 39, 69, 39 + ROW);
    node_to(h, N_NEXT_LABEL, 0, 51, 0, 51 + ROW);
    node_to(h, N_NEXT_VALUE, 69, 58, 69, 58 + ROW);
    Pair *q = NULL;
    int n = FN(int, 0x1a6980, int, u16, int, Pair**)(h, N_PLATE, 5, &q);
    if (!n) { FN(int, 0x1a83f0, int, u16, int)(h, N_PLATE, 5); n = FN(int, 0x1a6980, int, u16, int, Pair**)(h, N_PLATE, 5, &q); }
    int lines = n == 4 && q;                    /* yellow, green, orange, white: 3 high at y 0, 11, 22, 41 */
    if (lines) {
        if (q[2].g->y0 == 22 && q[2].g->y1 == 25) { q[2].g->y0 += ROW; q[2].g->y1 += ROW; }
        if (q[3].g->y0 == 41 && q[3].g->y1 == 44) { q[3].g->y0 += ROW; q[3].g->y1 += ROW; }
    }

    /* the MP row.  The plate's lines sit at (-2, 25) from the plate node; labels are 2 texels per unit */
    if (!alive(g_quads)) {
        g_quads = l2d_attach_seq(h, pc, N_PLATE, SEQ_QUADS);
        if (g_quads) FN(int, 0x1a7be0, int, int)(g_quads, 1);
    }
    if (g_quads) {
        Pair *m = NULL;
        int k = FN(int, 0x1a6350, int, int, Pair**)(g_quads, 1, &m);
        if (k >= 3 && m) {
            u32 c = 0x80000000u | (c_col & 0xffffff);
            int art = tex_stat_art_ready();
            int u0 = STAT_CELL_X, v0 = STAT_CELL_Y, u1 = u0 + STAT_CELL_W, v1 = v0 + STAT_CELL_H, y = 41;
            if (lines) set_quad(&m[0], -2, 25 + 22, 69, 25 + 25, q[1].p->u0, q[1].p->v0, q[1].p->u1, q[1].p->v1, c);
            else set_quad(&m[0], 0, 0, 0, 0, 0, 0, 0, 0, 0);
            if (art) {
                set_quad(&m[1], 0, y, STAT_CELL_W / 2, y + STAT_CELL_H / 2, u0, v0, u1, v1, 0x80000000u);      /* shadow */
                set_quad(&m[2], -1, y - 1, STAT_CELL_W / 2 - 1, y + STAT_CELL_H / 2 - 1, u0, v0, u1, v1, c);
            } else { set_quad(&m[1], 0, 0, 0, 0, 0, 0, 0, 0, 0); set_quad(&m[2], 0, 0, 0, 0, 0, 0, 0, 0, 0); }
            for (int i = 3; i < k; i++) set_quad(&m[i], 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
    }
    if (!alive(g_text)) {
        g_text = l2d_attach_seq(h, pc, N_PLATE, SEQ_TEXT);
        if (g_text) {
            /* as entry 15 of the layout's text table (the FP value): size 4, alignment 6, kind 1, pitch 8 */
            FontParam fp; memset(&fp, 0, sizeof fp);
            /* a text colour counts double (80 = full; FP's ff / 44 / 1a shows as ff / 88 / 34), so half of ours */
            u32 half = 0xff000000u | (((c_col & 0xff) + 1) >> 1) | ((((c_col >> 8) & 0xff) + 1) >> 1) << 8 | ((((c_col >> 16) & 0xff) + 1) >> 1) << 16;
            fp.text = ""; fp.colour = half; fp.pitch = 8; fp.align = 6; fp.size = 4; fp.kind = 1;
            fp.flags = 0x01 | 0x02 | 0x04 | 0x08 | 0x40 | 0x80;
            FN(int, 0x1a7450, int, FontParam*, int)(g_text, &fp, 0);
            FN(int, 0x1a7660, int, float, float)(g_text, 69.0f, 39.0f);
        }
    }
    if (g_text) {
        float cur = 0, max = 0; mp_get(&cur, &max);
        char b[32]; snprintf(b, sizeof b, "%d/%d", (int)(cur + 0.5f), (int)(max + 0.5f));
        FN(void, 0x1a7b20, int, const char*, int, void*)(g_text, b, 0, NULL);
    }
    int vis = FN(int, 0x1a6060, int)(h) != 0;
    if (g_quads) FN(int, 0x1a5b30, int, int)(g_quads, vis);
    if (g_text) FN(int, 0x1a5b30, int, int)(g_text, vis);
}
static u64 MSABI update_hook(u8 *self) {
    u64 r = FN(u64, FN_UPDATE, u8*)(self);
    frame(self);
    return r;
}

int status_check(void) {
    char b[32];
    GetPrivateProfileStringA("Menu", "StatusMP", "1", b, sizeof b, g_ini); c_on = atoi(b);
    GetPrivateProfileStringA("Menu", "StatusMPColor", "fff37e01", b, sizeof b, g_ini); c_col = (u32)strtoul(b, NULL, 16);
    if (!c_on) return 1;
    int bad = 0;
    if (G(u64, VT_UPDATE) != (u64)(g_base + FN_UPDATE)) { LOG("status: the panel's update slot does not match"); bad++; }
    if (!l2d_destroy_site_ok()) { LOG("status: Destroy does not match"); bad++; }
    return bad == 0;
}
void status_apply(void) {
    if (!c_on) return;
    if (!l2d_destroy_hook()) return;
    void *f = (void*)update_hook; u64 old = (u64)(g_base + FN_UPDATE);
    patch_bytes(VT_UPDATE, (u8*)&old, (u8*)&f, 8, "status update");
    LOG("status: MP row in the menu's character panel");
}

void status_own(char *out, int n) { own_add(out, n, "statquads", g_quads); own_add(out, n, "stattext", g_text); }
#ifndef _WIN32      /* offline test access */
void test_status_frame(u8 *self) { frame(self); }
int *test_status_handles(void) { static int h[3]; h[0] = g_parent; h[1] = g_quads; h[2] = g_text; return h; }
#endif

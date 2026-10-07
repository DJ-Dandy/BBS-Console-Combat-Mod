/* KH2's MP bar art, added to the game's gauge texture as it is loaded.

   The HD textures are PNG files.  4e19b0 decodes one with libpng (541d50 / 542140, mode 2 = BGRA, rows packed)
   and hands the pixels to 4e1630, which creates the Direct3D texture:
       4e1ee4  call 4e1630   rcx device, rdx description (+0x10 width, +0x18 height), r8 pixels, r9 name
   The hook sits on that call.  When the picture is the HUD gauge sheet (texture 2 of arc/pc/p00common.arc,
   1024x512, recognised by its content), the art block from mpart_gen.h is copied into an unused area of it.
   The MP bar (mp.c) then draws from that area.  If the sheet is not recognised - a texture mod, another game
   version - nothing is changed and the bar falls back to its plain look. */
#include "core.h"
#include <string.h>
#include "mod.h"
#define MPART_DATA
#include "mpart_gen.h"
#define STATART_DATA
#include "statart_gen.h"
#define HDART_DATA
#include "hdart_gen.h"

extern int g_debug;
static int g_art;           /* 1 once the art is in the gauge sheet */
static int c_art = 1;
int tex_art_ready(void) { return g_art; }
void tex_enable(int on) { c_art = on; }

/* pixels: 4 bytes each, w x h, rows packed.  Returns 1 if the art was added. */
static int tex_patch(u8 *pix, int w, int h) {
    if (w != 1024 || h != 512 || !pix) return 0;
    int n = (int)(sizeof mpart_checks / sizeof *mpart_checks), rgb = 0, bgr = 0;
    for (int i = 0; i < n; i++) {
        const u8 *p = pix + ((size_t)mpart_checks[i].y * (size_t)w + mpart_checks[i].x) * 4;
        int r = mpart_checks[i].r, g = mpart_checks[i].g, b = mpart_checks[i].b;
#define CLOSE(a, b) ((a) - (b) <= 3 && (b) - (a) <= 3)
        if (!CLOSE(p[3], mpart_checks[i].a) || !CLOSE(p[1], g)) continue;
        if (CLOSE(p[0], r) && CLOSE(p[2], b)) rgb++;
        if (CLOSE(p[0], b) && CLOSE(p[2], r)) bgr++;
#undef CLOSE
    }
    if (rgb < n - 1 && bgr < n - 1) return 0;                   /* not the gauge sheet */
    int swap = bgr >= rgb;
    for (int y = 0; y < MPART_H; y++) {                         /* the area must be unused */
        const u8 *row = pix + ((size_t)(MPART_Y + y) * (size_t)w + MPART_X) * 4;
        for (int x = 0; x < MPART_W; x++) if (row[x * 4 + 3]) { LOG("mp art: the area in the gauge texture is in use, left alone"); return 0; }
    }
    const u8 *r = mpart_rle, *end = mpart_rle + sizeof mpart_rle;
    int x = 0, y = 0;
    while (r < end && y < MPART_H) {
        int cnt = r[0];
        while (cnt-- && y < MPART_H) {
            u8 *d = pix + ((size_t)(MPART_Y + y) * (size_t)w + MPART_X + x) * 4;
            d[0] = swap ? r[3] : r[1]; d[1] = r[2]; d[2] = swap ? r[1] : r[3]; d[3] = r[4];
            if (++x == MPART_W) { x = 0; y++; }
        }
        r += 5;
    }
    return 1;
}

/* The "MP" label of the menu's character panel (status.c), added to the menu sheet (the texture of camp.l2d,
   1024x1024; four variants are loaded, all with the same labels).  The sheet is recognised by the shape of its
   "HP" and "FP" labels; the new label goes into an area no sprite uses: an M drawn for it (statart_gen.h) and the
   sheet's own P.  Grey like the other labels, which the game tints. */
static int g_stat_art;
int tex_stat_art_ready(void) { return g_stat_art; }
static int stat_patch(u8 *pix, int w, int h) {
    if (w != STAT_SHEET || h != STAT_SHEET || !pix) return 0;
#define PX(x, y) (pix + ((size_t)(y) * (size_t)w + (size_t)(x)) * 4)
    u32 k = 0x811c9dc5u;
    for (int y = 0; y < STAT_CHK_H; y++)
        for (int x = 0; x < STAT_CHK_W; x++) k = (k ^ (u32)(PX(STAT_CHK_X + x, STAT_CHK_Y + y)[3] >= 128)) * 0x01000193u;
    if (k != STAT_CHK) return 0;                                /* not the menu sheet */
    for (int y = -3; y < STAT_CELL_H + 3; y++)                  /* the area must be unused */
        for (int x = -3; x < STAT_CELL_W + 3; x++) if (PX(STAT_CELL_X + x, STAT_CELL_Y + y)[3]) return 0;
    for (int y = -2; y < STAT_CELL_H + 2; y++)                  /* grey underneath, so filtered edges stay clean */
        for (int x = -2; x < STAT_CELL_W + 2; x++) { u8 *d = PX(STAT_CELL_X + x, STAT_CELL_Y + y); d[0] = d[1] = d[2] = 128; d[3] = 0; }
    for (int y = 0; y < STAT_M_H; y++)
        for (int x = 0; x < STAT_M_W; x++) PX(STAT_CELL_X + STAT_M_X + x, STAT_CELL_Y + STAT_M_Y + y)[3] = stat_m[y][x];
    for (int y = 0; y < STAT_P_H; y++)
        for (int x = 0; x < STAT_P_W; x++) memcpy(PX(STAT_CELL_X + STAT_PD_X + x, STAT_CELL_Y + STAT_PD_Y + y), PX(STAT_P_X + x, STAT_P_Y + y), 4);
#undef PX
    return 1;
}

/* The words MAGIC, ITEMS and D-LINK for the headers of the command menu's lists (menu.c), added to the command
   sheet: the texture of bc01_00.l2d, 1024 x 1024, which holds the game's own "COMMANDS" label.  The sheet differs
   by language (the label, the word under it), so it is recognised by 24 pixels of the art all languages share; the
   words (hdart_gen.h, drawn by tools/hdfont.py) go to an area they all leave empty.  Grey and black only, so the
   byte order of the pixels does not matter. */
static int g_hd_art;
int tex_hd_art_ready(void) { return g_hd_art; }
static int hd_patch(u8 *pix, int w, int h) {
    if (w != HDART_SHEET || h != HDART_SHEET || !pix) return 0;
    int n = (int)(sizeof hdart_checks / sizeof *hdart_checks), ok = 0;
    for (int i = 0; i < n; i++) {
        const u8 *p = pix + ((size_t)hdart_checks[i].y * (size_t)w + hdart_checks[i].x) * 4;
        int r = hdart_checks[i].r, g = hdart_checks[i].g, b = hdart_checks[i].b, a = hdart_checks[i].a;
#define CLOSE(a, b) ((a) - (b) <= 3 && (b) - (a) <= 3)
        if (CLOSE(p[3], a) && CLOSE(p[1], g) && ((CLOSE(p[0], r) && CLOSE(p[2], b)) || (CLOSE(p[0], b) && CLOSE(p[2], r)))) ok++;
#undef CLOSE
    }
    if (ok < n - 1) return 0;                                   /* not the command sheet */
    for (int y = -2; y < HDART_H + 2; y++) {                    /* the area must be unused */
        const u8 *row = pix + ((size_t)(HDART_Y + y) * (size_t)w + HDART_X - 2) * 4;
        for (int x = 0; x < HDART_W + 4; x++) if (row[x * 4 + 3]) { LOG("header art: the area in the command texture is in use, left alone"); return 0; }
    }
    const u8 *r = hdart_rle, *end = hdart_rle + sizeof hdart_rle;
    int x = 0, y = 0;
    while (r < end && y < HDART_H) {
        int cnt = r[0];
        while (cnt-- && y < HDART_H) {
            u8 *d = pix + ((size_t)(HDART_Y + y) * (size_t)w + HDART_X + x) * 4;
            d[0] = d[1] = d[2] = r[1]; d[3] = r[2];
            if (++x == HDART_W) { x = 0; y++; }
        }
        r += 3;
    }
    return 1;
}

static void *MSABI tex_hook(void *dev, u8 *desc, u8 *pix, const char *name, int e) {
    if (desc && pix && (int)*(u32*)(desc + 0x10) == STAT_SHEET && (int)*(u32*)(desc + 0x18) == STAT_SHEET && stat_patch(pix, STAT_SHEET, STAT_SHEET)) {
        if (!g_stat_art) LOG("status: \"MP\" label added to the menu texture");
        g_stat_art = 1;
    }
    if (desc && pix && (int)*(u32*)(desc + 0x10) == HDART_SHEET && (int)*(u32*)(desc + 0x18) == HDART_SHEET && hd_patch(pix, HDART_SHEET, HDART_SHEET)) {
        if (!g_hd_art) LOG("menu: list header words added to the command texture");
        g_hd_art = 1;
    }
    if (c_art && desc && pix) {
        int w = (int)*(u32*)(desc + 0x10), h = (int)*(u32*)(desc + 0x18);
        if (w == 1024 && h == 512) {
            int ok = tex_patch(pix, w, h);
            if (ok) { if (!g_art) LOG("mp art: added to the gauge texture"); g_art = 1; }
            else if (g_debug) LOG("mp art: 1024x512 texture '%.40s' is not the gauge sheet", name ? name : "");
        }
    }
    return FN(void*, 0x4e1630, void*, u8*, u8*, const char*, int)(dev, desc, pix, name, e);
}

int tex_check(void) {
    u8 *p = g_base + 0x4e1ee4; s32 d; memcpy(&d, p + 1, 4);
    if (p[0] == 0xE8 && (u32)(0x4e1ee4 + 5 + d) == 0x4e1630) return 1;
    LOG("call site 4e1ee4 does not match");
    return 0;
}
void tex_apply(void) {
    hook_call(0x4e1ee4, 0x4e1630, tex_hook, "png_texture");
}

#ifndef _WIN32      /* offline test access */
int test_tex_patch(u8 *pix, int w, int h) { return tex_patch(pix, w, h); }
int test_stat_patch(u8 *pix, int w, int h) { return stat_patch(pix, w, h); }
void test_set_stat_art(int on) { g_stat_art = on; }
void test_set_art(int on) { g_art = on; }
int test_hd_patch(u8 *pix, int w, int h) { return hd_patch(pix, w, h); }
void test_set_hd_art(int on) { g_hd_art = on; }
#endif

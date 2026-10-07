/* Texture guard for the game's 2D layouts.

   Every node of a 2D layout (and every standalone sequence) is drawn by CD2SeqCtrl's draw function 1401aaea0
   (vtable 140641dc0 slot 3; a layout's draw 1401a2bb0, vtable 1406418b8 slot 2, calls it for each node).  Before
   its quads it binds the texture of the data block the node draws from: block (CD2SeqData, node+0x90) -> current
   texture (+0x90, a TIM2 image in a loaded file; +0x98 is the block's own one, +0xa0 the resource id of a texture
   the game put in its place with 1401a84e0) -> the PC texture object at image+0x28 [14010ccb0 -> 1404f0180].
   Nothing checks that the image is still there.  Twice in play (Terra, Castle of Dreams, the moment the first
   forced fight was won) that pointer led to memory that held other data by then, the "texture object" was the
   number 74f111e420036048 both times, and the game died reading through it (rva 4f0195).

   So the draw function is entered through guard_seq(): when the bind is about to happen and a pointer it would
   read through cannot be read, the block's own texture is put back if that one is sound (what the game's own
   "restore texture" 1401ac440 does, minus the release of the resource, which is left to the game), otherwise the
   node is not drawn this frame.  Either way the log gets the layout, the node, the block and the resource, which
   is what is needed to find out why the texture went away.  A draw that would have worked is never touched: the
   only test is "can the addresses the game is about to read be read".

   What it found the first time it ran: the nodes were the mod's own (the four menu entries, a Command Style offer
   and its timer), and their file, bc01_00.l2d, had been unloaded under them - the game destroyed the player's
   gauge and command objects and freed their files without destroying every 2D instance first, and the mod's
   instances are only looked after from the gauge's update.  They are now destroyed with the gauge (menu_shutdown).
   As a second line: a layout or sequence whose data handle is no longer registered is not drawn at all
   (data_gone), and l2d_live() lets a module notice such an instance of its own, destroy it and make a new one. */
#include "core.h"
#include "mod.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define VT_SEQ_DRAW 0x641dd8u       /* CD2SeqCtrl vtable slot 3 */
#define VT_LAY_DRAW 0x6418c8u       /* CD2LayCtrl vtable slot 2 */
#define FN_SEQ_DRAW 0x1aaea0u
#define FN_LAY_DRAW 0x1a2bb0u
#define VT_SEQDATA  0x641f40u
#define TIM2_MAGIC  0x324d4954u

static int c_on = 0;                   /* off unless asked for: the fix for the one known case is menu_shutdown() (menu.c) */
static u64 (MSABI *o_seq_draw)(u8 *self, u64 a, u64 b, u64 c);
static u64 (MSABI *o_lay_draw)(u8 *self, u64 a, u64 b, u64 c);
static u8 *g_lay;                   /* the layout being drawn, NULL for a standalone sequence */
static int g_events, g_fixed, g_skipped;

/* ---- can these bytes be read ---- */
/* Cost matters here: this runs for every 2D node drawn.  The first version asked VirtualQuery, behind a table of
   256 pages that was emptied now and then.  VirtualQuery also works out how far the region around the address
   goes, page by page, and the images sit in the game's own resource arena, hundreds of megabytes of like pages -
   so one question could cost a good part of a millisecond, and whether two busy pages shared a table slot (and so
   asked every time) depended on where the heap happened to be in that run: one launch was "insanely laggy", the
   next was not.  Now a page is probed by reading one byte of it through ReadProcessMemory (fails cleanly on memory
   that is not there, no region walk; none of these addresses is ever a stack page), the table is larger and is
   not emptied, and in the ordinary case no page is asked about at all (see g_seen). */
static int g_probes;
#ifdef _WIN32
#define PG_SLOTS 2048
static u64 g_pg[PG_SLOTS];
static int page_ok(u64 pg) {
    u64 *e = &g_pg[(pg ^ (pg >> 11)) & (PG_SLOTS - 1)];
    if (*e == pg) return 1;
    u8 b; SIZE_T got = 0; g_probes++;
    if (!ReadProcessMemory(GetCurrentProcess(), (void*)(pg << 12), &b, 1, &got) || got != 1) return 0;
    *e = pg; return 1;
}
#else
int (*test_guard_page)(u64 pg);     /* offline test: which pages count as readable (NULL = all) */
static int page_ok(u64 pg) { g_probes++; return test_guard_page ? test_guard_page(pg) : 1; }
#endif
static int mem_ok(const void *p, size_t n) {
    u64 a = (u64)p;
    if (a < 0x10000 || (a >> 47) || !n) return 0;
    for (u64 pg = a >> 12; pg <= (a + n - 1) >> 12; pg++) if (!page_ok(pg)) return 0;
    return 1;
}

/* a PC texture object as 1404f0180 reads it: the mark at +0x10 and, if that is right, the pointer at +0x70 */
static int obj_ok(const u8 *o) {
    if (!mem_ok(o, 0x18)) return 0;
    return *(u32*)(o + 0x10) != 0x02f02eb5u || mem_ok(o, 0x78);
}
/* 0 = binding this image reads nothing it should not; else why not (the game's own conditions, 14010ccb0) */
static int tm2_bad(const u8 *tm) {
    if (!mem_ok(tm, 0x50)) return 1;
    if (!(tm[0x3d] || (*(u16*)(tm + 0x3e) & 0x7ffc))) return 0;          /* the game binds nothing then */
    u64 nat = *(u64*)(tm + 0x28), nat2 = *(u64*)(tm + 0x40);      /* +0x40: the picture it is fading to, if any */
    if (nat && !obj_ok((void*)nat)) return 2;
    if (nat2 && !obj_ok((void*)nat2)) return 3;
    return 0;
}

/* ---- log ---- */
static u8 *data_obj(int h) {        /* CD2LayData / CD2SeqData of a data handle */
    u8 *mgr = G(u8*, 0x8f87fd8);
    if (!mgr || h <= 0) return NULL;
    u8 *e = ((u8 *(MSABI *)(u8*, int))(*(void***)mgr)[3])(mgr, h);
    return e ? *(u8**)(e + 8) : NULL;
}
static void name16(const u8 *p, char *out) {
    int i = 0;
    for (; i < 16; i++) { char ch = (char)p[i]; if (!ch) break; out[i] = (ch >= 0x20 && ch < 0x7f) ? ch : '#'; }
    out[i] = 0;
}
static void res_line(const char *what, u32 id) {
    if (!id) { LOG("     %s: none", what); return; }
    u8 *r = FN(u8*, 0x11a8a0, u32)(id);
    if (!mem_ok(r, 0x90)) { LOG("     %s: id %x is not a resource any more", what, id); return; }
    char nm[20]; name16(r + 0x38, nm);
    LOG("     %s: id %x \"%s\" state %d group %d users %d requests %d data %p size %llx flags %x %x", what, id, nm, r[0x11],
        *(s32*)(r + 0x50), *(s16*)(r + 0x14), *(s16*)(r + 0x16), *(void**)(r + 0x70), (unsigned long long)*(u64*)(r + 0x80),
        *(u16*)(r + 0x88), *(u16*)(r + 0x8a));
    if (*(u32*)(r + 0x5c)) res_line("  in archive", *(u32*)(r + 0x5c));
}
/* which loaded files hold this address now (the resource lists, walked under the resource system's own lock) */
static void owners(const u8 *addr) {
    u8 *cont = G(u8*, 0x8f7dc10);
    if (!cont || !mem_ok(cont, 0x60)) return;
    u8 *mx = cont + 0x58; void **vt = *(void***)mx;
    if (!mem_ok(vt, 0x20)) return;
    struct { char nm[20]; int type, state, group, users; u64 data, size; u32 id, arc; } hit[6]; int nh = 0, seen = 0;
    ((void (MSABI *)(u8*))vt[1])(mx);
    for (int t = 0; t < 0x2a; t++) {
        u8 *r = G(u8*, 0x8f7e328 + t * 0x20);
        for (int k = 0; k < 8192 && r && mem_ok(r, 0x90) && r[0x10] == t; k++, r = *(u8**)(r + 8)) {
            u64 d = *(u64*)(r + 0x70), sz = *(u64*)(r + 0x80); seen++;
            if (!d || (u64)addr < d || (u64)addr >= d + sz || nh >= 6) continue;
            name16(r + 0x38, hit[nh].nm); hit[nh].type = r[0x10]; hit[nh].state = r[0x11]; hit[nh].group = *(s32*)(r + 0x50);
            hit[nh].users = *(s16*)(r + 0x14); hit[nh].data = d; hit[nh].size = sz; hit[nh].id = *(u32*)(r + 0x58); hit[nh].arc = *(u32*)(r + 0x5c); nh++;
        }
    }
    ((void (MSABI *)(u8*))vt[3])(mx);
    if (!nh) LOG("     no loaded file holds that address now (%d files looked at)", seen);
    for (int i = 0; i < nh; i++)
        LOG("     that address now belongs to \"%s\" (type %d, id %x, in archive %x): state %d group %d users %d, data %llx size %llx, offset %llx",
            hit[i].nm, hit[i].type, hit[i].id, hit[i].arc, hit[i].state, hit[i].group, hit[i].users, (unsigned long long)hit[i].data,
            (unsigned long long)hit[i].size, (unsigned long long)((u64)addr - hit[i].data));
}
static void report(u8 *self, u8 *sd, u8 *tm, int why, const char *action) {
    static u8 *seen_self[16]; static u8 *seen_tm[16];
    for (int i = 0; i < 16; i++) if (seen_self[i] == self && seen_tm[i] == tm) return;
    seen_self[g_events & 15] = self; seen_tm[g_events & 15] = tm;
    if (g_events++ >= 24) return;
    char own[400]; mod_own_handles(own, sizeof own);
    LOG("!! texture guard: a 2D node was about to draw with a texture that is gone (%s) -> %s",
        why == 1 ? "image unreadable" : why == 2 ? "its texture object is not an address" : why == 3 ? "its second texture object is not an address" : "data block unreadable",
        action);
    if (g_lay && mem_ok(g_lay, 0xb0)) {
        u8 *ld = data_obj(*(s32*)(g_lay + 0x1c)); char nm[20] = "?";
        if (mem_ok(ld, 0xe8)) name16(ld + 8, nm);
        LOG("     layout %p handle %d of file \"%s\" (data handle %d), %d nodes, priority %d, control %d, flags %02x; node id %x",
            (void*)g_lay, *(s32*)(g_lay + 0x18), nm, *(s32*)(g_lay + 0x1c), *(s16*)(g_lay + 0xa8), *(s16*)(g_lay + 0x10),
            (s8)g_lay[0x82], g_lay[0x87], *(u16*)(self + 0xb0));
        if (mem_ok(ld, 0xe8)) res_line("layout file", *(u32*)(ld + 0x1c));
    } else
        LOG("     standalone sequence %p handle %d (data handle %d), priority %d, control %d, flags %02x",
            (void*)self, *(s32*)(self + 0x18), *(s32*)(self + 0x1c), *(s16*)(self + 0x10), (s8)self[0x82], self[0x87]);
    LOG("     the mod's own handles:%s", own[0] ? own : " none");
    if (mem_ok(sd, 0xa8)) {
        char nm[20]; name16(sd + 8, nm);
        LOG("     block %p \"%s\"%s: texture %p, its own texture %p, replacement id %x; HUD flags %08x",
            (void*)sd, nm, *(u64*)sd == (u64)(g_base + VT_SEQDATA) ? "" : " (NOT a data block any more)", (void*)tm,
            *(void**)(sd + 0x98), *(u32*)(sd + 0xa0), G(u32, 0x10f9ee48));
        res_line("block's file", *(u32*)(sd + 0x1c));
        res_line("replacement texture", *(u32*)(sd + 0xa0));
    } else LOG("     block %p cannot be read", (void*)sd);
    if (mem_ok(tm, 0x50)) {
        char hx[200]; int n = 0;
        for (int i = 0; i < 0x50; i += 8) n += snprintf(hx + n, sizeof hx - n, " %016llx", (unsigned long long)*(u64*)(tm + i));
        LOG("     what is at the texture's address now:%s", hx);
    }
    if (tm) owners(tm);
}

/* ---- an instance and the file it was made from ---- */
static u8 *inst_obj(int h) {        /* CD2LayCtrl / CD2SeqCtrl of an instance handle */
    u8 *mgr = G(u8*, 0x8f87fe0);
    if (!mgr || h <= 0) return NULL;
    u8 *e = ((u8 *(MSABI *)(u8*, int))(*(void***)mgr)[3])(mgr, h);
    return e ? *(u8**)(e + 8) : NULL;
}
/* 1 = the file (or block) this controller was made from is no longer registered: all its data pointers are dead */
static int data_gone(const u8 *ctl) {
    s32 d = *(s32*)(ctl + 0x1c);
    return d > 0 && G(u8*, 0x8f87fd8) && !data_obj(d);
}
int l2d_live(int *ph) {
    u8 *c = inst_obj(*ph);
    if (!c || !data_gone(c)) return 1;
    LOG("2D: instance %d outlived its file (data handle %d): destroyed, to be made again", *ph, *(s32*)(c + 0x1c));
    FN(void, 0x1a57f0, int)(*ph);       /* safe on dead data: the destroy path looks the data handle up first [1abcf0, 1a3cd0] */
    *ph = 0;
    return 0;
}
static int g_gone_logs;
static void gone_report(u8 *ctl, const char *what) {
    static u8 *seen[8];
    for (int i = 0; i < 8; i++) if (seen[i] == ctl) return;
    seen[g_gone_logs & 7] = ctl;
    if (g_gone_logs++ >= 16) return;
    char own[400]; mod_own_handles(own, sizeof own);
    LOG("!! texture guard: %s %p handle %d is alive but its file was unloaded (data handle %d) -> not drawn; priority %d, flags %02x; the mod's own handles:%s",
        what, (void*)ctl, *(s32*)(ctl + 0x18), *(s32*)(ctl + 0x1c), *(s16*)(ctl + 0x10), ctl[0x87], own[0] ? own : " none");
}

/* ---- the check: 0 draw as it is, 1 drawn with the block's own texture put back, 2 not drawn ---- */
/* What was found sound last time, per block: while the block still names the same image and the image the same
   texture objects, nothing has changed and nothing is asked again (three compares a node).  Any change - another
   image, another object, or other bytes where the image was - takes the full check. */
typedef struct { u8 *sd, *tm; u64 nat, nat2; } Seen;
#define SEEN_SLOTS 256
static Seen g_seen[SEEN_SLOTS];
static int guard_seq(u8 *self) {
    s8 grp = (s8)self[0x86];
    if (grp < 0 || grp > 3 || !G(u8, 0x8f88020 + grp) || !G(u8, 0x8f8802c) || (s8)self[0xb2] <= 0) return 0;   /* no bind */
    u8 *sd = *(u8**)(self + 0x90);
    if (!sd) return 0;                                  /* nothing of ours to judge: the game's own business */
    Seen *e = &g_seen[(((u64)sd >> 4) ^ ((u64)sd >> 12)) & (SEEN_SLOTS - 1)];
    if (e->sd == sd) {
        u8 *t = *(u8**)(sd + 0x90);
        if (t == e->tm && (!t || (*(u64*)(t + 0x28) == e->nat && *(u64*)(t + 0x40) == e->nat2))) return 0;
    }
    if (!mem_ok(sd, 0xa8)) { e->sd = NULL; report(self, sd, NULL, 4, "not drawn"); g_skipped++; return 2; }
    u8 *tm = *(u8**)(sd + 0x90);
    if (!tm) { e->sd = sd; e->tm = NULL; e->nat = e->nat2 = 0; return 0; }
    int why = tm2_bad(tm);
    if (!why) { e->sd = sd; e->tm = tm; e->nat = *(u64*)(tm + 0x28); e->nat2 = *(u64*)(tm + 0x40); return 0; }
    e->sd = NULL;
    u8 *own = *(u8**)(sd + 0x98);
    if (own && own != tm && !tm2_bad(own) && *(u32*)own == TIM2_MAGIC) {
        report(self, sd, tm, why, "the block's own texture is put back");
        *(u8**)(sd + 0x90) = own;
        g_fixed++; return 1;
    }
    report(self, sd, tm, why, "not drawn");
    g_skipped++; return 2;
}
static u64 MSABI seq_draw_hook(u8 *self, u64 a, u64 b, u64 c) {
    if (!g_lay && *(s32*)(self + 0x18) > 0 && data_gone(self)) { gone_report(self, "sequence"); g_skipped++; return 0; }
    if (guard_seq(self) == 2) return 0;
    return o_seq_draw(self, a, b, c);
}
static u64 MSABI lay_draw_hook(u8 *self, u64 a, u64 b, u64 c) {
    if (data_gone(self)) { gone_report(self, "layout"); g_skipped++; return 0; }
    u8 *prev = g_lay; g_lay = self;
    u64 r = o_lay_draw(self, a, b, c);
    g_lay = prev;
    return r;
}

/* ---- install ---- */
extern char g_ini[MAX_PATH + 32];
int guard_check(void) {
    char b[16]; GetPrivateProfileStringA("Safety", "TextureGuard", "0", b, sizeof b, g_ini); c_on = atoi(b);
    if (!c_on) return 1;
    if (G(u64, VT_SEQ_DRAW) != (u64)(g_base + FN_SEQ_DRAW) || G(u64, VT_LAY_DRAW) != (u64)(g_base + FN_LAY_DRAW)) {
        LOG("2D draw vtable slots do not match"); return 0;
    }
    return 1;
}
void guard_apply(void) {
    if (!c_on) return;
    o_seq_draw = (void*)(g_base + FN_SEQ_DRAW); o_lay_draw = (void*)(g_base + FN_LAY_DRAW);
    void *f = (void*)seq_draw_hook; u64 old = (u64)(g_base + FN_SEQ_DRAW);
    if (!patch_bytes(VT_SEQ_DRAW, (u8*)&old, (u8*)&f, 8, "2D node draw")) return;
    f = (void*)lay_draw_hook; old = (u64)(g_base + FN_LAY_DRAW);
    patch_bytes(VT_LAY_DRAW, (u8*)&old, (u8*)&f, 8, "2D layout draw");
    LOG("safety: 2D texture guard on");
}
void guard_stats(int *fixed, int *skipped) { *fixed = g_fixed; *skipped = g_skipped; }

#ifndef _WIN32      /* offline test access */
int test_guard_seq(u8 *self) { return guard_seq(self); }
u64 test_guard_draw(u8 *self, u64 (MSABI *orig)(u8*, u64, u64, u64)) { o_seq_draw = orig; return seq_draw_hook(self, 7, 0, 0); }
void test_guard_lay(u8 *lay) { g_lay = lay; }
int test_guard_probes(void) { return g_probes; }
u64 test_guard_lay_draw(u8 *self, u64 (MSABI *orig)(u8*, u64, u64, u64)) { o_lay_draw = orig; return lay_draw_hook(self, 7, 0, 0); }
#endif

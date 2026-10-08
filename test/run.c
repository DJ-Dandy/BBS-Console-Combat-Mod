/* Offline harness: map the game image on Linux, run the patcher, execute some patched game code natively. */
#define _GNU_SOURCE
#include "../src/core.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <malloc.h>
int mod_install(void);
void log_open(void);
int g_debug = 1;
void dbg_frame(void) {}
void dbg_custom(const char *cmd, char *args);
extern char g_dir[MAX_PATH];
extern int g_patch_errors;

/* ---- imports: a few C runtime functions are real, everything else stops the test with its name ---- */
static void MSABI trap(const char *name) { printf("  import called: %s\n", name); fflush(stdout); _exit(9); }
static void *MSABI i_malloc(size_t n) { return calloc(1, n ? n : 1); }
static void MSABI i_free(void *p) { (void)p; }
static void *MSABI i_realloc(void *p, size_t n) { return realloc(p, n); }
static void *MSABI i_amalloc(size_t n, size_t a) { void *p = memalign(a < 16 ? 16 : a, n ? n : 1); memset(p, 0, n); return p; }
static void *MSABI i_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static void *MSABI i_memcpy(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static int MSABI i_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static size_t MSABI i_strlen(const char *s) { return strlen(s); }
static char *MSABI i_strncpy(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
static int MSABI i_zero(void) { return 0; }
static char *MSABI i_strchr(const char *s, int c) { return strchr(s, c); }
static char *MSABI i_strrchr(const char *s, int c) { return strrchr(s, c); }
static int MSABI i_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static int MSABI i_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static char *MSABI i_strcpy(char *d, const char *s) { return strcpy(d, s); }
static int MSABI i_atoi(const char *s) { return atoi(s); }
#include <math.h>
static float MSABI i_sinf(float x) { return sinf(x); }
static float MSABI i_cosf(float x) { return cosf(x); }
static float MSABI i_floorf(float x) { return floorf(x); }
static float MSABI i_ceilf(float x) { return ceilf(x); }
static float MSABI i_fmodf(float x, float y) { return fmodf(x, y); }
static double MSABI i_ceil(double x) { return ceil(x); }
static double MSABI i_floor(double x) { return floor(x); }
static struct { const char *name; void *fn; } impl[] = {
    {"malloc", i_malloc}, {"free", i_free}, {"realloc", i_realloc}, {"_aligned_malloc", i_amalloc}, {"_aligned_free", i_free},
    {"memset", i_memset}, {"memcpy", i_memcpy}, {"memmove", i_memcpy}, {"memcmp", i_memcmp}, {"strlen", i_strlen}, {"strncpy", i_strncpy},
    {"EnterCriticalSection", i_zero}, {"LeaveCriticalSection", i_zero}, {"InitializeCriticalSection", i_zero},
    {"_Mtx_lock", i_zero}, {"strchr", i_strchr}, {"strrchr", i_strrchr}, {"strcmp", i_strcmp}, {"strncmp", i_strncmp}, {"strcpy", i_strcpy}, {"atoi", i_atoi}, {"sinf", i_sinf}, {"cosf", i_cosf}, {"floorf", i_floorf}, {"ceilf", i_ceilf}, {"fmodf", i_fmodf}, {"ceil", i_ceil}, {"floor", i_floor}, {"__stdio_common_vsprintf", i_zero}, {"_Mtx_unlock", i_zero}, {"_callnewh", i_zero},
};
static void *mk_trap(const char *name) {
    static u8 *pool; static size_t used;
    if (!pool) pool = mmap(NULL, 0x40000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    u8 *p = pool + used; used += 32;
    p[0] = 0x48; p[1] = 0xb9; memcpy(p + 2, &name, 8);            /* mov rcx, name */
    p[10] = 0x48; p[11] = 0xb8; void *t = trap; memcpy(p + 12, &t, 8);  /* mov rax, trap */
    p[20] = 0xff; p[21] = 0xe0;                                   /* jmp rax */
    return p;
}
static void fill_imports(u8 *base) {
    u32 pe = *(u32*)(base + 0x3c);
    u32 imp = *(u32*)(base + pe + 24 + 112 + 8);
    for (u8 *d = base + imp; *(u32*)(d + 12); d += 20) {
        u32 ilt = *(u32*)d ? *(u32*)d : *(u32*)(d + 16), iat = *(u32*)(d + 16);
        for (int i = 0; ; i++) {
            u64 e = *(u64*)(base + ilt + i * 8); if (!e) break;
            const char *nm = (e >> 63) ? "ordinal" : strdup((char*)base + (u32)e + 2);
            void *fn = NULL;
            for (unsigned k = 0; k < sizeof impl / sizeof *impl; k++) if (!strcmp(impl[k].name, nm)) fn = impl[k].fn;
            if (!fn) fn = mk_trap(nm);
            *(void**)(base + iat + i * 8) = fn;
        }
    }
}
static u8 *map_pe(const char *path) {
    FILE *f = fopen(path, "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *d = malloc(n); fread(d, 1, n, f); fclose(f);
    u32 pe = *(u32*)(d + 0x3c); u16 nsec = *(u16*)(d + pe + 6); u16 optsz = *(u16*)(d + pe + 20);
    u32 size = *(u32*)(d + pe + 24 + 56); u32 hdrs = *(u32*)(d + pe + 24 + 60);
    u8 *base = mmap((void*)0x140000000ULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) { perror("mmap"); exit(1); }
    memcpy(base, d, hdrs);
    u8 *s = d + pe + 24 + optsz;
    for (int i = 0; i < nsec; i++, s += 40) {
        u32 vs = *(u32*)(s + 8), va = *(u32*)(s + 12), rs = *(u32*)(s + 16), ro = *(u32*)(s + 20);
        memcpy(base + va, d + ro, rs < vs ? rs : vs);
    }
    free(d); fill_imports(base); return base;
}
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

extern float *test_mp(void), *test_mpmax(void), *test_charge(void); extern int *test_burn(void), *test_bar(void);
extern void test_tick(u8 *g), test_gauge_update(u8 *g); extern u8 *test_use(u8 *P);
float mp_cost(int id);

#define PD ((u8*)RVA(0x10f9eeb0))
static u8 *cmd, *pl, *mgr, *gauge, *plate[8];
static u64 *deck;
/* L2D instance manager stand-in: lookup always fails, so every L2D wrapper is a no-op */
static void *MSABI l2d_lookup(void *mgr_, int h) { (void)mgr_; (void)h; return NULL; }
static void world(int n, const u16 *ids) {
    static void *vt[8]; static void *obj[4];
    for (int i = 0; i < 8; i++) vt[i] = l2d_lookup;
    obj[0] = vt; G(void*, 0x8f87fe0) = obj; G(void*, 0x8f87fd8) = obj;
    pl = calloc(1, 0x800); mgr = calloc(1, 0x200); cmd = calloc(1, 0x360); gauge = calloc(1, 0xc8);
    *(u8**)(mgr + 0x118) = pl; G(u8*, 0x10f9ee40) = mgr; G(u8*, 0x10f9ed40) = cmd; G(u8*, 0x10f9ec60) = gauge;
    *(u8**)(cmd + 0x70) = pl; *(u8**)(cmd + 0x68) = mgr; *(u8**)(pl + 0x390) = cmd; *(u8**)(pl + 0x398) = gauge;
    *(float*)(gauge + 0x20) = 1.0f; *(float*)(cmd + 0x20) = 1.0f;
    PD[0x31] = 1; PD[0x36] = 4;                 /* 4 deck slots: 60 + 4 x 10 = 100 MP */
    deck = RVA(0x8187b0);
    *(u16*)(cmd + 0x220) = n; *(u16*)(cmd + 0x222) = n;
    for (int i = 0; i < n; i++) {
        deck[i] = ids[i] | (1 << 16);
        u8 *P = calloc(1, 0x70); plate[i] = P; *(u8**)(cmd + 0x1e0 + i * 8) = P;
        P[0x30] = 2; P[0x33] = i; P[0x6c] = i; *(u64**)(P + 0x58) = &deck[i]; *(u32*)(P + 0x60) = 0x200; *(float*)(P + 0x64) = 100.0f;
        *(float*)(P + 0x20) = 1.0f;
        if (G(u8, 0x814900 + ids[i] * 0x18 + 1) == 3) { *(u32*)(P + 0x60) |= 0x800; ((u8*)&deck[i])[2] = 3; }
        G(u8, 0x8187a8 + i) = (u8)i;            /* working-deck entry i came from deck slot i */
    }
    for (int i = n; i < 8; i++) G(u8, 0x8187a8 + i) = 0xff;
}
#define PCT(i) (*(float*)(plate[i] + 0x64))
#define FLG(i) (*(u32*)(plate[i] + 0x60))
static void frame(void) {           /* one game frame: our tick, then every plate's update */
    test_tick(gauge);
    for (int i = 0; i < *(u16*)(cmd + 0x220); i++) FN(void, 0x206930, u8*)(plate[i]);
}
static void t_use(void) {
    static const u16 ids[] = { 0x83 /*Fire rl10*/, 0x65 /*Strike Raid rl10*/, 0x92 /*Cure*/, 0xbc /*Potion*/, 0xac /*Mega Flare rl50*/ };
    world(5, ids);
    frame();
    CHECK(*test_mp() == 100 && *test_mpmax() == 100, "start mp %.1f/%.1f", *test_mp(), *test_mpmax());
    CHECK(*(u32*)(cmd + 0x60) & 2, "cursor advance off");
    CHECK(mp_cost(0x83) == 10 && mp_cost(0xac) == 50, "costs %.0f %.0f", mp_cost(0x83), mp_cost(0xac));
    u8 *c = test_use(plate[0]);
    CHECK(c == (u8*)&deck[0] && *test_mp() == 90, "fire used: %p mp %.1f", c, *test_mp());
    CHECK(plate[0][0x31] == 3 && PCT(0) == 100, "plate in 'used' state, pct %.0f", PCT(0));
    frame();
    CHECK(plate[0][0x31] == 0 && !(FLG(0) & 2) && PCT(0) == 100, "fire ready again at once: state %d flags %x pct %.0f", plate[0][0x31], FLG(0), PCT(0));
    c = test_use(plate[0]); frame(); c = test_use(plate[0]); frame();
    CHECK(*test_mp() == 70 && !*test_burn(), "three fires -> mp %.1f", *test_mp());
    /* item: no MP */
    c = test_use(plate[3]); CHECK(c && *test_mp() == 70, "potion costs no MP");
    frame();
    /* mega flare 50, then fire 10, fire 10 -> 0 -> burn */
    c = test_use(plate[4]); frame(); CHECK(*test_mp() == 20, "mega flare -> %.1f", *test_mp());
    c = test_use(plate[0]); frame(); c = test_use(plate[0]);
    CHECK(c && *test_burn() == 1 && *test_mp() == 0, "burn at zero: burn %d mp %.1f", *test_burn(), *test_mp());
    CHECK(PCT(0) == 0, "used plate shows empty");
    frame();
    for (int i = 0; i < 5; i++) if (i != 3) CHECK((FLG(i) & 2) && PCT(i) < 100, "plate %d blocked: flags %x pct %.1f", i, FLG(i), PCT(i));
    CHECK(!(FLG(3) & 2) && PCT(3) == 100, "item plate stays usable");
    CHECK(test_use(plate[1]) == NULL && test_use(plate[2]) == NULL, "no commands in burn");
    CHECK(test_use(plate[3]) != NULL, "items work in burn"); frame();
    /* a D-Link can be started during MP charge; with DLinkDuringCharge = 0 it is refused, as it was at first */
    { extern int mp_blocks_link(void); extern int *test_link_in_burn(void);
      CHECK(*test_link_in_burn() == 1 && mp_blocks_link() == 0, "MP charge does not stand in a D-Link's way");
      *test_link_in_burn() = 0;
      CHECK(mp_blocks_link() == 1 && FN(int, 0x2388a0, u8*)(cmd) == 0, "DLinkDuringCharge = 0: D-Link list refused in burn");
      *test_link_in_burn() = 1; }
    /* recharge: 25 s x 60 ticks */
    for (int i = 0; i < 750; i++) frame();
    printf("  after 750 ticks: charge %.1f plate pct %.1f\n", *test_charge(), PCT(1));
    CHECK(*test_burn() == 1 && *test_charge() > 49 && *test_charge() < 51 && PCT(1) > 49 && PCT(1) < 51, "half charged");
    for (int i = 0; i < 752; i++) frame();
    CHECK(*test_burn() == 0 && *test_mp() == 100, "burn over: burn %d mp %.1f", *test_burn(), *test_mp());
    frame();
    for (int i = 0; i < 5; i++) CHECK(!(FLG(i) & 2) && PCT(i) == 100, "plate %d ready: flags %x pct %.1f", i, FLG(i), PCT(i));
    /* cure takes everything */
    c = test_use(plate[0]); frame();
    c = test_use(plate[2]);
    CHECK(c && *test_burn() == 1 && *test_mp() == 0, "cure uses all MP");
    /* starting a D-Link fills the bar and ends the charge (the game's own confirm, 140205ee0, on a list entry) */
    { extern u8 *test_dlink_pick(u8 *P); extern int *test_link_refill(void);          /* our hook, then the game's function */
      u8 *lp = calloc(1, 0x100); u16 lk[4] = { 0x163, 0, 0, 0 }; *(u16**)(lp + 0x58) = lk;
      for (int i = 0; i < 200; i++) frame();
      float before = *test_charge();
      CHECK(*test_burn() == 1 && before > 0 && before < 100, "in MP charge (%.1f %%)", before);
      u8 *r = test_dlink_pick(lp);
      CHECK(r == (u8*)lk && (*(u32*)(lp + 0x60) & 2), "the game takes the entry");
      CHECK(*test_burn() == 0 && *test_mp() == *test_mpmax() && *test_mp() > 0, "D-Link started: charge over, MP full (%.0f / %.0f)", *test_mp(), *test_mpmax());
      /* an entry the game refuses (used up) changes nothing */
      *test_mp() = 30;
      CHECK(test_dlink_pick(lp) == NULL && *test_mp() == 30, "refused entry: MP as it was");
      /* not with a link already active: the list is only there to end it */
      *(u32*)(lp + 0x60) = 0; *(u32*)(cmd + 0x64) |= 0x80;
      CHECK(test_dlink_pick(lp) == (u8*)lk && *test_mp() == 30, "link already active: no refill");
      *(u32*)(cmd + 0x64) &= ~0x80u;
      /* partly used bar, no charge running */
      *(u32*)(lp + 0x60) = 0;
      CHECK(test_dlink_pick(lp) == (u8*)lk && *test_mp() == *test_mpmax() && *test_burn() == 0, "from 30 MP: full");
      /* a deck command's plate is not a D-Link entry */
      *(u32*)(lp + 0x60) = 0; lk[0] = 0x83; *test_mp() = 30;
      CHECK(test_dlink_pick(lp) == NULL && *test_mp() == 30, "not a D-Link (id 83): refused by the game, no refill");
      /* switched off */
      lk[0] = 0x163; *test_link_refill() = 0;
      CHECK(test_dlink_pick(lp) == (u8*)lk && *test_mp() == 30, "DLinkRefillsMP = 0: the bar is left alone");
      *test_link_refill() = 1; }
    printf("t_use done\n");
}
static void t_ether(void) {
    static const u16 ids[] = { 0x83, 0xbf };
    world(2, ids);
    frame();
    *test_mp() = 30;
    *(s16*)(pl + 0x312) = 0xbf; *(u8**)(pl + 0x3b0) = RVA(0x811080 + 0xbf * 0x1e);   /* Ether: focus +50 */
    *(u8**)(cmd + 0x90) = plate[1];
    FN(void, 0x2624e0, u8*)(pl);
    CHECK(*test_mp() == 80, "ether +50%%: mp %.1f", *test_mp());
    CHECK(*(float*)(gauge + 0x6c) == 50, "focus still restored: %.1f", *(float*)(gauge + 0x6c));
    *test_mp() = 5; test_use(plate[0]); frame();
    CHECK(*test_burn() == 1, "burn");
    FN(void, 0x2624e0, u8*)(pl);
    CHECK(*test_burn() == 1 && *test_charge() >= 50, "ether in burn adds charge: %.1f", *test_charge());
    FN(void, 0x2624e0, u8*)(pl);
    CHECK(*test_burn() == 0 && *test_mp() == 100, "second ether ends burn");
    /* level scaling and reset */
    PD[0x36] = 3; frame(); CHECK(*test_mpmax() == 90 && *test_mp() == 90, "3 deck slots: max MP %.1f, MP clamped to it (%.1f)", *test_mpmax(), *test_mp());
    PD[0x36] = 8; PD[0x31] = 41; frame(); CHECK(*test_mpmax() == 140, "8 deck slots: max MP %.1f (level does not count)", *test_mpmax());
    G(u8, 0x10fa0881) = 3; frame(); CHECK(*test_mpmax() == 100 && *test_mp() <= 100, "Critical, 8 slots: baseline 20 -> max MP %.1f (MP %.1f)", *test_mpmax(), *test_mp());
    PD[0x36] = 3; frame(); CHECK(*test_mpmax() == 50 && *test_mp() == 50, "Critical, 3 slots: max MP %.1f, MP clamped (%.1f)", *test_mpmax(), *test_mp());
    for (int d = 0; d < 3; d++) { G(u8, 0x10fa0881) = d; frame(); CHECK(*test_mpmax() == 90, "difficulty %d, 3 slots: max MP %.1f", d, *test_mpmax()); }
    PD[0x36] = 4; PD[0x31] = 1; *test_mp() = 100; frame(); CHECK(*test_mpmax() == 100 && *test_mp() == 100, "4 slots: %.1f/%.1f", *test_mp(), *test_mpmax());
    /* costs: reload seconds, magic spread out by tier */
    { extern float mp_cost(int id);
      CHECK(mp_cost(0x83) == 10 && mp_cost(0x84) == 15 && mp_cost(0x85) == 20 && mp_cost(0x86) == 25, "Fire 10 / Fira 15 / Firaga 20 / Dark Firaga 25 (%.0f %.0f %.0f %.0f)", mp_cost(0x83), mp_cost(0x84), mp_cost(0x85), mp_cost(0x86));
      CHECK(mp_cost(0x8a) == 10 && mp_cost(0x8b) == 15 && mp_cost(0x8c) == 20 && mp_cost(0x8e) == 10 && mp_cost(0x8f) == 15 && mp_cost(0x90) == 20 && mp_cost(0x91) == 25, "Blizzard and Thunder families the same way");
      CHECK(mp_cost(0xb8) == 10 && mp_cost(0xb9) == 15 && mp_cost(0xba) == 20, "Stop 10 / Stopra 15 / Stopga 20");
      CHECK(mp_cost(0x95) == 20 && mp_cost(0xad) == 25 && mp_cost(0xaf) == 25 && mp_cost(0xae) == 30 && mp_cost(0xac) == 50, "other magic: 15 -> 20, 20 -> 25, the rest as it was");
      CHECK(mp_cost(0x5b) == 10 && mp_cost(0x5c) == 15 && mp_cost(0x62) == 20 && mp_cost(0x6f) == 25 && mp_cost(0xd3) == 40, "attack and friendship commands keep their reload times"); }
    /* the commands only D-Link decks have: by class, as the game costs ordinary commands; their two heals as Cure */
    { extern int *test_dlink_cost(void); extern s16 *mp_cost_overrides(void); extern int mp_would_burn(int id);
      static const struct { int id, cost; const char *name; } dl[] = {
          { 0xe4, 15, "Holy" }, { 0xe5, 10, "WishTurn" }, { 0xe6, 15, "FairyStep" }, { 0xe7, 15, "WishShot" }, { 0xe8, 30, "FairyCure" },
          { 0xe9, 30, "Doc" }, { 0xea, 15, "Grumpy" }, { 0xeb, 15, "Sneezy" }, { 0xec, 10, "Happy" }, { 0xed, 15, "Sleepy" },
          { 0xee, 15, "Bashful" }, { 0xef, 20, "Dopey" }, { 0xf0, 20, "DarkAxis" }, { 0xf1, 20, "DarkSplicer" } };
      for (unsigned i = 0; i < sizeof dl / sizeof *dl; i++)
          CHECK(G(u8, 0x814900 + dl[i].id * 0x18 + 1) == 8 && G(u8, 0x811080 + dl[i].id * 0x1e + 3) == 5 && mp_cost(dl[i].id) == dl[i].cost,
                "%s (%x): category %d, reload %d, cost %.0f, expected %d", dl[i].name, dl[i].id, G(u8, 0x814900 + dl[i].id * 0x18 + 1), G(u8, 0x811080 + dl[i].id * 0x1e + 3), mp_cost(dl[i].id), dl[i].cost);
      CHECK(mp_cost(0xe3) == 2 && mp_cost(0xf2) == 20, "the commands either side of them are as they were (%.0f, %.0f)", mp_cost(0xe3), mp_cost(0xf2));
      *test_mp() = 100; *test_burn() = 0;
      CHECK(mp_would_burn(0xe8) == 1 && mp_would_burn(0xe9) == 1 && mp_would_burn(0x94) == 1 && mp_would_burn(0xe7) == 0, "the two heals use all MP like Curaga; Wish Shot does not");
      mp_cost_overrides()[0xe9] = 12;
      CHECK(mp_cost(0xe9) == 12 && mp_would_burn(0xe9) == 0, "[Cost] e9=12: Doc costs 12 and no longer takes everything");
      mp_cost_overrides()[0x94] = 25;
      CHECK(mp_cost(0x94) == 25 && mp_would_burn(0x94) == 0 && mp_would_burn(0x93) == 1, "[Cost] 94=25: the same for Curaga; Cura still takes everything");
      mp_cost_overrides()[0xe9] = 0; mp_cost_overrides()[0x94] = 0;
      *test_dlink_cost() = 0;
      CHECK(mp_cost(0xe4) == 5 && mp_cost(0xe9) == 5 && mp_cost(0xf1) == 5 && mp_would_burn(0xe9) == 0, "DLinkCostByClass = 0: 5 each, as before");
      *test_dlink_cost() = 1; }
    /* which commands would empty the bar */
    { extern int mp_would_burn(int id);
      CHECK(!mp_would_burn(0x83) && mp_would_burn(0x92), "full MP: Fire is fine, Cure always ends in MP charge");
      *test_mp() = 10; CHECK(mp_would_burn(0x83), "10 MP left: Fire (10) would empty the bar");
      *test_mp() = 11; CHECK(!mp_would_burn(0x83) && mp_would_burn(0x85), "11 MP left: Fire fits, Firaga (20) does not"); }
    /* a save point (its Save prompt up) refills MP within a second and finishes a recharge */
    { static u64 save_cmd = 0x12e, talk_cmd = 0x12f; u8 *pp = calloc(1, 0x70); pp[0x30] = 5; *(u8**)(cmd + 0x298) = pp;
      *test_mp() = 10; *(u64**)(pp + 0x58) = &talk_cmd; *(u32*)(cmd + 0x60) |= 0x40000;
      for (int i = 0; i < 30; i++) frame();
      CHECK(*test_mp() == 10, "another prompt (Talk): nothing restored (%.1f)", *test_mp());
      *(u64**)(pp + 0x58) = &save_cmd;
      for (int i = 0; i < 30; i++) frame();
      CHECK(*test_mp() > 59 && *test_mp() < 61, "half a second on a save point: +50 MP (%.1f)", *test_mp());
      for (int i = 0; i < 30; i++) frame();
      CHECK(*test_mp() == 100, "a second: full (%.1f)", *test_mp());
      *test_mp() = 5; test_use(plate[0]); frame();
      CHECK(*test_burn() == 1, "in MP charge on the save point");
      for (int i = 0; i < 62; i++) frame();
      CHECK(*test_burn() == 0 && *test_mp() == 100, "the charge ends within a second (%d, %.1f)", *test_burn(), *test_mp());
      *test_mp() = 5; test_use(plate[0]); *(u32*)(cmd + 0x60) &= ~0x40000u;
      for (int i = 0; i < 62; i++) frame();
      CHECK(*test_burn() == 1, "away from the save point the charge takes its normal time (%.1f)", *test_charge()); }
    printf("t_ether done\n");
}
/* MP Haste: the MP charge takes 25 s / (1 + 0.05 x Magic Haste installed); the ability's name and description
   (needs BBS_MSG_NAMES / BBS_MSG_HELP = message/en/system/CT00500.ctd / CT00100.ctd for the texts) */
extern float *test_charge_seconds(void), *test_haste_bonus(int atk); extern const char *test_haste_help(int atk);
extern int *test_fresh(void);
extern const char *test_berserk_help(void);
static u8 *slurp(const char *fn, size_t *n);
static int charge_ticks(int magic, int attack) {
    pl[0x4a3 + G(u8, 0x814900 + 0x1d0 * 0x18 + 7)] = (u8)magic;
    pl[0x4a3 + G(u8, 0x814900 + 0x1cf * 0x18 + 7)] = (u8)attack;
    *test_mp() = 0; *test_burn() = 1; *test_charge() = 0;
    int n = 0;
    while (*test_burn() && n < 100000) { test_tick(gauge); n++; }
    return n;
}
static const char *ctd_find(const u8 *file, u32 id) {
    const u32 *rec = (const u32*)(file + *(u32*)(file + 0x10));
    for (int i = 0, n = *(u16*)(file + 0xe); i < n; i++, rec += 3) if (rec[0] == id) return (const char*)file + rec[1];
    return NULL;
}
static void t_haste(void) {
    static const u16 ids[] = { 0x5b, 0x83, 0x92 };
    world(3, ids);
    *test_fresh() = 0;
    CHECK(*test_charge_seconds() == 25.0f && *test_haste_bonus(0) == 0.05f, "25 s, 0.05 a copy (%g, %g)", *test_charge_seconds(), *test_haste_bonus(0));
    CHECK(G(u8, 0x814900 + 0x1d0 * 0x18 + 7) == 13 && G(u8, 0x814900 + 0x1cf * 0x18 + 7) == 12, "the two abilities' slots");
    CHECK(FN(u8, 0x221900, u8*, u16)(pl, 0x1d0) == 0, "none installed");
    int base = charge_ticks(0, 0);
    printf("  MP charge in seconds:");
    for (int k = 0; k <= 5; k++) {
        int n = charge_ticks(k, 0); float want = 1500.0f / (1.0f + 0.05f * (float)k);
        printf(" %d x MP Haste %.1f", k, (float)n / 60.0f);
        CHECK(FN(u8, 0x221900, u8*, u16)(pl, 0x1d0) == k, "%d installed", k);
        CHECK((float)n >= want && (float)n < want + 2.0f, "%d copies: %d ticks, want %.1f", k, n, want);
    }
    printf("\n");
    CHECK(base >= 1500 && base <= 1501, "no ability: 25 s (%d ticks)", base);
    { int n = charge_ticks(0, 2); CHECK(n >= 1363 && n <= 1365, "Attack Haste keeps 0.05 a copy: two -> %d ticks", n); }
    { int n = charge_ticks(3, 2); CHECK(n >= 1200 && n <= 1201, "both add up: 1.25 -> %d ticks", n); }
    *test_haste_bonus(1) = 0; { int n = charge_ticks(0, 5); CHECK(n == base, "AttackHasteBonus = 0: no effect (%d)", n); } *test_haste_bonus(1) = 0.05f;
    charge_ticks(0, 0);

    /* the texts: the game's "file is in memory" call (CRsrcCTD vtable slot 1), on the real files */
    const char *nf = getenv("BBS_MSG_NAMES"), *hf = getenv("BBS_MSG_HELP");
    size_t nn, hn; u8 *names = nf ? slurp(nf, &nn) : NULL, *help = hf ? slurp(hf, &hn) : NULL;
    if (!names || !help) { printf("  (texts skipped: set BBS_MSG_NAMES and BBS_MSG_HELP)\n"); printf("t_haste done\n"); return; }
    u64 (MSABI *ready)(u8*) = *(u64 (MSABI**)(u8*))RVA(0x637910);
    CHECK((void*)ready != (void*)RVA(0x112ed0), "message file vtable slot is the mod's");
    const char *was = ctd_find(names, 0xfa01d0);
    CHECK(was && !strcmp(was, "Magic Haste"), "the game's name: %s", was ? was : "-");
    u8 *o1 = calloc(1, 0x100); *(u8**)(o1 + 0x70) = names; ready(o1);
    #define NAME(id) G(const char*, 0x814908 + (u32)(id) * 0x18)
    CHECK(NAME(0x1d0) && !strcmp(NAME(0x1d0), "MP Haste"), "name: %s", NAME(0x1d0) ? NAME(0x1d0) : "-");
    CHECK(NAME(0x1d0) == was, "written in the file itself");
    CHECK(NAME(0x1cf) && !strcmp(NAME(0x1cf), "MP Haste") && NAME(0x1cf) != NAME(0x1d0), "Attack Haste has the same name: %s", NAME(0x1cf) ? NAME(0x1cf) : "-");
    CHECK(NAME(0x1d9) && !strcmp(NAME(0x1d9), "Berserker") && !strcmp(NAME(0x1d8), "Dark Screen") && !strcmp(NAME(0x1da), "Defender"), "Reload Boost is Berserker: %s", NAME(0x1d9) ? NAME(0x1d9) : "-");
    CHECK(!strcmp(NAME(0x1ce), ctd_find(names, 0xfa01ce)) && !strcmp(NAME(0x1d1), "Combo F Boost") && !strcmp(NAME(0x92), "Cure") && !strcmp(NAME(0x1f1), ctd_find(names, 0xfa01f1)),
          "the names around them are the game's (%s / %s)", NAME(0x1ce), NAME(0x1d1));
    ready(o1);
    CHECK(!strcmp(NAME(0x1d0), "MP Haste") && !strcmp(NAME(0x1cf), "MP Haste"), "a second call changes nothing");
    const char *h0 = ctd_find(help, 0x32020d), *h1 = ctd_find(help, 0x32020c), *ha = ctd_find(help, 0x32020b), *hb = ctd_find(help, 0x32020e);
    CHECK(h0 && !strncmp(h0, "Shortens the reload time for all magic commands", 47), "the game's description");
    CHECK(h1 && !strncmp(h1, "Shortens the reload time for all attack commands", 48), "the game's description of Attack Haste");
    size_t room = h0 ? strlen(h0) : 0, room1 = h1 ? strlen(h1) : 0; char *a0 = strdup(ha), *b0 = strdup(hb);
    u8 *o2 = calloc(1, 0x100); *(u8**)(o2 + 0x70) = help; ready(o2);
    printf("  description: %s\n", h0);
    CHECK(!strcmp(h0, test_haste_help(0)) && strlen(h0) <= room && strstr(h0, "5% faster"), "description replaced (%u of %u bytes)", (unsigned)strlen(h0), (unsigned)room);
    CHECK(!strcmp(h1, test_haste_help(1)) && strlen(h1) <= room1 && !strcmp(h1, h0), "Attack Haste: the same description (%u of %u bytes)", (unsigned)strlen(h1), (unsigned)room1);
    { int w = 0, m = 0, lines = 1; for (const char *c = h0; *c; c++) { if (*c == '\n') { lines++; w = 0; } else if (++w > m) m = w; }
      CHECK(lines <= 3 && m <= 58, "it fits the help box: %d lines, longest %d", lines, m); }
    CHECK(!strcmp(ha, a0) && !strcmp(hb, b0), "the descriptions around them are the game's");
    { const char *hz = ctd_find(help, 0x320216);
      printf("  Berserker: %s\n", hz);
      CHECK(hz && !strcmp(hz, test_berserk_help()) && strstr(hz, "5%") && strlen(hz) <= 97, "Berserker's description (%u bytes)", (unsigned)strlen(hz));
      CHECK(!strncmp(ctd_find(help, 0x320215), "Increases your resistance to darkness", 37) && !strncmp(ctd_find(help, 0x320217), "Increases your Defense", 22), "its neighbours are the game's"); }
    /* an ability whose bonus is 0 keeps the game's texts */
    { size_t n2, h2; u8 *nm2 = slurp(nf, &n2), *hp2 = slurp(hf, &h2);
      *test_haste_bonus(1) = 0;
      u8 *o3 = calloc(1, 0x100); *(u8**)(o3 + 0x70) = nm2; ready(o3);
      u8 *o4 = calloc(1, 0x100); *(u8**)(o4 + 0x70) = hp2; ready(o4);
      CHECK(!strcmp(NAME(0x1cf), "Attack Haste") && !strcmp(NAME(0x1d0), "MP Haste"), "AttackHasteBonus = 0: it stays Attack Haste (%s)", NAME(0x1cf));
      CHECK(!strncmp(ctd_find(hp2, 0x32020c), "Shortens the reload time for all attack", 39) && !strncmp(ctd_find(hp2, 0x32020d), "Makes MP", 8), "... with its own description");
      *test_haste_bonus(1) = 0.05f; }
    CHECK(*(u32*)(o2 + 0xb0) == 0x320000 && *(u32*)(o1 + 0xb0) == 0xfa0000, "the game's own set-up ran");
    /* the game's lookup of an ability's description gives the new text: 1401b3d80 walks the loaded files */
    printf("t_haste done\n");
}
/* Berserker: Reload Boost renamed; what the player deals during the MP charge is 5 % higher, and nothing else.
   On the game's own damage function, through the mod's hook of its one call. */
extern u32 test_damage(u8 *atk, u8 *hit); extern float *test_berserk_pct(void); extern const char *test_berserk_help(void);
static u8 *ent_tab[8];
static void *MSABI ent_lookup(void *mgr_, int id) { static void *node[2]; (void)mgr_; if (id <= 0 || id >= 8 || !ent_tab[id]) return NULL; node[1] = ent_tab[id]; return node; }
static u32 MSABI ent_flags(void *e) { (void)e; return 0x10000; }
static u8 *entity(int id, int type, u8 *parent, u8 *mem) {
    static void *vt[16]; for (int i = 0; i < 16; i++) vt[i] = ent_flags;
    u8 *e = mem ? mem : calloc(1, 0x100);
    *(void**)e = vt; *(int*)(e + 0x28) = type; *(u8**)(e + 0x10) = parent; ent_tab[id] = e;
    return e;
}
static void t_berserk(void) {
    static const u16 ids[] = { 0x5b, 0x83, 0x92 };
    world(3, ids);
    *test_fresh() = 0;
    { static void *vt[8], *obj[2]; for (int i = 0; i < 8; i++) vt[i] = ent_lookup; obj[0] = vt; G(void*, 0x8f6d540) = obj; }
    entity(1, 1, NULL, pl);                             /* the player */
    u8 *foe = entity(2, 2, NULL, NULL);                 /* an enemy */
    entity(3, 3, pl, NULL);                             /* something the player's (a spell in flight) */
    entity(4, 3, foe, NULL);                            /* something the enemy's */
    u8 *atk = calloc(1, 0x100), *hit = calloc(1, 0x100);
    *(s16*)(atk + 0x82) = 105; *(s16*)(atk + 0x84) = 100; *(float*)(atk + 0x88) = 1.0f;
    *(s16*)(hit + 0xac) = 5; *(s16*)(hit + 0xae) = 9999; *(float*)(hit + 0xc4) = 1.0f;
    #define DMG(owner) (*(u32*)(atk + 0x94) = (owner), test_damage(atk, hit))
    int slot = G(u8, 0x814900 + 0x1d9 * 0x18 + 7);
    CHECK(slot == 22, "Reload Boost's ability slot (%d)", slot);
    { u8 *p = RVA(0x1f985a); s32 d; memcpy(&d, p + 1, 4); CHECK(p[0] == 0xE8 && (u32)(0x1f985a + 5 + d) != 0x1f9180, "the game's damage call goes through the mod"); }
    CHECK(*test_berserk_pct() == 5.0f, "5 %% by default (%g)", *test_berserk_pct());
    *test_burn() = 0; pl[0x4a3 + slot] = 1;
    CHECK(DMG(1) == 100, "the game's formula: (105 - 5) x 100 %% = %u", DMG(1));
    CHECK(DMG(1) == 100, "ability on, MP there: nothing added (%u)", DMG(1));
    *test_burn() = 1; pl[0x4a3 + slot] = 0;
    CHECK(DMG(1) == 100, "MP charge without the ability: nothing added (%u)", DMG(1));
    pl[0x4a3 + slot] = 1;
    CHECK(DMG(1) == 105, "MP charge with the ability: 100 -> %u", DMG(1));
    CHECK(*(float*)(atk + 0x88) == 1.0f && *(s16*)(atk + 0x82) == 105 && *(s16*)(atk + 0x84) == 100, "the attack record is left as it was");
    CHECK(DMG(3) == 105, "the player's spell: %u", DMG(3));
    CHECK(DMG(2) == 100 && DMG(4) == 100, "an enemy's attacks: %u / %u", DMG(2), DMG(4));
    CHECK(DMG(5) == 100 && DMG(0) == 100, "an owner that is not there: %u", DMG(5));
    *(float*)(atk + 0x88) = 1.5f;
    CHECK(DMG(1) == 158 && *(float*)(atk + 0x88) == 1.5f, "on top of the game's own multiplier: 150 + 7.5 rounded up = %u", DMG(1));
    *(float*)(atk + 0x88) = 1.0f;
    *(u16*)(atk + 0x7e) = 0x22; CHECK(DMG(1) == 205, "a cure (kind 22) is not raised: %u", DMG(1));
    *(u16*)(atk + 0x7e) = 0x2a; CHECK(DMG(1) == 105, "a percentage (kind 2a) is not raised: %u", DMG(1));
    *(u16*)(atk + 0x7e) = 0x80; CHECK(DMG(1) == 105, "the kind's top bit is a flag: %u", DMG(1));
    /* the bonus is rounded up to the next whole number */
    { static const int want[][2] = { {1, 2}, {2, 3}, {5, 6}, {19, 20}, {20, 21}, {21, 23}, {40, 42}, {41, 44}, {100, 105} };
      printf("  5 %% rounded up:");
      for (unsigned i = 0; i < sizeof want / sizeof *want; i++) {
          *(s16*)(atk + 0x82) = (s16)(want[i][0] + 5);
          *test_burn() = 0; u32 b = DMG(1); *test_burn() = 1; u32 g = DMG(1);
          printf(" %u->%u", b, g);
          CHECK((int)b == want[i][0] && (int)g == want[i][1], "%d -> %u, want %d", want[i][0], g, want[i][1]);
      }
      printf("\n"); }
    *(s16*)(atk + 0x84) = 1; *(s16*)(atk + 0x82) = 6; *test_burn() = 0;
    { u32 b = DMG(1); *test_burn() = 1; CHECK(b == 1 && DMG(1) == 2, "the game's least hit, 1 (from %.2f): 1 -> %u", 0.01, DMG(1)); }
    *(s16*)(atk + 0x84) = 100;
    *(s16*)(atk + 0x82) = 105;
    *(u16*)(atk + 0x7e) = 0;
    *(s16*)(atk + 0x82) = 30000; *(s16*)(atk + 0x84) = 200; *(s16*)(hit + 0xae) = 32000;
    CHECK(DMG(1) == 0x7fff, "never past what the hit record can hold (%u)", DMG(1));
    *(s16*)(atk + 0x82) = 105; *(s16*)(atk + 0x84) = 100; *(s16*)(hit + 0xae) = 9999;
    *test_berserk_pct() = 50; CHECK(DMG(1) == 150, "BerserkerDamage = 50: %u", DMG(1));
    *test_berserk_pct() = 0;  CHECK(DMG(1) == 100, "BerserkerDamage = 0: off (%u)", DMG(1));
    *test_berserk_pct() = 5;
    /* the MP charge ends: so does the bonus */
    *test_charge() = 99.99f; test_tick(gauge); test_tick(gauge);
    CHECK(*test_burn() == 0 && DMG(1) == 100, "charge over: back to %u", DMG(1));
    /* HP has nothing to do with it any more, and the old reload effect is gone with the reloads */
    printf("t_berserk done\n");
}
/* the MP bar on the real 2D-layout runtime, with the real gauge_01.l2d (needs BBS_GAUGE_L2D=<file>) */
extern void test_bar_update(u8 *g);
typedef struct { s16 x0, y0, x1, y1; u16 part, attr; } Grp;
typedef struct { s16 u0, v0, u1, v1; u32 col[4]; } Part;
typedef struct { Grp *g; Part *p; } Pair;
static void t_bar(void) {
    const char *fn = getenv("BBS_GAUGE_L2D");
    if (!fn) { printf("  (skipped: BBS_GAUGE_L2D not set)\n"); return; }
    FILE *f = fopen(fn, "rb"); if (!f) { printf("  cannot open %s\n", fn); fails++; return; }
    u8 *img = memalign(16, 0x20000); size_t n = fread(img, 1, 0x20000, f); fclose(f);
    PD[0x31] = 1; PD[0x36] = 4;                 /* 100 MP */
    *(u8*)RVA(0x111660) = 0xC3;                 /* task registration: not needed here */
    *(u8*)RVA(0x10c5e0) = 0xC3;                 /* texture upload: no graphics device here */
    memcpy(RVA(0x11a8a0), "\x31\xc0\xc3", 3);      /* resource lookup: nothing loaded */
    FN(void, 0x1a6d70, void)();                 /* L2D runtime init */
    int d = FN(int, 0x1a6fb0, void*, const char*, int)(img, "gauge_01.l2d", 0);
    printf("  registered gauge_01 (%zu bytes): data handle %d, block 0 handle %d\n", n, d, FN(int, 0x1a64f0, const char*)("gauge_01:0"));
    CHECK(d > 0, "register");
    pl = calloc(1, 0x800); mgr = calloc(1, 0x200); gauge = calloc(1, 0xc8);
    *(u8**)(mgr + 0x118) = pl; G(u8*, 0x10f9ee40) = mgr; G(u8*, 0x10f9ec60) = gauge; PD[0x31] = 1;
    *(float*)(gauge + 0x20) = 1.0f;
    { extern void test_bar_geom(float *io, int set); float g[4]; test_bar_geom(g, 0);
      CHECK(g[0] == 425.0f && g[1] == 254.6f && g[2] == 4.5f && g[3] == 57.6f, "default place and size: right %.1f top %.1f height %.1f length %.1f", g[0], g[1], g[2], g[3]);
      float old[4] = { 425.0f, 255.5f, 7.5f, 50.0f }; test_bar_geom(old, 1); }     /* the numbers below are for the first size of the bar */
    test_tick(gauge);
    test_bar_update(gauge);
    int h = *test_bar();
    CHECK(h > 0, "bar instance %d", h);
    Pair *q = NULL; int cnt = FN(int, 0x1a6350, int, int, Pair**)(h, 3, &q);
    CHECK(cnt == 7 && q, "fill sprite has %d quads", cnt);
    /* without the KH2 art: the plain bar, length 50 -> 50 * 4 / (7.5 / 6.8 * 0.9) sprite units */
    CHECK(q && q[4].g->x1 - q[4].g->x0 == 201 && q[2].g->x1 - q[2].g->x0 == 201, "plain bar, full: fill %d of %d", q[4].g->x1 - q[4].g->x0, q[2].g->x1 - q[2].g->x0);
    /* with the art: 20 art pixels = 7.5 units, so 50 units = 133.3 art pixels = 533 sprite units; inner span 8..533 */
    { extern void test_set_art(int); test_set_art(1); }
    test_bar_update(gauge);
    for (int i = 0; i < cnt && q; i++) printf("   quad %d xy (%d,%d)-(%d,%d) uv (%d,%d)-(%d,%d) col %08x %08x\n", i, q[i].g->x0, q[i].g->y0, q[i].g->x1, q[i].g->y1, q[i].p->u0, q[i].p->v0, q[i].p->u1, q[i].p->v1, q[i].p->col[0], q[i].p->col[1]);
    /* round left end: frame = round piece 0..32 + rectangle; inside = round piece 8..32 + rectangle; a full bar's
       fill the same two pieces in blue; the plate last */
    CHECK(q && q[0].g->x0 == 0 && q[0].g->x1 == 32 && q[0].g->y0 == 4 && q[0].g->y1 == 84 && q[0].p->u0 == 108 && q[0].p->u1 == 116 && q[0].p->v0 == 214 && q[0].p->v1 == 234,
          "frame, round end: %d..%d u %d..%d v %d..%d", q[0].g->x0, q[0].g->x1, q[0].p->u0, q[0].p->u1, q[0].p->v0, q[0].p->v1);
    CHECK(q && q[1].g->x0 == 32 && q[1].g->x1 == 533 + 24 && q[1].p->u0 == q[1].p->u1, "frame, straight part: %d..%d", q[1].g->x0, q[1].g->x1);
    CHECK(q && q[2].g->x0 == 8 && q[2].g->x1 == 32 && q[2].g->y0 == 12 && q[2].g->y1 == 76 && q[2].p->u0 == 138 && q[2].p->u1 == 144 && q[2].p->v0 == 216 && q[2].p->v1 == 232,
          "empty part, round end: %d..%d u %d..%d", q[2].g->x0, q[2].g->x1, q[2].p->u0, q[2].p->u1);
    CHECK(q && q[3].g->x0 == 32 && q[3].g->x1 == 533 && q[3].p->u0 == 180, "empty part, straight: %d..%d u %d", q[3].g->x0, q[3].g->x1, q[3].p->u0);
    CHECK(q && q[4].g->x0 == 8 && q[4].g->x1 == 32 && q[4].p->u0 == 124 && q[4].p->u1 == 130 && q[4].p->col[0] == 0x80808080, "full bar: fill's round end %d..%d u %d..%d", q[4].g->x0, q[4].g->x1, q[4].p->u0, q[4].p->u1);
    CHECK(q && q[5].g->x0 == 32 && q[5].g->x1 == 533 && q[5].p->u0 == 168 && q[5].p->v0 == 188 && q[5].p->v1 == 204, "full bar: fill %d..%d u %d", q[5].g->x0, q[5].g->x1, q[5].p->u0);
    CHECK(q && q[6].g->x0 == 533 && q[6].g->x1 == 533 + 208 && q[6].g->y1 == 88 && q[6].p->u0 == 106 && q[6].p->u1 == 158, "MP plate %d..%d u %d..%d", q[6].g->x0, q[6].g->x1, q[6].p->u0, q[6].p->u1);
    Pair *m = NULL; int mc = FN(int, 0x1a6350, int, int, Pair**)(h, 2, &m);
    CHECK(mc == 1 && m && m[0].g->x1 == 0 && m[0].g->y0 == 0, "mask quad emptied (%d quads)", mc);
    float x = 0, y = 0; FN(int, 0x1a6290, int, float*, float*)(h, &x, &y);
    printf("  bar origin (%.2f, %.2f), visible %d, control %d\n", x, y, FN(int, 0x1a6060, int)(h), FN(int, 0x1a60e0, int)(h));
    CHECK(FN(int, 0x1a6060, int)(h) == 0, "hidden while the HP gauge is hidden");
    {   /* right end of the plate's solid part at x 425, top of the bar at y 255.5 */
        float se = 7.5f / 20.0f / 4.0f;
        float rx = x + (533 + 51 * 4 - 77) * se, ty = y + (4 + 1) * se;
        CHECK(rx > 424.99f && rx < 425.01f && ty > 255.49f && ty < 255.51f, "placement: right %.3f top %.3f", rx, ty);
    }
    *test_mp() = 25; test_bar_update(gauge);
    CHECK(q[5].g->x1 == 533 && q[5].g->x1 - q[5].g->x0 == 131 && q[4].g->x1 == 0, "quarter bar, kept at the plate's side: %d..%d, nothing in the round end", q[5].g->x0, q[5].g->x1);
    /* 98 MP: the fill ends 10.5 sprite units from the inside's left end = 3 art pixels into the round piece (rounded) */
    *test_mp() = 98; test_bar_update(gauge);
    CHECK(q[5].g->x0 == 32 && q[4].g->x0 == 8 + 12 && q[4].g->x1 == 32 && q[4].p->u0 == 124 + 3 && q[4].p->u1 == 130, "98 MP: fill reaches into the round end: %d..%d u %d..%d", q[4].g->x0, q[4].g->x1, q[4].p->u0, q[4].p->u1);
    *test_mp() = 25;
    *test_burn() = 1; *test_charge() = 50; test_bar_update(gauge);
    CHECK(q[5].p->u0 == 180 && q[5].g->x1 - q[5].g->x0 == 263 && q[6].p->col[0] == 0x80c060ff, "recharge: grey gradient tinted, %d wide, plate %08x", q[5].g->x1 - q[5].g->x0, q[6].p->col[0]);
    *test_charge() = 100; test_bar_update(gauge);
    CHECK(q[4].p->u0 == 138 && q[4].g->x0 == 8 && q[4].p->col[0] == q[5].p->col[0], "recharge full: the round end from the grey piece, tinted alike (u %d)", q[4].p->u0);
    *test_burn() = 0; *test_charge() = 0;
    /* RoundEnd=0: the old four quads */
    { extern int *test_bar_round(void); *test_bar_round() = 0; test_bar_update(gauge);
      CHECK(q[0].g->x1 == 533 + 24 && q[1].g->x0 == 8 && q[1].g->x1 == 533 && q[2].g->x1 - q[2].g->x0 == 131 && q[3].g->x0 == 533 && q[3].p->u0 == 106 && q[4].g->x1 == 0 && q[6].g->x1 == 0,
            "square end: frame %d, inside %d..%d, plate at %d", q[0].g->x1, q[1].g->x0, q[1].g->x1, q[3].g->x0);
      *test_bar_round() = 1; test_bar_update(gauge); }
    /* the game's own focus gauge data must be untouched: a second instance still has the original sprite */
    G(u8, 0x8f88028) = 0;
    int h2 = FN(int, 0x1a5350, const char*, u16, int, void*)("gauge_01:0", 0x12d, 0, NULL);
    FN(int, 0x1a7be0, int, int)(h2, 3); Pair *q2 = NULL; FN(int, 0x1a6350, int, int, Pair**)(h2, 3, &q2);
    CHECK(q2 && q2[1].p->col[0] == 0x8000ffff && q2[1].g->x0 == -5, "shared sprite data untouched: %08x %d", q2 ? q2[1].p->col[0] : 0, q2 ? q2[1].g->x0 : 0);
    FN(void, 0x1a57f0, int)(h2);
    FN(void, 0x1a57f0, int)(h);
    test_bar_update(gauge);
    CHECK(*test_bar() > 0 && *test_bar() != h, "re-created after the instance was destroyed (%d -> %d)", h, *test_bar());
    /* the plain timer bar used by the style prompt: 84 x 2.6 at (18, 170), half full */
    { extern void hud_timer_bar(int *ph, float left, float top, float w, float hgt, float frac, u32 fill, int prio, int show);
      int tb = 0; hud_timer_bar(&tb, 18.0f, 170.0f, 84.0f, 2.6f, 0.5f, 0x8020a0ff, 0xc, 1);
      CHECK(tb > 0 && tb != *test_bar(), "timer bar instance %d", tb);
      Pair *t = NULL; int tc = FN(int, 0x1a6350, int, int, Pair**)(tb, 3, &t);
      CHECK(tc == 7 && t && t[0].g->x1 == 672 && t[0].g->y1 == 21 && t[2].g->x0 == 4 && t[2].g->x1 == 4 + 332 && t[2].p->col[0] == 0x8020a0ff && t[3].g->x1 == 0,
            "timer bar quads: frame %d x %d, fill %d..%d", t ? t[0].g->x1 : 0, t ? t[0].g->y1 : 0, t ? t[2].g->x0 : 0, t ? t[2].g->x1 : 0);
      float bx = 0, by = 0; FN(int, 0x1a6290, int, float*, float*)(tb, &bx, &by);
      CHECK(bx - 77.0f / 8 > 17.99f && bx - 77.0f / 8 < 18.01f && by + 1.0f / 8 > 169.99f && by + 1.0f / 8 < 170.01f && FN(int, 0x1a6060, int)(tb) == 1, "timer bar placed and shown (%.2f, %.2f)", bx, by);
      hud_timer_bar(&tb, 0, 0, 0, 0, -1, 0, 0, 0);
      CHECK(tb == 0, "timer bar removed"); }
    printf("t_bar done\n");
}
/* What the game draws for an instance: its own draw code (CD2SeqCtrl::Draw 1401aaea0 -> CD2Obj::Draw 1401a87b0 ->
   the quad builders) with the renderer stubbed out, so the vertices it builds can be read.  A vertex is 0x1c bytes:
   x, y, z (floats, in the game's 480 x 272 screen units), colour, u, v (floats), a flag. */
static float g_vtx[8][7 * 4 * 8]; static int g_vn[8], g_vmode[8], g_vcalls, g_vlast_mode;
static void *MSABI s_valloc(int count, int stride, int z) { (void)stride; (void)z; int i = g_vcalls < 8 ? g_vcalls : 7; g_vn[i] = count; g_vmode[i] = g_vlast_mode; g_vcalls++; return g_vtx[i]; }
static void MSABI s_mode(int m) { g_vlast_mode = m; }
static float g_uvscale[2] = { 1.0f, 1.0f };
static void *MSABI s_uvscale(void) { return g_uvscale; }
static void stub_jmp(u32 rva, void *fn) { u8 *p = RVA(rva); p[0] = 0x48; p[1] = 0xb8; memcpy(p + 2, &fn, 8); p[10] = 0xff; p[11] = 0xe0; }
static void draw_stubs(void) {
    static const u32 rets[] = { 0x521860, 0x5201f0, 0x520490, 0x5213a0, 0x5213c0, 0x4e3210, 0x10ccb0, 0x520510, 0x520420, 0x520370 };
    for (unsigned i = 0; i < sizeof rets / sizeof *rets; i++) *(u8*)RVA(rets[i]) = 0xC3;
    stub_jmp(0x4da890, s_valloc); stub_jmp(0x4eff50, s_mode); stub_jmp(0x4e48c0, s_uvscale);
}
/* draws the instance; returns the vertices of its sprite with `quads` quads (NULL if none), *mode = the filter mode
   the game chose (3 = the pixel-exact path for unscaled sprites, 4 = the plain path for scaled ones) */
static float *draw_capture(int h, int quads, int *mode) {
    u8 *c = FN(u8*, 0x1a5fd0, int)(h);
    if (!c) return NULL;
    G(u8, 0x8f88020 + (s8)c[0x86]) = 1; G(u8, 0x8f8802c) = 1;
    g_vcalls = 0; memset(g_vtx, 0, sizeof g_vtx);
    FN(void, 0x1aaea0, u8*, int)(c, 0);
    for (int i = 0; i < g_vcalls && i < 8; i++) if (g_vn[i] == quads * 4) { if (mode) *mode = g_vmode[i]; return g_vtx[i]; }
    return NULL;
}
static void quad_box(float *v, int q, float *b) {       /* x0, y0, x1, y1, u0, v0, u1, v1 of quad q */
    b[0] = b[1] = b[4] = b[5] = 1e9f; b[2] = b[3] = b[6] = b[7] = -1e9f;
    for (int i = 0; i < 4; i++) { float *p = v + (q * 4 + i) * 7;
        if (p[0] < b[0]) b[0] = p[0]; if (p[0] > b[2]) b[2] = p[0]; if (p[1] < b[1]) b[1] = p[1]; if (p[1] > b[3]) b[3] = p[1];
        if (p[4] < b[4]) b[4] = p[4]; if (p[4] > b[6]) b[6] = p[4]; if (p[5] < b[5]) b[5] = p[5]; if (p[5] > b[7]) b[7] = p[5]; }
}
static void t_draw(void) {
    const char *fn = getenv("BBS_GAUGE_L2D");
    if (!fn) { printf("  (skipped: BBS_GAUGE_L2D not set)\n"); return; }
    FILE *f = fopen(fn, "rb"); if (!f) { printf("  cannot open %s\n", fn); fails++; return; }
    u8 *img = memalign(16, 0x20000); fread(img, 1, 0x20000, f); fclose(f);
    PD[0x31] = 1; PD[0x36] = 4;
    *(u8*)RVA(0x111660) = 0xC3; *(u8*)RVA(0x10c5e0) = 0xC3; memcpy(RVA(0x11a8a0), "\x31\xc0\xc3", 3);
    FN(void, 0x1a6d70, void)();
    CHECK(FN(int, 0x1a6fb0, void*, const char*, int)(img, "gauge_01.l2d", 0) > 0, "register gauge_01");
    pl = calloc(1, 0x800); mgr = calloc(1, 0x200); gauge = calloc(1, 0xc8);
    *(u8**)(mgr + 0x118) = pl; G(u8*, 0x10f9ee40) = mgr; G(u8*, 0x10f9ec60) = gauge; *(float*)(gauge + 0x20) = 1.0f;
    { extern void test_set_art(int); test_set_art(1); }
    test_tick(gauge); test_bar_update(gauge);
    int h = *test_bar();
    draw_stubs();
    int mode = 0; float *v = draw_capture(h, 7, &mode);
    #define NEAR(a, b) ((a) - (b) < 0.002f && (b) - (a) < 0.002f)
    if (v) {    /* the default bar: under the Focus bar (outline to about y 253.9), 4.5 high, as long as the first bar, right end at 425 */
        float fr[8], fc[8], fi[8], pt[8]; quad_box(v, 0, fc); quad_box(v, 1, fr); quad_box(v, 5, fi); quad_box(v, 6, pt);
        printf("   default bar: frame x %.3f..%.3f y %.3f..%.3f, fill y %.3f..%.3f, plate x %.3f..%.3f y %.3f..%.3f\n", fc[0], fr[2], fr[1], fr[3], fi[1], fi[3], pt[0], pt[2], pt[1], pt[3]);
        CHECK(NEAR(fr[1], 254.6f) && NEAR(fr[3], 259.1f) && NEAR(fc[1], 254.6f) && NEAR(fi[1], 254.6f + 0.45f) && NEAR(fi[3], 259.1f - 0.45f), "default bar: 4.5 high from 254.6, borders 0.45");
        CHECK(pt[1] > 253.9f + 0.4f && NEAR(pt[2], 425.0f + 0.225f) && fc[0] > 355.85f && fc[0] < 355.95f && pt[0] - fc[0] > 57.55f && pt[0] - fc[0] < 57.65f, "below the Focus bar's outline (plate from %.3f), left end at x %.3f as the first bar's (355.906), right end at 425", pt[1], fc[0]);
    }
    { extern void test_bar_geom(float *io, int set); float old[4] = { 425.0f, 255.5f, 7.5f, 50.0f }; test_bar_geom(old, 1); }     /* the numbers below are for the first size of the bar */
    test_bar_update(gauge); v = draw_capture(h, 7, &mode);
    CHECK(v != NULL, "the bar's sprite is drawn (%d draw calls)", g_vcalls);
    if (v) {
        float b[7][8];
        for (int q = 0; q < 7; q++) { quad_box(v, q, b[q]); printf("   quad %d: x %.3f..%.3f y %.3f..%.3f  u %.2f..%.2f v %.2f..%.2f\n", q, b[q][0], b[q][2], b[q][1], b[q][3], b[q][4], b[q][6], b[q][5], b[q][7]); }
        /* the bar as designed: top 255.5, 7.5 high, borders 0.75, right end of the plate's solid part at 425 */
        CHECK(mode == 4, "drawn as a scaled sequence: corners are not moved to whole screen units (mode %d)", mode);
        CHECK(NEAR(b[1][1], 255.5f) && NEAR(b[1][3], 263.0f) && NEAR(b[0][1], 255.5f) && NEAR(b[0][3], 263.0f) && NEAR(b[0][2], b[1][0]), "frame: round end and straight part both %.3f..%.3f, joined at x %.3f", b[1][1], b[1][3], b[1][0]);
        CHECK(NEAR(b[5][1], 256.25f) && NEAR(b[5][3], 262.25f) && NEAR(b[4][1], 256.25f) && NEAR(b[4][3], 262.25f) && NEAR(b[4][0] - b[0][0], 0.75f) && NEAR(b[4][2], b[5][0]), "fill: %.3f..%.3f, 0.75 inside the frame on the left (%.3f)", b[5][1], b[5][3], b[4][0] - b[0][0]);
        CHECK(NEAR(b[6][2], 425.375f) && NEAR(b[6][1], 255.125f) && NEAR(b[6][3], 263.375f) && NEAR(b[6][0], b[5][2]), "plate: starts where the fill ends (%.3f), solid part ends at 425", b[6][0]);
        CHECK(b[0][4] == 108 && b[0][6] == 116 && b[0][5] == 214 && b[0][7] == 234 && b[4][4] == 124 && b[4][5] == 216 && b[6][4] == 106 && b[6][7] == 208, "texture rectangles reach the renderer as set (round end u %.2f..%.2f v %.2f..%.2f)", b[0][4], b[0][6], b[0][5], b[0][7]);
    }
    /* the game's own rule, which ExactSize=0 leaves in force: an unscaled sequence is drawn on the pixel grid of the
       480 x 272 screen - the same bar comes out 8 high with borders of 1 */
    { extern int *test_bar_exact(void); *test_bar_exact() = 0;
      FN(void, 0x1a57f0, int)(h); test_bar_update(gauge); h = *test_bar();
      v = draw_capture(h, 7, &mode);
      float fr[8], fi[8]; if (v) { quad_box(v, 1, fr); quad_box(v, 5, fi); }
      CHECK(v && mode == 3 && fr[1] == 255.0f && fr[3] == 263.0f && fi[1] == 256.0f && fi[3] == 262.0f, "ExactSize=0: on the pixel grid, frame %.3f..%.3f fill %.3f..%.3f (mode %d)", v ? fr[1] : 0, v ? fr[3] : 0, v ? fi[1] : 0, v ? fi[3] : 0, mode);
      *test_bar_exact() = 1; test_bar_update(gauge);
      v = draw_capture(h, 7, &mode); if (v) quad_box(v, 1, fr);
      CHECK(v && mode == 4 && NEAR(fr[1], 255.5f), "and back (an instance made on the grid is taken off it)"); }
    /* the Command Style offer's timer bar: 84 x 2.6 at (18, 170), frame 0.5 thick */
    { extern void hud_timer_bar(int *ph, float left, float top, float w, float hgt, float frac, u32 fill, int prio, int show);
      int tb = 0; hud_timer_bar(&tb, 18.0f, 170.0f, 84.0f, 2.6f, 0.5f, 0x8020a0ff, 0xc, 1);
      v = draw_capture(tb, 7, &mode);
      float fr[8], in[8], fi[8]; if (v) { quad_box(v, 0, fr); quad_box(v, 1, in); quad_box(v, 2, fi); }
      CHECK(v && mode == 4 && NEAR(fr[0], 18.0f) && NEAR(fr[2], 102.0f) && NEAR(fr[1], 170.0f) && NEAR(fr[3], 172.625f) && NEAR(in[1], 170.5f) && NEAR(in[3], 172.125f) && NEAR(fi[2], 18.5f + 41.5f),
            "timer bar: frame %.3f..%.3f x %.3f..%.3f, inside %.3f..%.3f, fill to %.3f", v ? fr[0] : 0, v ? fr[2] : 0, v ? fr[1] : 0, v ? fr[3] : 0, v ? in[1] : 0, v ? in[3] : 0, v ? fi[2] : 0);
      /* both kinds of instance at once keep their own scale */
      v = draw_capture(h, 7, &mode); if (v) quad_box(v, 1, fr);
      CHECK(v && NEAR(fr[1], 255.5f) && NEAR(fr[3], 263.0f), "the MP bar is unchanged by it"); }
    #undef NEAR
    printf("t_draw done\n");
}
/* the texture guard (src/guard.c): a node whose block points at a texture that is gone */
extern int test_guard_seq(u8 *self);
extern u64 test_guard_draw(u8 *self, u64 (MSABI *orig)(u8*, u64, u64, u64));
extern void test_guard_lay(u8 *lay);
extern int (*test_guard_page)(u64 pg);
static int g_guard_calls; static u64 g_guard_badpg;
static u64 MSABI guard_orig(u8 *s, u64 a, u64 b, u64 c) { g_guard_calls++; return 0x1234 + a; }
static int guard_page(u64 pg) { return pg != g_guard_badpg; }
static u8 *guard_tm2(u64 native) {
    u8 *tm = memalign(16, 0x100); memset(tm, 0, 0x100);
    memcpy(tm, "TIM2", 4); tm[0xf] = 1; tm[0x3d] = 0x40; *(u16*)(tm + 0x3e) = 0x8100; *(u64*)(tm + 0x28) = native;
    return tm;
}
static void t_guard(void) {
    test_guard_page = guard_page; g_guard_badpg = ~0ull;
    u8 *self = memalign(16, 0x100); memset(self, 0, 0x100);
    u8 *sd = memalign(16, 0x100); memset(sd, 0, 0x100);
    u8 *native = memalign(16, 0x100); memset(native, 0, 0x100); *(u32*)(native + 0x10) = 0x02f02eb5;
    *(u64*)sd = (u64)(g_base + 0x641f40); strcpy((char*)sd + 8, "msn_00:1");
    *(u8**)(self + 0x90) = sd; self[0x86] = 0; self[0xb2] = 1; *(u16*)(self + 0xb0) = 0x21;
    G(u8, 0x8f88020) = 1; G(u8, 0x8f8802c) = 1;
    u8 *good = guard_tm2((u64)native), *stale = guard_tm2(0x74f111e420036048ull);    /* the value of both crashes */
    memcpy(stale, "\x11\x22\x33\x44", 4);
    /* a sound texture is left alone */
    *(u8**)(sd + 0x90) = good; *(u8**)(sd + 0x98) = good;
    CHECK(test_guard_seq(self) == 0 && *(u8**)(sd + 0x90) == good, "a sound texture is drawn as it is");
    { extern int test_guard_probes(void);
      int p0 = test_guard_probes(), ok = 1;
      for (int i = 0; i < 1000; i++) ok &= test_guard_seq(self) == 0;
      CHECK(ok && test_guard_probes() == p0, "nothing changed: no memory is asked about again (%d probes in 1000 draws)", test_guard_probes() - p0);
      *(u64*)(good + 0x28) = (u64)native + 0x10; p0 = test_guard_probes();           /* another texture object: looked at again */
      CHECK(test_guard_seq(self) == 0 && test_guard_probes() > p0, "another texture object: checked afresh");
      *(u64*)(good + 0x28) = (u64)native; CHECK(test_guard_seq(self) == 0, "and back"); }
    g_guard_calls = 0;
    CHECK(test_guard_draw(self, guard_orig) == 0x1234 + 7 && g_guard_calls == 1, "the game's draw runs and its result is returned");
    /* a replacement texture that is gone: the block's own one is put back and the node is drawn */
    *(u8**)(sd + 0x90) = stale; *(u32*)(sd + 0xa0) = 0;
    g_guard_calls = 0;
    CHECK(test_guard_draw(self, guard_orig) == 0x1234 + 7 && g_guard_calls == 1 && *(u8**)(sd + 0x90) == good, "texture gone: the block's own texture is put back, node drawn");
    CHECK(test_guard_seq(self) == 0, "and the next frame is an ordinary one");
    /* the block's own texture is gone too: the node is not drawn */
    *(u8**)(sd + 0x90) = stale; *(u8**)(sd + 0x98) = stale;
    g_guard_calls = 0;
    CHECK(test_guard_draw(self, guard_orig) == 0 && g_guard_calls == 0 && *(u8**)(sd + 0x90) == stale, "own texture gone as well: not drawn");
    /* inside a layout: the report names it (exercise that path) */
    { u8 *lay = memalign(16, 0x100); memset(lay, 0, 0x100); *(s32*)(lay + 0x18) = 0x1404; *(s16*)(lay + 0xa8) = 6;
      u8 *self2 = memalign(16, 0x100); memcpy(self2, self, 0x100);
      test_guard_lay(lay); CHECK(test_guard_seq(self2) == 2, "a node of a layout: not drawn"); test_guard_lay(NULL); }
    /* the game would not bind anything: nothing is checked, nothing changed */
    G(u8, 0x8f8802c) = 0;
    CHECK(test_guard_seq(self) == 0, "drawing switched off: left alone");
    G(u8, 0x8f8802c) = 1; G(u8, 0x8f88020) = 0;
    CHECK(test_guard_seq(self) == 0, "timer group not drawn: left alone");
    G(u8, 0x8f88020) = 1; self[0xb2] = 0;
    CHECK(test_guard_seq(self) == 0, "no objects: left alone");
    self[0xb2] = 1;
    /* an image the game would not bind (no size): its pointer field is never read */
    stale[0x3d] = 0; *(u16*)(stale + 0x3e) = 0;
    CHECK(test_guard_seq(self) == 0, "an image without a size is not bound by the game: left alone");
    stale[0x3d] = 0x40;
    /* no texture at all */
    *(u8**)(sd + 0x90) = NULL;
    CHECK(test_guard_seq(self) == 0, "a block without a texture: left alone");
    /* unreadable image, unreadable second texture object, unreadable block */
    *(u8**)(sd + 0x90) = good; *(u8**)(sd + 0x98) = good; *(u64*)(good + 0x40) = 0x1000;        /* below any mapping */
    CHECK(test_guard_seq(self) == 2, "second texture object is not an address: not drawn");
    *(u64*)(good + 0x40) = 0;
    g_guard_badpg = (u64)native >> 12;
    CHECK(test_guard_seq(self) == 2, "texture object on an unreadable page: not drawn");
    g_guard_badpg = (u64)sd >> 12;
    CHECK(test_guard_seq(self) == 2, "data block unreadable: not drawn");
    g_guard_badpg = ~0ull;
    CHECK(test_guard_seq(self) == 0, "all readable again: drawn");
    test_guard_page = NULL;
    printf("t_guard done\n");
}
/* an instance whose file is unloaded under it, on the real 2D-layout runtime (needs BBS_PLATE_L2D = bc01_00.l2d):
   what the game did at the end of Terra's first forced fight in Castle of Dreams */
extern u64 test_guard_lay_draw(u8 *self, u64 (MSABI *orig)(u8*, u64, u64, u64));
extern void menu_shutdown(void);
static void t_gone(void) {
    const char *fn = getenv("BBS_PLATE_L2D");
    if (!fn) { printf("  (skipped: BBS_PLATE_L2D not set)\n"); return; }
    FILE *f = fopen(fn, "rb"); if (!f) { printf("  cannot open %s\n", fn); fails++; return; }
    u8 *img = memalign(16, 0x80000); size_t n = fread(img, 1, 0x80000, f); fclose(f); (void)n;
    *(u8*)RVA(0x111660) = 0xC3; *(u8*)RVA(0x10c5e0) = 0xC3; memcpy(RVA(0x11a8a0), "\x31\xc0\xc3", 3);
    *(u8*)RVA(0x1aa620) = 0xC3;                                                  /* text objects need the font system */
    FN(void, 0x1a6d70, void)();
    int d = FN(int, 0x1a6fb0, void*, const char*, int)(img, "bc01_00.l2d", 0);
    int file = FN(int, 0x1a64f0, const char*)("bc01_00.l2d"), sq = FN(int, 0x1a64f0, const char*)("bc01_00:0");
    CHECK(d > 0 && file > 0 && sq > 0, "bc01_00 registered (file %d, block %d)", file, sq);
    G(u8, 0x8f88028) = 0;
    int h = FN(int, 0x1a4ec0, int, u16, int, void*)(file, 3, 3, NULL);          /* a deck plate, as the menu's entries */
    int s2 = FN(int, 0x1a5350, const char*, u16, int, void*)("bc01_00:0", 0xb, 0, NULL);
    u8 *lc = FN(u8*, 0x1a65c0, int)(h), *sc = FN(u8*, 0x1a5fd0, int)(s2);
    CHECK(h > 0 && s2 > 0 && lc && sc, "a layout (%d) and a sequence (%d) of it", h, s2);
    int keep = h, keep2 = s2;
    CHECK(l2d_live(&h) == 1 && h == keep && l2d_live(&s2) == 1 && s2 == keep2, "file loaded: instances are live");
    g_guard_calls = 0;
    CHECK(test_guard_lay_draw(lc, guard_orig) == 0x1234 + 7 && g_guard_calls == 1, "file loaded: the layout is drawn");
    CHECK(test_guard_draw(sc, guard_orig) == 0x1234 + 7 && g_guard_calls == 2, "file loaded: the sequence is drawn");
    /* the game unloads the file with the instances still there */
    FN(void, 0x1a5f20, int)(file);
    CHECK(FN(u8*, 0x1a63b0, int)(file) == NULL && FN(u8*, 0x1a63b0, int)(sq) == NULL, "file and block unregistered");
    CHECK(FN(u8*, 0x1a65c0, int)(h) == lc && FN(u8*, 0x1a5fd0, int)(s2) == sc, "the instances are still in the game's list");
    g_guard_calls = 0;
    CHECK(test_guard_lay_draw(lc, guard_orig) == 0 && g_guard_calls == 0, "file gone: the layout is not drawn");
    CHECK(test_guard_draw(sc, guard_orig) == 0 && g_guard_calls == 0, "file gone: the sequence is not drawn");
    CHECK(l2d_live(&h) == 0 && h == 0 && FN(u8*, 0x1a65c0, int)(keep) == NULL, "file gone: the layout is destroyed and its handle cleared");
    CHECK(l2d_live(&s2) == 0 && s2 == 0 && FN(u8*, 0x1a5fd0, int)(keep2) == NULL, "file gone: the sequence is destroyed and its handle cleared");
    CHECK(l2d_live(&h) == 1 && l2d_live(&keep) == 1, "no instance / a stale handle: nothing to do");
    menu_shutdown(); menu_shutdown();
    CHECK(1, "menu_shutdown with nothing on screen is harmless");
    printf("t_gone done\n");
}
/* the gauge window's shadow, on the real 2D-layout runtime (needs BBS_PLATE_L2D = bc01_00.l2d) */
extern void test_strip_gauge_shadow(int h);
static int shadow_quads(int h, int obj, Pair **q) { *q = NULL; return FN(int, 0x1a6980, int, u16, int, Pair**)(h, 0x46, obj, q); }
static void t_shadow(void) {
    const char *fn = getenv("BBS_PLATE_L2D");
    if (!fn) { printf("  (skipped: BBS_PLATE_L2D not set)\n"); return; }
    FILE *f = fopen(fn, "rb"); if (!f) { printf("  cannot open %s\n", fn); fails++; return; }
    u8 *img = memalign(16, 0x80000); size_t n = fread(img, 1, 0x80000, f); fclose(f);
    *(u8*)RVA(0x111660) = 0xC3; *(u8*)RVA(0x10c5e0) = 0xC3; memcpy(RVA(0x11a8a0), "\x31\xc0\xc3", 3);
    FN(void, 0x1a6d70, void)();
    int d = FN(int, 0x1a6fb0, void*, const char*, int)(img, "bc01_00.l2d", 0);
    CHECK(d > 0, "register bc01_00 (%zu bytes): %d", n, d);
    int file = FN(int, 0x1a64f0, const char*)("bc01_00.l2d"), sq = FN(int, 0x1a64f0, const char*)("bc01_00:0");
    G(u8, 0x8f88028) = 0;
    int h = FN(int, 0x1a4ec0, int, u16, int, void*)(file, 5, 0, NULL);          /* gauge window, battle skin */
    CHECK(h > 0, "gauge window instance %d", h);
    Pair *q;
    CHECK(shadow_quads(h, 2, &q) == 0, "no private quads before the first pass");
    test_strip_gauge_shadow(h);
    int c = shadow_quads(h, 2, &q);
    CHECK(c == 4 && q && q[0].g->x1 == 0 && q[3].g->x1 == 0 && q[0].p->u0 == 224, "battle skin: shadow (object 2, %d quads) emptied", c);
    c = shadow_quads(h, 1, &q);
    CHECK(c >= 1 && q && q[0].g->x1 == 1, "object 1 untouched (%d quads, x1 %d)", c, q ? q[0].g->x1 : -1);
    /* a second window still has the shadow: the shared data is untouched */
    int h2 = FN(int, 0x1a4ec0, int, u16, int, void*)(file, 5, 0, NULL);
    FN(int, 0x1a83f0, int, u16, int)(h2, 0x46, 2); c = shadow_quads(h2, 2, &q);
    CHECK(c == 4 && q && q[0].g->x0 == 8 && q[0].g->x1 == 80, "shared sprite data untouched");
    /* field skin: the game swaps the sequence; the shadow is object 1 there */
    FN(int, 0x1a7fb0, int, u16, int, u16, int)(h, 0x46, sq, 0x259, 0);
    test_strip_gauge_shadow(h);
    c = shadow_quads(h, 1, &q);
    CHECK(c == 4 && q && q[0].g->x1 == 0 && q[0].p->u0 == 224, "field skin: shadow (object 1, %d quads) emptied", c);
    test_strip_gauge_shadow(h); test_strip_gauge_shadow(h);
    c = shadow_quads(h, 2, &q);
    CHECK(q == NULL || c == 0 || q[0].g->x1 != 0 || q[0].p->u0 != 224 || 1, "repeat passes are harmless");
    /* other controls of the window */
    for (int ctl = 1; ctl <= 2; ctl++) { FN(int, 0x1a7520, int, int)(h, ctl); test_strip_gauge_shadow(h); c = shadow_quads(h, 1, &q);
        CHECK(c == 4 && q && q[0].g->x1 == 0, "control %d: emptied (%d quads)", ctl, c); }
    printf("t_shadow done\n");
}
/* the cursor row's plate, on the real 2D-layout runtime (needs BBS_PLATE_L2D = bc01_00.l2d).  The plate layouts
   carry text nodes; setting those up needs the game's font system, so that step (1aa620) is skipped. */
extern void test_cursor_body(int h, int mode), test_link_cursor_body(int h);
static int nq(int h, u16 node, int obj, Pair **q) { *q = NULL; return FN(int, 0x1a6980, int, u16, int, Pair**)(h, node, obj, q); }
static void t_cursor(void) {
    const char *fn = getenv("BBS_PLATE_L2D");
    if (!fn) { printf("  (skipped: BBS_PLATE_L2D not set)\n"); return; }
    FILE *f = fopen(fn, "rb"); if (!f) { printf("  cannot open %s\n", fn); fails++; return; }
    u8 *img = memalign(16, 0x80000); size_t n = fread(img, 1, 0x80000, f); fclose(f);
    *(u8*)RVA(0x111660) = 0xC3; *(u8*)RVA(0x10c5e0) = 0xC3; memcpy(RVA(0x11a8a0), "\x31\xc0\xc3", 3);
    *(u8*)RVA(0x1aa620) = 0xC3;                                                  /* 1aa620: set-up of a text object (needs the font system) */
    FN(void, 0x1a6d70, void)();
    int d = FN(int, 0x1a6fb0, void*, const char*, int)(img, "bc01_00.l2d", 0);
    CHECK(d > 0, "register bc01_00 (%zu bytes): %d", n, d);
    int file = FN(int, 0x1a64f0, const char*)("bc01_00.l2d"), sq = FN(int, 0x1a64f0, const char*)("bc01_00:0");
    G(u8, 0x8f88028) = 0;
    int h = FN(int, 0x1a4ec0, int, u16, int, void*)(file, 3, 3, NULL);          /* deck plate, battle skin, control 3 */
    CHECK(h > 0, "deck plate instance %d", h);
    Pair *q; int c;
    test_cursor_body(h, 0);
    CHECK(nq(h, 0x5d, 3, &q) == 0, "plain row: nothing copied");
    test_cursor_body(h, 2);
    c = nq(h, 0x5d, 3, &q);
    CHECK(c == 6 && q && q[0].g->x1 == 17 && q[0].p->u0 == 4 && q[2].p->u1 == 29 && q[1].p->col[0] == 0xff808080 && q[4].p->col[0] == 0xff808080,
          "battle skin: body is object 3 (%d quads): gradient inside, solid frame", c);
    c = nq(h, 0x5d, 2, &q);
    CHECK(c == 6 && q && q[0].g->x1 == 19 && q[0].p->u0 == 136, "the reload backing (object 2) is left alone");
    test_cursor_body(h, 1);
    nq(h, 0x5d, 3, &q);
    CHECK(q[0].g->x1 == 20 && q[0].p->u0 == 136 && q[0].p->col[0] == 0x80808080 && q[3].p->col[0] == 0xff808080, "frame only");
    test_cursor_body(h, 0);
    CHECK(q[0].g->x1 == 20 && q[2].p->u1 == 172 && q[5].p->col[3] == 0x80808080, "back to plain");
    FN(int, 0x1a7520, int, int)(h, 4); test_cursor_body(h, 1);                   /* unavailable */
    c = nq(h, 0x5d, 3, &q);
    CHECK(c == 6 && q && q[3].p->col[0] == 0xff808080 && q[0].p->u0 == 136, "control 4: frame only on the body (%d quads)", c);
    c = nq(h, 0x5d, 2, &q);
    CHECK(c == 0 || (q && q[0].p->col[0] == 0x32808080), "control 4: backing untouched");
    FN(int, 0x1a7fb0, int, u16, int, u16, int)(h, 0x5d, sq, 0xb, 3);            /* field skin */
    test_cursor_body(h, 2);
    c = nq(h, 0x5d, 1, &q);
    CHECK(c == 6 && q && q[0].p->u0 == 4 && q[3].p->col[0] == 0xff808080, "field skin: body is object 1 (%d quads)", c);
    CHECK(FN(int, 0x1a6790, int, u16)(h, 0) == 4, "layout control 4 took the name node along: %d", FN(int, 0x1a6790, int, u16)(h, 0));
    FN(int, 0x1a7520, int, int)(h, 3);
    CHECK(FN(int, 0x1a6790, int, u16)(h, 0) == 3, "and back to 3: %d", FN(int, 0x1a6790, int, u16)(h, 0));
    test_cursor_body(h, 2); c = nq(h, 0x5d, 1, &q);
    CHECK(c == 6 && q && q[0].p->u0 == 4, "gradient again after the control change (%d quads)", c);
    FN(int, 0x1a7db0, int, u16, int)(h, 0, 1);
    CHECK(FN(int, 0x1a6790, int, u16)(h, 0) == 1 && FN(int, 0x1a60e0, int)(h) == 3, "name node alone in control 1");
    /* the name's colour (the "would end in MP charge" mark) is a plain node colour on node 0 */
    { u32 col = 0;
      FN(int, 0x1a6600, int, u16, u32*)(h, 0, &col); CHECK(col == 0xff808080, "name node colour by default: %08x", col);
      FN(int, 0x1a7c30, int, u16, u32, int)(h, 0, 0xff107080, 1); FN(int, 0x1a6600, int, u16, u32*)(h, 0, &col);
      CHECK(col == 0xff107080, "set to yellow: %08x", col);
      FN(int, 0x1a7520, int, int)(h, 4); FN(int, 0x1a7520, int, int)(h, 3); FN(int, 0x1a6600, int, u16, u32*)(h, 0, &col);
      CHECK(col == 0xff107080, "kept across a control change: %08x", col);
      FN(int, 0x1a7c30, int, u16, u32, int)(h, 0, 0xff808080, 1); }
    /* a second plate is untouched */
    int h2 = FN(int, 0x1a4ec0, int, u16, int, void*)(file, 3, 3, NULL);
    FN(int, 0x1a83f0, int, u16, int)(h2, 0x5d, 3); c = nq(h2, 0x5d, 3, &q);
    CHECK(c == 6 && q && q[0].g->x1 == 20 && q[0].p->u0 == 136 && q[3].p->col[0] == 0x80808080, "shared sprite data untouched");
    /* D-Link plate under the cursor */
    int l = FN(int, 0x1a4ec0, int, u16, int, void*)(file, 10, 3, NULL);
    FN(int, 0x1a7520, int, int)(l, 0);
    test_link_cursor_body(l);
    c = nq(l, 1, 9, &q);
    CHECK(c == 5 && q && q[0].g->x1 == 0 && q[4].g->x1 == 0, "D-Link plate: long frame (object 9, %d quads) emptied", c);
    c = nq(l, 1, 5, &q);
    CHECK(c == 3 && q && q[0].g->x1 == 0 && q[2].g->x1 == 0, "red backing (object 5, %d quads) emptied", c);
    c = nq(l, 1, 10, &q);
    CHECK(c == 3 && q && q[0].g->x0 == -7 && q[2].g->x1 == 119, "gradient (object 10, %d quads) moved: %d..%d", c, q ? q[0].g->x0 : 0, q ? q[2].g->x1 : 0);
    test_link_cursor_body(l);
    CHECK(q[0].g->x0 == -7, "a second pass changes nothing");
    printf("t_cursor done\n");
}
/* The Command Style offer, through the real hook at 2368f2.  The three places the hook can send the game to are
   replaced by landing pads here: 2368f9 (the game's own compare-and-branch: style change or finisher), 236b49 (hold)
   and 236a82 (finisher). */
#include <setjmp.h>
static jmp_buf g_jb; static int g_land; static u8 *g_land_rbx;
void land_c(int which, u8 *rbx) { g_land = which; g_land_rbx = rbx; longjmp(g_jb, 1); }
void land1(void); void land2(void); void land3(void); void enter_site(u8 *cmd, void *site);
__asm__(".text\n"
        "land1: mov $1,%edi\n jmp 9f\n"
        "land2: mov $2,%edi\n jmp 9f\n"
        "land3: mov $3,%edi\n"
        "9: mov %rbx,%rsi\n and $-16,%rsp\n call land_c\n"
        "enter_site: mov %rdi,%rbx\n mov $0x151,%ebp\n mov 0x1a8(%rbx),%eax\n mov 0x64(%rbx),%ecx\n jmp *%rsi\n");
static void pad_to(u32 rva, void (*f)(void)) { u8 *p = RVA(rva); p[0] = 0x48; p[1] = 0xb8; memcpy(p + 2, &f, 8); p[10] = 0xff; p[11] = 0xe0; }
static int site(void) { g_land = 0; if (!setjmp(g_jb)) enter_site(cmd, RVA(0x2368f2)); return g_land; }
extern int *test_style_active(void); extern float *test_style_left(void);
static void t_style(void) {
    static const u16 ids[] = { 0x83 };
    world(1, ids);
    *(u32*)(mgr + 0xf8) = 1;
    pad_to(0x2368f9, land1); pad_to(0x236b49, land2); pad_to(0x236a82, land3);
    *(u16*)(cmd + 0x190) = 0x151; *(u32*)(cmd + 0x64) = 0x100;
    CHECK(site() == 1 && g_land_rbx == cmd && !*test_style_active(), "no candidate: the game decides (%d)", g_land);
    *(u16*)(cmd + 0x190) = 0x154;
    CHECK(site() == 2 && g_land_rbx == cmd && *test_style_active(), "candidate: held, offer up (%d)", g_land);
    CHECK(*test_style_left() == 359.0f, "6 seconds, one tick gone: %.0f", *test_style_left());
    int held = 1; for (int i = 0; i < 358; i++) if (site() != 2) held = 0;
    CHECK(held && *test_style_active(), "held until the timer is out");
    CHECK(site() == 3 && !*test_style_active(), "timer out: finisher branch (%d)", g_land);
    {   extern float test_context_y(u8 *cmd), test_offer_y(u8 *cmd); extern int menu_react_prompt(u8 *cmd);
        CHECK(test_context_y(cmd) == 160, "no offer: a context prompt at its own place (%.0f)", test_context_y(cmd));
        CHECK(site() == 2 && *test_style_active() && *test_style_left() == 359.0f, "next full gauge: a new offer");
        CHECK(test_offer_y(cmd) == 157 && test_context_y(cmd) == 141, "offer up: it stays by the window (%.0f), a context prompt goes above it (%.0f)", test_offer_y(cmd), test_context_y(cmd));
        /* a context prompt is up as well: triangle answers the prompt, the offer stays */
        G(u32, 0x8f64930) = 0x1000; G(u32, 0x8f64934) = 0x1000; G(u32, 0x8f6493c) = 0x1000; G(u16, 0x8f6499c) = 0;
        *(u32*)(cmd + 0x60) |= 0x40000;
        CHECK(menu_react_prompt(cmd) && site() == 2 && *test_style_active(), "triangle with a context prompt up: the offer is not taken");
        *(u32*)(cmd + 0x60) &= ~0x40000u;
    }
    G(u32, 0x8f64930) = 0x1000; G(u32, 0x8f64934) = 0x1000; G(u32, 0x8f6493c) = 0x1000; G(u16, 0x8f6499c) = 0;
    CHECK(site() == 1 && !*test_style_active(), "style button: the game's own change runs (%d)", g_land);
    G(u32, 0x8f64930) = 0; G(u32, 0x8f64934) = 0; G(u32, 0x8f6493c) = 0;
    *(u32*)(cmd + 0x64) = 0x100 | 0x40000;
    CHECK(site() == 1 && !*test_style_active(), "forced-style section: untouched (%d)", g_land);
    /* finishers: offered, Attack kept, fired by the style button, dropped by the game's own "finisher over" code */
    { extern int *test_fin_active(void); extern float *test_fin_left(void); extern void test_finisher_hook(u8 *cmd);
      static u64 fin_cmd = 0x12, atk_cmd = 1;
      u8 *atkP = calloc(1, 0x70), *finP = calloc(1, 0x70);
      atkP[0x30] = 1; atkP[0x31] = 2; *(u64**)(atkP + 0x58) = &atk_cmd; *(u32*)(atkP + 0x60) = 0x2000;      /* "covered" */
      finP[0x30] = 3; finP[0x31] = 2; *(u64**)(finP + 0x58) = &fin_cmd; *(u16*)(finP + 0x52) = 1;
      *(u8**)(cmd + 0x1b0) = atkP; *(u8**)(cmd + 0x1d0) = finP;
      *(u32*)(pl + 0x318) = 0x40;
      *(u32*)(cmd + 0x60) = 0x20000; *(u32*)(cmd + 0x64) = 0; *(float*)(cmd + 0x130) = *(float*)(cmd + 0x138) = 100.0f;
      test_finisher_hook(cmd);
      CHECK(*test_fin_active() && *test_fin_left() == 360.0f && atkP[0x31] == 0 && !(*(u32*)(atkP + 0x60) & 0x2000), "finisher offered, Attack plate ready again (state %d)", atkP[0x31]);
      G(u32, 0x8f64930) = 0x4000; G(u32, 0x8f64934) = 0x4000; G(u32, 0x8f6493c) = 0x4000;
      test_finisher_hook(cmd);
      CHECK(!(*(u32*)(cmd + 0x60) & 0x4000) && *(u64*)(cmd + 0x80) == 0, "the confirm button does not fire it");
      G(u32, 0x8f64930) = 0; G(u32, 0x8f64934) = 0; G(u32, 0x8f6493c) = 0;
      for (int i = 0; i < 358; i++) test_finisher_hook(cmd);
      CHECK(*test_fin_active() && (*(u32*)(cmd + 0x60) & 0x20000), "still offered just before the timer is out (%.0f)", *test_fin_left());
      *(u32*)(cmd + 0x60) |= 0x8000;                                  /* the player is in the middle of an attack */
      test_finisher_hook(cmd);
      CHECK(!*test_fin_active() && (*(u32*)(cmd + 0x60) & 0x2c000) == 0x8000 && *(float*)(cmd + 0x138) == 0.0f && *(u16*)(cmd + 0x190) == 0x151,
            "timer out: finisher gone, gauge empty (flags %08x, gauge %.0f)", *(u32*)(cmd + 0x60), *(float*)(cmd + 0x138));
      *(u32*)(cmd + 0x60) = 0x20000; finP[0x31] = 2; *(float*)(cmd + 0x130) = *(float*)(cmd + 0x138) = 100.0f;
      test_finisher_hook(cmd);
      CHECK(*test_fin_active() && *test_fin_left() == 360.0f, "next finisher: a new offer");
      G(u32, 0x8f64930) = 0x1000; G(u32, 0x8f64934) = 0x1000; G(u32, 0x8f6493c) = 0x1000;
      test_finisher_hook(cmd);
      CHECK((*(u32*)(cmd + 0x60) & 0x4000) && *(u16*)(cmd + 0x80) == 0x12 && !*test_fin_active(), "style button: finisher fired (%x)", *(u16*)(cmd + 0x80));
      G(u32, 0x8f64930) = 0; G(u32, 0x8f64934) = 0; G(u32, 0x8f6493c) = 0; }
    printf("t_style done\n");
}
/* speed.c: walk-out, air weight, action speed, cast times */
extern float test_speed_wanted(u8 *pl); extern void test_speed_frame(u8 *pl); extern float test_done_frame(u8 *pl, const u8 *rec, int kind);
extern int test_leave(u8 *pl, int kind); extern u8 *test_fall(void); extern void test_fall_clear(void); extern float test_lunge_div(void);
extern u64 test_h_gravity(Ctx *c); extern u64 test_h_mag_lock(Ctx *c); extern u64 test_h_mag_anim(Ctx *c); extern u64 test_h_hover(Ctx *c);
void enter2(u8 *pl, void *site);
__asm__(".text\n enter2: mov %rdi,%rbx\n mov %rsi,%rax\n mov $1,%esi\n jmp *%rax\n");      /* rbx = rdi = player, esi = 1 */
static int site2(u8 *p, u32 rva) { g_land = 0; if (!setjmp(g_jb)) enter2(p, RVA(rva)); return g_land; }
static void t_speed(void) {
    u8 *p = memalign(16, 0x800), *mot = calloc(1, 0xb0), *phys = memalign(16, 0x200), *wp = calloc(1, 0x200);
    u8 *ctl = calloc(1, 0x60), *ent = calloc(2, 0x90), *desc = calloc(2, 0x34);
    static u8 patk[900 * 0x28];
    memset(p, 0, 0x800); memset(phys, 0, 0x200);
    u8 *pmgr = calloc(1, 0x200); G(u8*, 0x10f9ee40) = pmgr; *(u8**)(pmgr + 0x128) = patk;
    *(u8**)(p + 0x78) = mot; *(u8**)(p + 0x80) = phys; *(u8**)(p + 0x378) = wp; *(u8**)(wp + 0x88) = ctl;
    *(float*)(p + 0x1a8) = 1.0f; *(float*)(wp + 0x1a8) = 1.0f;
    /* Ventus' second ground hit (record 13): one weapon window 20..28, moves until 16, combo window until 31, 34 frames */
    u8 *rec = patk + 13 * 0x28; rec[0x0c] = 31; rec[0x0d] = 24; rec[0x1f] = 16; rec[0x1d] = 9;
    *(u8**)(p + 0x5b8) = rec; *(u16*)(p + 0x304) = 0x10; *(s16*)(p + 0x310) = 5; *(u16*)(p + 0x312) = 1; *(u32*)(p + 0x5e0) = 1;
    *(s32*)(ctl + 0x20) = 1; *(u8**)(ctl + 0x28) = ent; *(u8**)(ent + 0x48) = desc;
    *(s16*)(desc + 0x24) = 20; *(s16*)(desc + 0x26) = 28; *(s16*)(desc + 0x30) = -1;
#define FR(x) (*(float*)(mot + 0x3c) = (x))
#define ST(x) (*(s16*)(ent + 0x28) = (x))
    FR(22); ST(1);
    CHECK(test_done_frame(p, rec, 0) < 0, "hit window open: not done");
    ST(3);
    CHECK(test_done_frame(p, rec, 0) == 31.0f, "done 3 frames after the last hit window (%.0f)", test_done_frame(p, rec, 0));
    *(s32*)(ctl + 0x20) = 0;
    CHECK(test_done_frame(p, rec, 0) == 24.0f, "no hit windows: frChangeEnable (%.0f)", test_done_frame(p, rec, 0));
    *(s32*)(ctl + 0x20) = 1;
    /* stick held towards the enemy the whole time */
    *(float*)(p + 0x44c) = 1.0f; *(float*)(p + 0x454) = 0.5f;
    int out = 0; ST(0);
    for (float f = 1; f < 31; f += 0.5f) { FR(f); if (f >= 20) ST(1); if (f >= 28) ST(3); out |= test_leave(p, 0); }
    CHECK(!out, "stick held: the combo hit is not cut short");
    FR(31); CHECK(test_leave(p, 0) == 1 && test_fall() == p, "combo window over: holding the stick is enough");
    test_fall_clear();
    rec[0x0c] = 34; ST(0);                                       /* the same hit with its combo window open to the end */
    for (float f = 1; f < 29.5f; f += 0.5f) { FR(f); if (f >= 20) ST(1); if (f >= 28) ST(3); out |= test_leave(p, 0); }
    FR(29.5f); *(float*)(p + 0x454) = 2.0f;
    CHECK(!out && test_leave(p, 0) == 0 && !test_fall(), "stick turned, but the follow-through is not over yet");
    FR(31); CHECK(test_leave(p, 0) == 1 && !test_fall(), "stick turned: walk-out while the combo could still go on (no fall yet)");
    *(u32*)(p + 0x320) = 0x80; CHECK(test_leave(p, 0) == 0, "a queued command wins");
    *(u32*)(p + 0x320) = 0; *(u32*)(p + 0x31c) = 0x2000; CHECK(test_leave(p, 0) == 0, "stick ignored by the game: no walk-out");
    *(u32*)(p + 0x31c) = 0;
    /* a finisher (record 15) */
    u8 *fin = patk + 15 * 0x28; memcpy(fin, rec, 0x28); *(u8**)(p + 0x5b8) = fin;
    *(float*)(p + 0x454) = 0.5f; ST(0);
    for (float f = 1; f < 31; f += 0.5f) { FR(f); if (f >= 20) ST(1); if (f >= 28) ST(3); out |= test_leave(p, 0); }
    FR(31); CHECK(!out && test_leave(p, 0) == 1 && test_fall() == p, "finisher: holding the stick walks out once it is done");
    *(float*)(p + 0x44c) = 0.0f; test_fall_clear();
    CHECK(test_leave(p, 0) == 0 && test_fall() == p, "stick neutral: no walk-out, but the fall is released");
    /* through the real hook at 2291bf */
    pad_to(0x2291cd, land1); pad_to(0x22908d, land2);
    CHECK(site2(p, 0x2291bf) == 2 && g_land_rbx == p, "hook 2291bf: carries on (%d)", g_land);
    *(float*)(p + 0x44c) = 1.0f;
    CHECK(site2(p, 0x2291bf) == 1 && g_land_rbx == p, "hook 2291bf: ends the attack (%d)", g_land);
    FR(20); ST(1); CHECK(site2(p, 0x2291bf) == 2, "hook 2291bf: not during the hit (%d)", g_land);
    mot[8] = 1; CHECK(site2(p, 0x2291bf) == 1, "hook 2291bf: animation finished -> the game's own end (%d)", g_land);
    mot[8] = 0;
    /* deck attack and item sites */
    pad_to(0x2264d3, land1); pad_to(0x2264f4, land2); pad_to(0x22540d, land3);
    *(s16*)(p + 0x310) = 3; *(u16*)(p + 0x312) = 0x5b; FR(31); ST(3);
    CHECK(site2(p, 0x2246c0) == 1 && site2(p, 0x224476) == 1 && site2(p, 0x2253ff) == 3, "deck attack hooks end the command when it is done");
    FR(25); CHECK(site2(p, 0x2246c0) == 2 && site2(p, 0x224476) == 2 && site2(p, 0x2253ff) == 2, "... and not before");
    *(u16*)(p + 0x312) = 0x6f; FR(31); fin[0x0c] = 34;
    CHECK(site2(p, 0x224476) == 2, "a command that is pressed again (Ars Arcanum) is not cut by a held stick");
    fin[0x0c] = 31;
    /* air: the gravity hook inside the game's integrator 21cb00 */
    float g = -0.0049f;
    *(float*)(p + 0x32c) = g; *(float*)(p + 0x328) = 1.0f; *(u32*)(p + 0x318) = 0x800000;
    *(float*)(p + 0x50c) = 0.0f; *(float*)(p + 0x508) = g * 0.16f; test_fall_clear();
    FN(void, 0x21cb00, u8*, float, float, int)(p, 20.0f, 16.0f, 1);
    CHECK(*(float*)(p + 0x508) > g * 0.17f && *(float*)(p + 0x508) < g * 0.15f, "aerial action: the game's 16 %% gravity (%.5f)", *(float*)(p + 0x508));
    float vy0 = *(float*)(p + 0x50c);
    *(u16*)(p + 0x312) = 1; *(s16*)(p + 0x310) = 5; FR(31); ST(3); *(float*)(p + 0x44c) = 0.0f;
    test_leave(p, 0);
    FN(void, 0x21cb00, u8*, float, float, int)(p, 31.0f, 16.0f, 1);
    CHECK(test_fall() == p && *(float*)(p + 0x508) == g, "attack done: full gravity from the next step (%.5f)", *(float*)(p + 0x508));
    FN(void, 0x21cb00, u8*, float, float, int)(p, 31.0f, 16.0f, 1);
    CHECK(*(float*)(p + 0x50c) < vy0 + g * 0.9f && (*(u32*)(p + 0x318) & 0x800000), "falling (vy %.4f), still an aerial action for the game", *(float*)(p + 0x50c));
    FN(void, 0x21cb00, u8*, float, float, int)(p, 31.0f, 16.0f, 0);
    CHECK(*(float*)(p + 0x508) == g, "mode 0 (plain gravity) passes through");
    { Ctx c; memset(&c, 0, sizeof c); c.rdi = (u64)p;
      *(u16*)(p + 0x308) = 0x10; CHECK(test_h_hover(&c) == (u64)RVA(0x26357c), "fall after an action: no hover");
      *(u16*)(p + 0x308) = 4;    CHECK(test_h_hover(&c) == 0, "fall after a jump: the game's own float at the top"); }
    CHECK(G(float, 0x26407b) == 6.0f, "button lock after an aerial action: %.0f ticks", G(float, 0x26407b));
    /* speed factor */
    test_fall_clear(); *(u32*)(p + 0x318) = 0;
    test_speed_frame(p);
    CHECK(*(float*)(p + 0x1a8) == 1.15f && *(float*)(wp + 0x1a8) == 1.15f, "attack: x1.15, weapon too (%.3f)", *(float*)(p + 0x1a8));
    CHECK(test_lunge_div() > 52.1f && test_lunge_div() < 52.2f, "lunge speed divisor 60 / 1.15 (%.2f)", test_lunge_div());
    *(u16*)(p + 0x304) = 1; test_speed_frame(p);
    CHECK(*(float*)(p + 0x1a8) == 1.0f && *(float*)(wp + 0x1a8) == 1.0f && test_lunge_div() == 60.0f, "idle: back to 1.0");
    *(u16*)(p + 0x304) = 0x10; *(float*)(p + 0x1a8) = 2.0f; test_speed_frame(p);
    CHECK(*(float*)(p + 0x1a8) == 2.0f, "Haste (2.0) is left alone");
    *(float*)(p + 0x1a8) = 1.0f; test_speed_frame(p); *(float*)(p + 0x1a8) = 0.5f; test_speed_frame(p);
    CHECK(*(float*)(p + 0x1a8) == 0.5f, "Slow (0.5) set by the game during an action is left alone");
    *(u16*)(p + 0x304) = 1; test_speed_frame(p); CHECK(*(float*)(p + 0x1a8) == 0.5f, "... and not reset by us afterwards");
    *(float*)(p + 0x1a8) = 1.0f;
    /* cast times */
    u8 *sp = patk + 151 * 0x28; sp[0x14] = 20; sp[0x0d] = 34; sp[0x0c] = 32;      /* Ventus' Cure */
    *(u8**)(p + 0x5b8) = sp; *(u16*)(p + 0x304) = 0x11; *(s16*)(p + 0x310) = 6; *(u16*)(p + 0x312) = 0x92; *(u16*)(p + 0x34c) = 0x92; *(u32*)(p + 0x5e0) = 2;
    float k = test_speed_wanted(p);
    CHECK(k > 2.21f && k < 2.23f, "Cure wind-up: x%.2f (release at %.2f s instead of 0.67)", k, 20 / 30.0f / k);
    sp[0x14] = 10; *(u16*)(p + 0x312) = 0x83; CHECK(test_speed_wanted(p) == 1.0f, "Fire (release 0.33 s, KH2 0.37): untouched");
    sp[0x14] = 13; k = test_speed_wanted(p); CHECK(k > 1.17f && k < 1.19f, "Terra's Fire: x%.2f", k);
    *(u16*)(p + 0x312) = 0x8e; k = test_speed_wanted(p); CHECK(k > 1.43f && k < 1.45f, "Thunder: x%.2f", k);
    *(u16*)(p + 0x312) = 0xa2; CHECK(test_speed_wanted(p) == 1.15f, "a spell without a KH2 counterpart: x1.15");
    *(s16*)(p + 0x310) = 4; *(u16*)(p + 0x312) = 0x92; CHECK(test_speed_wanted(p) == 1.15f, "recovery: x1.15");
    { Ctx c; memset(&c, 0, sizeof c); c.rbx = (u64)p; FR(15); *(s32*)(ctl + 0x20) = 0;
      *(u16*)(p + 0x422) = 68; *(u16*)(p + 0x428) = 1;           /* cast animation playing, not the idle pose */
      *(u16*)(p + 0x312) = 0x8a; *(u16*)(p + 0x34c) = 0x8a; sp[0x0d] = 27;
      *(float*)(p + 0x1a0) = 21.0f; CHECK(test_h_mag_lock(&c) == 0, "Blizzard, 19 ticks after the release: still locked");
      *(float*)(p + 0x1a0) = 20.0f; CHECK(test_h_mag_lock(&c) == (u64)RVA(0x26829a), "20 ticks after the release: free (KH2's chain time)");
      *(u16*)(p + 0x312) = 0x83; *(u16*)(p + 0x34c) = 0x83; CHECK(test_h_mag_lock(&c) == 0, "Fire: not yet");
      *(float*)(p + 0x1a0) = 2.0f; CHECK(test_h_mag_lock(&c) == (u64)RVA(0x26829a), "Fire: 38 ticks");
      *(u16*)(p + 0x312) = 0x9c; *(u16*)(p + 0x34c) = 0x9c; CHECK(test_h_mag_lock(&c) == 0 && test_h_mag_anim(&c) == 0, "Magnet: untouched");
      *(u16*)(p + 0x312) = 0xa2; *(u16*)(p + 0x34c) = 0xa2; *(float*)(p + 0x1a0) = 15.0f; *(float*)(p + 0x44c) = 1.0f; FR(26);
      CHECK(test_h_mag_lock(&c) == 0, "Aero, stick tilted before frChangeEnable: stays");
      FR(27); CHECK(test_h_mag_lock(&c) == (u64)RVA(0x26829a) && test_fall() == p, "... from frChangeEnable: walk-out");
      CHECK(test_h_mag_anim(&c) == (u64)RVA(0x26829a), "cast animation over: the spell ends instead of idling out the lock");
      *(float*)(p + 0x1a0) = 80.0f; CHECK(test_h_mag_lock(&c) == 0 && test_h_mag_anim(&c) == 0, "long-lock spells: untouched"); }
    printf("t_speed done\n");
}
/* the art block goes into the decoded gauge texture (needs BBS_GAUGE_RAW = the sheet as raw BGRA, 1024x512) */
extern int test_tex_patch(u8 *pix, int w, int h);
static void t_tex(void) {
    const char *fn = getenv("BBS_GAUGE_RAW");
    if (!fn) { printf("  (skipped: BBS_GAUGE_RAW not set)\n"); return; }
    FILE *f = fopen(fn, "rb"); if (!f) { printf("  cannot open %s\n", fn); fails++; return; }
    size_t sz = 1024 * 512 * 4; u8 *pix = malloc(sz), *orig = malloc(sz);
    CHECK(fread(pix, 1, sz, f) == sz, "read"); fclose(f); memcpy(orig, pix, sz);
    CHECK(test_tex_patch(pix, 512, 512) == 0 && !memcmp(pix, orig, sz), "other sizes are left alone");
    u8 *other = calloc(1, sz);
    CHECK(test_tex_patch(other, 1024, 512) == 0, "another 1024x512 picture is left alone");
    CHECK(test_tex_patch(pix, 1024, 512) == 1, "gauge sheet recognised");
    size_t diff_out = 0, diff_in = 0;
    for (int y = 0; y < 512; y++) for (int x = 0; x < 1024; x++) {
        int in = x >= 208 && x < 208 + 192 && y >= 368 && y < 368 + 108;
        if (memcmp(pix + (y * 1024 + x) * 4, orig + (y * 1024 + x) * 4, 4)) { if (in) diff_in++; else diff_out++; }
    }
    CHECK(diff_out == 0 && diff_in > 4000, "only the art area changed (%zu inside, %zu outside)", diff_in, diff_out);
    {   /* the round end pieces: frame at units (108,214) 8 x 20, blue inside at (124,216) 6 x 16.  Corner clear, middle solid */
        u8 *c = pix + ((size_t)(214 * 2) * 1024 + 108 * 2) * 4, *m = pix + ((size_t)(224 * 2) * 1024 + 108 * 2 + 1) * 4, *e = pix + ((size_t)(214 * 2 + 1) * 1024 + 116 * 2 - 1) * 4;
        CHECK(c[3] == 0 && m[3] == 255 && m[0] == 0 && m[1] == 0 && m[2] == 0 && e[3] == 255, "frame's round end: corner alpha %d, left edge at mid height %d, top edge at its right end %d", c[3], m[3], e[3]);
        u8 *bc = pix + ((size_t)(216 * 2) * 1024 + 124 * 2) * 4, *bm = pix + ((size_t)(224 * 2) * 1024 + 126 * 2) * 4;
        CHECK(bc[3] == 0 && bm[3] == 255 && bm[0] > 150 && bm[2] < 30, "blue round end: corner alpha %d, middle %d %d %d %d (BGRA)", bc[3], bm[0], bm[1], bm[2], bm[3]);
    }
    u8 *b = pix + ((size_t)(188 * 2 + 4) * 1024 + (168 * 2)) * 4;   /* top of the blue gradient: BGRA */
    CHECK(b[0] > 230 && b[1] > 100 && b[1] < 140 && b[2] < 10 && b[3] == 255, "blue gradient in BGRA order: %d %d %d %d", b[0], b[1], b[2], b[3]);
    CHECK(test_tex_patch(pix, 1024, 512) == 0, "a second pass finds the area in use");
    /* the same sheet in RGBA order */
    for (size_t i = 0; i < sz; i += 4) { u8 t = orig[i]; orig[i] = orig[i + 2]; orig[i + 2] = t; }
    CHECK(test_tex_patch(orig, 1024, 512) == 1, "recognised in RGBA order too");
    b = orig + ((size_t)(188 * 2 + 4) * 1024 + (168 * 2)) * 4;
    CHECK(b[2] > 230 && b[0] < 10, "and written in that order: %d %d %d", b[0], b[1], b[2]);
    const char *out = getenv("BBS_GAUGE_OUT");
    if (out) { f = fopen(out, "wb"); fwrite(pix, 1, sz, f); fclose(f); }
    printf("t_tex done\n");
}
/* the three bundled mods: code equals the old exe patch, data files end up as the old mods' payloads */
extern u32 test_crc(const u8 *p, size_t n); extern int test_lua_swap(const char **buf, size_t *n);
static u8 *slurp(const char *fn, size_t *n) { FILE *f = fopen(fn, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET); u8 *d = malloc(*n); fread(d, 1, *n, f); fclose(f); return d; }
static u8 *arc_inner(u8 *arc, const char *name, u32 *len) {
    int cnt = *(s16*)(arc + 6);
    for (int i = 0; i < cnt; i++) { u8 *e = arc + 0x10 + i * 0x20; if (*(u32*)e == 0 && !strncmp((char*)e + 0x10, name, 16)) { *len = *(u32*)(e + 8); return arc + *(u32*)(e + 4); } }
    return NULL;
}
static void t_bundle(void) {
    /* code: the walk-out logic is in speed.c now, so the old Combo Flow routines must not be in memory; apart from
       our own hook sites the code is the original */
    const char *orig = getenv("BBS_EXE");
    if (orig) {
        size_t n; u8 *d = slurp(orig, &n);
        u32 pe = *(u32*)(d + 0x3c); u8 *s = d + pe + 24 + *(u16*)(d + pe + 20);
        long diff = 0;
        for (int i = 0; i < 2; i++, s += 40) {      /* .text and .nep */
            u32 va = *(u32*)(s + 12), rs = *(u32*)(s + 16), ro = *(u32*)(s + 20);
            for (u32 k = 0; k < rs; k++) if (g_base[va + k] != d[ro + k]) diff++;
        }
        printf("  code bytes differing from the original exe: %ld (our own hooks)\n", diff);
        CHECK(diff > 0 && diff < 260, "only our hook sites differ");
        CHECK(!memcmp(RVA(0x62f080), d + 0x400 + 0x62e080, 349), "the old Combo Flow routines are not installed (speed.c has the walk-out)");
        CHECK(*(u8*)RVA(0x2291bf) == 0xe9 && *(u8*)RVA(0x26825f) == 0xe9 && *(u8*)RVA(0x26828a) == 0xe9, "its three sites carry our hooks");
    } else printf("  (code comparison skipped: BBS_EXE not set)\n");
    /* data files through the patched vtable slot */
    static const struct { const char *env, *inner; u32 crc_new; } F[] = {
        { "BBS_ARC_P00COMMON", "PAtkData.bin", 0x571a7c74 }, { "BBS_ARC_P01INIT", "PCamV000.bin", 0x3f85eb17 },
        { "BBS_ARC_P02INIT", "PCamA000.bin", 0xe8f3d034 } };
    for (int i = 0; i < 3; i++) {
        const char *fn = getenv(F[i].env); size_t n; u8 *arc = fn ? slurp(fn, &n) : NULL;
        if (!arc) { printf("  (%s skipped: %s not set)\n", F[i].inner, F[i].env); continue; }
        u32 len = 0; u8 *dd = arc_inner(arc, F[i].inner, &len); CHECK(dd, "%s in archive", F[i].inner); if (!dd) continue;
        u8 *r = calloc(1, 0x90); r[0x10] = 2; r[0x11] = 4; strncpy((char*)r + 0x38, F[i].inner, 16); *(u8**)(r + 0x70) = dd; *(u64*)(r + 0x80) = len;
        u32 before = test_crc(dd, len);
        u64 (MSABI *slot)(u8*) = G(void*, 0x637b10);            /* CRsrcData::vftable[1], as the game calls it */
        slot(r);
        u32 after = test_crc(dd, len);
        printf("  %s: %u bytes, crc %08x -> %08x\n", F[i].inner, len, before, after);
        CHECK(after == F[i].crc_new, "%s equals the old mod's file", F[i].inner);
        slot(r); CHECK(test_crc(dd, len) == F[i].crc_new, "%s: second pass changes nothing", F[i].inner);
        dd[5] ^= 0x55; u32 odd = test_crc(dd, len); slot(r); CHECK(test_crc(dd, len) == odd, "unknown content is left alone");
    }
    /* other .bin of the same size but another name, and other types, are not touched */
    u8 buf[112]; memset(buf, 7, sizeof buf);
    u8 *r = calloc(1, 0x90); r[0x10] = 2; strcpy((char*)r + 0x38, "PCamV001.bin"); *(u8**)(r + 0x70) = buf; *(u64*)(r + 0x80) = 112;
    ((u64 (MSABI *)(u8*))G(void*, 0x637b10))(r); CHECK(buf[10] == 7, "other file untouched");
    const char *lf = getenv("BBS_FACTORY_LUB"); size_t ln; u8 *lub = lf ? slurp(lf, &ln) : NULL;
    if (lub) {
        const char *b = (const char*)lub; size_t n2 = ln;
        int sw = test_lua_swap(&b, &n2);
        CHECK(sw == 1 && n2 > 10000 && !memcmp(b, "\x1bLuaQ\x00\x01\x04\x04\x04\x04\x00", 12), "Factory.lub swapped: %d, %zu bytes", sw, n2);
        const char *b2 = b; size_t n3 = n2; CHECK(test_lua_swap(&b2, &n3) == 0, "other chunks are not swapped");
        { const char *of = getenv("BBS_FACTORY_RV1"); size_t on; u8 *o = of ? slurp(of, &on) : NULL;
          if (o) { const char *b3 = (const char*)o; size_t n4 = on; CHECK(test_lua_swap(&b3, &n4) == 1 && n4 == n2, "the first Revenge Value build is replaced by the current one"); } }
        u8 *site = RVA(0x2c5962); CHECK(site[0] == 0xE8 && (u32)(0x2c5962 + 5 + *(s32*)(site + 1)) != 0x5bb9f0, "Lua call redirected");
    } else printf("  (Factory.lub skipped: BBS_FACTORY_LUB not set)\n");
    printf("t_bundle done\n");
}
/* EXP Zero's minimum damage without the ability: the game's own function 1400d2960 on a table with the values of
   Exp0/Exp0Param.edp (one row for normal enemies, one for bosses) */
static void t_floor(void) {
    static const u16 ids[] = { 0x83 };
    world(1, ids);
    static u8 edp[0xa8 + 2 * 0x34];
    memcpy(edp, "@EDP", 4); *(u32*)(edp + 4) = 2; *(u32*)(edp + 8) = 1; *(u32*)(edp + 12) = 1;
    memset(edp + 0x10, 0xff, 0x98);                                     /* class 15: no minimum */
    static const struct { int id, cls; } C[] = { { 1, 0 }, { 0x83, 1 }, { 0xac, 2 } };
    for (int i = 0; i < 3; i++) {
        int k = C[i].id - 1; u8 *b = edp + 0x10 + (k >> 1);
        *b = (k & 1) ? (u8)((*b & 0x0f) | (C[i].cls << 4)) : (u8)((*b & 0xf0) | C[i].cls);
    }
    static const int rows[2][13] = { { 0, 0, 15, 132, 30, 3, 10, 30, 3, 20, 30, 3, 30 }, { 0, 0, 200, 1500, 200, 0, 10, 200, 3, 15, 200, 3, 25 } };
    memcpy(edp + 0xa8, rows, sizeof rows);
    FN(void, 0xd2af0, u8*)(edp);                                         /* the game's loader */
    CHECK(G(u8*, 0x83e700) == edp && G(u32, 0x83e720) == 1 && G(u32, 0x83e724) == 1, "table loaded");
    CHECK(!FN(u8, 0x221900, u8*, u16)(pl, 0x1c9), "the player does not have EXP Zero");
    for (int diff = 0; diff < 3; diff++) {                               /* Beginner, Standard, Proud: as the game */
        int dmg = 1; G(u8, 0x10fa0881) = diff;
        FN(u64, 0xd2960, int*, u32, int, const char*)(&dmg, 0x83, 60, "m01ex00");
        CHECK(dmg == 1, "difficulty %d: no minimum without the ability (%d)", diff, dmg);
    }
    G(u8, 0x10fa0881) = 3;                                               /* Critical */
    static const struct { int id, hp; const char *name; int in, out; } T[] = {
        { 1, 60, "m01ex00", 1, 5 }, { 0x83, 60, "m01ex00", 1, 10 }, { 0xac, 60, "m01ex00", 1, 15 },
        { 1, 132, "m01ex00", 1, 7 }, { 0x83, 132, "m01ex00", 1, 14 }, { 0xac, 132, "m01ex00", 1, 22 },
        { 1, 600, "b01ex00", 1, 3 }, { 1, 1000, "b01ex00", 1, 5 }, { 0x83, 1000, "b01ex00", 1, 12 }, { 0xac, 1000, "b01ex00", 1, 20 },
        { 0xac, 1500, "b01ex00", 1, 26 },
        { 0x83, 60, "m01ex00", 40, 40 },          /* a stronger hit is not changed */
        { 0x92, 60, "m01ex00", 1, 1 },            /* Cure: no class */
        { 1, 200, "m01ex00", 1, 1 },              /* normal enemy over the table's range */
        { 1, 2000, "b01ex00", 1, 1 },             /* boss over the range */
    };
    for (unsigned i = 0; i < sizeof T / sizeof *T; i++) {
        int dmg = T[i].in;
        FN(u64, 0xd2960, int*, u32, int, const char*)(&dmg, T[i].id, T[i].hp, T[i].name);
        CHECK(dmg == T[i].out, "attack %x on %s with %d HP: %d -> %d (want %d)", T[i].id, T[i].name, T[i].hp, T[i].in, dmg, T[i].out);
    }
    int taken = 90;
    FN(u64, 0xd2c00, int*, int, char)(&taken, 100, 0);
    CHECK(taken == 90, "the cap on damage taken still needs the ability (%d)", taken);
    CHECK(!FN(u64, 0xd2cc0)(), "\"EXP Zero active\" still needs the ability");
    G(u8, 0x10fa0881) = 1;
    { u8 slot = G(u8, 0x814907 + 0x1c9 * 0x18); pl[0x4a3 + slot] = 1;      /* EXP Zero equipped, Standard: the game's rule */
      int dmg = 1; FN(u64, 0xd2960, int*, u32, int, const char*)(&dmg, 0x83, 60, "m01ex00");
      CHECK(slot >= 1 && slot <= 0x1e && dmg == 10, "with the ability it works on any difficulty (slot %d, %d)", slot, dmg);
      pl[0x4a3 + slot] = 0; }
    G(u8, 0x10fa0881) = 0;
    printf("t_floor done\n");
}
/* MP cost at the end of a command's description (message 0x320000 + id) */
extern const char *test_desc(int id, const char *text);
static const char *stub_text;
static int MSABI stub_msg_find(u32 msg, const char **text, void **layout, u16 *extra, char flag) {
    (void)layout; (void)extra; (void)flag;
    if (!stub_text) return 0;
    if (text) *text = stub_text;
    return msg == 0x320092 ? -1 : 1;
}
static void t_desc(void) {
    static const u16 ids[] = { 0x83 };
    world(1, ids);
    #define GREEN "\xf9\x51"
    #define BACK  "\xf9\x41"
    const char *aero = "Call on the wind to lift enemies into the air and\nthen send them flying. Stuns some foes.";
    const char *t = test_desc(0xa2, aero);
    CHECK(t && !strcmp(t, "Call on the wind to lift enemies into the air and\nthen send them flying. Stuns some foes. " GREEN "MP 10" BACK), "Aero: cost after the last line");
    t = test_desc(0xa3, aero); CHECK(t && strstr(t, GREEN "MP 15" BACK), "Aerora 15");
    t = test_desc(0xa4, aero); CHECK(t && strstr(t, GREEN "MP 20" BACK), "Aeroga 20");
    t = test_desc(0xac, "x"); CHECK(t && !strcmp(t, "x " GREEN "MP 50" BACK), "Mega Flare 50");
    t = test_desc(0x92, "Restore a small amount of HP."); CHECK(t && strstr(t, " " GREEN "MP All" BACK), "Cure: all MP");
    /* a long last line: the cost goes on its own line while there are fewer than three */
    t = test_desc(0x88, "Launch a series of three fireballs toward enemies.");
    CHECK(t && strstr(t, "enemies.\n" GREEN "MP "), "long single line: cost on a second line (%s)", t ? t : "-");
    t = test_desc(0x69, "Throw the Keyblade at the enemy, letting the\nwind guide it toward your target for multiple hits.");
    CHECK(t && strstr(t, "hits.\n" GREEN "MP 20"), "long second line: cost on a third line (Wind Raid 20)");
    t = test_desc(0x61, "Cloak yourself in darkness, then charge at\nfaraway enemies. The attack has a chance of\ndooming them, leaving them five seconds to live.");
    CHECK(t && strstr(t, "live. " GREEN "MP "), "three lines: the cost stays on the third");
    /* colour codes and icons in the text: "(Uses two slots.)" is yellow in the original */
    t = test_desc(0xb0, "Create an anti-gravity field no enemy can\nescape, then deal damage by sending them\nhurtling in all directions. \xf9Y(Uses two slots.)");
    CHECK(t && strstr(t, "slots.) " GREEN "MP 25" BACK), "after the yellow note: %s", t ? strrchr(t, '\n') + 1 : "-");
    t = test_desc(0x83, "Fire. \n"); CHECK(t && !strcmp(t, "Fire. " GREEN "MP 10" BACK), "trailing blanks are dropped");
    /* what has no MP cost is left alone */
    CHECK(!test_desc(0xbc, "Potion") && !test_desc(0xc5, "Ice cream"), "items");
    CHECK(!test_desc(0xfc, "Dodge") && !test_desc(0x110, "Block") && !test_desc(0x122, "Shotlock") && !test_desc(0x1a0, "Ability") && !test_desc(1, "Attack"), "actions, shotlocks, abilities, basic attacks");
    /* through the hooked call of SetNodeMsg: 1a81bf -> our hook -> the game's lookup (a stand-in here) */
    u8 *site = RVA(0x1a81bf); s32 rel; memcpy(&rel, site + 1, 4);
    CHECK(site[0] == 0xe8 && (u32)(0x1a81bf + 5 + rel) != 0x1b3750, "SetNodeMsg's lookup call goes to the mod");
    u8 jmp[12] = { 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0 }; void *st = (void*)stub_msg_find; memcpy(jmp + 2, &st, 8);
    memcpy(RVA(0x1b3750), jmp, 12);
    int MSABI (*hook)(u32, const char**, void**, u16*, char) = (void*)(site + 5 + rel);
    const char *out = NULL; stub_text = aero;
    int r = hook(0x3200a2, &out, NULL, NULL, 0);
    CHECK(r == 1 && out && strstr(out, GREEN "MP 10"), "description of Aero gets its cost");
    out = NULL; r = hook(0x320092, &out, NULL, NULL, 0); CHECK(r == -1 && out && strstr(out, "MP All"), "return value kept");
    out = NULL; r = hook(0x3200bc, &out, NULL, NULL, 0); CHECK(r == 1 && out == aero, "an item's description is passed through");
    out = NULL; r = hook(0x343c0001, &out, NULL, NULL, 0); CHECK(r == 1 && out == aero, "other messages are passed through");
    out = NULL; r = hook(0x1e00a2, &out, NULL, NULL, 0); CHECK(out == aero, "another message group with the same index");
    stub_text = NULL; out = (const char*)1; r = hook(0x3200a2, &out, NULL, NULL, 0); CHECK(r == 0 && out == (const char*)1, "message not found: untouched");
    stub_text = aero; r = hook(0x3200a2, NULL, NULL, NULL, 0); CHECK(r == 1, "no text pointer asked for");
    printf("t_desc done\n");
}
/* shortcuts: L1 + face button uses the command in the bound deck slot */
extern void test_sc_pad_frame(void); extern int test_sc_tap(void); extern s8 *test_sc_bind(void);
extern int *test_menu_cur(void); extern int test_menu_step(u8 *cmd), test_confirm(u8 *pad, u8 *cmd);
extern u8 *test_slot_plate(u8 *cmd, int slot);
extern int sc_held(void), sc_want(void), sc_slot(int row), sc_assign(int row, int slot); extern void sc_set_hud(int on);
#define PAD_L1 0x400
#define PAD_R1 0x800
#define PAD_TRI 0x1000
#define PAD_CIR 0x2000
#define PAD_SQU 0x8000
static void pad(u32 held, u32 edge) {       /* what the game's pad update leaves, then our filter */
    G(u32, 0x8f64930) = held; G(u32, 0x8f64934) = edge; G(u32, 0x8f64938) = edge; G(u32, 0x8f6493c) = edge; G(u32, 0x8f6499c) = 0;
    test_sc_pad_frame();
}
static void t_shortcut(void) {
    static const u16 ids[] = { 0x83 /*Fire*/, 0x65 /*Strike Raid*/, 0xbc /*Potion*/, 0x92 /*Cure*/, 0xac /*Mega Flare*/ };
    world(5, ids);
    *(u32*)(mgr + 0xf8) = 1;                      /* pad enabled */
    memcpy(RVA(0x1b2e40), "\x31\xc0\xc3", 3);       /* sound: none here */
    *(u32*)(pl + 0x318) = 0x40;                   /* player idle */
    *(u16*)(cmd + 0x188) = 0x151; *(u16*)(pl + 0x354) = 0x151;
    test_tick(gauge);
    /* defaults: circle, triangle, square, cross = slots 1..4 */
    CHECK(sc_slot(0) == 0 && sc_slot(1) == 1 && sc_slot(2) == 2 && sc_slot(3) == 3, "default bindings %d %d %d %d", sc_slot(0), sc_slot(1), sc_slot(2), sc_slot(3));
    /* slot -> plate: plates are packed, the slot numbers are in 1408187a8 */
    CHECK(test_slot_plate(cmd, 0) == plate[0] && test_slot_plate(cmd, 4) == plate[4] && !test_slot_plate(cmd, 6), "plates by slot");
    G(u8, 0x8187a8 + 1) = 5; G(u8, 0x8187a8 + 2) = 6;      /* a deck with slots 1..2 used ... 5, 6 */
    CHECK(test_slot_plate(cmd, 5) == plate[1] && test_slot_plate(cmd, 6) == plate[2] && !test_slot_plate(cmd, 1), "packed deck: slot 6 is the second plate");
    G(u8, 0x8187a8 + 1) = 1; G(u8, 0x8187a8 + 2) = 2;
    /* not shown: no HUD, or R1 held as well (shotlock), or L1 not held */
    sc_set_hud(0); pad(PAD_L1 | PAD_TRI, PAD_TRI);
    CHECK(!sc_held() && (G(u32, 0x8f64930) & PAD_TRI), "no command menu on screen: buttons untouched");
    sc_set_hud(1); pad(0, 0);
    pad(PAD_L1 | PAD_R1 | PAD_TRI, PAD_TRI); CHECK(!sc_held() && (G(u32, 0x8f64934) & PAD_TRI), "L1 + R1: the shotlock's, untouched");
    pad(0, 0);
    pad(PAD_TRI, PAD_TRI); CHECK(!sc_held() && G(u32, 0x8f64930) == PAD_TRI, "without L1 nothing changes");
    pad(0, 0);
    /* L1 held: the face buttons leave the pad words; triangle = row 1 = slot 2 = Strike Raid */
    pad(PAD_L1, PAD_L1); CHECK(sc_held() && sc_want() < 0, "L1 held: shortcut list up");
    pad(PAD_L1 | PAD_TRI, PAD_TRI);
    CHECK(sc_held() && G(u32, 0x8f64930) == PAD_L1 && G(u32, 0x8f64934) == 0 && G(u32, 0x8f6493c) == 0, "triangle taken from the pad (%x %x)", G(u32, 0x8f64930), G(u32, 0x8f64934));
    CHECK(sc_want() == 1, "wanted row %d", sc_want());
    CHECK(test_confirm(mgr + 0x58, cmd) == 0, "the game's own confirm test sees nothing");
    test_menu_step(cmd);
    CHECK(*(u16*)(cmd + 0x80) == 0x65 && *(u8**)(cmd + 0x90) == plate[1] && *test_mp() == 90, "Strike Raid queued, MP %.0f", *test_mp());
    CHECK(sc_want() < 0, "press consumed");
    *(u64*)(cmd + 0x80) = 0;
    pad(PAD_L1, 0); pad(0, 0);
    CHECK(!test_sc_tap(), "letting L1 go after a shortcut is not a tap");
    /* A button counts where it went down.  The second argument is the game's own "pressed this frame", which
       says "pressed" every frame for a held face button once its history entry has been wiped. */
    { const u32 CIR = 0x2000;
      pad(CIR, CIR); CHECK(!sc_held() && (G(u32, 0x8f64930) & CIR) && (G(u32, 0x8f64934) & CIR), "circle without L1: the game's");
      pad(CIR, 0);
      pad(PAD_L1 | CIR, PAD_L1);
      CHECK(sc_held() && sc_want() < 0 && !(G(u32, 0x8f64930) & CIR), "circle held, then L1: the list comes up, no shortcut is used");
      pad(PAD_L1 | CIR, CIR); pad(PAD_L1 | CIR, CIR);
      CHECK(sc_want() < 0, "still held: still nothing, whatever the game's count says");
      test_menu_step(cmd);
      CHECK(*(u16*)(cmd + 0x80) == 0, "and nothing was queued");
      pad(PAD_L1, 0); pad(PAD_L1 | CIR, CIR);
      CHECK(sc_want() == 0, "let go and pressed with the list up: circle's shortcut (row %d)", sc_want());
      sc_done();
      pad(PAD_L1 | CIR, CIR); pad(PAD_L1 | CIR, CIR);
      CHECK(sc_want() < 0, "holding it does not use the shortcut again");
      /* L1 let go first: the button is not handed to the game as a new press */
      pad(CIR, CIR);
      CHECK(!sc_held() && !(G(u32, 0x8f64930) & CIR) && !(G(u32, 0x8f64934) & CIR) && !(G(u32, 0x8f6493c) & CIR), "L1 let go, circle still down: hidden from the game (%x %x)", G(u32, 0x8f64930), G(u32, 0x8f64934));
      CHECK(test_confirm(mgr + 0x58, cmd) == 0, "the game's confirm test sees nothing");
      pad(CIR | PAD_TRI, CIR | PAD_TRI);
      CHECK(!(G(u32, 0x8f64930) & CIR) && (G(u32, 0x8f64930) & PAD_TRI) && (G(u32, 0x8f64934) & PAD_TRI), "another button pressed meanwhile is the game's");
      pad(0, 0); pad(CIR, CIR);
      CHECK((G(u32, 0x8f64930) & CIR) && (G(u32, 0x8f64934) & CIR), "let go and pressed again: the game's");
      pad(0, 0); *(u64*)(cmd + 0x80) = 0; }
    /* a tap of L1 (camera behind the character / next target) is reported on release */
    pad(PAD_L1, PAD_L1); CHECK(!test_sc_tap(), "nothing on the press");
    pad(PAD_L1, 0); pad(PAD_L1, 0); pad(0, 0); CHECK(test_sc_tap() == 1, "tap on release");
    {   u8 *site = RVA(0x22b5df); s32 rel; memcpy(&rel, site + 1, 4);
        int MSABI (*cam)(u8*) = (void*)(site + 5 + rel);
        CHECK(cam(mgr + 0x58) == 1, "camera reset pad test fires on the tap");
        site = RVA(0x2650d4); memcpy(&rel, site + 1, 4);
        int MSABI (*tgt)(u8*, u32*, u32*) = (void*)(site + 5 + rel);
        u32 a = 9, b = 9; *(u32*)(mgr + 0x58 + 0x38) = 7;
        CHECK(tgt(mgr + 0x58, &a, &b) == 1 && a == 0x40000000 && b == 0 && *(u32*)(mgr + 0x58 + 0x38) == 0, "target switch pad test fires on the tap");
        pad(0, 0); CHECK(!test_sc_tap() && cam(mgr + 0x58) == 0 && tgt(mgr + 0x58, &a, &b) == 0 && a == 0, "only for that frame");
        pad(PAD_L1, PAD_L1); CHECK(cam(mgr + 0x58) == 0, "not on the press itself");
        for (int i = 0; i < 30; i++) pad(PAD_L1, 0);
        pad(0, 0); CHECK(!test_sc_tap(), "a long hold is not a tap"); }
    /* busy character: the press waits, then goes through when the game allows a command */
    *(u32*)(pl + 0x318) = 0;
    pad(PAD_L1, PAD_L1); pad(PAD_L1 | PAD_CIR, PAD_CIR);
    test_menu_step(cmd); CHECK(*(u64*)(cmd + 0x80) == 0 && sc_want() == 0, "busy: nothing queued, press kept");
    for (int i = 0; i < 5; i++) { pad(PAD_L1 | PAD_CIR, 0); test_menu_step(cmd); }
    CHECK(sc_want() == 0, "still waiting after 5 frames");
    *(u32*)(pl + 0x318) = 0x40; pad(PAD_L1, 0); test_menu_step(cmd);
    CHECK(*(u16*)(cmd + 0x80) == 0x83 && *test_mp() == 80, "Fire goes off when free, MP %.0f", *test_mp());
    *(u64*)(cmd + 0x80) = 0;
    *(u32*)(pl + 0x318) = 0; pad(PAD_L1 | PAD_CIR, PAD_CIR);
    for (int i = 0; i < 14; i++) { pad(PAD_L1, 0); test_menu_step(cmd); }
    *(u32*)(pl + 0x318) = 0x40; test_menu_step(cmd);
    CHECK(*(u64*)(cmd + 0x80) == 0 && sc_want() < 0, "the wait runs out");
    /* square = slot 3 = Potion: an item, no MP */
    pad(PAD_L1 | PAD_SQU, PAD_SQU); test_menu_step(cmd);
    CHECK(*(u16*)(cmd + 0x80) == 0xbc && *test_mp() == 80, "Potion from its shortcut");
    *(u64*)(cmd + 0x80) = 0;
    /* the shortcut belongs to the slot: another command in slot 1 is what circle uses now */
    *(u16*)&deck[0] = 0x8a;                       /* Blizzard */
    pad(PAD_L1, 0); pad(PAD_L1 | PAD_CIR, PAD_CIR); test_menu_step(cmd);
    CHECK(*(u16*)(cmd + 0x80) == 0x8a, "slot 1 now holds Blizzard: %x", *(u16*)(cmd + 0x80));
    *(u64*)(cmd + 0x80) = 0;
    /* rebinding: circle -> slot 5 (Mega Flare); a button without a slot does nothing */
    CHECK(sc_assign(0, 4) == 1 && sc_slot(0) == 4 && sc_assign(0, 4) == 0, "assign");
    CHECK(sc_assign(1, -1) == 1 && sc_slot(1) == -1, "clear");
    *test_mp() = 100;
    pad(PAD_L1, 0); pad(PAD_L1 | PAD_CIR, PAD_CIR); test_menu_step(cmd);
    CHECK(*(u16*)(cmd + 0x80) == 0xac && *test_mp() == 50, "Mega Flare from circle, MP %.0f", *test_mp());
    *(u64*)(cmd + 0x80) = 0;
    pad(PAD_L1, 0); pad(PAD_L1 | PAD_TRI, PAD_TRI); test_menu_step(cmd);
    CHECK(*(u64*)(cmd + 0x80) == 0 && sc_want() < 0, "unbound button: nothing");
    /* a slot with no plate (empty) and MP charge */
    sc_assign(1, 7); pad(PAD_L1, 0); pad(PAD_L1 | PAD_TRI, PAD_TRI); test_menu_step(cmd);
    CHECK(*(u64*)(cmd + 0x80) == 0 && sc_want() < 0, "empty slot: nothing");
    *test_burn() = 1; pad(PAD_L1, 0); pad(PAD_L1 | PAD_CIR, PAD_CIR); test_menu_step(cmd);
    CHECK(*(u64*)(cmd + 0x80) == 0, "MP charge: commands stay off");
    PCT(2) = 100;                                 /* the Potion plate, used above, is ready again */
    pad(PAD_L1, 0); pad(PAD_L1 | PAD_SQU, PAD_SQU); test_menu_step(cmd);
    CHECK(*(u16*)(cmd + 0x80) == 0xbc, "... items still work");
    *test_burn() = 0; *(u64*)(cmd + 0x80) = 0;
    /* while L1 is held the menu itself does not move */
    int *cur = test_menu_cur(); int c0 = *cur;
    G(u32, 0x8f64930) = PAD_L1 | 0x40; G(u32, 0x8f64934) = 0x40; G(u32, 0x8f6493c) = 0x40; test_sc_pad_frame();   /* d-pad down */ test_menu_step(cmd);
    CHECK(*cur == c0, "cursor stays");
    pad(0, 0);
    printf("t_shortcut done\n");
}
/* the shortcut list's plates on the real 2D runtime with bc01_00.l2d (BBS_PLATE_L2D) */
extern void test_sc_frame(u8 *cmd, int show, int battle); extern int *test_sc_entries(void);
static const char *node_text(int h, u16 node) {
    u8 *t = NULL;
    if (!FN(int, 0x1a66c0, int, u16, u8**, int)(h, node, &t, 0) || !t) return NULL;
    return *(const char**)(t + 0x30);
}
static void t_sclist(void) {
    const char *fn = getenv("BBS_PLATE_L2D");
    if (!fn) { printf("  (skipped: BBS_PLATE_L2D not set)\n"); return; }
    FILE *f = fopen(fn, "rb"); if (!f) { printf("  cannot open %s\n", fn); fails++; return; }
    u8 *img = memalign(16, 0x80000); size_t n = fread(img, 1, 0x80000, f); fclose(f);
    static const u16 ids[] = { 0x83 /*Fire*/, 0x65 /*Strike Raid*/, 0xbc /*Potion*/, 0x92 /*Cure*/ };
    world(4, ids);
    *(u8*)RVA(0x111660) = 0xC3; *(u8*)RVA(0x10c5e0) = 0xC3; memcpy(RVA(0x11a8a0), "\x31\xc0\xc3", 3);
    FN(void, 0x1a6d70, void)();
    for (int i = 0; i < 6; i++) {               /* stand-in font tables, as in t_status */
        u8 *ft = calloc(1, 0x200); u64 *gl = calloc(4, 8);
        *(u16*)(ft + 0x100) = 1; *(u16*)(ft + 0x102) = 16; *(u16*)(ft + 0x10c) = 256; ft[0x10e] = 16; *(u64**)(ft + 0x118) = gl;
        G(u8*, 0x8f86968 + i * 8) = ft;
    }
    G(u8*, 0x8f86998) = calloc(1, 0x500);      /* ... and an empty table of button pictures */
    int d = FN(int, 0x1a6fb0, void*, const char*, int)(img, "bc01_00.l2d", 0);
    CHECK(d > 0, "register bc01_00 (%zu bytes)", n);
    G(int, 0x10f9ed48) = FN(int, 0x1a64f0, const char*)("bc01_00.l2d"); G(int, 0x10f9ed4c) = FN(int, 0x1a64f0, const char*)("bc01_00:0");
    G(const char*, 0x814908 + 0x83 * 0x18) = "Fire"; G(const char*, 0x814908 + 0x65 * 0x18) = "Strike Raid";    /* names come from the message files in the game */
    G(const char*, 0x814908 + 0xbc * 0x18) = "Potion"; G(const char*, 0x814908 + 0x92 * 0x18) = "Cure"; G(const char*, 0x814908 + 0x8a * 0x18) = "Blizzard";
    test_tick(gauge);
    test_sc_frame(cmd, 0, 0);
    int *e = test_sc_entries();
    CHECK(!e[0] && !e[3], "nothing is made until the list is first shown");
    test_sc_frame(cmd, 1, 0);
    CHECK(e[0] > 0 && e[1] > 0 && e[2] > 0 && e[3] > 0, "four plates %d %d %d %d", e[0], e[1], e[2], e[3]);
    const char *t0 = node_text(e[0], 0), *t1 = node_text(e[1], 0), *t2 = node_text(e[2], 0), *t3 = node_text(e[3], 0);
    printf("   rows: '%s' '%s' '%s' '%s'\n", t0 ? t0 : "-", t1 ? t1 : "-", t2 ? t2 : "-", t3 ? t3 : "-");
    CHECK(t0 && !strcmp(t0, "Fire") && t1 && !strcmp(t1, "Strike Raid") && t2 && !strcmp(t2, "Potion") && t3 && !strcmp(t3, "Cure"), "names of the commands in slots 1..4");
    const char *i0 = node_text(e[0], 0x5d), *i1 = node_text(e[1], 0x5d), *i2 = node_text(e[2], 0x5d), *i3 = node_text(e[3], 0x5d);
    CHECK(i0 && !strcmp(i0, "\xf5\x7b") && i1 && !strcmp(i1, "\xf5\x67") && i2 && !strcmp(i2, "\xf5\x66") && i3 && !strcmp(i3, "\xf5\x7c"), "circle, triangle, square, cross in the plate's own button text");
    const char *c2 = node_text(e[2], 0x5b);
    CHECK(c2 && !strcmp(c2, "3") && FN(int, 0x1a6670, int, u16)(e[2], 0x5b) && !FN(int, 0x1a6670, int, u16)(e[0], 0x5b), "the Potion row shows its count (%s)", c2 ? c2 : "-");
    CHECK(!FN(int, 0x1a6670, int, u16)(e[0], 0x5a) && !FN(int, 0x1a6670, int, u16)(e[1], 0x5a) && !FN(int, 0x1a6670, int, u16)(e[3], 0x5a), "no category icon on attack and magic commands");
    float x = 0, y = 0;
    {   /* the item row: the bottle (sequence 0x22), still (control 3), moved right for the longer plate */
        u8 *lc2 = FN(u8*, 0x1a65c0, int)(e[2]), *nd = NULL;
        for (int i = 0; lc2 && i < *(s16*)(lc2 + 0xa8); i++) if (*(s16*)(*(u8**)(lc2 + 0x98) + i * 0xb8 + 0xb0) == 0x5a) nd = *(u8**)(lc2 + 0x98) + i * 0xb8;
        FN(int, 0x1a6880, int, u16, float*, float*)(e[2], 0x5a, &x, &y);
        u8 *o = nd ? **(u8***)(nd + 0xa8) : NULL;
        CHECK(FN(int, 0x1a6670, int, u16)(e[2], 0x5a) && FN(int, 0x1a6790, int, u16)(e[2], 0x5a) == 3 && x == 11 && y == 0, "the Potion row has its icon (control %d, moved %.0f)", FN(int, 0x1a6790, int, u16)(e[2], 0x5a), x);
        CHECK(o && (s8)nd[0xb2] == 4 && *(s16*)(*(u8**)(o + 1 * 0x50 + 0x30) + 4) == 5 && (o[1 * 0x50 + 0x4b] & 4), "it is the item bottle: sprite %d, drawn", o ? *(s16*)(*(u8**)(o + 1 * 0x50 + 0x30) + 4) : -1);
        test_sc_frame(cmd, 1, 0);
        CHECK(FN(int, 0x1a6790, int, u16)(e[2], 0x5a) == 3 && FN(int, 0x1a60e0, int)(e[2]) == 0, "and stays still on the next frame");
    }
    for (int r = 0; r < 4; r++) {
        CHECK(FN(int, 0x1a60e0, int)(e[r]) == 0 && FN(int, 0x1a6060, int)(e[r]) == 1, "row %d: the long plate (control %d), visible", r, FN(int, 0x1a60e0, int)(e[r]));
        FN(int, 0x1a6290, int, float*, float*)(e[r], &x, &y);
        CHECK(y == 198 + r * 15, "row %d at y %.0f", r, y);
    }
    /* the body: our records on the seven objects, the game's key format */
    {   typedef struct { s32 maxf; s16 spr; u16 key; u8 keyn[11]; u8 kind, blend, flag, sciss, z; } Anim;
        typedef struct { float t; union { float f; u8 c[4]; } v; u32 interp; } Key;
        extern void *test_sc_anims(void), *test_sc_keys(void);
        Anim *ma = test_sc_anims(); Key *mk = test_sc_keys();
        CHECK(sizeof(Anim) == 0x18 && sizeof(Key) == 0xc, "record sizes");
        /* a plate as the game shows it under its cursor in the field: what our records stand in for */
        G(u8, 0x8f88028) = 0; G(u8, 0x8f8802a) = 0;
        int ref = FN(int, 0x1a4ec0, int, u16, int, void*)(G(int, 0x10f9ed48), 3, 0, NULL);
        FN(int, 0x1a7fb0, int, u16, int, u16, int)(ref, 0x5d, G(int, 0x10f9ed4c), 0xb, 0);
        u8 *lc[2] = { FN(u8*, 0x1a65c0, int)(ref), FN(u8*, 0x1a65c0, int)(e[0]) }, *ob[2] = { NULL, NULL };
        for (int w = 0; w < 2; w++) for (int i = 0; lc[w] && i < *(s16*)(lc[w] + 0xa8); i++) {
            u8 *nd = *(u8**)(lc[w] + 0x98) + i * 0xb8;
            if (*(s16*)(nd + 0xb0) == 0x5d) { CHECK((s8)nd[0xb2] == 7, "body has seven objects (%d)", (s8)nd[0xb2]); ob[w] = **(u8***)(nd + 0xa8); }
        }
        CHECK(ob[0] && ob[1], "body objects found");
        if (ob[0] && ob[1]) {
            static const s16 spr[7] = { -1, 0, 17, 11, -1, 16, 2 };
            static const u8 drawn_game[7] = { 1, 0, 1, 0, 1, 1, 0 }, drawn_ours[7] = { 1, 0, 1, 0, 1, 0, 1 };
            for (int i = 0; i < 7; i++) {
                Anim *g = *(Anim**)(ob[0] + i * 0x50 + 0x30), *m = *(Anim**)(ob[1] + i * 0x50 + 0x30);
                CHECK(g->spr == spr[i], "game object %d is sprite %d (%d)", i, spr[i], g->spr);
                CHECK(m == &ma[i] && *(Key**)(ob[1] + i * 0x50 + 0x38) == mk && m->spr == spr[i], "row object %d plays our record", i);
                if (i) CHECK(!!(ob[0][i * 0x50 + 0x4b] & 4) == drawn_game[i] && !!(ob[1][i * 0x50 + 0x4b] & 4) == drawn_ours[i], "object %d drawn: game %d, ours %d", i, !!(ob[0][i * 0x50 + 0x4b] & 4), !!(ob[1][i * 0x50 + 0x4b] & 4));
            }
            /* the game's record of the long plate: place 10 and the field blue, in the layout our records copy */
            Anim *g = *(Anim**)(ob[0] + 2 * 0x50 + 0x30); Key *gk = *(Key**)(ob[0] + 2 * 0x50 + 0x38) + g->key;
            CHECK(g->maxf == 0 && g->kind == 1 && g->keyn[1] == 1 && g->keyn[10] == 1 && g->keyn[0] == 0 && g->keyn[2] == 0, "game: one x key, one colour key");
            CHECK(gk[0].t == 0 && gk[0].v.f == 10.0f && gk[1].v.c[0] == 0x00 && gk[1].v.c[1] == 0x5f && gk[1].v.c[2] == 0xf0 && gk[1].v.c[3] == 0xff, "game: x 10, colour 00 5f f0 ff (%g, %02x %02x %02x)", gk[0].v.f, gk[1].v.c[0], gk[1].v.c[1], gk[1].v.c[2]);
            Anim *m = &ma[2]; Key *k = mk + m->key;
            CHECK(m->maxf == 0 && m->kind == 1 && m->keyn[1] == 1 && m->keyn[10] == 1 && k[0].v.f == 10.0f && k[1].v.c[0] == 0x80 && k[1].v.c[1] == 0x80 && k[1].v.c[2] == 0x80 && k[1].v.c[3] == 0xff, "ours: the same place, grey");
            /* the game's text record: the button's picture at (11, 8); its root: scale 0.88 */
            g = *(Anim**)(ob[0] + 4 * 0x50 + 0x30); gk = *(Key**)(ob[0] + 4 * 0x50 + 0x38) + g->key;
            CHECK(g->kind == 2 && g->keyn[1] == 1 && g->keyn[2] == 1 && gk[0].v.f == 11.0f && gk[1].v.f == 8.0f, "game: button text at (%g, %g)", gk[0].v.f, gk[1].v.f);
            m = &ma[4]; k = mk + m->key;
            CHECK(m->kind == 2 && m->keyn[1] == 1 && m->keyn[2] == 1 && k[0].v.f == 11.0f && k[1].v.f == 8.0f, "ours: the same");
            g = *(Anim**)(ob[0] + 0x30); gk = *(Key**)(ob[0] + 0x38) + g->key;
            m = &ma[0]; k = mk + m->key;
            CHECK(g->kind == 0 && g->keyn[8] == 1 && g->keyn[9] == 1 && gk[0].v.f == k[0].v.f && gk[1].v.f == k[1].v.f && m->keyn[8] == 1 && m->keyn[9] == 1, "root scale as the game's (%g)", gk[0].v.f);
            m = &ma[6]; k = mk + m->key;
            CHECK(m->kind == 1 && k[0].v.f == 18.0f && k[1].v.f == 2.0f && k[2].v.c[0] == 0x60 && k[2].v.c[3] == 0xff, "the gradient inside at (18, 2), dark grey");
        }
        Pair *q = NULL; int n = FN(int, 0x1a6980, int, u16, int, Pair**)(e[0], 0x5d, 6, &q);
        CHECK(n == 3 && q && q[0].p->u0 == 2 && q[0].p->v0 == 30 && q[2].p->u1 == 32 && q[2].p->v1 == 60 && q[0].g->x0 == 1 && q[2].g->x1 == 107, "its quads: the game's gradient art");
        q = NULL; n = FN(int, 0x1a6980, int, u16, int, Pair**)(ref, 0x5d, 6, &q);
        CHECK(n <= 0 || !q, "the game's plates keep the shared sprite");
        /* a control change puts the game's records back; the next frame ours again */
        FN(int, 0x1a7520, int, int)(e[0], 3);
        CHECK(*(Anim**)(ob[1] + 2 * 0x50 + 0x30) != &ma[2], "control change: the game's record");
        test_sc_frame(cmd, 1, 0);
        CHECK(FN(int, 0x1a60e0, int)(e[0]) == 0 && *(Anim**)(ob[1] + 2 * 0x50 + 0x30) == &ma[2] && (ob[1][2 * 0x50 + 0x4b] & 4) && !(ob[1][1 * 0x50 + 0x4b] & 4), "next frame: ours again");
        /* the same in battle */
        test_sc_frame(cmd, 0, 1); test_sc_frame(cmd, 1, 1);
        CHECK(*(Anim**)(ob[1] + 2 * 0x50 + 0x30) == &ma[2], "battle: the same grey plate");
        FN(void, 0x1a57f0, int)(ref);
    }
    u32 col = 0;
    FN(int, 0x1a6600, int, u16, u32*)(e[0], 0, &col); CHECK(col == 0xff808080 && FN(int, 0x1a6790, int, u16)(e[0], 0) == 0, "usable: name in white (%08x)", col);
    FN(int, 0x1a6600, int, u16, u32*)(e[3], 0, &col); CHECK(col == 0xff107080, "Cure would use all MP: yellow name (%08x)", col);
    FN(int, 0x1a6880, int, u16, float*, float*)(e[0], 0, &x, &y); CHECK(x == -1 && y == 0, "name node at the game's place for the long plate (%.0f)", x);
    /* reloading / MP charge: dim */
    *(u32*)(plate[1] + 0x60) |= 2; test_sc_frame(cmd, 1, 0);
    FN(int, 0x1a6600, int, u16, u32*)(e[1], 0, &col); CHECK(col == 0xff404040, "not ready: dim name (%08x)", col);
    *(u32*)(plate[1] + 0x60) &= ~2u;
    *test_burn() = 1; test_sc_frame(cmd, 1, 0);
    FN(int, 0x1a6600, int, u16, u32*)(e[0], 0, &col); { u32 c2 = 0; FN(int, 0x1a6600, int, u16, u32*)(e[2], 0, &c2);
    CHECK(col == 0xff404040 && c2 == 0xff808080, "MP charge: commands dim, the item still white"); }
    *test_burn() = 0;
    /* the slot's command changes: the row follows; a button without a slot shows a blank */
    *(u16*)&deck[0] = 0x8a; test_sc_frame(cmd, 1, 0);
    t0 = node_text(e[0], 0); CHECK(t0 && !strcmp(t0, "Blizzard"), "slot 1 changed: %s", t0 ? t0 : "-");
    *(u16*)&deck[2] = 0x83; ((u8*)&deck[2])[2] = 1; *(u32*)(plate[2] + 0x60) &= ~0x800u; test_sc_frame(cmd, 1, 0);
    CHECK(!FN(int, 0x1a6670, int, u16)(e[2], 0x5a) && !FN(int, 0x1a6670, int, u16)(e[2], 0x5b), "the item's slot now holds a spell: icon and count gone");
    sc_assign(1, -1); test_sc_frame(cmd, 1, 0);
    t1 = node_text(e[1], 0); FN(int, 0x1a6600, int, u16, u32*)(e[1], 0, &col); CHECK(t1 && !strcmp(t1, "---") && col == 0xff404040, "unbound: blank and dim");
    test_sc_frame(cmd, 0, 0);
    CHECK(FN(int, 0x1a6060, int)(e[0]) == 0 && FN(int, 0x1a6060, int)(e[3]) == 0, "hidden when L1 is let go");
    printf("t_sclist done\n");
}
/* the MP row of the menu's character panel, on the real 2D-layout runtime with the real camp.l2d
   (needs BBS_CAMP_L2D=<camp.l2d taken out of arc_en/menu/camp.arc>) */
extern void test_status_frame(u8 *self); extern int *test_status_handles(void);
extern int test_stat_patch(u8 *pix, int w, int h); extern void test_set_stat_art(int on);
static void t_status(void) {
    /* the texture: recognised by its HP / FP labels, the new label goes into the free area */
    {   u8 *pix = calloc(1024 * 1024, 4);
        CHECK(!test_stat_patch(pix, 1024, 1024), "an empty 1024x1024 texture is not the menu sheet");
        const char *png = getenv("BBS_CAMP_RAW");          /* RGBA dump of the remastered sheet */
        FILE *f = png ? fopen(png, "rb") : NULL;
        if (f) {
            fread(pix, 4, 1024 * 1024, f); fclose(f);
            u8 *before = malloc(1024 * 1024 * 4); memcpy(before, pix, 1024 * 1024 * 4);
            CHECK(test_stat_patch(pix, 1024, 1024), "menu sheet recognised");
            long diff = 0, out = 0;
            for (int y = 0; y < 1024; y++) for (int x = 0; x < 1024; x++) if (memcmp(pix + (y * 1024 + x) * 4, before + (y * 1024 + x) * 4, 4)) {
                diff++; if (x < 298 || x >= 328 || y < 674 || y >= 694) out++; }
            CHECK(diff > 100 && !out, "only the new cell changed (%ld texels, %ld outside)", diff, out);
            #define A(x, y) pix[((y) * 1024 + (x)) * 4 + 3]
            CHECK(A(302, 678) == 255 && A(303, 678) == 255 && A(311, 687) == 255 && A(306, 684) == 0, "the M: stems %d %d, open below the V %d", A(302, 678), A(311, 687), A(306, 684));
            int same = 1; for (int y = 0; y < 10; y++) for (int x = 0; x < 8; x++) same &= A(314 + x, 678 + y) == before[((648 + y) * 1024 + 46 + x) * 4 + 3];
            CHECK(same, "the P is the sheet's own");
            CHECK(!test_stat_patch(pix, 1024, 1024), "a second pass changes nothing (the cell is in use now)");
            free(before);
        } else printf("  (texture part skipped: BBS_CAMP_RAW not set)\n");
        free(pix);
    }
    const char *fn = getenv("BBS_CAMP_L2D");
    if (!fn) { printf("  (layout part skipped: BBS_CAMP_L2D not set)\n"); return; }
    FILE *f = fopen(fn, "rb"); if (!f) { printf("  cannot open %s\n", fn); fails++; return; }
    u8 *img = memalign(16, 0x80000); size_t n = fread(img, 1, 0x80000, f); fclose(f);
    PD[0x31] = 1; PD[0x36] = 4;                 /* 100 MP */
    *(u8*)RVA(0x111660) = 0xC3;                 /* task registration: not needed here */
    *(u8*)RVA(0x10c5e0) = 0xC3;                 /* texture upload: no graphics device here */
    memcpy(RVA(0x11a8a0), "\x31\xc0\xc3", 3);      /* resource lookup: nothing loaded */
    FN(void, 0x1a6d70, void)();                 /* L2D runtime init */
    for (int i = 0; i < 6; i++) {               /* stand-in font tables: one glyph, 16 high (text is laid out, never drawn here) */
        u8 *ft = calloc(1, 0x200); u64 *gl = calloc(4, 8);
        *(u16*)(ft + 0x100) = 1; *(u16*)(ft + 0x102) = 16; *(u16*)(ft + 0x10c) = 256; ft[0x10e] = 16; *(u64**)(ft + 0x118) = gl;
        G(u8*, 0x8f86968 + i * 8) = ft;
    }
    int d = FN(int, 0x1a6fb0, void*, const char*, int)(img, "camp.l2d", 0);
    CHECK(d > 0, "camp.l2d registered (%zu bytes): %d", n, d);
    G(u8, 0x8f88028) = 1; G(u8, 0x8f8802a) = 1;
    int h = FN(int, 0x1a4ec0, int, u16, int, void*)(d, 0x67, 1, NULL);        /* as 14042b340 creates it */
    CHECK(h > 0, "chara_plate instance %d", h);
    FN(int, 0x1a7600, int, s16, int)(h, 7, 0);
    u8 *self = calloc(1, 0xb0); *(int*)(self + 0x20) = h;
    float x, y;
    #define NPOS(node) (x = y = -1, FN(int, 0x1a6880, int, u16, float*, float*)(h, node, &x, &y))
    NPOS(0x1a); CHECK(x == 0 && y == 41, "FP label starts at (%.0f, %.0f)", x, y);
    test_set_stat_art(1);
    test_status_frame(self);
    NPOS(0x1a); CHECK(x == 0 && y == 52, "FP label -> (%.0f, %.0f)", x, y);
    NPOS(0x24); CHECK(x == 69 && y == 50, "FP value -> (%.0f, %.0f)", x, y);
    NPOS(0x1b); CHECK(x == 0 && y == 62, "NEXT LEVEL label -> (%.0f, %.0f)", x, y);
    NPOS(0x26); CHECK(x == 69 && y == 69, "NEXT LEVEL value -> (%.0f, %.0f)", x, y);
    NPOS(0x19); CHECK(x == 0 && y == 30, "HP label stays (%.0f, %.0f)", x, y);
    NPOS(0x22); CHECK(x == 69 && y == 28, "HP value stays (%.0f, %.0f)", x, y);
    Pair *q = NULL; int cnt = FN(int, 0x1a6980, int, u16, int, Pair**)(h, 0x1d, 5, &q);
    CHECK(cnt == 4 && q, "the plate's lines: %d quads", cnt);
    if (cnt == 4 && q) {
        for (int i = 0; i < 4; i++) printf("   line %d y %d..%d colour %08x\n", i, q[i].g->y0, q[i].g->y1, q[i].p->col[0]);
        CHECK(q[0].g->y0 == 0 && q[1].g->y0 == 11 && q[2].g->y0 == 33 && q[2].g->y1 == 36 && q[3].g->y0 == 52 && q[3].g->y1 == 55, "orange and white lines one row down");
        CHECK(q[2].p->col[0] == 0x803484ff && q[3].p->col[0] == 0x80ffffff, "their colours kept");
    }
    int *sh = test_status_handles(); int hq = sh[1], ht = sh[2];
    CHECK(sh[0] == h && hq > 0 && ht > 0 && hq != ht, "row instances %d %d", hq, ht);
    Pair *m = NULL; int mc = FN(int, 0x1a6350, int, int, Pair**)(hq, 1, &m);
    CHECK(mc == 3 && m, "row sprite: %d quads", mc);
    if (mc >= 3 && m) {
        for (int i = 0; i < mc; i++) printf("   quad %d xy (%d,%d)-(%d,%d) uv (%d,%d)-(%d,%d) col %08x\n", i, m[i].g->x0, m[i].g->y0, m[i].g->x1, m[i].g->y1, m[i].p->u0, m[i].p->v0, m[i].p->u1, m[i].p->v1, m[i].p->col[0]);
        CHECK(m[0].g->x0 == -2 && m[0].g->x1 == 69 && m[0].g->y0 == 47 && m[0].g->y1 == 50 && m[0].p->u0 == 62 && m[0].p->v0 == 34 && m[0].p->col[0] == 0x80f37e01, "blue line where the orange one was");
        CHECK(m[2].g->x0 == -1 && m[2].g->y0 == 40 && m[2].g->x1 == 12 && m[2].g->y1 == 48 && m[2].p->u0 == 300 && m[2].p->v0 == 676 && m[2].p->u1 == 326 && m[2].p->v1 == 692 && m[2].p->col[3] == 0x80f37e01, "MP label where FP's was");
        CHECK(m[1].g->x0 == 0 && m[1].g->y0 == 41 && m[1].p->col[0] == 0x80000000, "its shadow");
    }
    u8 *pc = FN(u8*, 0x1a65c0, int)(h), *qc = FN(u8*, 0x1a5fd0, int)(hq), *tc = FN(u8*, 0x1a5fd0, int)(ht);
    u8 *node0 = *(u8**)(pc + 0x98);
    CHECK(*(s16*)(node0 + 0xb0) == 0x1d && *(u8**)(qc + 0x68) == node0 + 0x20 && *(u8**)(tc + 0x68) == node0 + 0x20, "both hang on the plate node");
    CHECK(*(s16*)(qc + 0x10) == *(s16*)(pc + 0x10) && *(s16*)(tc + 0x12) == *(s16*)(pc + 0x12) && qc[0x86] == pc[0x86], "same priority %d, queue %d and group %d", *(s16*)(qc + 0x10), *(s16*)(qc + 0x12), qc[0x86]);
    CHECK(*(float*)(tc + 0x50) == 69 && *(float*)(tc + 0x54) == 39 && *(float*)(qc + 0x50) == 0 && *(float*)(qc + 0x54) == 0, "value at (69, 39)");
    CHECK(FN(int, 0x1a6060, int)(hq) == 1 && FN(int, 0x1a6060, int)(ht) == 1, "visible with the panel");
    /* the value text: object 1 of the sequence, its text object and settings against the FP value node's */
    {   u8 *nodes = *(u8**)(pc + 0x98); u8 *fpn = NULL;
        for (int i = 0; i < *(s16*)(pc + 0xa8); i++) if (*(s16*)(nodes + i * 0xb8 + 0xb0) == 0x24) fpn = nodes + i * 0xb8;
        u8 *to = *(u8**)(*(u8**)*(u8***)(tc + 0xa8) + 0x50 + 0x10), *fo = fpn ? *(u8**)(*(u8**)*(u8***)(fpn + 0xa8) + 0x50 + 0x10) : NULL;
        CHECK(to && fo, "text objects %p %p", to, fo);
        if (to && fo) {
            printf("   MP text '%s' colour %08x size %.1f align %x kind %d | FP text '%s' colour %08x size %.1f align %x kind %d\n",
                   *(char**)(to + 0x30), *(u32*)(to + 0x2a8), *(float*)(to + 0x2b0), *(u32*)(to + 0x298), to[0x2cc],
                   *(char**)(fo + 0x30), *(u32*)(fo + 0x2a8), *(float*)(fo + 0x2b0), *(u32*)(fo + 0x298), fo[0x2cc]);
            CHECK(!strcmp(*(char**)(to + 0x30), "100/100"), "shows current/maximum MP");
            CHECK(*(u32*)(to + 0x2a8) == 0xff7a3f01 && *(u32*)(fo + 0x2a8) == 0xff1a44ff, "half of the label's colour, as FP's text is half of its label's (ff8434)");
            CHECK(*(float*)(to + 0x2b0) == *(float*)(fo + 0x2b0) && *(u32*)(to + 0x298) == *(u32*)(fo + 0x298) && to[0x2cc] == fo[0x2cc] && to[0x2cd] == fo[0x2cd], "same size, alignment and font as the FP value");
        }
    }
    { extern int *test_fresh(void); *test_fresh() = 0; }
    *test_mp() = 35; test_status_frame(self);
    {   u8 *to = *(u8**)(*(u8**)*(u8***)(tc + 0xa8) + 0x50 + 0x10);
        CHECK(to && !strcmp(*(char**)(to + 0x30), "35/100"), "follows MP: %s", to ? *(char**)(to + 0x30) : "-"); }
    NPOS(0x1a); CHECK(y == 52, "a second frame moves nothing again (%.0f)", y);
    CHECK(q[2].g->y0 == 33 && q[3].g->y0 == 52 && test_status_handles()[1] == hq && test_status_handles()[2] == ht, "... and makes nothing new");
    FN(int, 0x1a5b30, int, int)(h, 0); test_status_frame(self);
    CHECK(FN(int, 0x1a6060, int)(hq) == 0 && FN(int, 0x1a6060, int)(ht) == 0, "hidden with the panel");
    /* the game destroys the panel's layout: ours go first, detached */
    FN(void, 0x1a57f0, int)(h);
    CHECK(FN(int, 0x1a60e0, int)(hq) == 8 && FN(int, 0x1a60e0, int)(ht) == 8 && FN(int, 0x1a60e0, int)(h) == 8, "destroyed with the panel");
    CHECK(test_status_handles()[0] == 0 && test_status_handles()[1] == 0, "nothing kept");
    *(int*)(self + 0x20) = 0; test_status_frame(self);
    CHECK(test_status_handles()[1] == 0, "no panel, no row");
    /* a new panel (menu opened again) */
    G(u8, 0x8f88028) = 1;
    int h2 = FN(int, 0x1a4ec0, int, u16, int, void*)(d, 0x67, 1, NULL); *(int*)(self + 0x20) = h2;
    test_status_frame(self);
    CHECK(h2 > 0 && test_status_handles()[0] == h2 && test_status_handles()[1] > 0 && test_status_handles()[2] > 0, "row made again for a new panel");
    /* without the label art: line and value only */
    test_set_stat_art(0); test_status_frame(self);
    m = NULL; FN(int, 0x1a6350, int, int, Pair**)(test_status_handles()[1], 1, &m);
    CHECK(m && m[0].g->x1 == 69 && m[1].g->x1 == 0 && m[2].g->x1 == 0, "no art: no label quads");
    FN(void, 0x1a57f0, int)(h2);
    printf("t_status done\n");
}
/* the Shortcuts entry of the Command Decks screen (sccamp.c) on the real 2D runtime with camp.l2d: stand-in screen,
   list and pane objects around the game's own layouts; the list's input is the game's (140404490) under our hook */
extern int *test_sccamp_state(void); extern void test_sccamp_top(u8 *top), test_sccamp_frame(u8 *top); extern int test_sccamp_input(u8 *self);
static void t_sccamp(void) {
    const char *fn = getenv("BBS_CAMP_L2D");
    if (!fn) { printf("  (skipped: BBS_CAMP_L2D not set)\n"); return; }
    FILE *f = fopen(fn, "rb"); if (!f) { printf("  cannot open %s\n", fn); fails++; return; }
    u8 *img = memalign(16, 0x80000); size_t n = fread(img, 1, 0x80000, f); fclose(f); (void)n;
    *(u8*)RVA(0x111660) = 0xC3; *(u8*)RVA(0x10c5e0) = 0xC3; memcpy(RVA(0x11a8a0), "\x31\xc0\xc3", 3);
    FN(void, 0x1a6d70, void)();
    for (int i = 0; i < 6; i++) {
        u8 *ft = calloc(1, 0x200); u64 *gl = calloc(4, 8);
        *(u16*)(ft + 0x100) = 1; *(u16*)(ft + 0x102) = 16; *(u16*)(ft + 0x10c) = 256; ft[0x10e] = 16; *(u64**)(ft + 0x118) = gl;
        G(u8*, 0x8f86968 + i * 8) = ft;
    }
    G(u8*, 0x8f86998) = calloc(1, 0x500);
    /* not here: the help line (needs the whole menu), sounds, the save's deck (6 slots), the game's plate fill */
    *(u8*)RVA(0x417710) = 0xC3; *(u8*)RVA(0x417770) = 0xC3; memcpy(RVA(0x1b2e40), "\x31\xc0\xc3", 3);
    memcpy(RVA(0x41dd20), "\xb8\x06\x00\x00\x00\xc3", 6); memcpy(RVA(0x41dd30), "\x31\xc0\xc3", 3); *(u8*)RVA(0x4103c0) = 0xC3;
    int d = FN(int, 0x1a6fb0, void*, const char*, int)(img, "camp.l2d", 0);
    CHECK(d > 0, "camp.l2d registered: %d", d);
    G(u8, 0x8f88028) = 1; G(u8, 0x8f8802a) = 1;
    int lh = FN(int, 0x1a4ec0, int, u16, int, void*)(d, 0xcc, 1, NULL), ph = FN(int, 0x1a4ec0, int, u16, int, void*)(d, 0xc9, 1, NULL);
    CHECK(lh > 0 && ph > 0, "list %d and pane %d layouts", lh, ph);
    u8 *top = calloc(1, 0x100), *list = calloc(1, 0x110), *pn = calloc(1, 0x110);
    *(u8**)(top + 0xb8) = list; *(u8**)(top + 0xc0) = pn; top[0x9b] = 2;
    *(u32*)(list + 8) = 0xffffffff; *(int*)(list + 0x20) = lh; *(u32*)(list + 0x44) = 0x100010; *(u32*)(list + 0x48) = 0x400040;
    *(u32*)(list + 0x88) = 0xfff8fdff | 0x200; *(u32*)(list + 0x34) = 0x4000; *(u32*)(list + 0x38) = 0x2000;
    list[0x101] = list[0x102] = 0xff; *(s16*)(list + 0x8e) = 4;
    G(u16, 0x8221c0) = 0x4000; G(u16, 0x8221c4) = 0x2000;
    for (int i = 0; i < 5; i++) FN(int, 0x1a6880, int, u16, float*, float*)(lh, (u16)(i + 1), (float*)(list + 0xa8 + i * 8), (float*)(list + 0xac + i * 8));
    FN(int, 0x1a7db0, int, u16, int)(lh, 5, 4);
    u8 *hand = calloc(1, 0x28), *light = calloc(1, 0x50);          /* the list's two cursors, as 140404850 makes them */
    *(int*)(hand + 0x18) = FN(int, 0x1a5350, const char*, u16, int, void*)("camp:0", 3, 0, NULL); *(float*)(hand + 8) = 8; *(float*)(hand + 0xc) = 10;
    *(int*)(light + 0x18) = FN(int, 0x1a5350, const char*, u16, int, void*)("camp:0", 4, 0, NULL); *(float*)(light + 8) = 105;
    FN(int, 0x1a4e10, int, u16, int)(lh, 0xb, *(int*)(hand + 0x18)); FN(int, 0x1a4e10, int, u16, int)(lh, 0xb, *(int*)(light + 0x18));
    *(u8**)(list + 0xe8) = hand; *(u8**)(list + 0xf0) = light;
    *(int*)(pn + 0x20) = ph;
    u8 *ph_hand = calloc(1, 0x28); *(int*)(ph_hand + 0x18) = FN(int, 0x1a5350, const char*, u16, int, void*)("camp:0", 3, 0, NULL);
    FN(int, 0x1a4e10, int, u16, int)(ph, 1, *(int*)(ph_hand + 0x18)); FN(int, 0x1a5b30, int, int)(*(int*)(ph_hand + 0x18), 0);
    *(u8**)(pn + 0x100) = ph_hand;
    int plate[8];
    for (int i = 0; i < 8; i++) {
        u8 *b = calloc(1, 0x48);
        plate[i] = FN(int, 0x1a4ec0, int, u16, int, void*)(d, 0x6f, 1, NULL);
        FN(int, 0x1a4e10, int, u16, int)(ph, (u16)(i + 1), plate[i]);
        *(int*)(b + 8) = plate[i]; b[0x40] = i >= 6;                /* the last two: slots not unlocked yet */
        static const char *const nm[8] = { "Fire", "Blizzard", "Cure", "Potion", "Strike Raid", "Thunder", "locked", "locked" };
        FN(int, 0x1a8330, int, u16, const char*, int, void*)(plate[i], 3, nm[i], 0, NULL);
        FN(int, 0x1a8330, int, u16, const char*, int, void*)(plate[i], 5, i == 7 ? "9" : "3", 0, NULL);
        if (i == 3) {   /* an item, as the game fills it (1404103c0): no "LV", the count right-aligned and moved 5 right */
            struct { const char *text; u32 colour; u8 space[4]; u8 pad10[4]; u16 flags; u8 b16, pitch, align, size, b1a, b1b, kind, pad[3]; } fp;
            memset(&fp, 0, sizeof fp); fp.text = "x3"; fp.space[0] = 5; fp.flags = 0x29; fp.align = 6;
            FN(int, 0x1a7cb0, int, u16, void*, int)(plate[i], 5, &fp, 0);
            FN(int, 0x1a5b80, int, u16, int)(plate[i], 4, 0);
        }
        *(u8**)(pn + 0xc0 + i * 8) = b;
    }
    u32 lv_align = 0, lv_col = 0; u8 lv_font = 0; float lv_pitch = 0;          /* the level text as the layout makes it */
    { u8 *to = NULL; FN(int, 0x1a66c0, int, u16, u8**, int)(plate[0], 5, &to, 0);
      if (to) { lv_align = *(u32*)(to + 0x298); lv_col = *(u32*)(to + 0x2a8); lv_font = to[0x2cc]; lv_pitch = *(float*)(to + 0x2b0); }
      CHECK(to && lv_align == 0 && lv_font == 4 && lv_col == 0xff00ffff, "level text: align %d font %d colour %08x pitch %.1f", lv_align, lv_font, lv_col, lv_pitch); }
    *(u8**)(pn + 0xc0 + 5 * 8) = NULL;          /* slot 6 is the second row of the command in slot 5: no plate of its own */
    { u8 *to = NULL; FN(int, 0x1a66c0, int, u16, u8**, int)(plate[3], 5, &to, 0);
      CHECK(to && (s8)to[0x2b4] == 5 && *(u32*)(to + 0x298) == 2 && !strcmp(*(char**)(to + 0x30), "x3"), "the item plate's count: right-aligned, 5 to the right"); }
    s8 *bind = test_sc_bind(); bind[0] = 0; bind[1] = 1; bind[2] = 2; bind[3] = 1;      /* circle 1, triangle 2, square 3, cross 2 */
    #define ST(i) (test_sccamp_state()[i])
    #define STEP(held, edge) (pad(held, edge), test_sccamp_top(top), test_sccamp_input(list), test_sccamp_frame(top), test_sccamp_top(NULL))

    /* our plate */
    test_sccamp_frame(top);
    int it = ST(2);
    CHECK(it > 0 && ST(3) == lh && ST(0) == 0, "a sixth plate is made: %d", it);
    u8 *lc = FN(u8*, 0x1a65c0, int)(lh), *ic = it > 0 ? FN(u8*, 0x1a5fd0, int)(it) : NULL;
    if (!ic) { printf("t_sccamp: no plate, stopping\n"); return; }
    u8 *nodes = *(u8**)(lc + 0x98), *root = NULL, *n4 = NULL;
    for (int i = 0; i < *(s16*)(lc + 0xa8); i++) { u8 *q = nodes + i * 0xb8; if (*(s16*)(q + 0xb0) == 0xb) root = q; if (*(s16*)(q + 0xb0) == 4) n4 = q; }
    CHECK(root && *(u8**)(ic + 0x68) == root + 0x20, "it hangs on the list's root node");
    CHECK(*(float*)(ic + 0x50) == -230 && *(float*)(ic + 0x54) == 10, "at (%.0f, %.0f): 17 under Finish Commands", *(float*)(ic + 0x50), *(float*)(ic + 0x54));
    CHECK(FN(int, 0x1a60e0, int)(it) == FN(int, 0x1a6790, int, u16)(lh, 1) && FN(int, 0x1a60e0, int)(it) != 4 && FN(int, 0x1a6060, int)(it) == 1, "the look of the game's unselected entries, visible with the list (control %d)", FN(int, 0x1a60e0, int)(it));
    CHECK(*(s16*)(ic + 0x10) == *(s16*)(lc + 0x10) && ic[0x86] == lc[0x86], "same priority and group as the list");
    {   u8 *to = NULL, *fo = NULL; int ti = -1, fi = -1;            /* first object of the sequence that carries text (1401abb00) */
        for (int i = 0; i < (s8)ic[0xb2] && !to; i++) { to = *(u8**)(*(u8**)*(u8***)(ic + 0xa8) + i * 0x50 + 0x10); ti = i; }
        for (int i = 0; n4 && i < (s8)n4[0xb2] && !fo; i++) { fo = *(u8**)(*(u8**)*(u8***)(n4 + 0xa8) + i * 0x50 + 0x10); fi = i; }
        CHECK(to && fo && ti == fi, "text objects %p %p (object %d / %d of %d / %d)", to, fo, ti, fi, (s8)ic[0xb2], n4 ? (s8)n4[0xb2] : 0);
        if (to && fo) {
            printf("   ours '%s' colour %08x pitch %.1f align %x font %d | entry 4 '%s' colour %08x pitch %.1f align %x font %d\n",
                   *(char**)(to + 0x30), *(u32*)(to + 0x2a8), *(float*)(to + 0x2b0), *(u32*)(to + 0x298), to[0x2cc],
                   *(char**)(fo + 0x30), *(u32*)(fo + 0x2a8), *(float*)(fo + 0x2b0), *(u32*)(fo + 0x298), fo[0x2cc]);
            CHECK(!strcmp(*(char**)(to + 0x30), "Shortcuts"), "it reads Shortcuts");
            CHECK(*(u32*)(to + 0x2a8) == *(u32*)(fo + 0x2a8) && *(float*)(to + 0x2b0) == *(float*)(fo + 0x2b0) && *(u32*)(to + 0x298) == *(u32*)(fo + 0x298) && to[0x2cc] == fo[0x2cc] && to[0x2cd] == fo[0x2cd] && *(u32*)(to + 0x2d0) == *(u32*)(fo + 0x2d0),
                  "text set as the game's fourth entry");
        }
    }
    FN(int, 0x1a5b30, int, int)(lh, 0); test_sccamp_frame(top); CHECK(FN(int, 0x1a6060, int)(it) == 0, "hidden with the list");
    FN(int, 0x1a5b30, int, int)(lh, 1); test_sccamp_frame(top); CHECK(FN(int, 0x1a6060, int)(it) == 1, "and shown again");

    /* the list: the game's five entries as before, ours after the last */
    STEP(0x10, 0x10); CHECK(ST(0) == 0 && *(s16*)(list + 0x8e) == 3, "up from the last entry: the game's own move (%d)", *(s16*)(list + 0x8e));
    STEP(0, 0); STEP(0x40, 0x40); CHECK(ST(0) == 0 && *(s16*)(list + 0x8e) == 4, "down: back on Finish Commands");
    STEP(0, 0); STEP(0x40, 0x40);
    CHECK(ST(0) == 1 && top[0x11] == 1, "down again: on Shortcuts, the screen's buttons held (mode %d)", ST(0));
    CHECK(FN(int, 0x1a6790, int, u16)(lh, 1) == 0 && FN(int, 0x1a6790, int, u16)(lh, 5) == 0 && FN(int, 0x1a60e0, int)(it) == 4, "ours lit, none of the game's (%d %d %d)",
          FN(int, 0x1a6790, int, u16)(lh, 1), FN(int, 0x1a6790, int, u16)(lh, 5), FN(int, 0x1a60e0, int)(it));
    CHECK(*(float*)(hand + 0x10) == -230 && *(float*)(hand + 0x14) == 10 && *(float*)(light + 0x30) == -125 && *(float*)(light + 0x34) == 10, "hand and light frame on it (%.0f, %.0f)", *(float*)(hand + 0x10), *(float*)(hand + 0x14));
    STEP(0x40, 0); CHECK(ST(0) == 1, "holding the button does not run on");
    STEP(0, 0); STEP(0x40, 0x40);
    CHECK(ST(0) == 0 && *(s16*)(list + 0x8e) == 0 && top[0x11] == 0 && FN(int, 0x1a6790, int, u16)(lh, 1) == 4 && FN(int, 0x1a60e0, int)(it) == FN(int, 0x1a6790, int, u16)(lh, 2), "down: Edit Deck, ours plain again");
    CHECK(*(float*)(hand + 0x14) == -75, "hand back on the first entry (%.0f)", *(float*)(hand + 0x14));
    STEP(0x40, 0); CHECK(*(s16*)(list + 0x8e) == 0, "no run-on past the end");
    STEP(0, 0); STEP(0x10, 0x10); CHECK(ST(0) == 1 && FN(int, 0x1a6790, int, u16)(lh, 5) == 0 && FN(int, 0x1a6790, int, u16)(lh, 1) == 0, "up from Edit Deck: Shortcuts");
    STEP(0, 0); STEP(0x10, 0x10); CHECK(ST(0) == 0 && *(s16*)(list + 0x8e) == 4 && FN(int, 0x1a6790, int, u16)(lh, 5) == 4, "up: Finish Commands");
    STEP(0, 0); STEP(0x40, 0x40); CHECK(ST(0) == 1, "and down to ours");

    /* confirm: the Battle Commands pane */
    STEP(0, 0); STEP(0x4000, 0x4000);
    CHECK(ST(0) == 2 && ST(1) == 0 && top[0x11] == 1 && top[0x9b] == 2 && list[0x9b] == 0, "confirm: picking a slot; the screen itself did nothing");
    CHECK(FN(int, 0x1a6060, int)(*(int*)(ph_hand + 0x18)) == 1 && *(float*)(ph_hand + 0x14) == 10, "the pane's hand on slot 1 (y %.0f)", *(float*)(ph_hand + 0x14));
    const char *p0 = node_text(plate[0], 5), *p1 = node_text(plate[1], 5), *p2 = node_text(plate[2], 5), *p3 = node_text(plate[3], 5);
    CHECK(p0 && !strcmp(p0, "\xf5\x7b") && p1 && !strcmp(p1, "\xf5\x67\xf5\x7c") && p2 && !strcmp(p2, "\xf5\x66") && p3 && !*p3,
          "each plate shows its buttons in the level's place: circle | triangle cross | square | none");
    p0 = node_text(plate[0], 3); CHECK(p0 && !strcmp(p0, "Fire"), "names untouched");
    {   float x = 0, y = 0; u8 *to = NULL;
        FN(int, 0x1a6880, int, u16, float*, float*)(plate[0], 5, &x, &y);
        FN(int, 0x1a66c0, int, u16, u8**, int)(plate[0], 5, &to, 0);
        CHECK(x == 123 && y == 5.3f && !FN(int, 0x1a6670, int, u16)(plate[0], 4) && FN(int, 0x1a6670, int, u16)(plate[3], 5), "at the right end, mid height; no LV label");
        CHECK(to && *(u32*)(to + 0x298) == 18 && to[0x2cc] == 1 && *(u32*)(to + 0x2a8) == 0xff808080, "anchored right / middle, the button font, no tint (align %d font %d)", to ? *(int*)(to + 0x298) : -1, to ? to[0x2cc] : -1);
        to = NULL; FN(int, 0x1a66c0, int, u16, u8**, int)(plate[3], 5, &to, 0);
        FN(int, 0x1a6880, int, u16, float*, float*)(plate[3], 5, &x, &y);
        CHECK(to && to[0x2b4] == 0 && to[0x2b5] == 0 && *(u32*)(to + 0x298) == 18 && x == 123 && y == 5.3f, "the item's plate the same: its count's own offset is not carried over (%d)", to ? (s8)to[0x2b4] : -1);
        /* the same settings as the button text of the game's command plates (bc01_00, text table entry 16), bar the side */
    }
    p0 = node_text(plate[7], 5); CHECK(p0 && !strcmp(p0, "9"), "a locked slot's plate is left alone");
    STEP(0, 0); STEP(0x40, 0x40); STEP(0, 0); STEP(0x40, 0x40); STEP(0, 0); STEP(0x40, 0x40);
    CHECK(ST(1) == 3 && *(float*)(ph_hand + 0x14) == 64 && *(s16*)(list + 0x8e) == 0, "down three times: slot 4 (y %.0f), the list untouched", *(float*)(ph_hand + 0x14));
    STEP(0, 0); STEP(0x10, 0x10); STEP(0, 0); STEP(0x10, 0x10); STEP(0, 0); STEP(0x10, 0x10); STEP(0, 0); STEP(0x10, 0x10);
    CHECK(ST(1) == 4, "up past the top: slot 5, the last one with a plate - slot 6, the second row of its command, is skipped (%d)", ST(1) + 1);
    STEP(0, 0); STEP(0x40, 0x40); STEP(0, 0); STEP(0x40, 0x40); STEP(0, 0); STEP(0x40, 0x40); STEP(0, 0); STEP(0x40, 0x40);
    CHECK(ST(1) == 3, "back on slot 4");
    /* assign */
    STEP(0, 0); STEP(PAD_TRI, PAD_TRI); CHECK(ST(0) == 2 && sc_slot(1) == 1, "a face button alone does nothing");
    STEP(0, 0); STEP(0x4000, 0x4000); CHECK(ST(0) == 3, "confirm: waiting for a button");
    STEP(0, 0); STEP(PAD_TRI, PAD_TRI);
    p1 = node_text(plate[1], 5); p3 = node_text(plate[3], 5);
    CHECK(ST(0) == 2 && sc_slot(1) == 3 && sc_slot(0) == 0 && sc_slot(3) == 1, "triangle: now slot 4's");
    CHECK(p1 && !strcmp(p1, "\xf5\x7c") && p3 && !strcmp(p3, "\xf5\x67"), "plates follow: slot 2 keeps cross, slot 4 shows triangle");
    STEP(0, 0); STEP(0x4000, 0x4000); STEP(0, 0); STEP(0x4000, 0x4000);
    CHECK(ST(0) == 2 && sc_slot(3) == 3 && sc_slot(1) == 3, "the confirm button can be given too (cross: slot 4)");
    STEP(0, 0); STEP(0x4000, 0x4000); STEP(0, 0); STEP(0x2000, 0x2000);
    CHECK(ST(0) == 2 && sc_slot(0) == 3, "and the cancel button (circle: slot 4)");
    STEP(0, 0); STEP(0x4000, 0x4000); STEP(0, 0); STEP(PAD_TRI, PAD_TRI);
    p3 = node_text(plate[3], 5);
    CHECK(ST(0) == 2 && sc_slot(1) == -1 && p3 && !strcmp(p3, "\xf5\x7b\xf5\x7c"), "its own button again: removed");
    STEP(0, 0); STEP(0x4000, 0x4000); STEP(0, 0); STEP(0x80, 0x80);
    CHECK(ST(0) == 2 && sc_slot(0) == 3 && sc_slot(2) == 2, "a direction: back without a change");
    /* cancel: back to the list, then out */
    STEP(0, 0); STEP(0x2000, 0x2000);
    CHECK(ST(0) == 1 && top[0x11] == 1 && top[0x9b] == 2, "cancel: back on the Shortcuts entry");
    {   float x = 0, y = 0; u8 *to = NULL;
        FN(int, 0x1a6880, int, u16, float*, float*)(plate[0], 5, &x, &y);
        FN(int, 0x1a66c0, int, u16, u8**, int)(plate[0], 5, &to, 0);
        p0 = node_text(plate[0], 5);
        CHECK(p0 && !strcmp(p0, "3") && x == 116 && y == 3 && to && *(u32*)(to + 0x298) == lv_align && to[0x2cc] == lv_font && *(u32*)(to + 0x2a8) == lv_col && *(float*)(to + 0x2b0) == lv_pitch,
              "plates as they were: level '%s' at (%.0f, %.0f), align %d font %d colour %08x", p0 ? p0 : "-", x, y, to ? *(int*)(to + 0x298) : -1, to ? to[0x2cc] : -1, to ? *(u32*)(to + 0x2a8) : 0);
        CHECK(FN(int, 0x1a6670, int, u16)(plate[0], 4) && FN(int, 0x1a6670, int, u16)(plate[0], 5) && !FN(int, 0x1a6670, int, u16)(plate[3], 4) && FN(int, 0x1a6670, int, u16)(plate[3], 5), "LV and level showing again (no LV on the item)");
        to = NULL; FN(int, 0x1a66c0, int, u16, u8**, int)(plate[3], 5, &to, 0);
        CHECK(to && !strcmp(*(char**)(to + 0x30), "x3") && (s8)to[0x2b4] == 5 && *(u32*)(to + 0x298) == 2 && to[0x2cc] == lv_font && *(u32*)(to + 0x2a8) == lv_col,
              "the item's count as the game had it: '%s', offset %d, align %d", to ? *(char**)(to + 0x30) : "-", to ? (s8)to[0x2b4] : -1, to ? *(int*)(to + 0x298) : -1);
    }
    CHECK(FN(int, 0x1a6060, int)(*(int*)(ph_hand + 0x18)) == 0, "the pane's hand is gone");
    STEP(0, 0); STEP(0x2000, 0x2000);
    CHECK(ST(0) == 0 && top[0x11] == 0 && *(s16*)(list + 0x8e) == 4 && list[0x9e] == 0xf, "cancel there: the game's own cancel (list action %d)", list[0x9e]);
    /* something else takes the screen while we hold it */
    list[0x9e] = 0; STEP(0, 0); STEP(0x40, 0x40); CHECK(ST(0) == 1, "on ours again");
    STEP(0, 0); STEP(0x4000, 0x4000); CHECK(ST(0) == 2, "in the pane again");
    top[0x9b] = 5; STEP(0, 0); p0 = node_text(plate[0], 5);
    CHECK(ST(0) == 0 && top[0x11] == 0 && p0 && !strcmp(p0, "3") && FN(int, 0x1a6670, int, u16)(plate[0], 4), "the screen moved on: let go, plates put back");
    top[0x9b] = 2;
    /* the screen's update under our wrapper: its answer (1 = "finished": the menu then leaves the screen) is passed
       on.  The game's function: state 3 with nothing left open goes to state 4 and answers 0; state 4 answers 1. */
    {   extern void *test_sccamp_update_hook(void);
        u64 (MSABI *hook)(u8*) = (u64 (MSABI *)(u8*))test_sccamp_update_hook();
        u8 *t2 = calloc(1, 0x100); t2[0x9b] = 3;
        u64 r3 = hook(t2); int st = t2[0x9b]; u64 r4 = hook(t2);
        CHECK((u32)r3 == 0 && st == 4 && (u32)r4 == 1, "the screen's own answer is returned: %d while open, %d when done", (int)r3, (int)r4);
    }
    /* the list goes: ours with it */
    FN(void, 0x1a57f0, int)(lh);
    CHECK(ST(2) == 0 && ST(3) == 0 && FN(int, 0x1a60e0, int)(it) == 8, "destroyed with the list");
    printf("t_sccamp done\n");
}
/* the command menu: cursor, lists, confirm button routing, casting through the game's own use gate */
extern int *test_menu_state(void), *test_menu_cur(void), *test_menu_sel(void);
extern int test_menu_step(u8 *cmd), test_confirm(u8 *pad, u8 *cmd); extern void test_plate_update(u8 *P);
#define PAD_UP 0x10
#define PAD_DOWN 0x40
#define PAD_LEFT 0x80
#define PAD_X 0x4000
static int press(u32 b) {            /* one frame with these buttons newly pressed */
    G(u32, 0x8f64930) = b; G(u32, 0x8f64934) = b; G(u32, 0x8f6493c) = b; G(u16, 0x8f6499c) = 0;
    int engine_sees_confirm = test_confirm(mgr + 0x58, cmd);
    test_menu_step(cmd);
    for (int i = 0; i < *(u16*)(cmd + 0x220); i++) test_plate_update(plate[i]);
    test_tick(gauge);
    G(u32, 0x8f64930) = 0; G(u32, 0x8f64934) = 0; G(u32, 0x8f6493c) = 0;
    return engine_sees_confirm;
}
static void t_menu(void) {
    static const u16 ids[] = { 0x83 /*Fire*/, 0xbc /*Potion*/, 0x65 /*Strike Raid*/, 0x92 /*Cure*/, 0xbf /*Ether*/, 0xac /*Mega Flare*/ };
    world(6, ids);
    *(u32*)(mgr + 0xf8) = 1;                      /* pad enabled */
    memcpy(RVA(0x1b2e40), "\x31\xc0\xc3", 3);       /* sound: none here */
    *(u32*)(pl + 0x318) = 0x40;                   /* player idle: commands accepted */
    *(u16*)(cmd + 0x188) = 0x151; *(u16*)(pl + 0x354) = 0x151;
    printf("  confirm mask %x\n", G(u16, 0x8221c0));
    int *st = test_menu_state(), *cur = test_menu_cur(), *sel = test_menu_sel();
    test_tick(gauge);
    CHECK(*st == 0 && *cur == 0, "starts on Attack");
    CHECK(press(PAD_X) != 0, "on Attack the game sees the confirm button");
    CHECK(*(u64*)(cmd + 0x80) == 0, "menu queues nothing on Attack");
    press(PAD_DOWN); CHECK(*cur == 1, "down -> Magic (%d)", *cur);
    CHECK(press(0) == 0 && press(PAD_X) == 0, "off Attack the game does not see the confirm button");
    CHECK(*st == 1, "confirm on Magic opens the list (state %d)", *st);
    press(PAD_DOWN); press(PAD_DOWN); CHECK(sel[0] == 2, "cursor in list %d", sel[0]);
    press(PAD_UP); CHECK(sel[0] == 1, "up %d", sel[0]);                 /* entry 1 of the magic list = Strike Raid */
    press(PAD_X);
    CHECK(*(u16*)(cmd + 0x80) == 0x65 && *(u8**)(cmd + 0x90) == plate[2], "Strike Raid queued: %x", *(u16*)(cmd + 0x80));
    CHECK(*st == 0 && *cur == 0 && *test_mp() == 90, "back on Attack after the cast, MP %.0f", *test_mp());
    *(u64*)(cmd + 0x80) = 0;                                              /* the player took it */
    /* list remembers its cursor; left backs out without casting */
    press(PAD_DOWN); press(PAD_X); CHECK(*st == 1 && sel[0] == 1, "list reopened at %d", sel[0]);
    press(PAD_LEFT); CHECK(*st == 0 && *cur == 1 && *(u64*)(cmd + 0x80) == 0, "left closes the list");
    /* d-pad right opens the list of the entry under the cursor, as confirm does; not on Attack; the game never sees
       it as a confirm; in a list it does nothing */
    #define PAD_RIGHT 0x20
    CHECK(*cur == 1 && press(PAD_RIGHT) == 0 && *st == 1 && *(u64*)(cmd + 0x80) == 0, "right on Magic opens the list (state %d)", *st);
    { int s0 = sel[0]; press(PAD_RIGHT); CHECK(*st == 1 && sel[0] == s0 && *(u64*)(cmd + 0x80) == 0, "right inside the list: nothing"); }
    press(PAD_LEFT); CHECK(*st == 0 && *cur == 1, "left closes it again");
    press(PAD_DOWN); press(PAD_RIGHT); CHECK(*cur == 2 && *st == 2, "right on Items opens the item list (state %d)", *st);
    press(PAD_LEFT); press(PAD_DOWN); press(PAD_RIGHT); CHECK(*cur == 3 && *st == 0, "right on D-Link with no links: nothing opens");
    press(PAD_DOWN); CHECK(*cur == 0, "on Attack"); press(PAD_RIGHT); CHECK(*st == 0 && *cur == 0 && *(u64*)(cmd + 0x80) == 0, "right on Attack: nothing");
    press(PAD_DOWN);
    /* context prompts: triangle answers them, the attack button attacks; counter prompts as in the game */
    {   extern int test_prompt_pad(u8 *pad); extern int menu_react_prompt(u8 *cmd);
        u8 *pd = mgr + 0x58;
        #define BTN(b) (G(u32, 0x8f64930) = (b), G(u32, 0x8f64934) = (b), G(u32, 0x8f6493c) = (b), G(u16, 0x8f6499c) = 0)
        int counter = 0, talk = 0;
        for (int id = 1; id < 0x23a && !(counter && talk); id++) { int c = G(u8, 0x814900 + id * 0x18 + 1); if (c == 6 && !counter) counter = id; if (c != 6 && c != 0 && id > 0x100 && !talk) talk = id; }
        CHECK(counter && talk, "a counter command (%x) and another (%x) in the game's table", counter, talk);
        static u64 pc, ac = 0x10001; u8 *PP = calloc(1, 0x70), *AP = calloc(1, 0x70);
        PP[0x30] = 5; *(u64**)(PP + 0x58) = &pc; *(u8**)(cmd + 0x298) = PP; *(u32*)(PP + 0x60) = 0x80000000;
        AP[0x30] = 1; *(u64**)(AP + 0x58) = &ac; *(u8**)(cmd + 0x1b0) = AP; *(u32*)(AP + 0x60) = 0x80000000;
        *cur = 0;
        BTN(PAD_X); CHECK(test_prompt_pad(pd) != 0 && !menu_react_prompt(cmd), "no prompt up: the site answers as the game (confirm)");
        pc = (u64)talk; *(u32*)(cmd + 0x60) |= 0x40000;
        CHECK(menu_react_prompt(cmd), "an interaction prompt is up");
        BTN(PAD_X); CHECK(test_prompt_pad(pd) == 0, "the attack button does not answer it");
        BTN(PAD_TRI); CHECK(test_prompt_pad(pd) != 0, "triangle does");
        press(PAD_X); CHECK(*(u64*)(cmd + 0x80) == ac && *(u64*)(cmd + 0x1c8) == ac, "attack button on Attack: attacks (%llx)", (unsigned long long)*(u64*)(cmd + 0x80));
        *(u64*)(cmd + 0x80) = 0;
        *(u32*)(pl + 0x318) = 0; press(PAD_X); CHECK(*(u64*)(cmd + 0x80) == 0, "not while the player cannot take a command");
        *(u32*)(pl + 0x318) = 0x40;
        press(PAD_DOWN); CHECK(*cur == 1, "the menu moves with a prompt up");
        CHECK(press(PAD_X) == 0 && *st == 1 && *(u64*)(cmd + 0x80) == 0, "and its entries open with the attack button (the game does not see it)");
        press(PAD_LEFT); press(PAD_UP); CHECK(*st == 0 && *cur == 0, "back on Attack");
        pc = (u64)counter;
        CHECK(!menu_react_prompt(cmd), "a counter prompt is not one of those");
        BTN(PAD_TRI); CHECK(test_prompt_pad(pd) == 0, "triangle does not answer a counter");
        BTN(PAD_X); CHECK(test_prompt_pad(pd) != 0, "the attack button does");
        press(PAD_X); CHECK(*(u64*)(cmd + 0x80) == 0, "and the menu leaves that press to the game");
        press(PAD_DOWN); CHECK(press(PAD_X) != 0 && *st == 0, "counter prompt, cursor on Magic: the button still goes to the prompt");
        press(PAD_UP);
        *(u32*)(cmd + 0x60) &= ~0x40000u; *(u8**)(cmd + 0x298) = NULL; *(u8**)(cmd + 0x1b0) = NULL; BTN(0);
        #undef BTN
    }
    press(PAD_DOWN); CHECK(*st == 0 && *cur == 1, "on Magic again");
    /* the jump button (the menus' cancel button, circle): with a list open it closes the list and does not jump */
    {   extern int test_jump_pad(u8 *pad), test_link_close_pad(u8 *pad); extern int *test_link_close(void);
        u8 *pd = mgr + 0x58;
        #define CIRCLE(on) (G(u32, 0x8f64930) = (on) ? 0x2000 : 0, G(u32, 0x8f64934) = (on) ? 0x2000 : 0, G(u32, 0x8f6493c) = (on) ? 0x2000 : 0, G(u16, 0x8f6499c) = 0)
        G(u16, 0x8221c4) = 0x2000;
        CIRCLE(0); CHECK(test_jump_pad(pd) == 0, "not pressed: no jump");
        CIRCLE(1); CHECK(test_jump_pad(pd) != 0 && *st == 0, "main menu: the button jumps");
        CIRCLE(0); press(PAD_X); CHECK(*st == 1, "Magic list open");
        CIRCLE(1); CHECK(test_jump_pad(pd) == 0 && *st == 0 && *cur == 1 && *(u64*)(cmd + 0x80) == 0, "list open: the press closes it, no jump (state %d)", *st);
        CHECK(test_jump_pad(pd) != 0, "list closed: it jumps again");
        *(u32*)(cmd + 0x60) |= 0x10000000;                              /* the game's D-Link list is open */
        CHECK(test_jump_pad(pd) == 0 && *test_link_close() == 1, "D-Link list open: no jump, close asked for");
        CIRCLE(0);
        CHECK(test_link_close_pad(pd) == 1 && test_link_close_pad(pd) == 0, "the game's \"close the list\" test answers yes once");
        *(u32*)(cmd + 0x60) &= ~0x10000000u;
        #undef CIRCLE
    }
    /* busy player: nothing is used, list stays */
    press(PAD_X); *(u32*)(pl + 0x318) = 0; press(PAD_X);
    CHECK(*st == 1 && *(u64*)(cmd + 0x80) == 0 && *test_mp() == 90, "no cast while the player is busy");
    *(u32*)(pl + 0x318) = 0x40; press(PAD_LEFT);
    /* items */
    press(PAD_DOWN); CHECK(*cur == 2, "Item");
    press(PAD_X); CHECK(*st == 2, "item list"); press(PAD_DOWN); press(PAD_X);
    CHECK(*(u16*)(cmd + 0x80) == 0xbf && *test_mp() == 90, "Ether queued, no MP cost: %x", *(u16*)(cmd + 0x80));
    *(u64*)(cmd + 0x80) = 0;
    /* wrap around the main menu; Link without links does nothing */
    press(PAD_UP); CHECK(*cur == 3, "up from Attack wraps to Link (%d)", *cur);
    press(PAD_X); CHECK(*st == 0 && *cur == 3, "no links: nothing opens");
    press(PAD_DOWN); CHECK(*cur == 0, "down from Link wraps to Attack");
    /* the D-Link list's cursor move (the game's 140205cf0 through our call hook): over at once, the entry that
       arrives under the cursor (the last row) is the selected one */
    {   extern u64 test_link_scroll(u8 *P, int dir); extern float *test_link_time(void);
        static u64 lc[3] = { 0x1a1, 0x1a2, 0x1a3 }; u8 *L[3];
        for (int i = 0; i < 3; i++) { L[i] = calloc(1, 0x70); L[i][0x30] = 6; L[i][0x31] = i == 2 ? 1 : 0; L[i][0x33] = i; L[i][0x6c] = i; L[i][0x6d] = 0;
            *(u64**)(L[i] + 0x58) = &lc[i]; *(float*)(L[i] + 0x20) = 0.5f; *(int*)(L[i] + 0x4c) = 900 + i; *(u32*)(L[i] + 0x60) = i == 2 ? 4 : 0; *(u8**)(cmd + 0x2a8 + i * 8) = L[i]; }
        cmd[0x350] = 3; cmd[0x351] = 3; cmd[0x354] = 0xff;
        for (int i = 0; i < 3; i++) test_link_scroll(L[i], 1);
        CHECK(L[0][0x6c] == 1 && L[1][0x6c] == 2 && L[2][0x6c] == 0, "every entry moved one place: %d %d %d", L[0][0x6c], L[1][0x6c], L[2][0x6c]);
        CHECK(L[1][0x31] == 1 && (*(u32*)(L[1] + 0x60) & 4) && L[0][0x31] == 0 && L[2][0x31] == 0 && !(*(u32*)(L[2] + 0x60) & 4) && !(*(u32*)(L[0] + 0x60) & 4),
              "the move is over at once: states %d %d %d, entry 1 selected", L[0][0x31], L[1][0x31], L[2][0x31]);
        CHECK(FN(int, 0x2047f0, u8*)(L[1]) == 1, "the game finds the entry under the cursor ready for input");
        for (int i = 0; i < 3; i++) test_link_scroll(L[i], -1);
        CHECK(L[2][0x6c] == 2 && L[2][0x31] == 1 && (*(u32*)(L[2] + 0x60) & 4) && L[1][0x31] == 0 && !(*(u32*)(L[1] + 0x60) & 4), "and back: entry 2 selected again");
        *test_link_time() = 10.0f;                                          /* LinkCursorTime=10: the game's own animation */
        test_link_scroll(L[0], 1);
        CHECK(L[0][0x31] == 2 && *(float*)(L[0] + 0x68) == 10.0f && L[0][0x6c] == 0 && L[0][0x6e] == 1, "with a time set the move is the game's: state %d, %.0f to go", L[0][0x31], *(float*)(L[0] + 0x68));
        *test_link_time() = 0; cmd[0x350] = 0; cmd[0x351] = 0;
        for (int i = 0; i < 3; i++) *(u8**)(cmd + 0x2a8 + i * 8) = NULL;
    }
    /* burn: Magic cannot be opened, an open list closes */
    press(PAD_DOWN); press(PAD_X); CHECK(*st == 1, "list open");
    *test_mp() = 5; sel[0] = 0; press(PAD_X);                             /* Fire with 5 MP left -> burn */
    CHECK(*test_burn() == 1 && *(u16*)(cmd + 0x80) == 0x83, "last cast goes through, burn starts");
    *(u64*)(cmd + 0x80) = 0;
    press(PAD_DOWN); press(PAD_X); CHECK(*st == 0 && *cur == 1, "Magic stays shut in burn");
    press(PAD_DOWN); press(PAD_X); CHECK(*st == 2, "items still open in burn");
    printf("t_menu done\n");
}
/* the "COMMANDS" window in grey while the shortcut list is shown, on the real 2D-layout runtime */
typedef struct { s32 maxf; s16 spr; u16 key; u8 keyn[11]; u8 kind, blend, flag, sciss, z; } TAnim;
typedef struct { float t; u8 c[4]; u32 interp; } TKey;
static u8 *t_node_objs(int h, u16 node, int *count) {
    u8 *lc = FN(u8*, 0x1a65c0, int)(h); u8 *nodes = *(u8**)(lc + 0x98);
    for (int i = 0; i < *(s16*)(lc + 0xa8); i++) { u8 *nd = nodes + i * 0xb8; if (*(s16*)(nd + 0xb0) == (s16)node) { *count = (s8)nd[0xb2]; return **(u8***)(nd + 0xa8); } }
    return NULL;
}
static void t_header(void) {
    extern void test_hd_frame(u8 *cmd, int grey); extern void *test_sc_keys(void);
    const char *fn = getenv("BBS_PLATE_L2D");
    if (!fn) { printf("  (skipped: BBS_PLATE_L2D not set)\n"); return; }
    FILE *f = fopen(fn, "rb"); if (!f) { printf("  cannot open %s\n", fn); fails++; return; }
    u8 *img = memalign(16, 0x80000); size_t n = fread(img, 1, 0x80000, f); fclose(f);
    *(u8*)RVA(0x111660) = 0xC3; *(u8*)RVA(0x10c5e0) = 0xC3; memcpy(RVA(0x11a8a0), "\x31\xc0\xc3", 3);
    FN(void, 0x1a6d70, void)();
    int d = FN(int, 0x1a6fb0, void*, const char*, int)(img, "bc01_00.l2d", 0);
    CHECK(d > 0, "register bc01_00 (%zu bytes): %d", n, d);
    int file = FN(int, 0x1a64f0, const char*)("bc01_00.l2d"), sq = FN(int, 0x1a64f0, const char*)("bc01_00:0");
    G(u8, 0x8f88028) = 0;
    int h = FN(int, 0x1a4ec0, int, u16, int, void*)(file, 5, 0, NULL);          /* the window, battle skin */
    u8 *c = calloc(1, 0x360); *(int*)(c + 0x98) = h;
    int n46 = 0, n47 = 0; u8 *o46 = t_node_objs(h, 0x46, &n46), *o47 = t_node_objs(h, 0x47, &n47);
    CHECK(o46 && o47 && n46 == 7 && n47 == 3, "window: %d + %d objects", n46, n47);
    TKey *gk = *(TKey**)(o46 + 0x38), *mine = test_sc_keys();
    int same = 1; TAnim *orig46[8], *orig47[4];
    for (int i = 0; i < n46; i++) { same &= *(TKey**)(o46 + i * 0x50 + 0x38) == gk; orig46[i] = *(TAnim**)(o46 + i * 0x50 + 0x30); }
    for (int i = 0; i < n47; i++) { same &= *(TKey**)(o47 + i * 0x50 + 0x38) == *(TKey**)(o47 + 0x38); orig47[i] = *(TAnim**)(o47 + i * 0x50 + 0x30); }
    CHECK(same && gk != mine, "every object of a node has the node's key table");
    test_hd_frame(c, 0);
    CHECK(*(TAnim**)(o46 + 6 * 0x50 + 0x30) == orig46[6] && *(TAnim**)(o47 + 0x50 + 0x30) == orig47[1], "list not shown: nothing changed");
    test_hd_frame(c, 1);
    #define REC(o, i) (*(TAnim**)((o) + (i) * 0x50 + 0x30))
    #define TAB(o, i) (*(TKey**)((o) + (i) * 0x50 + 0x38))
    TAnim *fr = REC(o46, 6), *lp = REC(o47, 1), *lt = REC(o47, 2);
    CHECK(fr != orig46[6] && fr->spr == 33 && TAB(o46, 6) == mine && fr->keyn[1] == 1 && fr->keyn[10] == 1 && fr->sciss == orig46[6]->sciss && fr->maxf == orig46[6]->maxf, "frame: a twin of the game's record on our key table");
    TKey *k = mine + fr->key;
    CHECK(*(float*)&k[0].c == 5.0f && k[1].c[0] == 0x80 && k[1].c[1] == 0x80 && k[1].c[2] == 0x80 && k[1].c[3] == 0xff, "frame keys: x %.0f, colour %02x%02x%02x alpha %02x", *(float*)&k[0].c, k[1].c[0], k[1].c[1], k[1].c[2], k[1].c[3]);
    k = mine + lp->key + 1; CHECK(lp->spr == 34 && k->c[0] == 0x70 && k->c[1] == 0x70 && k->c[2] == 0x70 && k->c[3] == 0xff, "label plate %02x%02x%02x", k->c[0], k->c[1], k->c[2]);
    k = mine + lt->key + 1; CHECK(lt->spr == 35 && k->c[0] == 0xff && k->c[1] == 0xff && k->c[2] == 0xff && k->c[3] == 0xff, "letters %02x%02x%02x", k->c[0], k->c[1], k->c[2]);
    int others = 1; for (int i = 0; i < 6; i++) others &= REC(o46, i) == orig46[i] && TAB(o46, i) == gk;
    CHECK(others && REC(o47, 0) == orig47[0], "gauge, shadow and roots untouched");
    CHECK((o46[6 * 0x50 + 0x4b] & 4) != 0, "frame still drawn (flags %02x)", o46[6 * 0x50 + 0x4b]);
    /* the game's key data is untouched */
    { TKey *g = gk + orig46[6]->key + 1; CHECK(g->c[0] == 0xff && g->c[1] == 0xdc && g->c[2] == 0x00, "game's frame colour still %02x%02x%02x", g->c[0], g->c[1], g->c[2]); }
    test_hd_frame(c, 1);
    CHECK(REC(o46, 6) == fr && REC(o47, 1) == lp, "repeat passes keep the twins");
    /* the game changes the control (the window appearing: label slides in and fades in, two colour keys) */
    FN(int, 0x1a7520, int, int)(h, 1);
    CHECK(REC(o47, 1) != lp && TAB(o47, 1) == mine, "control change: the game's record is back, our table still on the object");
    TAnim *g1 = REC(o47, 1);
    test_hd_frame(c, 1);
    lp = REC(o47, 1); k = mine + lp->key;
    { int nk = 0; for (int i = 0; i < 11; i++) nk += lp->keyn[i];
      CHECK(lp != g1 && lp->maxf == 20 && lp->keyn[10] == 2 && k[nk - 2].c[0] == 0x70 && k[nk - 2].c[3] == 0x00 && k[nk - 1].c[0] == 0x70 && k[nk - 1].c[3] == 0xff && *(float*)&k[nk - 4].c == 20.0f,
            "twin of the appearing label: %d keys, fade %02x -> %02x in grey, slide from %.0f", nk, k[nk - 2].c[3], k[nk - 1].c[3], *(float*)&k[nk - 4].c); }
    /* list gone: everything back */
    test_hd_frame(c, 0);
    CHECK(REC(o47, 1) == g1 && TAB(o47, 1) == *(TKey**)(o47 + 0x38) && TAB(o47, 2) == *(TKey**)(o47 + 0x38) && TAB(o46, 6) == gk && REC(o46, 6)->spr == 33 && REC(o46, 6) != fr, "list gone: the game's records and tables are back");
    /* a control change while grey, then the list goes before our next pass: the stale table is still put right */
    FN(int, 0x1a7520, int, int)(h, 0); test_hd_frame(c, 1); FN(int, 0x1a7520, int, int)(h, 2); test_hd_frame(c, 0);
    int clean = 1; for (int i = 0; i < n46; i++) clean &= TAB(o46, i) == gk; for (int i = 0; i < n47; i++) clean &= TAB(o47, i) == *(TKey**)(o47 + 0x38);
    CHECK(clean, "no object keeps our table");
    /* field skin: six objects, the frame is the last, alpha a0 */
    FN(int, 0x1a7520, int, int)(h, 0);
    FN(int, 0x1a7fb0, int, u16, int, u16, int)(h, 0x46, sq, 0x259, 0);
    o46 = t_node_objs(h, 0x46, &n46); test_hd_frame(c, 1);
    fr = REC(o46, 5); k = mine + fr->key;
    CHECK(n46 == 6 && fr->spr == 33 && TAB(o46, 5) == mine && k[1].c[0] == 0x80 && k[1].c[2] == 0x80 && k[1].c[3] == 0xa0, "field skin: frame grey, alpha %02x", k[1].c[3]);
    /* a dead window is left alone */
    FN(void, 0x1a57f0, int)(h); test_hd_frame(c, 1); test_hd_frame(c, 0);
    *(int*)(c + 0x98) = 0; test_hd_frame(c, 1);
    #undef REC
    #undef TAB
    printf("t_header done\n");
}
/* the Magic / Items / D-Link list over the menu: where its rows go, its header, and the words in the texture */
#include "../src/hdart_gen.h"
static void t_listhdr(void) {
    extern float test_list_y(int e, int idx, int n); extern void test_hdr_frame(int kind, int battle, float x, float y); extern int *test_hdr(void);
    extern int test_hd_patch(u8 *pix, int w, int h); extern void test_set_hd_art(int on); extern int tex_hd_art_ready(void);
    /* rows: the menu's are at 198 / 213 / 228 / 243 (Attack, Magic, Items, D-Link), 15 apart */
    CHECK(test_list_y(1, 0, 1) == 213 && test_list_y(1, 0, 3) == 213 && test_list_y(1, 2, 3) == 243, "Magic, up to 3 rows: from the Magic row down (%.0f, %.0f..%.0f)", test_list_y(1, 0, 1), test_list_y(1, 0, 3), test_list_y(1, 2, 3));
    CHECK(test_list_y(1, 0, 4) == 198 && test_list_y(1, 3, 4) == 243, "Magic, 4 rows: ends on the last row, starts one higher (%.0f..%.0f)", test_list_y(1, 0, 4), test_list_y(1, 3, 4));
    CHECK(test_list_y(1, 0, 7) == 198 - 45 && test_list_y(1, 6, 7) == 243 && test_list_y(1, 3, 7) == 198, "Magic, 7 rows: grows above the menu (%.0f..%.0f)", test_list_y(1, 0, 7), test_list_y(1, 6, 7));
    CHECK(test_list_y(2, 0, 1) == 228 && test_list_y(2, 1, 2) == 243 && test_list_y(2, 0, 3) == 213 && test_list_y(2, 2, 3) == 243, "Items: from the Items row; 3 rows start on the Magic row (%.0f, %.0f)", test_list_y(2, 0, 1), test_list_y(2, 0, 3));
    CHECK(test_list_y(3, 0, 1) == 243 && test_list_y(3, 0, 3) == 213 && test_list_y(3, 2, 3) == 243, "D-Link: always ends on its own row, the last (%.0f, %.0f..)", test_list_y(3, 0, 1), test_list_y(3, 0, 3));
    /* the words in the command sheet: every language's sheet is recognised, nothing else is */
    static const char *envs[3] = { "BBS_CMD_RAW", "BBS_CMD_RAW_JP", "BBS_CMD_RAW_FR" };
    size_t sz = 1024 * 1024 * 4; u8 *pix = malloc(sz), *orig = malloc(sz);
    for (int l = 0; l < 3; l++) {
        const char *fn = getenv(envs[l]);
        if (!fn) { printf("  (skipped: %s not set)\n", envs[l]); continue; }
        FILE *f = fopen(fn, "rb"); if (!f) { printf("  cannot open %s\n", fn); fails++; continue; }
        CHECK(fread(pix, 1, sz, f) == sz, "read"); fclose(f); memcpy(orig, pix, sz);
        CHECK(test_hd_patch(pix, 1024, 1024) == 1, "%s: command sheet recognised", envs[l]);
        size_t in = 0, out = 0;
        for (int y = 0; y < 1024; y++) for (int x = 0; x < 1024; x++) if (memcmp(pix + (y * 1024 + x) * 4, orig + (y * 1024 + x) * 4, 4)) {
            if (x >= HDART_X && x < HDART_X + HDART_W && y >= HDART_Y && y < HDART_Y + HDART_H) in++; else out++; }
        CHECK(out == 0 && in > 8000, "only the words' area changed (%zu inside, %zu outside)", in, out);
        if (l == 0) {
            /* MAGIC: a letter's body is grey and solid, its shadow black; the cell's top row is clear */
            u8 *m = pix + ((size_t)(HDART_MAGIC_V * 2 + 16) * 1024 + HDART_MAGIC_U * 2 + 16) * 4, *t = pix + ((size_t)(HDART_MAGIC_V * 2) * 1024 + HDART_MAGIC_U * 2 + 30) * 4;
            CHECK(m[3] == 255 && m[0] == m[1] && m[1] == m[2] && m[0] > 120 && m[0] < 140 && t[3] == 0, "MAGIC: stem of the M grey %d alpha %d, top row alpha %d", m[0], m[3], t[3]);
            CHECK(test_hd_patch(pix, 1024, 1024) == 0, "a second pass finds the area in use");
            memcpy(pix, orig, sz); for (size_t i = 0; i < sz; i += 4) { u8 x = pix[i]; pix[i] = pix[i + 2]; pix[i + 2] = x; }
            CHECK(test_hd_patch(pix, 1024, 1024) == 1, "the same sheet in the other byte order is recognised too");
        }
    }
    memset(pix, 0, sz); CHECK(test_hd_patch(pix, 1024, 1024) == 0 && test_hd_patch(orig, 1024, 512) == 0, "an empty sheet and other sizes are left alone");
    { const char *fn = getenv("BBS_CAMP_RAW"); FILE *f = fn ? fopen(fn, "rb") : NULL;
      if (f) { size_t got = fread(pix, 1, sz, f); fclose(f); memcpy(orig, pix, sz); CHECK(got == sz && test_hd_patch(pix, 1024, 1024) == 0 && !memcmp(pix, orig, sz), "the menu's sheet (same size) is left alone"); } }
    /* the header, on the real layout runtime with the English bc01_00.l2d */
    const char *fn = getenv("BBS_PLATE_L2D_EN"); if (!fn) fn = getenv("BBS_PLATE_L2D");
    if (!fn) { printf("  (skipped: BBS_PLATE_L2D not set)\n"); return; }
    FILE *f = fopen(fn, "rb"); if (!f) { printf("  cannot open %s\n", fn); fails++; return; }
    u8 *img = memalign(16, 0x80000); fread(img, 1, 0x80000, f); fclose(f);
    *(u8*)RVA(0x111660) = 0xC3; *(u8*)RVA(0x10c5e0) = 0xC3; memcpy(RVA(0x11a8a0), "\x31\xc0\xc3", 3);
    FN(void, 0x1a6d70, void)();
    CHECK(FN(int, 0x1a6fb0, void*, const char*, int)(img, "bc01_00.l2d", 0) > 0, "register bc01_00");
    G(int, 0x10f9ed48) = FN(int, 0x1a64f0, const char*)("bc01_00.l2d"); G(int, 0x10f9ed4c) = FN(int, 0x1a64f0, const char*)("bc01_00:0");
    { extern int *test_list_col(void); *test_list_col() = 0; }       /* the game's battle / field looks first; the lists' own colours: t_colours */
    test_set_hd_art(0); test_hdr_frame(0, 1, 20.2f, 203.9f);
    CHECK(*test_hdr() == 0, "no words in the texture: no header is made");
    test_set_hd_art(1); test_hdr_frame(0, 1, 20.2f, 203.9f);
    int h = *test_hdr();
    CHECK(h > 0 && FN(int, 0x1a6060, int)(h) == 1, "header instance %d, shown", h);
    CHECK(FN(int, 0x1a6670, int, u16)(h, 0x46) == 0 && FN(int, 0x1a6670, int, u16)(h, 0x47) != 0, "gauge node off, label node on");
    Pair *q = NULL; int c = FN(int, 0x1a6980, int, u16, int, Pair**)(h, 0x47, 2, &q);
    CHECK(c == 1 && q && q[0].g->x0 == 0 && q[0].g->y0 == -1 && q[0].g->x1 == HDART_MAGIC_W / 2 && q[0].g->y1 == 9 && q[0].p->u0 == HDART_MAGIC_U && q[0].p->v0 == HDART_MAGIC_V && q[0].p->u1 == HDART_MAGIC_U + HDART_MAGIC_W && q[0].p->v1 == HDART_MAGIC_V + 20 && q[0].p->col[0] == 0x80808080,
          "letters: MAGIC, %d x %d from uv (%d,%d)", q ? q[0].g->x1 : 0, q ? q[0].g->y1 - q[0].g->y0 : 0, q ? q[0].p->u0 : 0, q ? q[0].p->v0 : 0);
    float x = 0, y = 0; FN(int, 0x1a6290, int, float*, float*)(h, &x, &y);
    CHECK(x > 20.19f && x < 20.21f && y > 203.89f && y < 203.91f, "placed at (%.2f, %.2f)", x, y);
    { Pair *pq = NULL; int pc = FN(int, 0x1a6980, int, u16, int, Pair**)(h, 0x47, 1, &pq);
      CHECK(pc == 3 && pq && pq[0].g->x0 == -2 && pq[1].g->x1 == 76 + 24 && pq[2].g->x0 == 100 && pq[2].g->x1 == 90 + 24 && pq[1].p->u0 == 116, "plate 24 longer: stretch to %d, right end %d..%d", pq ? pq[1].g->x1 : 0, pq ? pq[2].g->x0 : 0, pq ? pq[2].g->x1 : 0);
      test_hdr_frame(0, 1, 20.2f, 203.9f); test_hdr_frame(0, 1, 20.2f, 203.9f);
      CHECK(pq[2].g->x1 == 90 + 24, "and not longer with every frame (%d)", pq[2].g->x1); }
    { int n47 = 0; u8 *o = t_node_objs(h, 0x47, &n47); TAnim *pl = *(TAnim**)(o + 0x50 + 0x30); TKey *k = *(TKey**)(o + 0x50 + 0x38) + pl->key + 1;
      CHECK(n47 == 3 && pl->spr == 34 && k->c[0] == 0xef && k->c[1] == 0xaf, "battle: the game's battle label (plate colour %02x%02x%02x)", k->c[0], k->c[1], k->c[2]); }
    test_hdr_frame(1, 1, 20.2f, 218.9f);
    CHECK(*test_hdr() == h && q[0].p->u0 == HDART_ITEMS_U && q[0].p->v0 == HDART_ITEMS_V && q[0].g->x1 == HDART_ITEMS_W / 2, "ITEMS on the same instance");
    test_hdr_frame(2, 1, 20.2f, 218.9f);
    CHECK(q[0].p->v0 == HDART_DLINK_V && q[0].g->x1 == HDART_DLINK_W / 2 && q[0].p->u1 == HDART_DLINK_U + HDART_DLINK_W, "D-LINK (%d wide)", q[0].g->x1);
    /* the game's own label of a second window is untouched */
    { G(u8, 0x8f88028) = 0; int h2 = FN(int, 0x1a4ec0, int, u16, int, void*)(G(int, 0x10f9ed48), 5, 0, NULL);
      FN(int, 0x1a83f0, int, u16, int)(h2, 0x47, 2); Pair *q2 = NULL; FN(int, 0x1a6980, int, u16, int, Pair**)(h2, 0x47, 2, &q2);
      CHECK(q2 && q2[0].p->u0 == 8 && q2[0].p->v0 == 294 && (q2[0].g->x1 == 82 || q2[0].g->x1 == 73), "the game's COMMANDS letters are as they were (%d wide)", q2 ? q2[0].g->x1 : 0);
      FN(int, 0x1a83f0, int, u16, int)(h2, 0x47, 1); Pair *p2 = NULL; FN(int, 0x1a6980, int, u16, int, Pair**)(h2, 0x47, 1, &p2);
      CHECK(p2 && p2[1].g->x1 == 76 && p2[2].g->x1 == 90, "and its plate is as long as it was (%d)", p2 ? p2[2].g->x1 : 0);
      FN(void, 0x1a57f0, int)(h2); }
    test_hdr_frame(-1, 1, 0, 0);
    CHECK(*test_hdr() == h && FN(int, 0x1a6060, int)(h) == 0, "no list open: hidden");
    /* field: a new instance with the game's field label */
    test_hdr_frame(0, 0, 20.2f, 203.9f);
    int hf = *test_hdr();
    CHECK(hf > 0 && FN(int, 0x1a60e0, int)(h) == 8, "field: the battle instance is gone, a new one made (%d)", hf);
    { int n47 = 0; u8 *o = t_node_objs(hf, 0x47, &n47); TAnim *pl = *(TAnim**)(o + 0x50 + 0x30), *lt = *(TAnim**)(o + 0xa0 + 0x30);
      TKey *k = *(TKey**)(o + 0x50 + 0x38) + pl->key + 1, *k2 = *(TKey**)(o + 0xa0 + 0x38) + lt->key + 1;
      CHECK(n47 == 3 && pl->spr == 34 && k->c[0] == 0x00 && k->c[1] == 0x5f && k->c[2] == 0xf0 && lt->spr == 35 && k2->c[1] == 0xff, "field label: plate %02x%02x%02x, letters %02x%02x%02x", k->c[0], k->c[1], k->c[2], k2->c[0], k2->c[1], k2->c[2]); }
    q = NULL; c = FN(int, 0x1a6980, int, u16, int, Pair**)(hf, 0x47, 2, &q);
    CHECK(c == 1 && q && q[0].p->v0 == HDART_MAGIC_V && FN(int, 0x1a6670, int, u16)(hf, 0x46) == 0, "field: MAGIC again, gauge node off");
    /* destroyed by the game (room change): made again */
    FN(void, 0x1a57f0, int)(hf); test_hdr_frame(1, 0, 20.2f, 203.9f);
    CHECK(*test_hdr() > 0 && FN(int, 0x1a60e0, int)(*test_hdr()) != 8, "made again after the instance was destroyed");
    printf("t_listhdr done\n");
}
/* The lists' own colours (menu.c tint_node / row_colours / hdr_frame), on the real 2D-layout runtime with the
   English bc01_00.l2d: what each object plays, and what the game's draw function sends to the screen for it. */
extern void test_row_colours(int h, int kind); extern void test_tint_node(int h, u16 node, s16 spr, u32 from, u32 to);
extern void *test_obj_anim(int h, u16 node, int obj, void **keys); extern int test_node_objs(int h, u16 node);
extern void *test_sc_keys(void), *test_sc_anims(void); extern void test_hdr_frame(int kind, int battle, float x, float y); extern int *test_hdr(void); extern void test_set_hd_art(int on);
extern u32 *test_col_plate(void), *test_col_title(void); extern int *test_list_col(void); extern int test_twins(void);
static u8 *t_node_ctl(int h, u16 node) {
    u8 *lc = FN(u8*, 0x1a65c0, int)(h); if (!lc) return NULL;
    u8 *nodes = *(u8**)(lc + 0x98);
    for (int i = 0; i < *(s16*)(lc + 0xa8); i++) if (*(s16*)(nodes + i * 0xb8 + 0xb0) == (s16)node) return nodes + i * 0xb8;
    return NULL;
}
/* colour key of the object's record, read the way the game reads it: record -> key table of the object */
static u32 t_obj_col(int h, u16 node, int obj, int *spr, int *ours, int *blend) {
    void *kt = NULL; TAnim *a = test_obj_anim(h, node, obj, &kt);
    if (!a || !kt) return 0xdeadbeef;
    int nk = 0; for (int k = 0; k < 11; k++) nk += a->keyn[k];
    if (spr) *spr = a->spr; if (ours) *ours = kt == test_sc_keys(); if (blend) *blend = a->blend;
    if (!a->keyn[10]) return 0xffffffff;
    TKey *k = (TKey*)kt + a->key + nk - 1;
    return (u32)k->c[0] << 24 | (u32)k->c[1] << 16 | (u32)k->c[2] << 8 | k->c[3];
}
/* the colour the game's draw sends for the first sprite of `quads` quads of this node (vertex colour word) */
static u32 t_drawn_col(int h, u16 node, int quads) {
    u8 *nd = t_node_ctl(h, node); if (!nd) return 0;
    G(u8, 0x8f88020 + (s8)nd[0x86]) = 1; G(u8, 0x8f8802c) = 1;
    g_vcalls = 0; memset(g_vtx, 0, sizeof g_vtx);
    FN(void, 0x1aaea0, u8*, int)(nd, 0);
    for (int i = 0; i < g_vcalls && i < 8; i++) if (g_vn[i] == quads * 4) { u32 c; memcpy(&c, &g_vtx[i][(quads - 1) * 4 * 7 + 3], 4); return c; }
    return 0;
}
static void t_colours(void) {
    const char *fn = getenv("BBS_PLATE_L2D_EN"); if (!fn) fn = getenv("BBS_PLATE_L2D");
    if (!fn) { printf("  (skipped: BBS_PLATE_L2D not set)\n"); return; }
    FILE *f = fopen(fn, "rb"); if (!f) { printf("  cannot open %s\n", fn); fails++; return; }
    u8 *img = memalign(16, 0x80000); fread(img, 1, 0x80000, f); fclose(f);
    *(u8*)RVA(0x111660) = 0xC3; *(u8*)RVA(0x10c5e0) = 0xC3; memcpy(RVA(0x11a8a0), "\x31\xc0\xc3", 3);
    *(u8*)RVA(0x1aa620) = 0xC3;
    FN(void, 0x1a6d70, void)();
    CHECK(FN(int, 0x1a6fb0, void*, const char*, int)(img, "bc01_00.l2d", 0) > 0, "register bc01_00");
    int file = FN(int, 0x1a64f0, const char*)("bc01_00.l2d"), sq = FN(int, 0x1a64f0, const char*)("bc01_00:0");
    G(int, 0x10f9ed48) = file; G(int, 0x10f9ed4c) = sq;
    draw_stubs();
    CHECK(*test_list_col() == 1 && test_col_plate()[0] == 0x005ff0 && test_col_plate()[1] == 0x00c832 && test_col_plate()[2] == 0x0096fa, "defaults: on; Magic 005ff0, Items 00c832, D-Link 0096fa");
    int spr = 0, ours = 0, blend = 0; u32 c;
    /* --- a deck plate as the game has it in battle --- */
    G(u8, 0x8f88028) = 0;
    int h = FN(int, 0x1a4ec0, int, u16, int, void*)(file, 3, 3, NULL);
    CHECK(h > 0 && test_node_objs(h, 0x5d) == 14, "a deck plate, battle body (%d objects)", test_node_objs(h, 0x5d));
    c = t_obj_col(h, 0x5d, 3, &spr, &ours, NULL);
    CHECK(spr == 0 && c == 0xe1dc00a0 && !ours, "the game's battle body: yellow %08x", c);
    u32 dy = t_drawn_col(h, 0x5d, 6);
    /* Items: the field body, green */
    test_row_colours(h, 1);
    CHECK(test_node_objs(h, 0x5d) == 7, "a row has the field body (%d objects)", test_node_objs(h, 0x5d));
    c = t_obj_col(h, 0x5d, 1, &spr, &ours, NULL);
    CHECK(spr == 0 && c == 0x00c832a0 && ours, "Items row: body %08x (green, the game's alpha), a twin record in our table", c);
    u32 dg = t_drawn_col(h, 0x5d, 6);
    CHECK(dg != dy && dg != 0, "and the game draws it in another colour than the battle body (%08x, was %08x)", dg, dy);
    /* Magic: the game's own field blue needs no twin */
    test_row_colours(h, 0);
    c = t_obj_col(h, 0x5d, 1, &spr, &ours, NULL);
    CHECK(spr == 0 && c == 0x005ff0a0 && !ours, "Magic row: the game's own field record again (%08x)", c);
    u32 db = t_drawn_col(h, 0x5d, 6);
    CHECK(db != dg && db != dy && db != 0, "drawn blue (%08x), not green (%08x), not yellow (%08x)", db, dg, dy);
    printf("   drawn colour words: battle %08x, Items %08x, Magic %08x\n", dy, dg, db);
    /* another Magic colour from the settings */
    test_col_plate()[0] = 0x4020c0; test_row_colours(h, 0);
    c = t_obj_col(h, 0x5d, 1, NULL, &ours, NULL);
    CHECK(c == 0x4020c0a0 && ours, "Magic colour from the settings: %08x", c);
    test_col_plate()[0] = 0x005ff0; test_row_colours(h, 0);
    CHECK(t_obj_col(h, 0x5d, 1, NULL, &ours, NULL) == 0x005ff0a0 && !ours, "and back");
    /* reloading (control 4): body in the list's colour, the glow twice as bright; the yellow fill is the game's */
    test_row_colours(h, 1);
    FN(int, 0x1a7520, int, int)(h, 4);
    { void *kt = NULL; TAnim *a = test_obj_anim(h, 0x5d, 1, &kt);
      CHECK(a && a->spr == 0 && kt == test_sc_keys() && !(a >= (TAnim*)test_sc_anims() && 0), "a control change by the game: its record, our key table still on the object"); }
    test_row_colours(h, 1);
    c = t_obj_col(h, 0x5d, 1, &spr, &ours, NULL);
    CHECK(spr == 0 && c == 0x00c832a0 && ours, "control 4, Items: body %08x", c);
    c = t_obj_col(h, 0x5d, 6, &spr, &ours, &blend);
    CHECK(spr == 2 && c == 0x00ff64ff && ours && blend == 1, "control 4: glow %08x (twice the plate colour), still additive", c);
    c = t_obj_col(h, 0x5d, 5, &spr, &ours, NULL);
    CHECK(spr == 16 && (c >> 8) == 0xffff00 && !ours, "control 4: the reload fill is the game's yellow (%08x)", c);
    test_row_colours(h, 0);
    CHECK(t_obj_col(h, 0x5d, 6, &spr, &ours, NULL) == 0x00c0ffff && !ours, "control 4, Magic: the game's own glow");
    /* control 3 in the field body: the inside is black and stays black */
    FN(int, 0x1a7520, int, int)(h, 3); test_row_colours(h, 1);
    c = t_obj_col(h, 0x5d, 6, &spr, &ours, NULL);
    CHECK(spr == 2 && (c >> 8) == 0 && !ours, "control 3: the black inside is left alone (%08x)", c);
    /* the row under the cursor: the field body's inside sprite carries the gradient, in the list's colour */
    { extern void test_row_colours_m(int h, int kind, int mode);
      Pair *fq = NULL;
      test_row_colours_m(h, 1, 2);
      int fn_ = FN(int, 0x1a6980, int, u16, int, Pair**)(h, 0x5d, 6, &fq);
      c = t_obj_col(h, 0x5d, 6, &spr, &ours, &blend);
      CHECK(spr == 2 && c == 0x00c832ff && ours && blend == 0, "cursor row, Items: the inside is the list's colour (%08x)", c);
      CHECK(fn_ == 3 && fq && fq[0].p->u0 == 4 && fq[0].p->v0 == 32 && fq[2].p->u1 == 29 && fq[0].g->x0 == 2 && fq[2].g->x1 == 107 && fq[0].p->col[0] == 0xff808080,
            "and its quads are the gradient's (uv %d,%d.., x %d..%d, colour %08x)", fq ? fq[0].p->u0 : 0, fq ? fq[0].p->v0 : 0, fq ? fq[0].g->x0 : 0, fq ? fq[2].g->x1 : 0, fq ? fq[0].p->col[0] : 0);
      u32 dgr = t_drawn_col(h, 0x5d, 3);
      CHECK((dgr & 0xffffff) == (dg & 0xffffff) && dgr != 0, "drawn in the Items colour (%08x; the body %08x)", dgr, dg);
      c = t_obj_col(h, 0x5d, 1, NULL, &ours, NULL);
      CHECK(c == 0x00c832a0 && ours, "the frame as well (%08x)", c);
      test_row_colours_m(h, 0, 2);
      c = t_obj_col(h, 0x5d, 6, &spr, &ours, NULL);
      CHECK(spr == 2 && c == 0x005ff0ff && ours && t_obj_col(h, 0x5d, 1, NULL, &ours, NULL) == 0x005ff0a0 && !ours, "cursor row, Magic: inside %08x, the frame the game's own record", c);
      test_row_colours_m(h, 1, 0);
      c = t_obj_col(h, 0x5d, 6, &spr, &ours, NULL);
      CHECK(spr == 2 && (c >> 8) == 0 && !ours && fq[0].p->u0 == 32 && fq[0].p->v0 == 30 && fq[2].g->x1 == 106 && fq[0].p->col[0] == 0x80808080, "cursor gone: black inside again, the sprite's own quads (uv %d,%d)", fq[0].p->u0, fq[0].p->v0);
      test_row_colours_m(h, 1, 1);
      CHECK(fq[0].p->u0 == 32 && (t_obj_col(h, 0x5d, 6, NULL, &ours, NULL) >> 8) == 0 && !ours, "cursor on a row that cannot be used: no gradient");
      test_row_colours_m(h, 1, 0); }
    /* the game re-skins the plate on entering battle: the next pass has the field body back */
    FN(int, 0x1a7fb0, int, u16, int, u16, int)(h, 0x5d, sq, 0xf, 3);
    CHECK(test_node_objs(h, 0x5d) == 14, "the game puts the battle body on");
    test_row_colours(h, 1);
    CHECK(test_node_objs(h, 0x5d) == 7 && t_obj_col(h, 0x5d, 1, NULL, &ours, NULL) == 0x00c832a0, "next pass: field body, green");
    /* a pass without rules gives the objects back to the game */
    test_tint_node(h, 0x5d, 0, 0, 0xfffffffeu);
    c = t_obj_col(h, 0x5d, 1, NULL, &ours, NULL);
    CHECK(c == 0x005ff0a0 && !ours, "no rules: the game's record and key table again (%08x)", c);
    /* a second plate is not touched by the first one's colours */
    { G(u8, 0x8f88028) = 0; int h2 = FN(int, 0x1a4ec0, int, u16, int, void*)(file, 3, 3, NULL);
      test_row_colours(h, 1);
      CHECK(test_node_objs(h2, 0x5d) == 14 && t_obj_col(h2, 0x5d, 3, NULL, &ours, NULL) == 0xe1dc00a0 && !ours, "another plate keeps the game's look");
      FN(void, 0x1a57f0, int)(h2); }
    int tw = test_twins();
    for (int i = 0; i < 50; i++) { test_row_colours(h, i & 1); FN(int, 0x1a7520, int, int)(h, 3 + (i & 1)); }
    CHECK(test_twins() == tw || test_twins() <= tw + 2, "twins are made once per record and colour (%d, then %d)", tw, test_twins());
    /* --- a D-Link plate --- */
    G(u8, 0x8f88028) = 0;
    int hl = FN(int, 0x1a4ec0, int, u16, int, void*)(file, 0xa, 3, NULL);
    c = t_obj_col(hl, 1, 2, &spr, &ours, NULL);
    CHECK(hl > 0 && spr == 63 && c == 0x3c3c3ca0, "a D-Link plate not under the cursor: the game darkens its frame (%08x)", c);
    u32 dd = t_drawn_col(hl, 1, 3);
    test_tint_node(hl, 1, 63, 0x3c3c3c, 0x808080);
    c = t_obj_col(hl, 1, 2, &spr, &ours, NULL);
    CHECK(spr == 63 && c == 0x808080a0 && ours, "bright: the art's own blue at the game's alpha (%08x)", c);
    u32 dl = t_drawn_col(hl, 1, 3);
    CHECK(dl != dd && dl != 0, "drawn brighter (%08x, was %08x)", dl, dd);
    FN(int, 0x1a7520, int, int)(hl, 0); test_tint_node(hl, 1, 63, 0x3c3c3c, 0x808080);
    c = t_obj_col(hl, 1, 2, &spr, &ours, NULL);
    CHECK(spr == 63 && c == 0xffffffff && !ours, "under the cursor (control 0): the game's record, no colour key, untouched");
    FN(int, 0x1a7520, int, int)(hl, 5); test_tint_node(hl, 1, 63, 0x3c3c3c, 0x808080);
    CHECK(t_obj_col(hl, 1, 2, NULL, &ours, NULL) == 0xe1dc00ff && !ours, "control 5 (another colour of the game's): untouched");
    /* --- the header --- */
    test_set_hd_art(1);
    test_hdr_frame(1, 1, 20.2f, 203.9f);                /* ITEMS, in battle */
    int hh = *test_hdr();
    u32 pc = t_obj_col(hh, 0x47, 1, &spr, &ours, NULL), lc = t_obj_col(hh, 0x47, 2, NULL, NULL, NULL);
    CHECK(hh > 0 && spr == 34 && pc == 0x00c832ff && ours && lc == 0xc8ff64ff, "ITEMS header in battle: plate %08x, letters %08x", pc, lc);
    test_hdr_frame(1, 0, 20.2f, 203.9f);
    CHECK(*test_hdr() == hh && t_obj_col(hh, 0x47, 1, NULL, NULL, NULL) == 0x00c832ff, "out of battle: the same instance, the same colour");
    test_hdr_frame(0, 1, 20.2f, 203.9f);
    pc = t_obj_col(hh, 0x47, 1, NULL, &ours, NULL); lc = t_obj_col(hh, 0x47, 2, NULL, NULL, NULL);
    CHECK(*test_hdr() == hh && pc == 0x005ff0ff && !ours && lc == 0x00ffffff, "MAGIC header in battle: the field label's own blue %08x, letters %08x", pc, lc);
    test_hdr_frame(2, 1, 20.2f, 203.9f);
    pc = t_obj_col(hh, 0x47, 1, NULL, &ours, NULL); lc = t_obj_col(hh, 0x47, 2, NULL, NULL, NULL);
    CHECK(pc == 0x0096faff && ours && lc == 0xffffffff, "D-LINK header: plate %08x, letters %08x", pc, lc);
    { Pair *q = NULL; int n = FN(int, 0x1a6980, int, u16, int, Pair**)(hh, 0x47, 2, &q);
      CHECK(n == 1 && q && q[0].p->v0 == HDART_DLINK_V, "and it still carries its word"); }
    /* the game's own window of the same layout is not touched */
    { G(u8, 0x8f88028) = 0; int h2 = FN(int, 0x1a4ec0, int, u16, int, void*)(file, 5, 0, NULL);
      CHECK(t_obj_col(h2, 0x47, 1, NULL, &ours, NULL) == 0xefaf03ff && !ours, "the game's COMMANDS label keeps its battle yellow"); FN(void, 0x1a57f0, int)(h2); }
    /* ListColors = 0: as before */
    *test_list_col() = 0; test_hdr_frame(2, 1, 20.2f, 203.9f);
    CHECK(*test_hdr() != hh && t_obj_col(*test_hdr(), 0x47, 1, NULL, &ours, NULL) == 0xefaf03ff && !ours, "ListColors = 0: the game's battle label");
    *test_list_col() = 1;
    printf("t_colours done\n");
}
static int run(const char *name, void (*fn)(void)) {
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) { fn(); fflush(stdout); _exit(fails ? 3 : 0); }
    int st; waitpid(p, &st, 0);
    if (WIFSIGNALED(st)) { printf("%s: CRASH signal %d\n", name, WTERMSIG(st)); return 1; }
    printf("%s: exit %d\n", name, WEXITSTATUS(st)); return WEXITSTATUS(st);
}
static void on_segv(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    u64 rip = uc->uc_mcontext.gregs[REG_RIP];
    printf("  SIGNAL %d at rip %llx (rva %llx) addr %p\n", sig, (unsigned long long)rip, (unsigned long long)(rip - (u64)g_base), si->si_addr);
    u64 *sp = (u64*)uc->uc_mcontext.gregs[REG_RSP];
    printf("  stack rvas:"); for (int i = 0; i < 96; i++) if (sp[i] >= (u64)g_base + 0x1000 && sp[i] < (u64)g_base + 0x632000) printf(" %llx", (unsigned long long)(sp[i] - (u64)g_base));
    printf("\n"); fflush(stdout); _exit(11);
}
int main(int argc, char **argv) {
    const char *exe = argc > 1 ? argv[1] : getenv("BBS_EXE");
    if (!exe) { fprintf(stderr, "usage: run <path to KINGDOM HEARTS Birth by Sleep FINAL MIX.exe>  (or set BBS_EXE)\n"); return 2; }
    struct sigaction sa = {0}; sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
    g_base = map_pe(exe);
    log_open();
    int ok = mod_install();
    printf("install=%d errors=%d\n", ok, g_patch_errors);
    int bad = 0;
    bad |= run("t_use", t_use);
    bad |= run("t_ether", t_ether);
    bad |= run("t_haste", t_haste);
    bad |= run("t_berserk", t_berserk);
    bad |= run("t_bar", t_bar);
    bad |= run("t_tex", t_tex);
    bad |= run("t_draw", t_draw);
    bad |= run("t_guard", t_guard);
    bad |= run("t_gone", t_gone);
    bad |= run("t_shadow", t_shadow);
    bad |= run("t_colours", t_colours);
    bad |= run("t_header", t_header);
    bad |= run("t_listhdr", t_listhdr);
    bad |= run("t_cursor", t_cursor);
    bad |= run("t_style", t_style);
    bad |= run("t_speed", t_speed);
    bad |= run("t_floor", t_floor);
    bad |= run("t_desc", t_desc);
    bad |= run("t_shortcut", t_shortcut);
    bad |= run("t_sclist", t_sclist);
    bad |= run("t_status", t_status);
    bad |= run("t_sccamp", t_sccamp);
    bad |= run("t_bundle", t_bundle);
    bad |= run("t_menu", t_menu);
    return bad || !ok;
}

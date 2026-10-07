/* The changes of the three earlier mods, applied in memory instead of to the game files:
     Combo Flow     code at five places in the exe + 89 bytes of PAtkData.bin
     KH2 Camera     the camera parameter files PCam?000.bin / BCam????.bin
     Revenge Value  Factory.lub (enemy AI factory script) in CommonLua.arc
   Data files are plain "CRsrcData" resources whose bytes are read straight from the loaded archive image, so
   they are patched in place when the resource is created (vtable slot 1 of CRsrcData, a bare `ret` in the
   game).  Factory.lub has another length, so the buffer is swapped at the one call that loads Lua chunks. */
#include "core.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "bundle_gen.h"
#include "mod.h"

extern char g_dir[MAX_PATH], g_ini[MAX_PATH + 32];
extern int g_debug;
static int c_combo = 1, c_camera = 1, c_revenge = 1;

#define FACTORY_OLD_SIZE 638
#define FACTORY_OLD_CRC  0x951b6dfdu
/* the first Revenge Value build (the standalone mod, still installed through the Mod Manager or its own installer):
   replaced as well, so that the bosses added since are there */
#define FACTORY_RV1_SIZE 10208
#define FACTORY_RV1_CRC  0x9fcc7bc9u
static u32 crc32_(const u8 *p, size_t n);
static int is_factory(const char *buf, size_t n) {
    if (!buf) return 0;
    if (n == FACTORY_OLD_SIZE) return crc32_((const u8*)buf, n) == FACTORY_OLD_CRC;
    if (n == FACTORY_RV1_SIZE) return crc32_((const u8*)buf, n) == FACTORY_RV1_CRC;
    return 0;
}
#define VT_SLOT   0x637b10      /* CRsrcData::vftable[1] */
#define VT_STUB   0x114bc0      /* what it points to: ret */
#define LUA_CALL  0x2c5962      /* call luaL_loadbuffer */
#define LUA_LOAD  0x5bb9f0

static u32 crc32_(const u8 *p, size_t n) {
    static u32 t[256]; static int ready;
    if (!ready) { for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0 - (c & 1))); t[i] = c; } ready = 1; }
    u32 c = 0xffffffffu;
    while (n--) c = t[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

/* CRsrcData: +0x10 type (2 = .bin), +0x38 name, +0x70 data, +0x80 size.  Runs on the game's loader threads. */
static u64 MSABI rsrc_loaded(u8 *r) {
    if (r[0x10] != 2) return 0;
    const char *name = (const char*)(r + 0x38);
    u8 *data = *(u8**)(r + 0x70); u64 size = *(u64*)(r + 0x80);
    if (!data) data = *(u8**)(r + 0x68);        /* revived from the cache: only the planned address is set yet */
    if (!data) return 0;
    for (unsigned i = 0; i < sizeof df_files / sizeof *df_files; i++) {
        if (size != df_files[i].size || strncmp(name, df_files[i].name, 16) != 0) continue;
        if (df_files[i].group == 0 ? !c_combo : !c_camera) return 0;
        u32 c = crc32_(data, size);
        if (c == df_files[i].crc_new) { if (g_debug) LOG("bundle: %s already changed", name); return 0; }
        if (c != df_files[i].crc_old) { LOG("bundle: %s is not the expected original (crc %08x): left alone", name, c); return 0; }
        for (unsigned k = 0; k < df_files[i].n; k++) data[df_files[i].set[2 * k]] = (u8)df_files[i].set[2 * k + 1];
        if (g_debug) LOG("bundle: %s patched (%d bytes)", name, df_files[i].n);
        return 0;
    }
    return 0;
}

/* luaL_loadbuffer(L, buf, size, name) at the game's script loader */
static int MSABI lua_load_hook(void *L, const char *buf, size_t n, const char *name) {
    if (c_revenge && is_factory(buf, n)) {
        if (g_debug) LOG("bundle: Factory.lub replaced (%d bytes)", (int)sizeof rv_factory);
        buf = (const char*)rv_factory; n = sizeof rv_factory;
    }
    return FN(int, LUA_LOAD, void*, const char*, size_t, const char*)(L, buf, n, name);
}

static int ini_i(const char *key, int def) {
    char p[MAX_PATH + 32], b[32], d[32];
    snprintf(p, sizeof p, "%s", g_ini); snprintf(d, sizeof d, "%d", def);
    GetPrivateProfileStringA("Bundle", key, d, b, sizeof b, p);
    return atoi(b);
}
/* state of the Combo Flow code: 0 = original, 1 = already present (the old exe patch is installed), -1 = neither */
static int combo_state(void) {
    int old = 0, neu = 0, n = sizeof cf_code / sizeof *cf_code;
    for (int i = 0; i < n; i++) {
        if (!memcmp(g_base + cf_code[i].rva, cf_code[i].old, cf_code[i].len)) old++;
        else if (!memcmp(g_base + cf_code[i].rva, cf_code[i].neu, cf_code[i].len)) neu++;
    }
    return old == n ? 0 : neu == n ? 1 : -1;
}
/* speed.c takes over the walk-out logic and hooks the same three places of the game code */
int bundle_combo_in_exe(void) { return combo_state() == 1; }
void bundle_restore_sites(void) {
    if (combo_state() != 1) return;
    for (unsigned i = 0; i < sizeof cf_code / sizeof *cf_code; i++)
        if (cf_code[i].rva < 0x62f000) patch_bytes(cf_code[i].rva, cf_code[i].neu, cf_code[i].old, cf_code[i].len, "combo flow (undo)");
    LOG("bundle: the old Combo Flow exe patch is installed; its three hooks are replaced by the speed module");
}
int bundle_check(void) {
    c_combo = ini_i("ComboFlow", 1); c_camera = ini_i("Camera", 1); c_revenge = ini_i("RevengeValue", 1);
    int bad = 0;
    crc32_((const u8*)"", 0);                    /* build the table now: the hooks run on several threads */
    if (G(u64, VT_SLOT) != (u64)(g_base + VT_STUB)) { LOG("bundle: resource vtable slot does not match"); bad++; }
    u8 *p = g_base + LUA_CALL; s32 d; memcpy(&d, p + 1, 4);
    if (p[0] != 0xE8 || (u32)(LUA_CALL + 5 + d) != LUA_LOAD) { LOG("bundle: Lua call site does not match"); bad++; }
    if (c_combo && combo_state() < 0) { LOG("bundle: Combo Flow code sites do not match"); bad++; }
    return bad == 0;
}
void bundle_apply(void) {
    if (c_combo && speed_enabled()) LOG("bundle: Combo Flow walk-out is handled by the speed module");
    else if (c_combo) {
        if (combo_state() == 1) LOG("bundle: Combo Flow code is already in the exe (old mod still installed)");
        else {
            for (unsigned i = 0; i < sizeof cf_code / sizeof *cf_code; i++)
                patch_bytes(cf_code[i].rva, cf_code[i].old, cf_code[i].neu, cf_code[i].len, "combo flow");
            LOG("bundle: Combo Flow code applied");
        }
    }
    if (c_combo || c_camera) {
        void *f = (void*)rsrc_loaded; u64 old = (u64)(g_base + VT_STUB);
        patch_bytes(VT_SLOT, (u8*)&old, (u8*)&f, 8, "rsrc vtable");
    }
    if (c_revenge) hook_call(LUA_CALL, LUA_LOAD, lua_load_hook, "lua load");
    LOG("bundle: combo flow %d, camera %d, revenge value %d", c_combo, c_camera, c_revenge);
}

#ifndef _WIN32
u64 test_rsrc_loaded(u8 *r) { return rsrc_loaded(r); }
int test_lua_swap(const char **buf, size_t *n) {      /* the decision of lua_load_hook, without calling Lua */
    if (c_revenge && is_factory(*buf, *n)) { *buf = (const char*)rv_factory; *n = sizeof rv_factory; return 1; }
    return 0;
}
u32 test_crc(const u8 *p, size_t n) { return crc32_(p, n); }
#endif

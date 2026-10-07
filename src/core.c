#include "core.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

u8 *g_base;
int g_patch_errors;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logcs;
static int g_logcs_init;
char g_dir[MAX_PATH];             /* game folder: log, debug channel, off switch */
char g_ini[MAX_PATH + 32];        /* settings file: game folder if it has one, else next to the DLL */

void log_open(void) {
    char p[MAX_PATH + 32];
    InitializeCriticalSection(&g_logcs); g_logcs_init = 1;
    snprintf(p, sizeof p, "%sbbskh2_log.txt", g_dir);
    g_log = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}
void logf_(const char *fmt, ...) {
    char buf[2048]; va_list ap; DWORD w;
    if (g_log == INVALID_HANDLE_VALUE) return;
    int n = snprintf(buf, sizeof buf, "[%8lu] ", (unsigned long)GetTickCount());
    va_start(ap, fmt); n += vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap); va_end(ap);
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    if (g_logcs_init) EnterCriticalSection(&g_logcs);
    WriteFile(g_log, buf, n, &w, NULL);
    if (g_logcs_init) LeaveCriticalSection(&g_logcs);
}

/* ---------- near allocation ---------- */
static u8 *g_near, *g_near_end;
void *near_alloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!g_near || g_near + n > g_near_end) {
        size_t chunk = 0x40000; if (n > chunk) chunk = (n + 0xffff) & ~(size_t)0xffff;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
        u8 *lo = g_base + nt->OptionalHeader.SizeOfImage;
        u8 *p = NULL;
        for (u8 *a = (u8*)(((u64)lo + 0xffff) & ~(u64)0xffff); a < g_base + 0x70000000; a += 0x10000) {
            p = VirtualAlloc(a, chunk, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) break;
        }
        if (!p) for (u8 *a = (u8*)(((u64)g_base - chunk) & ~(u64)0xffff); a > g_base - 0x70000000; a -= 0x10000) {
            p = VirtualAlloc(a, chunk, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) break;
        }
        if (!p) { LOG("near_alloc FAILED"); g_patch_errors++; return NULL; }
        g_near = p; g_near_end = p + chunk;
        LOG("near block %p (+%llx from base)", p, (unsigned long long)(p - g_base));
    }
    void *r = g_near; g_near += n; memset(r, 0, n); return r;
}

static int wr(void *dst, const void *src, size_t n) {
    DWORD old;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(dst, src, n);
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return 1;
}
int patch_bytes(u32 rva, const u8 *expect, const u8 *repl, size_t n, const char *name) {
    u8 *p = g_base + rva;
    if (expect && memcmp(p, expect, n) != 0) {
        LOG("PATCH MISMATCH %s @%x", name, rva); g_patch_errors++; return 0;
    }
    if (!wr(p, repl, n)) { LOG("PATCH WRITE FAIL %s @%x", name, rva); g_patch_errors++; return 0; }
    return 1;
}
int patch_u32(u32 rva, u32 expect, u32 repl, const char *name) { return patch_bytes(rva, (u8*)&expect, (u8*)&repl, 4, name); }
int patch_u8(u32 rva, u8 expect, u8 repl, const char *name) { return patch_bytes(rva, &expect, &repl, 1, name); }
int patch_riprel(u32 disp_rva, u32 tail, u32 old_target_rva, void *new_target, const char *name) {
    u8 *next = g_base + disp_rva + 4 + tail;
    s32 old = (s32)((s64)(g_base + old_target_rva) - (s64)next);
    s64 nd = (s64)(u8*)new_target - (s64)next;
    if (nd > 0x7fffffffLL || nd < -0x80000000LL) { LOG("PATCH RANGE %s @%x", name, disp_rva); g_patch_errors++; return 0; }
    s32 n32 = (s32)nd;
    return patch_bytes(disp_rva, (u8*)&old, (u8*)&n32, 4, name);
}

/* ---------- code emission helpers ---------- */
typedef struct { u8 *p; } Em;
static void eb(Em *e, const void *b, size_t n) { memcpy(e->p, b, n); e->p += n; }
#define EB(e, ...) do { static const u8 _b[] = { __VA_ARGS__ }; eb(e, _b, sizeof _b); } while (0)
static void e_jmp_abs(Em *e, void *target) { EB(e, 0xFF, 0x25, 0, 0, 0, 0); eb(e, &target, 8); }

/* copy stolen bytes to e->p, fixing up rip-relative displacements */
static int emit_stolen(Em *e, const Steal *s) {
    u8 *src = g_base + s->rva; u8 *dst = e->p;
    memcpy(dst, src, s->len);
    for (int i = 0; i < s->nfix; i++) {
        s32 d; memcpy(&d, src + s->fix[i], 4);
        u8 *target = src + s->fixend[i] + d;
        s64 nd = (s64)target - (s64)(dst + s->fixend[i]);
        if (nd > 0x7fffffffLL || nd < -0x80000000LL) return 0;
        s32 n32 = (s32)nd; memcpy(dst + s->fix[i], &n32, 4);
    }
    e->p += s->len; return 1;
}
/* compare the live bytes with the expected ones, ignoring displacement fields (they may have been re-pointed already) */
static int steal_matches(const Steal *s) {
    const u8 *p = g_base + s->rva;
    for (int i = 0; i < s->len; i++) {
        int masked = 0;
        for (int k = 0; k < s->nfix; k++) if (i >= s->fix[k] && i < s->fix[k] + 4) masked = 1;
        if (!masked && p[i] != s->bytes[i]) return 0;
    }
    return 1;
}
static int install_jmp(const Steal *s, u8 *stub, const char *name) {
    u8 buf[24]; memset(buf, 0x90, sizeof buf);
    s64 d = (s64)stub - (s64)(g_base + s->rva + 5);
    if (d > 0x7fffffffLL || d < -0x80000000LL) { LOG("HOOK RANGE %s", name); g_patch_errors++; return 0; }
    buf[0] = 0xE9; s32 d32 = (s32)d; memcpy(buf + 1, &d32, 4);
    return patch_bytes(s->rva, NULL, buf, s->len, name);
}

void *hook_fn(const Steal *s, void *replacement, const char *name) {
    if (!steal_matches(s)) { LOG("HOOK MISMATCH %s @%x", name, s->rva); g_patch_errors++; return NULL; }
    u8 *mem = near_alloc(64 + s->len); if (!mem) return NULL;
    Em e = { mem };
    u8 *stub = e.p; e_jmp_abs(&e, replacement);
    u8 *tramp = e.p;
    if (!emit_stolen(&e, s)) { LOG("HOOK RELOC FAIL %s", name); g_patch_errors++; return NULL; }
    e_jmp_abs(&e, g_base + s->rva + s->len);
    if (!install_jmp(s, stub, name)) return NULL;
    return tramp;
}

int hook_ctx(const Steal *s, CtxHook h, const char *name) {
    if (!steal_matches(s)) { LOG("HOOK MISMATCH %s @%x", name, s->rva); g_patch_errors++; return 0; }
    u8 *mem = near_alloc(0x140 + s->len); if (!mem) return 0;
    Em e = { mem };
    u8 *stub = e.p;
    EB(&e, 0x9C);                                   /* pushfq */
    EB(&e, 0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57);/* push rax rcx rdx rbx rbp rsi rdi */
    EB(&e, 0x41,0x50, 0x41,0x51, 0x41,0x52, 0x41,0x53, 0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57); /* push r8..r15 */
    EB(&e, 0x48, 0x83, 0xEC, 0x60);                 /* sub rsp,0x60 */
    EB(&e, 0xF3,0x0F,0x7F,0x04,0x24);               /* movdqu [rsp],xmm0 */
    EB(&e, 0xF3,0x0F,0x7F,0x4C,0x24,0x10);          /* xmm1 */
    EB(&e, 0xF3,0x0F,0x7F,0x54,0x24,0x20);          /* xmm2 */
    EB(&e, 0xF3,0x0F,0x7F,0x5C,0x24,0x30);          /* xmm3 */
    EB(&e, 0xF3,0x0F,0x7F,0x64,0x24,0x40);          /* xmm4 */
    EB(&e, 0xF3,0x0F,0x7F,0x6C,0x24,0x50);          /* xmm5 */
    EB(&e, 0x48, 0x89, 0xE1);                       /* mov rcx,rsp */
    EB(&e, 0x48, 0x89, 0xE3);                       /* mov rbx,rsp */
    EB(&e, 0x48, 0x83, 0xE4, 0xF0);                 /* and rsp,-16 */
    EB(&e, 0x48, 0x83, 0xEC, 0x20);                 /* sub rsp,0x20 */
    EB(&e, 0xFF, 0x15); u8 *call_disp = e.p; EB(&e, 0,0,0,0);   /* call [rip+handler] */
    EB(&e, 0x48, 0x89, 0xDC);                       /* mov rsp,rbx */
    EB(&e, 0x48, 0x89, 0x05); u8 *slot_disp1 = e.p; EB(&e, 0,0,0,0); /* mov [rip+slot],rax */
    EB(&e, 0xF3,0x0F,0x6F,0x04,0x24);
    EB(&e, 0xF3,0x0F,0x6F,0x4C,0x24,0x10);
    EB(&e, 0xF3,0x0F,0x6F,0x54,0x24,0x20);
    EB(&e, 0xF3,0x0F,0x6F,0x5C,0x24,0x30);
    EB(&e, 0xF3,0x0F,0x6F,0x64,0x24,0x40);
    EB(&e, 0xF3,0x0F,0x6F,0x6C,0x24,0x50);
    EB(&e, 0x48, 0x83, 0xC4, 0x60);                 /* add rsp,0x60 */
    EB(&e, 0x41,0x5F, 0x41,0x5E, 0x41,0x5D, 0x41,0x5C, 0x41,0x5B, 0x41,0x5A, 0x41,0x59, 0x41,0x58);
    EB(&e, 0x5F, 0x5E, 0x5D, 0x5B, 0x5A, 0x59, 0x58);/* pop rdi rsi rbp rbx rdx rcx rax */
    /* test the slot without touching flags we are about to restore: compare before popfq */
    EB(&e, 0x48, 0x83, 0x3D); u8 *slot_disp2 = e.p; EB(&e, 0,0,0,0, 0x00); /* cmp qword [rip+slot],0 */
    EB(&e, 0x74, 0x07);                             /* je +7 (normal path) */
    EB(&e, 0x9D);                                   /* popfq */
    EB(&e, 0xFF, 0x25); u8 *slot_disp3 = e.p; EB(&e, 0,0,0,0);  /* jmp [rip+slot] */
    EB(&e, 0x9D);                                   /* normal: popfq */
    if (!emit_stolen(&e, s)) { LOG("HOOK RELOC FAIL %s", name); g_patch_errors++; return 0; }
    e_jmp_abs(&e, g_base + s->rva + s->len);
    /* data */
    u8 *hptr = e.p; { void *hh = (void*)h; eb(&e, &hh, 8); }
    u8 *slot = e.p; { u64 z = 0; eb(&e, &z, 8); }
    s32 d;
    d = (s32)(hptr - (call_disp + 4)); memcpy(call_disp, &d, 4);
    d = (s32)(slot - (slot_disp1 + 4)); memcpy(slot_disp1, &d, 4);
    d = (s32)(slot - (slot_disp2 + 5)); memcpy(slot_disp2, &d, 4);
    d = (s32)(slot - (slot_disp3 + 4)); memcpy(slot_disp3, &d, 4);
    return install_jmp(s, stub, name);
}

int hook_call(u32 call_rva, u32 old_target_rva, void *replacement, const char *name) {
    u8 *p = g_base + call_rva;
    s32 old = (s32)((s64)old_target_rva - (s64)(call_rva + 5));
    u8 exp[5] = { 0xE8 }; memcpy(exp + 1, &old, 4);
    if (memcmp(p, exp, 5) != 0) { LOG("CALLHOOK MISMATCH %s @%x", name, call_rva); g_patch_errors++; return 0; }
    u8 *stub = near_alloc(16); if (!stub) return 0;
    Em e = { stub }; e_jmp_abs(&e, replacement);
    s64 d = (s64)stub - (s64)(p + 5);
    if (d > 0x7fffffffLL || d < -0x80000000LL) { g_patch_errors++; return 0; }
    u8 rep[5] = { 0xE8 }; s32 d32 = (s32)d; memcpy(rep + 1, &d32, 4);
    return patch_bytes(call_rva, exp, rep, 5, name);
}

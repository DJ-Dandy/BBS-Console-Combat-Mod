/* BBS KH2-style mod.  The DLL sits next to the game exe under the name of a system DLL the game imports
   (DINPUT8.dll, or dbghelp.dll; see build.sh): Windows looks in the game folder first, so the game loads it at
   startup.  Every call meant for the system DLL is forwarded to the real one.  DINPUT8 is the default because
   the OpenKH mod loader (Panacea) uses the name dbghelp.dll.  Under any other name it can also be loaded by
   Panacea from a mod's dll folder.  Either way the game is patched in memory when the DLL is loaded. */
#include "core.h"
#include "mod.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern char g_dir[MAX_PATH], g_ini[MAX_PATH + 32];
void log_open(void);
int mod_install(void);           /* mp.c */
void dbg_start(void);            /* debug.c */

/* ---------------- system DLL proxy ---------------- */
extern void *proxy_table[];
extern const char *const proxy_names[];
extern const int proxy_count;
extern const char proxy_dll[];          /* "\\dinput8.dll" */
void proxy_fail(void);
static HMODULE g_real;
/* stand-in used only if the system DLL cannot supply it: create every folder along a path that ends in '\\' */
static BOOL WINAPI own_MakeSureDirectoryPathExists(const char *path) {
    char b[MAX_PATH]; size_t n = strlen(path);
    if (n >= sizeof b) return FALSE;
    for (size_t i = 0; i < n; i++) {
        b[i] = path[i];
        if ((path[i] == '\\' || path[i] == '/') && i > 2) {
            b[i] = 0;
            if (!CreateDirectoryA(b, NULL) && GetLastError() != ERROR_ALREADY_EXISTS && GetFileAttributesA(b) == INVALID_FILE_ATTRIBUTES) return FALSE;
            b[i] = path[i];
        }
    }
    return TRUE;
}
void *proxy_resolve(int i) {
    if (!g_real) {
        char p[MAX_PATH + 16]; UINT n = GetSystemDirectoryA(p, MAX_PATH);
        lstrcpyA(p + n, proxy_dll);
        g_real = LoadLibraryA(p);
    }
    void *f = g_real ? (void*)GetProcAddress(g_real, proxy_names[i]) : NULL;
    if (!f && !strcmp(proxy_names[i], "MakeSureDirectoryPathExists")) f = (void*)own_MakeSureDirectoryPathExists;
    if (!f) f = (void*)proxy_fail;
    proxy_table[i] = f;
    return f;
}

/* ---------------- crash logging ---------------- */
static volatile LONG g_crashes;
/* A crash inside the game's 2D-layout drawing (texture bind 14010ccb0 <- CD2SeqCtrl::Draw 1401aaea0 <- CD2LayCtrl::Draw
   1401a2bb0) says nothing about WHICH instance was being drawn: the registers that held it are gone by then.  The
   callers' saved registers are still on the stack though, so the stack is searched for pointers to layout / sequence
   controllers (objects whose first word is one of the two vtables) and what is found is written to the log: the
   instance's handle, whether it is one of the mod's own, and for every node the data block it draws from (name,
   current / original texture, substituted texture id) - a node whose texture is the address the crash read from is
   the one.  Runs only here, in the exception filter; every read is checked first. */
#define VT_LAYCTRL 0x6418b8
#define VT_SEQCTRL 0x641dc0
/* no IsBadReadPtr here: it works by faulting, and this handler would be entered again for its fault (that is what
   cut the report short the second time the crash was seen) */
static int rd_ok(const void *p, size_t n) {
    u64 a = (u64)p;
    if (a < 0x10000 || (a >> 47) || (a & 7) || !n) return 0;
    for (u64 pg = a >> 12; pg <= (a + n - 1) >> 12; pg++) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void*)(pg << 12), &mi, sizeof mi)) return 0;
        if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mi.Protect & 0xff)) return 0;
    }
    return 1;
}
/* how many 8-byte words can be read from sp upwards, at most max: the thread's stack ends at its base */
static int stack_words(u64 *sp, int max) {
    u64 top = (u64)((NT_TIB*)NtCurrentTeb())->StackBase, a = (u64)sp;
    if (a & 7) return 0;
    if (top > a && top - a < (u64)max * 8) max = (int)((top - a) / 8);
    while (max > 0 && !rd_ok(sp, (size_t)max * 8)) max /= 2;
    return max;
}
static void blk_name(u8 *data, char *out) {        /* CD2LayData / CD2SeqData: name at +8, 16 bytes */
    strcpy(out, "?");
    if (!rd_ok(data, 0xa8)) return;
    int i = 0;
    for (; i < 15; i++) { char ch = (char)data[8 + i]; if (!ch) break; out[i] = (ch >= 0x20 && ch < 0x7f) ? ch : '#'; }
    out[i] = 0;
}
static void seq_report(u8 *sc, const char *what, u64 bad) {
    u8 *data = *(u8**)(sc + 0x90);
    char nm[20]; blk_name(data, nm);
    u8 *tm = rd_ok(data, 0xa8) ? *(u8**)(data + 0x90) : NULL, *otm = rd_ok(data, 0xa8) ? *(u8**)(data + 0x98) : NULL;
    u64 native = rd_ok(tm, 0x50) ? *(u64*)(tm + 0x28) : 0;
    LOG("     %s id %x data %p \"%s\" tex %p%s (native %llx%s) orig tex %p subst %x  ctl %d flags %02x frame %.1f",
        what, *(u16*)(sc + 0xb0), (void*)data, nm, (void*)tm, rd_ok(tm, 0x50) ? "" : " UNREADABLE", (unsigned long long)native,
        bad && native == bad ? " <== the address the crash read" : "", (void*)otm, rd_ok(data, 0xa8) ? *(u32*)(data + 0xa0) : 0,
        (s8)sc[0x82], sc[0x87], *(float*)(sc + 0x78));
}
static void l2d_crash_report(CONTEXT *c) {
    u64 *sp = (u64*)c->Rsp;
    int nw = stack_words(sp, 256);
    u8 *seen[8]; int ns = 0;
    char own[400]; mod_own_handles(own, sizeof own);
    for (int i = 0; i < nw && ns < 8; i++) {
        u8 *o = (u8*)sp[i];
        if (!rd_ok(o, 0xb8)) continue;
        u64 vt = *(u64*)o;
        int lay = vt == (u64)(g_base + VT_LAYCTRL), seq = vt == (u64)(g_base + VT_SEQCTRL);
        if (!lay && !seq) continue;
        int dup = 0; for (int k = 0; k < ns; k++) if (seen[k] == o) dup = 1;
        if (dup) continue;
        if (!ns) LOG("   2D instances on the stack (mod's own handles:%s)", own[0] ? own : " none");
        seen[ns++] = o;
        if (lay) {
            int nn = *(s16*)(o + 0xa8); u8 *nodes = *(u8**)(o + 0x98);
            LOG("   layout %p handle %d from data handle %d: %d nodes, priority %d, flags %02x, control %d, frame %.1f, end action %d",
                (void*)o, *(s32*)(o + 0x18), *(s32*)(o + 0x1c), nn, *(s16*)(o + 0x10), o[0x87], (s8)o[0x82], *(float*)(o + 0x78), (s8)o[0x83]);
            for (int k = 0; k < nn && k < 40; k++) { u8 *nd = nodes + (size_t)k * 0xb8; if (!rd_ok(nd, 0xb8)) break; seq_report(nd, "node", c->Rdx); }
        } else {
            LOG("   sequence %p handle %d from data handle %d, priority %d", (void*)o, *(s32*)(o + 0x18), *(s32*)(o + 0x1c), *(s16*)(o + 0x10));
            seq_report(o, "seq", c->Rdx);
        }
    }
}
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_crashes) > 6) return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG busy;                  /* a fault inside this report is not reported in turn */
    if (InterlockedExchange(&busy, 1)) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    u8 *ip = (u8*)c->Rip;
    LOG("!! EXCEPTION %08lx at %p (rva %llx) info0=%llx info1=%llx", code, ip, (unsigned long long)(ip - g_base),
        (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0], (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    LOG("   rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx rsp=%llx", c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    LOG("   r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx", c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15);
    u64 *sp = (u64*)c->Rsp; char line[900]; int n = 0;
    int nw = stack_words(sp, 128);
    if (nw > 0) {
        for (int i = 0; i < nw && n < 800; i++) {
            u64 v = sp[i];
            if (v >= (u64)g_base + 0x1000 && v < (u64)g_base + 0x632000) n += snprintf(line + n, sizeof line - n, " %llx", (unsigned long long)(v - (u64)g_base));
        }
        line[n] = 0; LOG("   stack rvas:%s", line);
    }
    l2d_crash_report(c);
    { int fx = 0, sk = 0; guard_stats(&fx, &sk); if (fx || sk) LOG("   texture guard so far: %d put back, %d not drawn", fx, sk); }
    InterlockedExchange(&busy, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

static int is_bbs(void) {
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    const char *b = strrchr(exe, '\\'); b = b ? b + 1 : exe;
    return _stricmp(b, "KINGDOM HEARTS Birth by Sleep FINAL MIX.exe") == 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        if (!is_bbs()) return TRUE;                 /* stay inert for the other games in this folder */
        if (GetEnvironmentVariableA("BBSKH2_LOADED", NULL, 0)) return TRUE;   /* a second copy under another name */
        SetEnvironmentVariableA("BBSKH2_LOADED", "1");
        g_base = (u8*)GetModuleHandleA(NULL);
        GetModuleFileNameA(NULL, g_dir, MAX_PATH);
        char *s = strrchr(g_dir, '\\'); if (s) s[1] = 0;
        snprintf(g_ini, sizeof g_ini, "%sbbskh2.ini", g_dir);
        if (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES) {
            char d[MAX_PATH]; GetModuleFileNameA(h, d, MAX_PATH);
            char *e = strrchr(d, '\\'); if (e) e[1] = 0;
            snprintf(g_ini, sizeof g_ini, "%sbbskh2.ini", d);
        }
        log_open();
        LOG("bbskh2 loading; exe base %p; settings %s", g_base, g_ini);
        AddVectoredExceptionHandler(1, veh);
        char off[MAX_PATH + 32]; snprintf(off, sizeof off, "%sbbskh2_off.txt", g_dir);
        if (GetFileAttributesA(off) != INVALID_FILE_ATTRIBUTES) LOG("bbskh2_off.txt present: patches not applied");
        else {
            int ok = mod_install();
            LOG("install %s (errors=%d)", ok ? "OK" : "FAILED", g_patch_errors);
        }
        dbg_start();
    }
    return TRUE;
}

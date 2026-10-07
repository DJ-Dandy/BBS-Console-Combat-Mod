/* Debug channel: polls bbskh2_cmd.txt in the game folder, executes commands, appends output to the log.
   Only active when bbskh2_debug.txt exists next to the DLL. */
#include "core.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
extern char g_dir[MAX_PATH];
void dbg_custom(const char *cmd, char *args);   /* mp.c */

/* main-thread call queue */
typedef struct { volatile LONG pending; u32 rva; u64 a[4]; u64 ret; } MainCall;
static MainCall g_mc;
void dbg_frame(void) {       /* called from a main-thread hook */
    if (g_mc.pending == 1) {
        g_mc.ret = FN(u64, g_mc.rva, u64, u64, u64, u64)(g_mc.a[0], g_mc.a[1], g_mc.a[2], g_mc.a[3]);
        LOG("call %x(%llx,%llx,%llx,%llx) = %llx", g_mc.rva, g_mc.a[0], g_mc.a[1], g_mc.a[2], g_mc.a[3], g_mc.ret);
        g_mc.pending = 0;
    }
}
static int readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    const u8 *a = p, *end = a + n;
    while (a < end) {
        if (!VirtualQuery(a, &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mbi.Protect & 0xEE)) return 0;
        a = (u8*)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}
int dbg_readable(const void *p, size_t n) { return readable(p, n); }
static void hexdump(u8 *p, size_t n) {
    char line[160];
    if (!readable(p, n)) { LOG("  %p not readable", p); return; }
    for (size_t i = 0; i < n; i += 16) {
        int k = snprintf(line, sizeof line, "  %p:", p + i);
        for (size_t j = i; j < i + 16 && j < n; j++) k += snprintf(line + k, sizeof line - k, " %02x", p[j]);
        LOG("%s", line);
    }
}
static u8 *addr_of(const char *s) {     /* "x123" = absolute, otherwise rva; "*"prefix = deref pointer at rva, "+off" suffix */
    int deref = 0; u8 *a;
    while (*s == '*') { deref++; s++; }
    char *end;
    if (*s == 'x') a = (u8*)strtoull(s + 1, &end, 16); else a = g_base + strtoull(s, &end, 16);
    while (deref--) { if (!readable(a, 8)) return NULL; a = *(u8**)a; }
    if (*end == '+') a += strtoull(end + 1, NULL, 16);
    return a;
}
static void exec_line(char *l) {
    char cmd[32] = {0}; int n = 0;
    while (*l == ' ') l++;
    if (!*l || *l == '#') return;
    sscanf(l, "%31s%n", cmd, &n); char *args = l + n; while (*args == ' ') args++;
    LOG("> %s %s", cmd, args);
    if (!strcmp(cmd, "r")) {
        char a[64]; unsigned len = 64; sscanf(args, "%63s %x", a, &len);
        u8 *p = addr_of(a); if (p) hexdump(p, len); else LOG("  bad addr");
    } else if (!strcmp(cmd, "w")) {
        char a[64]; int k = 0; sscanf(args, "%63s%n", a, &k); u8 *p = addr_of(a); char *h = args + k;
        if (!p) { LOG("  bad addr"); return; }
        while (*h) {
            while (*h == ' ') h++;
            if (!h[0] || !h[1]) break;
            char b[3] = { h[0], h[1], 0 }; u8 v = (u8)strtoul(b, NULL, 16);
            DWORD old; if (VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) { *p = v; VirtualProtect(p, 1, old, &old); }
            p++; h += 2;
        }
    } else if (!strcmp(cmd, "call")) {
        unsigned rva = 0; unsigned long long a0 = 0, a1 = 0, a2 = 0, a3 = 0;
        sscanf(args, "%x %llx %llx %llx %llx", &rva, &a0, &a1, &a2, &a3);
        g_mc.rva = rva; g_mc.a[0] = a0; g_mc.a[1] = a1; g_mc.a[2] = a2; g_mc.a[3] = a3; g_mc.pending = 1;
        for (int i = 0; i < 300 && g_mc.pending; i++) Sleep(10);
        if (g_mc.pending) { g_mc.pending = 0; LOG("  call not serviced (no main-thread hook running)"); }
    } else if (!strcmp(cmd, "sleep")) {
        Sleep(atoi(args));
    } else dbg_custom(cmd, args);
}
static DWORD WINAPI dbg_thread(LPVOID p) {
    (void)p;
    char path[MAX_PATH + 32], tmp[MAX_PATH + 32];
    snprintf(path, sizeof path, "%sbbskh2_cmd.txt", g_dir);
    snprintf(tmp, sizeof tmp, "%sbbskh2_cmd.run", g_dir);
    for (;;) {
        Sleep(200);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        DeleteFileA(tmp);
        if (!MoveFileA(path, tmp)) continue;
        HANDLE f = CreateFileA(tmp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) continue;
        static char buf[65536]; DWORD got = 0;
        ReadFile(f, buf, sizeof buf - 1, &got, NULL); CloseHandle(f); DeleteFileA(tmp);
        buf[got] = 0;
        char *save = NULL;
        for (char *l = strtok_s(buf, "\r\n", &save); l; l = strtok_s(NULL, "\r\n", &save)) exec_line(l);
        LOG("> done");
    }
    return 0;
}
int g_debug;
void dbg_start(void) {
    char p[MAX_PATH + 32]; snprintf(p, sizeof p, "%sbbskh2_debug.txt", g_dir);
    if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) return;
    g_debug = 1;
    LOG("debug channel enabled");
    CreateThread(NULL, 0, dbg_thread, NULL, 0, NULL);
}

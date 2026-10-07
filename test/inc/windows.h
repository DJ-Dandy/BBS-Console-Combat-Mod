/* minimal Windows shim for running the patcher on Linux */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
typedef void *HANDLE; typedef void *LPVOID; typedef unsigned long DWORD_; typedef uint32_t DWORD; typedef int BOOL; typedef long LONG;
typedef const char *LPCSTR; typedef void *HINSTANCE; typedef void *HMODULE; typedef unsigned int UINT;
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define MAX_PATH 260
#define TRUE 1
#define FALSE 0
#define MEM_RESERVE 0x2000
#define MEM_COMMIT 0x1000
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_READWRITE 4
#define GENERIC_WRITE 0
#define FILE_SHARE_READ 0
#define FILE_SHARE_WRITE 0
#define FILE_SHARE_DELETE 0
#define CREATE_ALWAYS 0
#define FILE_ATTRIBUTE_NORMAL 0
typedef struct { int x; } CRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION *c) { (void)c; }
static inline void EnterCriticalSection(CRITICAL_SECTION *c) { (void)c; }
static inline void LeaveCriticalSection(CRITICAL_SECTION *c) { (void)c; }
static inline HANDLE CreateFileA(const char *p, int a, int b, void *c, int d, int e, void *f) { (void)p;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f; return (HANDLE)stderr; }
static inline BOOL WriteFile(HANDLE h, const void *b, DWORD n, DWORD *w, void *o) { (void)o; *w = (DWORD)fwrite(b, 1, n, (FILE*)h); return 1; }
static inline DWORD GetTickCount(void) { return 0; }
static inline void *VirtualAlloc(void *addr, size_t n, int t, int p) {
    (void)t; (void)p;
    void *r = mmap(addr, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | (addr ? MAP_FIXED_NOREPLACE : 0), -1, 0);
    return r == MAP_FAILED ? NULL : r;
}
static inline BOOL VirtualProtect(void *a, size_t n, DWORD p, DWORD *old) { (void)a;(void)n;(void)p; *old = 0; return 1; }
static inline HANDLE GetCurrentProcess(void) { return NULL; }
static inline BOOL FlushInstructionCache(HANDLE h, const void *a, size_t n) { (void)h;(void)a;(void)n; return 1; }
typedef struct { uint16_t e_magic; uint8_t pad[58]; int32_t e_lfanew; } IMAGE_DOS_HEADER;
typedef struct { uint32_t Signature; uint8_t FileHeader[20]; struct { uint8_t pad[56]; uint32_t SizeOfImage; } OptionalHeader; } IMAGE_NT_HEADERS;
static inline BOOL WritePrivateProfileStringA(const char *s, const char *k, const char *v, const char *file) { (void)s; (void)k; (void)v; (void)file; return 1; }
static inline DWORD GetPrivateProfileStringA(const char *s, const char *k, const char *def, char *out, DWORD n, const char *file) {
    (void)s; (void)k; (void)file; snprintf(out, n, "%s", def); return (DWORD)strlen(out);
}

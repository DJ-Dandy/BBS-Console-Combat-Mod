#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#define MSABI
#else
#define MSABI __attribute__((ms_abi))
#endif
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;

extern u8 *g_base;                 /* exe image base */
#define RVA(x) ((void*)(g_base + (x)))
#define G(type, rva) (*(type*)(g_base + (rva)))
#define FN(ret, rva, ...) ((ret(MSABI *)(__VA_ARGS__))(g_base + (rva)))

void logf_(const char *fmt, ...);
#define LOG(...) logf_(__VA_ARGS__)

/* near memory (within +-2GB of the exe image) */
void *near_alloc(size_t n);

/* raw byte patch with verification; returns 1 on success */
int patch_bytes(u32 rva, const u8 *expect, const u8 *repl, size_t n, const char *name);
int patch_u32(u32 rva, u32 expect, u32 repl, const char *name);
int patch_u8(u32 rva, u8 expect, u8 repl, const char *name);
/* re-point a RIP-relative disp32 located at disp_rva (instruction ends at disp_rva+4+tail) from old target to new absolute address */
int patch_riprel(u32 disp_rva, u32 tail, u32 old_target_rva, void *new_target, const char *name);

/* register context passed to mid-function hooks */
typedef struct Ctx {
    u8  xmm[6][16];
    u64 r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax;
    u64 rflags;
    /* original rsp at hook site = (u64)(&rflags + 1) */
} Ctx;
#define CTX_RSP(c) ((u64)((c) + 1))
/* handler returns 0 to continue (stolen instructions run, then back to site+len),
   or an absolute address to jump to instead (stolen instructions are skipped). */
typedef u64 (MSABI *CtxHook)(Ctx *c);

/* description of stolen bytes */
typedef struct Steal {
    u32 rva;            /* hook site */
    u8  len;            /* bytes stolen (>=5), instruction aligned */
    u8  nfix;           /* number of rip-relative/rel32 fixups */
    u8  fix[4];         /* offsets (within the stolen bytes) of disp32 fields */
    u8  fixend[4];      /* offset of the end of the instruction owning each disp32 */
    u8  bytes[24];      /* expected original bytes */
} Steal;

int hook_ctx(const Steal *s, CtxHook h, const char *name);
/* function-entry hook; returns trampoline to call the original (NULL on failure) */
void *hook_fn(const Steal *s, void *replacement, const char *name);

/* redirect a direct call (E8 rel32) at call_rva whose original target is old_target_rva */
int hook_call(u32 call_rva, u32 old_target_rva, void *replacement, const char *name);
extern int g_patch_errors;

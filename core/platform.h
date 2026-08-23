/* DOS Mobile core — freestanding platform layer (no libc).
 * Builds for wasm32 (clang --target=wasm32 -nostdlib) and natively for test harnesses. */
#pragma once

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef short s16;
typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;
typedef long long s64;
typedef __SIZE_TYPE__ usize;

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifdef __wasm__
#define EXPORT(n) __attribute__((export_name(n)))
#define IMPORT(n) __attribute__((import_module("env"), import_name(n)))
#else
#define EXPORT(n)
#define IMPORT(n)
#endif

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define INLINE static inline __attribute__((always_inline))
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Unaligned little-endian access helpers (wasm and x86 are both little-endian). */
typedef u16 __attribute__((aligned(1))) u16u;
typedef u32 __attribute__((aligned(1))) u32u;
typedef u64 __attribute__((aligned(1))) u64u;
INLINE u16 ld16(const void *p) { return *(const u16u *)p; }
INLINE u32 ld32(const void *p) { return *(const u32u *)p; }
INLINE void st16(void *p, u16 v) { *(u16u *)p = v; }
INLINE void st32(void *p, u32 v) { *(u32u *)p = v; }

/* ---- host services (imported from the JS worker, or provided by the native harness) ---- */
IMPORT("host_log") void host_log(const char *s, int len);
/* Disk sectors are served by the host (images live in the worker's chunk cache). 0 = ok. */
IMPORT("host_disk_read") int host_disk_read(int drive, u32 lba, u32 count, void *dst);
IMPORT("host_disk_write") int host_disk_write(int drive, u32 lba, u32 count, const void *src);

/* ---- memory ---- */
void *dm_alloc(u32 size); /* zeroed bump allocation; never freed */
INLINE void dm_memcpy(void *d, const void *s, usize n) { __builtin_memcpy(d, s, n); }
INLINE void dm_memset(void *d, int v, usize n) { __builtin_memset(d, v, n); }
INLINE void dm_memmove(void *d, const void *s, usize n) { __builtin_memmove(d, s, n); }

/* ---- logging (tiny printf: %d %u %x %X %s %c %% with optional 0-pad width, %llx/%lld) ---- */
void dm_log(const char *fmt, ...);
int dm_fmt(char *out, int cap, const char *fmt, __builtin_va_list ap);
void dm_panic(const char *msg); /* logs and halts the machine */
int dm_strlen(const char *s);

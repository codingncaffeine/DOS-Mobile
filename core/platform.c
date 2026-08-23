#include "platform.h"

/* ---------------- allocation ---------------- */
#ifdef __wasm__
static u8 *heap_cur, *heap_end;
static const u32 WASM_PAGE = 65536;

void *dm_alloc(u32 size) {
  size = (size + 15) & ~15u;
  if (!heap_cur) {
    u32 pages = __builtin_wasm_memory_size(0);
    heap_cur = (u8 *)(__builtin_wasm_memory_grow(0, 0) * WASM_PAGE); /* current end */
    (void)pages;
    heap_end = heap_cur;
  }
  if ((u32)(heap_end - heap_cur) < size) {
    u32 need = size - (u32)(heap_end - heap_cur);
    u32 pages = (need + WASM_PAGE - 1) / WASM_PAGE;
    if (__builtin_wasm_memory_grow(0, pages) == (usize)-1) dm_panic("out of wasm memory");
    heap_end += pages * WASM_PAGE;
  }
  u8 *p = heap_cur;
  heap_cur += size;
  dm_memset(p, 0, size);
  return p;
}
#else
#include <stdlib.h>
#include <string.h>
void *dm_alloc(u32 size) {
  void *p = calloc(1, size ? size : 1);
  if (!p) dm_panic("out of memory");
  return p;
}
#endif

int dm_strlen(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

/* ---------------- formatting ---------------- */
static int put_num(char *out, int cap, int pos, u64 v, int base, int upper, int width, int zero, int neg) {
  char tmp[24];
  int n = 0;
  if (v == 0) tmp[n++] = '0';
  while (v) {
    int d = (int)(v % base);
    tmp[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
    v /= base;
  }
  if (neg) tmp[n++] = '-';
  int pad = width - n;
  while (pad-- > 0) {
    if (pos < cap) out[pos] = zero ? '0' : ' ';
    pos++;
  }
  while (n--) {
    if (pos < cap) out[pos] = tmp[n];
    pos++;
  }
  return pos;
}

int dm_fmt(char *out, int cap, const char *fmt, __builtin_va_list ap) {
  int pos = 0;
  for (const char *p = fmt; *p; p++) {
    if (*p != '%') {
      if (pos < cap) out[pos] = *p;
      pos++;
      continue;
    }
    p++;
    int zero = 0, width = 0, ll = 0;
    if (*p == '0') { zero = 1; p++; }
    while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
    while (*p == 'l') { ll++; p++; }
    switch (*p) {
      case 'd': {
        s64 v = ll >= 2 ? __builtin_va_arg(ap, s64) : (s64)__builtin_va_arg(ap, int);
        int neg = v < 0;
        pos = put_num(out, cap, pos, (u64)(neg ? -v : v), 10, 0, width, zero, neg);
        break;
      }
      case 'u': {
        u64 v = ll >= 2 ? __builtin_va_arg(ap, u64) : (u64)__builtin_va_arg(ap, unsigned);
        pos = put_num(out, cap, pos, v, 10, 0, width, zero, 0);
        break;
      }
      case 'x':
      case 'X': {
        u64 v = ll >= 2 ? __builtin_va_arg(ap, u64) : (u64)__builtin_va_arg(ap, unsigned);
        pos = put_num(out, cap, pos, v, 16, *p == 'X', width, zero, 0);
        break;
      }
      case 'c': {
        int ch = __builtin_va_arg(ap, int);
        if (pos < cap) out[pos] = (char)ch;
        pos++;
        break;
      }
      case 's': {
        const char *s = __builtin_va_arg(ap, const char *);
        if (!s) s = "(null)";
        while (*s) {
          if (pos < cap) out[pos] = *s;
          pos++;
          s++;
        }
        break;
      }
      case '%':
        if (pos < cap) out[pos] = '%';
        pos++;
        break;
      default:
        if (pos < cap) out[pos] = '?';
        pos++;
        break;
    }
  }
  if (pos < cap) out[pos] = 0;
  else if (cap > 0) out[cap - 1] = 0;
  return pos;
}

void dm_log(const char *fmt, ...) {
  char buf[512];
  __builtin_va_list ap;
  __builtin_va_start(ap, fmt);
  int n = dm_fmt(buf, sizeof buf, fmt, ap);
  __builtin_va_end(ap);
  if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
  host_log(buf, n);
}

extern void machine_fatal(void);
void dm_panic(const char *msg) {
  dm_log("PANIC: %s", msg);
  machine_fatal();
}

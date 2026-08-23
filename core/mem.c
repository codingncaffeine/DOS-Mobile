#include "mem.h"

u8 *ram;
u32 ram_size;
u32 a20_mask = 0xFFFFFFFFu;

void mem_init(u32 ram_bytes) {
  if (ram_bytes < 0x100000) ram_bytes = 0x100000;
  ram_size = ram_bytes;
  ram = (u8 *)dm_alloc(ram_bytes);
  /* Upper memory area reads as 0xFF (no option ROMs) until something maps it. */
  dm_memset(ram + 0xC0000, 0xFF, 0x30000);
}

extern void tlb_flush(void);
void mem_set_a20(int enabled) {
  u32 m = enabled ? 0xFFFFFFFFu : ~0x100000u;
  if (m != a20_mask) { a20_mask = m; tlb_flush(); }
}

#define IS_VGA(a) ((a) >= 0xA0000 && (a) < 0xC0000)
#define IS_ROM(a) ((a) >= 0xF0000 && (a) < 0x100000)

u8 mem_rd8(u32 a) {
  a &= a20_mask;
  if (LIKELY(a < ram_size)) {
    if (UNLIKELY(IS_VGA(a))) return vga_mem_rd(a);
    return ram[a];
  }
  return 0xFF;
}

u16 mem_rd16(u32 a) {
  a &= a20_mask;
  if (LIKELY(a + 1 < ram_size && !IS_VGA(a) && !IS_VGA(a + 1))) return ld16(ram + a);
  return (u16)(mem_rd8(a) | (mem_rd8(a + 1) << 8));
}

u32 mem_rd32(u32 a) {
  a &= a20_mask;
  if (LIKELY(a + 3 < ram_size && !IS_VGA(a) && !IS_VGA(a + 3))) return ld32(ram + a);
  return (u32)mem_rd8(a) | ((u32)mem_rd8(a + 1) << 8) | ((u32)mem_rd8(a + 2) << 16) |
         ((u32)mem_rd8(a + 3) << 24);
}

void mem_wr8(u32 a, u8 v) {
  a &= a20_mask;
  if (LIKELY(a < ram_size)) {
    if (UNLIKELY(IS_VGA(a))) { vga_mem_wr(a, v); return; }
    if (UNLIKELY(a >= 0xC0000 && a < 0x100000)) return; /* ROM / unmapped UMB */
    ram[a] = v;
  }
}

void mem_wr16(u32 a, u16 v) {
  a &= a20_mask;
  if (LIKELY(a + 1 < ram_size && (a + 1 < 0xA0000 || a >= 0x100000))) { st16(ram + a, v); return; }
  mem_wr8(a, (u8)v);
  mem_wr8(a + 1, (u8)(v >> 8));
}

void mem_wr32(u32 a, u32 v) {
  a &= a20_mask;
  if (LIKELY(a + 3 < ram_size && (a + 3 < 0xA0000 || a >= 0x100000))) { st32(ram + a, v); return; }
  mem_wr8(a, (u8)v);
  mem_wr8(a + 1, (u8)(v >> 8));
  mem_wr8(a + 2, (u8)(v >> 16));
  mem_wr8(a + 3, (u8)(v >> 24));
}

void mem_copy_in(u32 addr, const void *src, u32 n) {
  const u8 *s = (const u8 *)src;
  for (u32 i = 0; i < n; i++) mem_wr8(addr + i, s[i]);
}

void mem_copy_out(void *dst, u32 addr, u32 n) {
  u8 *d = (u8 *)dst;
  for (u32 i = 0; i < n; i++) d[i] = mem_rd8(addr + i);
}

void mem_fill(u32 addr, u8 v, u32 n) {
  for (u32 i = 0; i < n; i++) mem_wr8(addr + i, v);
}

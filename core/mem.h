/* Physical memory map:
 *   00000-9FFFF  conventional RAM (640 KB)
 *   A0000-BFFFF  video memory window (VGA)
 *   C0000-EFFFF  upper memory: option ROM / UMB space (reads 0xFF unless mapped)
 *   F0000-FFFFF  system BIOS ROM (write-protected)
 *   100000-...   extended RAM */
#pragma once
#include "platform.h"

extern u8 *ram;        /* covers [0, ram_size) with the ROM mirrored in at F0000 */
extern u32 ram_size;   /* total installed RAM in bytes (>= 1 MB) */
extern u32 a20_mask;   /* 0xFFFFFFFF when the A20 gate is open, ~0x100000 when closed */

void mem_init(u32 ram_bytes);
void mem_set_a20(int enabled);

/* Physical accessors (after A20 masking). */
u8 mem_rd8(u32 addr);
u16 mem_rd16(u32 addr);
u32 mem_rd32(u32 addr);
void mem_wr8(u32 addr, u8 v);
void mem_wr16(u32 addr, u16 v);
void mem_wr32(u32 addr, u32 v);

/* Bulk helpers for the BIOS / disk code (physical, no MMIO side effects checked beyond ranges). */
void mem_copy_in(u32 addr, const void *src, u32 n);
void mem_copy_out(void *dst, u32 addr, u32 n);
void mem_fill(u32 addr, u8 v, u32 n);

/* VGA memory window hooks implemented in vga.c */
u8 vga_mem_rd(u32 addr);
void vga_mem_wr(u32 addr, u8 v);

/* Linear → physical translation (paging) with a TLB, and linear-address accessors. */
#pragma once
#include "platform.h"

void tlb_flush(void);
void tlb_flush_page(u32 lin);
/* Returns 1 and fills *phys; on a page fault raises #PF (error code + CR2) and returns 0. */
int lin_translate(u32 lin, int write, int user, u32 *phys);

u8 lin_rd8_slow(u32 lin);
u16 lin_rd16_slow(u32 lin);
u32 lin_rd32_slow(u32 lin);
void lin_wr8_slow(u32 lin, u8 v);
void lin_wr16_slow(u32 lin, u16 v);
void lin_wr32_slow(u32 lin, u32 v);
int lin_probe_write_slow(u32 lin, u32 size);
/* Prepare the instruction-fetch page cache for a linear address; returns 0 on fault. */
int fetch_page_prepare(u32 lin);
/* bulk guest copies through linear addresses (BIOS buffers, disk transfers) */
void lin_copy_in(u32 lin, const void *src, u32 n);
void lin_copy_out(void *dst, u32 lin, u32 n);

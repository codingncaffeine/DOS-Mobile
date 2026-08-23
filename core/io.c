#include "io.h"
#include "sched.h"

static io_rd_fn rd_tab[65536];
static io_wr_fn wr_tab[65536];

static u8 rd_none(u16 port) { (void)port; return 0xFF; }
static void wr_none(u16 port, u8 v) { (void)port; (void)v; }

void io_init(void) {
  for (int i = 0; i < 65536; i++) { rd_tab[i] = rd_none; wr_tab[i] = wr_none; }
}

void io_register(u16 base, u16 count, io_rd_fn rd, io_wr_fn wr) {
  for (u32 p = base; p < (u32)base + count && p < 65536; p++) {
    if (rd) rd_tab[p] = rd;
    if (wr) wr_tab[p] = wr;
  }
}

/* ISA bus I/O is slow on every generation: charge ~1 µs per byte access. */
u8 io_rd8(u16 port) { sched_io_cost(); return rd_tab[port](port); }
void io_wr8(u16 port, u8 v) { sched_io_cost(); wr_tab[port](port, v); }
u16 io_rd16(u16 port) { return (u16)(io_rd8(port) | (io_rd8(port + 1) << 8)); }
u32 io_rd32(u16 port) { return (u32)io_rd16(port) | ((u32)io_rd16(port + 2) << 16); }
void io_wr16(u16 port, u16 v) { io_wr8(port, (u8)v); io_wr8(port + 1, (u8)(v >> 8)); }
void io_wr32(u16 port, u32 v) { io_wr16(port, (u16)v); io_wr16(port + 2, (u16)(v >> 16)); }

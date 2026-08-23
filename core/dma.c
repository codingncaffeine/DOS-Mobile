#include "dma.h"
#include "io.h"
#include "mem.h"

typedef struct {
  u16 base_addr, base_count;
  u16 cur_addr, cur_count;
  u8 mode;
  u8 page;
} DmaChan;

typedef struct {
  DmaChan ch[4];
  u8 flipflop;
  u8 mask;
  u8 status;
} DmaCtl;

static DmaCtl dma[2]; /* [0] = 8-bit channels 0-3, [1] = 16-bit channels 4-7 */

void dma_init(void) {
  dm_memset(dma, 0, sizeof dma);
  dma[0].mask = 0x0F;
  dma[1].mask = 0x0F;
}

static const u8 page_reg_chan[16] = {8, 2, 3, 1, 8, 8, 8, 0, 8, 6, 7, 5, 8, 8, 8, 8}; /* 0x80+n -> channel */

static u8 rd_dma(u16 port) {
  if (port >= 0x80 && port <= 0x8F) {
    u8 c = page_reg_chan[port - 0x80];
    if (c < 8) return dma[c >> 2].ch[c & 3].page;
    return 0;
  }
  int c2 = port >= 0xC0;
  DmaCtl *d = &dma[c2];
  u16 p = c2 ? (u16)((port - 0xC0) >> 1) : port;
  if (p < 8) {
    DmaChan *ch = &d->ch[p >> 1];
    u16 v = (p & 1) ? ch->cur_count : ch->cur_addr;
    u8 out = d->flipflop ? (u8)(v >> 8) : (u8)v;
    d->flipflop ^= 1;
    return out;
  }
  if (p == 8) { u8 s = d->status; d->status &= 0xF0; return s; }
  return 0;
}

static void wr_dma(u16 port, u8 v) {
  if (port >= 0x80 && port <= 0x8F) {
    u8 c = page_reg_chan[port - 0x80];
    if (c < 8) dma[c >> 2].ch[c & 3].page = v;
    return;
  }
  int c2 = port >= 0xC0;
  DmaCtl *d = &dma[c2];
  u16 p = c2 ? (u16)((port - 0xC0) >> 1) : port;
  if (p < 8) {
    DmaChan *ch = &d->ch[p >> 1];
    if (p & 1) {
      if (d->flipflop) ch->base_count = (u16)((ch->base_count & 0x00FF) | (v << 8));
      else ch->base_count = (u16)((ch->base_count & 0xFF00) | v);
      ch->cur_count = ch->base_count;
    } else {
      if (d->flipflop) ch->base_addr = (u16)((ch->base_addr & 0x00FF) | (v << 8));
      else ch->base_addr = (u16)((ch->base_addr & 0xFF00) | v);
      ch->cur_addr = ch->base_addr;
    }
    d->flipflop ^= 1;
    return;
  }
  switch (p) {
    case 8: break;                        /* command */
    case 9: break;                        /* request */
    case 10: {                            /* single mask */
      u8 c = v & 3;
      if (v & 4) d->mask |= (u8)(1 << c);
      else d->mask &= (u8)~(1 << c);
      break;
    }
    case 11: d->ch[v & 3].mode = v; break;
    case 12: d->flipflop = 0; break;
    case 13: d->flipflop = 0; d->mask = 0x0F; d->status = 0; break; /* master reset */
    case 14: d->mask = 0; break;
    case 15: d->mask = v & 0x0F; break;
    default: break;
  }
}

void dma_register_ports(void) {
  io_register(0x00, 16, rd_dma, wr_dma);
  io_register(0x80, 16, rd_dma, wr_dma);
  io_register(0xC0, 32, rd_dma, wr_dma);
}

int dma_channel_masked(int channel) {
  return (dma[channel >> 2].mask >> (channel & 3)) & 1;
}

u32 dma_channel_remaining(int channel) {
  DmaChan *ch = &dma[channel >> 2].ch[channel & 3];
  u32 units = (u32)ch->cur_count + 1;
  return channel >= 4 ? units * 2 : units;
}

int dma_channel_autoinit(int channel) {
  return (dma[channel >> 2].ch[channel & 3].mode >> 4) & 1;
}

u32 dma_device_read(int channel, u8 *dst, u32 bytes, int *tc) {
  *tc = 0;
  if (dma_channel_masked(channel)) return 0;
  DmaCtl *d = &dma[channel >> 2];
  DmaChan *ch = &d->ch[channel & 3];
  int wide = channel >= 4;
  u32 unit = wide ? 2u : 1u;
  u32 done = 0;
  while (done + unit <= bytes) {
    u32 phys = wide ? (((u32)(ch->page & 0xFE) << 16) | ((u32)ch->cur_addr << 1))
                    : (((u32)ch->page << 16) | ch->cur_addr);
    dst[done] = mem_rd8(phys);
    if (wide) dst[done + 1] = mem_rd8(phys + 1);
    done += unit;
    ch->cur_addr++;
    int at_tc = ch->cur_count == 0;
    ch->cur_count--;
    if (at_tc) {
      d->status |= (u8)(1 << (channel & 3));
      *tc = 1;
      if ((ch->mode >> 4) & 1) { ch->cur_addr = ch->base_addr; ch->cur_count = ch->base_count; }
      else { d->mask |= (u8)(1 << (channel & 3)); break; }
    }
  }
  return done;
}

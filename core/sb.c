#include "sb.h"
#include "io.h"
#include "opl.h"
#include "dma.h"
#include "pic.h"
#include "sched.h"

#define SB_BASE 0x220
#define SB_IRQ 5
#define SB_DMA8 1
#define SB_DMA16 5
#define OUT_RATE 48000

typedef struct {
  /* DSP command engine */
  u8 in_buf[8];
  u8 in_need, in_have, cmd;
  u8 out_buf[8];
  u8 out_head, out_count;
  u8 speaker_on;
  u8 test_reg;
  /* playback voice */
  u8 active;        /* 0 idle, 8 = 8-bit DMA, 16 = 16-bit DMA */
  u8 autoinit;
  u8 stereo;        /* SB16 mode bit or SBPro mixer bit */
  u8 signed_fmt;
  u32 rate;         /* samples/sec */
  u32 block_bytes;  /* transfer length in bytes */
  u32 phase;        /* 16.16 source position within the fetched pair */
  s32 last_l, last_r, next_l, next_r;
  u32 remain;       /* bytes left in the current block */
  u8 paused;
  u8 direct_dac;    /* last direct-DAC sample (command 10h) */
  s64 direct_ns;
  /* mixer */
  u8 mixer_idx;
  u8 mixer[256];
} SB;

static SB sb;

static void out_push(u8 v) { if (sb.out_count < 8) sb.out_buf[(sb.out_head + sb.out_count++) & 7] = v; }
static u8 out_pop(void) {
  if (!sb.out_count) return 0xFF;
  u8 v = sb.out_buf[sb.out_head & 7];
  sb.out_head++;
  sb.out_count--;
  return v;
}

static void voice_start(int bits, int autoinit, int stereo, int sign, u32 bytes) {
  sb.active = (u8)bits;
  sb.autoinit = (u8)autoinit;
  sb.stereo = (u8)stereo;
  sb.signed_fmt = (u8)sign;
  sb.block_bytes = bytes;
  sb.remain = bytes;
  sb.paused = 0;
  sb.phase = 0x10000; /* force an immediate fetch */
}

static void dsp_command(u8 cmd);

static void dsp_write(u8 v) {
  if (sb.in_need) {
    sb.in_buf[sb.in_have++] = v;
    if (sb.in_have < sb.in_need) return;
    sb.in_need = 0;
    u8 c = sb.cmd;
    u8 *a = sb.in_buf;
    switch (c) {
      case 0x10: sb.direct_dac = v; sb.direct_ns = emu_now_ns(); break;
      case 0x40: sb.rate = 1000000u / (256 - a[0]); if (sb.stereo) sb.rate /= 2; break;
      case 0x41: case 0x42: sb.rate = (u32)((a[0] << 8) | a[1]); break;
      case 0x48: sb.block_bytes = (u32)(a[0] | (a[1] << 8)) + 1; break;
      case 0x14: case 0x91: voice_start(8, 0, (sb.mixer[0x0E] >> 1) & 1, 0, (u32)(a[0] | (a[1] << 8)) + 1); break;
      case 0x1C: voice_start(8, 1, (sb.mixer[0x0E] >> 1) & 1, 0, sb.block_bytes); break;
      case 0x80: /* silence block: just raise the IRQ after the duration */
        pic_raise_irq(SB_IRQ);
        break;
      case 0xE0: out_push((u8)~v); break;
      case 0xE4: sb.test_reg = v; break;
      default:
        if ((c & 0xF0) == 0xB0 || (c & 0xF0) == 0xC0) { /* SB16 program DMA: mode + len */
          int bits = (c & 0xF0) == 0xB0 ? 16 : 8;
          int autoinit = (c >> 2) & 1;
          int stereo = (a[0] >> 5) & 1;
          int sign = (a[0] >> 4) & 1;
          u32 samples = (u32)(a[1] | (a[2] << 8)) + 1;
          u32 bytes = samples * (bits / 8);
          voice_start(bits, autoinit, stereo, sign, bytes);
        }
        break;
    }
    return;
  }
  dsp_command(v);
}

extern int cpu_trace_faults;
static void dsp_command(u8 cmd) {
  if (cpu_trace_faults >= 2) dm_log("SB DSP cmd %02x", cmd);
  sb.cmd = cmd;
  sb.in_have = 0;
  switch (cmd) {
    case 0x10: sb.in_need = 1; break;
    case 0x14: case 0x91: sb.in_need = 2; break;
    case 0x1C: voice_start(8, 1, (sb.mixer[0x0E] >> 1) & 1, 0, sb.block_bytes); break;
    case 0x24: sb.in_need = 2; break; /* ADC: accept and ignore */
    case 0x40: sb.in_need = 1; break;
    case 0x41: case 0x42: sb.in_need = 2; break;
    case 0x48: sb.in_need = 2; break;
    case 0x80: sb.in_need = 2; break;
    case 0xD0: case 0xD5: sb.paused = 1; break;
    case 0xD4: case 0xD6: sb.paused = 0; break;
    case 0xD9: case 0xDA: sb.autoinit = 0; break;
    case 0xD1: sb.speaker_on = 1; break;
    case 0xD3: sb.speaker_on = 0; break;
    case 0xD8: out_push(sb.speaker_on ? 0xFF : 0x00); break;
    case 0xE0: sb.in_need = 1; break;
    case 0xE1: out_push(4); out_push(5); break;
    case 0xE4: sb.in_need = 1; break;
    case 0xE8: out_push(sb.test_reg); break;
    case 0xF2: pic_raise_irq(SB_IRQ); sb.mixer[0x82] |= 1; break;
    case 0xF8: out_push(0); break;
    default:
      if ((cmd & 0xF0) == 0xB0 || (cmd & 0xF0) == 0xC0) sb.in_need = 3;
      break;
  }
}

/* ---------------- mixer ---------------- */
static u8 mixer_read(u8 idx) {
  switch (idx) {
    case 0x80: return 0x02; /* IRQ 5 */
    case 0x81: return (u8)((1 << SB_DMA8) | (1 << SB_DMA16)); /* DMA 1 + 5 */
    case 0x82: return sb.mixer[0x82];
    default: return sb.mixer[idx];
  }
}

/* ---------------- ports ---------------- */
static u8 sb_rd(u16 port) {
  switch (port - SB_BASE) {
    case 0x0: case 0x2: return opl_status(); /* FM status mirrors */
    case 0x5: return mixer_read(sb.mixer_idx);
    case 0x8: return opl_status();
    case 0xA: return out_pop();
    case 0xC: return 0x00; /* write-buffer status: always ready */
    case 0xE: /* read-buffer status + 8-bit IRQ ack */
      sb.mixer[0x82] &= (u8)~1;
      pic_lower_irq(SB_IRQ);
      return sb.out_count ? 0x80 : 0x00;
    case 0xF: /* 16-bit IRQ ack */
      sb.mixer[0x82] &= (u8)~2;
      pic_lower_irq(SB_IRQ);
      return 0;
    default: return 0xFF;
  }
}

static void sb_wr(u16 port, u8 v) {
  switch (port - SB_BASE) {
    case 0x0: opl_addr(0, v); break;
    case 0x1: opl_data(v); break;
    case 0x2: opl_addr(1, v); break;
    case 0x3: opl_data(v); break;
    case 0x4: sb.mixer_idx = v; break;
    case 0x5:
      if (sb.mixer_idx == 0) { dm_memset(sb.mixer, 0, sizeof sb.mixer); sb.mixer[0x22] = 0xCC; sb.mixer[0x04] = 0xCC; }
      else if (sb.mixer_idx != 0x82) sb.mixer[sb.mixer_idx] = v;
      break;
    case 0x8: opl_addr(0, v); break;
    case 0x9: opl_data(v); break;
    case 0x6: /* DSP reset */
      if (v & 1) { sb.active = 0; sb.out_count = 0; sb.in_need = 0; }
      else { sb.out_count = 0; out_push(0xAA); }
      break;
    case 0xC: dsp_write(v); break;
    default: break;
  }
}

/* FM at 388h-38Bh */
static u8 fm_rd(u16 port) { (void)port; return opl_status(); }
static void fm_wr(u16 port, u8 v) {
  switch (port & 3) {
    case 0: opl_addr(0, v); break;
    case 1: case 3: opl_data(v); break;
    case 2: opl_addr(1, v); break;
  }
}

/* MPU-401 UART detection stub */
static u8 mpu_out[4];
static u8 mpu_count;
static u8 mpu_rd(u16 port) {
  if (port & 1) return mpu_count ? 0x40 : 0x80; /* status: bit7 = no data (0 when data ready) */
  if (mpu_count) return mpu_out[--mpu_count];
  return 0xFE;
}
static void mpu_wr(u16 port, u8 v) {
  if (port & 1) { /* command */
    if (v == 0xFF || v == 0x3F) { mpu_out[0] = 0xFE; mpu_count = 1; }
  }
  /* data port: MIDI bytes — forwarded to a host synthesiser in a later phase */
}

void sb_register_ports(void) {
  io_register(SB_BASE, 16, sb_rd, sb_wr);
  io_register(0x388, 4, fm_rd, fm_wr);
  io_register(0x330, 2, mpu_rd, mpu_wr);
}

void sb_init(void) {
  dm_memset(&sb, 0, sizeof sb);
  sb.rate = 11025;
  sb.block_bytes = 2048;
  sb.mixer[0x22] = 0xCC;
  sb.mixer[0x04] = 0xCC;
  opl_init();
}

/* ---------------- rendering ---------------- */
static int fetch_frame(void) {
  /* pull one source frame (1 or 2 samples) from DMA into next_l/next_r */
  int chan = sb.active == 16 ? SB_DMA16 : SB_DMA8;
  u8 buf[4];
  u32 need = (u32)((sb.active == 16 ? 2 : 1) * (sb.stereo ? 2 : 1));
  int tc = 0;
  u32 got = dma_device_read(chan, buf, need, &tc);
  if (got < need) {
    if (tc) goto ended;
    return 0;
  }
  if (sb.active == 16) {
    s16 l = (s16)(buf[0] | (buf[1] << 8));
    s16 r = sb.stereo ? (s16)(buf[2] | (buf[3] << 8)) : l;
    if (!sb.signed_fmt) { l = (s16)((u16)l ^ 0x8000); r = (s16)((u16)r ^ 0x8000); }
    sb.next_l = l;
    sb.next_r = r;
  } else {
    int l = buf[0], r = sb.stereo ? buf[1] : buf[0];
    if (sb.signed_fmt) { sb.next_l = (s8)l * 256; sb.next_r = (s8)r * 256; }
    else { sb.next_l = (l - 128) * 256; sb.next_r = (r - 128) * 256; }
  }
  if (sb.remain > need) sb.remain -= need;
  else {
    sb.remain = 0;
  ended:
    /* block complete */
    if (sb.active == 16) sb.mixer[0x82] |= 2; else sb.mixer[0x82] |= 1;
    pic_raise_irq(SB_IRQ);
    if (sb.autoinit) sb.remain = sb.block_bytes;
    else if (sb.remain == 0) sb.active = 0;
  }
  return 1;
}

void sb_render(s32 *dst, u32 frames) {
  /* direct DAC (command 10h): hold the level for a short while */
  if (!sb.active) {
    if (sb.speaker_on && emu_ns - sb.direct_ns < 100000000LL && sb.direct_ns) {
      s32 v = ((s32)sb.direct_dac - 128) * 128;
      for (u32 f = 0; f < frames; f++) { dst[f * 2] += v; dst[f * 2 + 1] += v; }
    }
    return;
  }
  if (sb.paused || !sb.rate) return;
  u32 step = (u32)(((u64)sb.rate << 16) / OUT_RATE);
  for (u32 f = 0; f < frames; f++) {
    sb.phase += step;
    while (sb.phase >= 0x10000) {
      sb.phase -= 0x10000;
      sb.last_l = sb.next_l;
      sb.last_r = sb.next_r;
      if (!fetch_frame()) { sb.phase = 0; break; }
      if (!sb.active) break;
    }
    /* linear interpolation between last and next */
    s32 l = sb.last_l + (((sb.next_l - sb.last_l) * (s32)(sb.phase >> 8)) >> 8);
    s32 r = sb.last_r + (((sb.next_r - sb.last_r) * (s32)(sb.phase >> 8)) >> 8);
    dst[f * 2] += l;
    dst[f * 2 + 1] += r;
    if (!sb.active) break;
  }
}

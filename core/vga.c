#include "vga.h"
#include "io.h"
#include "sched.h"
#include "mem.h"
#include "mouse.h"

#define FB_MAX_W 1280
#define FB_MAX_H 1024

typedef struct {
  u8 plane[4][65536];
  u8 misc_out;
  u8 seq_idx, seq[8];
  u8 crtc_idx, crtc[32];
  u8 gc_idx, gc[16];
  u8 attr_idx, attr[32], attr_flip, attr_pas;
  u8 dac_mask, dac_read_idx, dac_write_idx, dac_sub, dac_state;
  u8 dac[256][3];
  u32 dac_rgb[256];
  u8 feature;
  u8 cga_mode, cga_color;
  u8 latch[4];
  u32 *fb;
  int fb_w, fb_h;
  u32 frame_id;
  u32 frame_count;
  s64 frame_start_ns;
  s64 frame_ns, line_ns;
  int total_lines, visible_lines;
} VGA;

static VGA vga;

/* ---------------- timing ---------------- */
static void vga_frame_event(void);

static void update_timing(void) {
  /* 400-line modes run at 70 Hz (449 total lines), 350/480-line modes at 60 Hz. */
  int vde = vga.crtc[0x12] | ((vga.crtc[7] & 2) << 7) | ((vga.crtc[7] & 0x40) << 3);
  int vtotal = vga.crtc[6] | ((vga.crtc[7] & 1) << 8) | ((vga.crtc[7] & 0x20) << 4);
  vga.visible_lines = vde + 1;
  vga.total_lines = vtotal + 2;
  if (vga.total_lines < vga.visible_lines + 1) vga.total_lines = vga.visible_lines + 1;
  int hz = (vga.visible_lines <= 400 && vga.total_lines < 500) ? 70 : 60;
  vga.frame_ns = 1000000000LL / hz;
  vga.line_ns = vga.frame_ns / vga.total_lines;
}

static int current_line(void) {
  s64 t = emu_ns - vga.frame_start_ns;
  if (t < 0) return 0;
  s64 l = t / vga.line_ns;
  return l >= vga.total_lines ? vga.total_lines - 1 : (int)l;
}

static void vga_frame_event(void) {
  vga.frame_start_ns = emu_ns;
  vga.frame_count++;
  vga_render_frame();
  vga.frame_id++;
  sched_set(EV_VGA, vga.frame_start_ns + vga.frame_ns, vga_frame_event);
}

/* ---------------- DAC ---------------- */
static void dac_update(int i) {
  u8 r = vga.dac[i][0] & 63, g = vga.dac[i][1] & 63, b = vga.dac[i][2] & 63;
  u32 R = (r << 2) | (r >> 4), G = (g << 2) | (g >> 4), B = (b << 2) | (b >> 4);
  vga.dac_rgb[i] = 0xFF000000u | (B << 16) | (G << 8) | R; /* RGBA byte order in memory */
}

void vga_set_dac(int i, u8 r, u8 g, u8 b) {
  vga.dac[i & 255][0] = r; vga.dac[i & 255][1] = g; vga.dac[i & 255][2] = b;
  dac_update(i & 255);
}
void vga_get_dac(int i, u8 *r, u8 *g, u8 *b) { *r = vga.dac[i & 255][0]; *g = vga.dac[i & 255][1]; *b = vga.dac[i & 255][2]; }

static void load_default_dac(int colors256) {
  /* entries 0-63: the EGA 6-bit rgbRGB set, as every 16-colour mode expects */
  for (int i = 0; i < 64; i++) {
    u8 r = (u8)(((i >> 2) & 1) * 42 + ((i >> 5) & 1) * 21);
    u8 g = (u8)(((i >> 1) & 1) * 42 + ((i >> 4) & 1) * 21);
    u8 b = (u8)(((i >> 0) & 1) * 42 + ((i >> 3) & 1) * 21);
    vga_set_dac(i, r, g, b);
  }
  if (!colors256) {
    for (int i = 64; i < 256; i++) vga_set_dac(i, 0, 0, 0);
    return;
  }
  /* 256-colour default: 16 CGA colours, 16 greys, 9 hue rings, 8 black */
  static const u8 cga[16][3] = {
    {0, 0, 0}, {0, 0, 42}, {0, 42, 0}, {0, 42, 42}, {42, 0, 0}, {42, 0, 42}, {42, 21, 0}, {42, 42, 42},
    {21, 21, 21}, {21, 21, 63}, {21, 63, 21}, {21, 63, 63}, {63, 21, 21}, {63, 21, 63}, {63, 63, 21}, {63, 63, 63}};
  for (int i = 0; i < 16; i++) vga_set_dac(i, cga[i][0], cga[i][1], cga[i][2]);
  static const u8 greys[16] = {0, 5, 8, 11, 14, 17, 20, 24, 28, 32, 36, 40, 45, 50, 56, 63};
  for (int i = 0; i < 16; i++) vga_set_dac(16 + i, greys[i], greys[i], greys[i]);
  static const u8 maxv[3] = {63, 28, 16};
  int idx = 32;
  for (int g = 0; g < 3; g++) {
    int hi = maxv[g];
    int mins[3] = {0, hi / 2, (hi * 3) / 4 + (hi == 63 ? 0 : 0)};
    for (int s = 0; s < 3; s++) {
      int lo = mins[s];
      /* 24 hues: blue → magenta → red → yellow → green → cyan → (blue) */
      for (int h = 0; h < 24; h++) {
        int sext = h / 4, step = h % 4;
        int up = lo + ((hi - lo) * step) / 4;
        int down = hi - ((hi - lo) * step) / 4;
        int r = lo, gg = lo, b = lo;
        switch (sext) {
          case 0: b = hi; r = up; break;
          case 1: r = hi; b = down; break;
          case 2: r = hi; gg = up; break;
          case 3: gg = hi; r = down; break;
          case 4: gg = hi; b = up; break;
          default: b = hi; gg = down; break;
        }
        vga_set_dac(idx++, (u8)r, (u8)gg, (u8)b);
      }
    }
  }
  for (; idx < 256; idx++) vga_set_dac(idx, 0, 0, 0);
}

/* ---------------- memory ---------------- */
u8 *vga_plane(int p) { return vga.plane[p & 3]; }

static int window_offset(u32 addr, u32 *off) {
  u32 a = addr - 0xA0000;
  switch ((vga.gc[6] >> 2) & 3) {
    case 0: break;                                  /* A0000-BFFFF */
    case 1: if (a >= 0x10000) return 0; break;      /* A0000-AFFFF */
    case 2: if (a < 0x10000 || a >= 0x18000) return 0; a -= 0x10000; break;
    default: if (a < 0x18000) return 0; a -= 0x18000; break;
  }
  *off = a;
  return 1;
}

u8 vga_mem_rd(u32 addr) {
  u32 a;
  if (!window_offset(addr, &a)) return 0xFF;
  u8 memmode = vga.seq[4];
  if (memmode & 8) { /* chain 4 */
    u32 off = a & 0xFFFC;
    for (int p = 0; p < 4; p++) vga.latch[p] = vga.plane[p][off];
    return vga.plane[a & 3][off];
  }
  if (!(memmode & 4)) { /* odd/even */
    u32 off = (a & 0xFFFE) | ((vga.misc_out >> 5) & 1 ? 0 : 0);
    for (int p = 0; p < 4; p++) vga.latch[p] = vga.plane[p][off];
    int plane = (a & 1) | (vga.gc[4] & 2);
    return vga.plane[plane][off];
  }
  u32 off = a & 0xFFFF;
  for (int p = 0; p < 4; p++) vga.latch[p] = vga.plane[p][off];
  if (vga.gc[5] & 8) { /* read mode 1: colour compare */
    u8 res = 0xFF;
    u8 cmp = vga.gc[2], dc = vga.gc[7];
    for (int p = 0; p < 4; p++) {
      if (!(dc & (1 << p))) continue;
      u8 want = (cmp & (1 << p)) ? 0xFF : 0x00;
      res &= (u8)~(vga.latch[p] ^ want);
    }
    return res;
  }
  return vga.plane[vga.gc[4] & 3][off];
}

void vga_mem_wr(u32 addr, u8 v) {
  u32 a;
  if (!window_offset(addr, &a)) return;
  u8 memmode = vga.seq[4];
  u8 mask = vga.seq[2] & 0xF;
  u32 off;
  if (memmode & 8) {
    off = a & 0xFFFC;
    mask &= (u8)(1 << (a & 3));
  } else if (!(memmode & 4)) {
    off = a & 0xFFFE;
    mask &= (a & 1) ? 0x0A : 0x05;
  } else {
    off = a & 0xFFFF;
  }
  u8 wmode = vga.gc[5] & 3;
  u8 bitmask = vga.gc[8];
  u8 rot = vga.gc[3] & 7;
  u8 func = (vga.gc[3] >> 3) & 3;
  u8 setreset = vga.gc[0], enable_sr = vga.gc[1];
  u8 out[4];
  switch (wmode) {
    case 0: {
      u8 rv = (u8)((v >> rot) | (v << (8 - rot)));
      for (int p = 0; p < 4; p++) {
        u8 val = (enable_sr & (1 << p)) ? ((setreset & (1 << p)) ? 0xFF : 0x00) : rv;
        switch (func) { case 1: val &= vga.latch[p]; break; case 2: val |= vga.latch[p]; break; case 3: val ^= vga.latch[p]; break; default: break; }
        out[p] = (u8)((val & bitmask) | (vga.latch[p] & ~bitmask));
      }
      break;
    }
    case 1:
      for (int p = 0; p < 4; p++) out[p] = vga.latch[p];
      break;
    case 2:
      for (int p = 0; p < 4; p++) {
        u8 val = (v & (1 << p)) ? 0xFF : 0x00;
        switch (func) { case 1: val &= vga.latch[p]; break; case 2: val |= vga.latch[p]; break; case 3: val ^= vga.latch[p]; break; default: break; }
        out[p] = (u8)((val & bitmask) | (vga.latch[p] & ~bitmask));
      }
      break;
    default: { /* mode 3 */
      u8 rv = (u8)((v >> rot) | (v << (8 - rot)));
      u8 bm = rv & bitmask;
      for (int p = 0; p < 4; p++) {
        u8 val = (setreset & (1 << p)) ? 0xFF : 0x00;
        out[p] = (u8)((val & bm) | (vga.latch[p] & ~bm));
      }
      break;
    }
  }
  for (int p = 0; p < 4; p++)
    if (mask & (1 << p)) vga.plane[p][off] = out[p];
}

/* ---------------- ports ---------------- */
static int mono(void) { return !(vga.misc_out & 1); }

static void attr_write(u8 v) {
  if (!vga.attr_flip) {
    vga.attr_idx = v & 0x1F;
    vga.attr_pas = (v >> 5) & 1;
  } else {
    if (vga.attr_idx < 32) vga.attr[vga.attr_idx] = v;
  }
  vga.attr_flip ^= 1;
}

static u8 rd_3cx(u16 port) {
  switch (port) {
    case 0x3C0: return (u8)(vga.attr_idx | (vga.attr_pas << 5));
    case 0x3C1: return vga.attr[vga.attr_idx & 0x1F];
    case 0x3C2: return 0x10; /* input status 0: switch sense set */
    case 0x3C3: return 1;
    case 0x3C4: return vga.seq_idx;
    case 0x3C5: return vga.seq[vga.seq_idx & 7];
    case 0x3C6: return vga.dac_mask;
    case 0x3C7: return vga.dac_state;
    case 0x3C8: return vga.dac_write_idx;
    case 0x3C9: {
      u8 v = vga.dac[vga.dac_read_idx][vga.dac_sub];
      if (++vga.dac_sub == 3) { vga.dac_sub = 0; vga.dac_read_idx++; }
      return v;
    }
    case 0x3CA: return vga.feature;
    case 0x3CC: return vga.misc_out;
    case 0x3CE: return vga.gc_idx;
    case 0x3CF: return vga.gc[vga.gc_idx & 15];
    default: return 0xFF;
  }
}

static void wr_3cx(u16 port, u8 v) {
  switch (port) {
    case 0x3C0: attr_write(v); break;
    case 0x3C2: vga.misc_out = v; break;
    case 0x3C3: break;
    case 0x3C4: vga.seq_idx = v; break;
    case 0x3C5: vga.seq[vga.seq_idx & 7] = v; break;
    case 0x3C6: vga.dac_mask = v; break;
    case 0x3C7: vga.dac_read_idx = v; vga.dac_sub = 0; vga.dac_state = 3; break;
    case 0x3C8: vga.dac_write_idx = v; vga.dac_sub = 0; vga.dac_state = 0; break;
    case 0x3C9:
      vga.dac[vga.dac_write_idx][vga.dac_sub] = v & 63;
      if (++vga.dac_sub == 3) { dac_update(vga.dac_write_idx); vga.dac_sub = 0; vga.dac_write_idx++; }
      break;
    case 0x3CE: vga.gc_idx = v; break;
    case 0x3CF: vga.gc[vga.gc_idx & 15] = v; break;
    default: break;
  }
}

static u8 rd_crtc(u16 port) {
  int base = mono() ? 0x3B4 : 0x3D4;
  if (port == base) return vga.crtc_idx;
  if (port == base + 1) return vga.crtc[vga.crtc_idx & 31];
  if (port == 0x3DA || port == 0x3BA) {
    vga.attr_flip = 0;
    u8 s = 0;
    int line = current_line();
    if (line >= vga.visible_lines) s |= 0x09; /* vertical retrace + display disabled */
    else {
      s64 t = (emu_ns - vga.frame_start_ns) % vga.line_ns;
      if (t > (vga.line_ns * 4) / 5) s |= 0x01; /* horizontal blanking */
    }
    return s;
  }
  if (port == 0x3D9) return vga.cga_color;
  if (port == 0x3D8) return vga.cga_mode;
  return 0xFF;
}

static void wr_crtc(u16 port, u8 v) {
  int base = mono() ? 0x3B4 : 0x3D4;
  if (port == base) { vga.crtc_idx = v; return; }
  if (port == base + 1) {
    int i = vga.crtc_idx & 31;
    if (i <= 7 && (vga.crtc[0x11] & 0x80) && i != 7) return; /* write protect 0-7 */
    if (i == 7 && (vga.crtc[0x11] & 0x80)) { vga.crtc[7] = (u8)((vga.crtc[7] & ~0x10) | (v & 0x10)); return; }
    vga.crtc[i] = v;
    if (i == 6 || i == 7 || i == 0x12) update_timing();
    return;
  }
  if (port == 0x3DA || port == 0x3BA) { vga.feature = v; return; }
  if (port == 0x3D8) { vga.cga_mode = v; return; }
  if (port == 0x3D9) { vga.cga_color = v; return; }
}

void vga_register_ports(void) {
  io_register(0x3C0, 16, rd_3cx, wr_3cx);
  io_register(0x3B0, 16, rd_crtc, wr_crtc);
  io_register(0x3D0, 16, rd_crtc, wr_crtc);
}

u8 vga_read_crtc(int idx) { return vga.crtc[idx & 31]; }
void vga_write_crtc(int idx, u8 v) { vga.crtc[idx & 31] = v; if ((idx & 31) == 6 || (idx & 31) == 7 || (idx & 31) == 0x12) update_timing(); }
u16 vga_crtc_port(void) { return mono() ? 0x3B4 : 0x3D4; }
void vga_set_attr_reg(int idx, u8 v) { vga.attr[idx & 31] = v; }
u8 vga_get_attr_reg(int idx) { return vga.attr[idx & 31]; }
int vga_mode_is_text(void) { return !(vga.attr[0x10] & 1); }

/* ---------------- mode tables ---------------- */
typedef struct {
  u8 mode;
  u8 misc;
  u8 seq[5];
  u8 crtc[25];
  u8 gc[9];
  u8 attr[21];
  u8 colors256;
} ModeRegs;

static const ModeRegs mode_tab[] = {
  {0x00, 0x67, {3, 0x08, 3, 0, 2},
   {0x2D, 0x27, 0x28, 0x90, 0x2B, 0xA0, 0xBF, 0x1F, 0, 0x4F, 0x0D, 0x0E, 0, 0, 0, 0, 0x9C, 0x8E, 0x8F, 0x14, 0x1F, 0x96, 0xB9, 0xA3, 0xFF},
   {0, 0, 0, 0, 0, 0x10, 0x0E, 0x0F, 0xFF},
   {0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x0C, 0, 0x0F, 8, 0}, 0},
  {0x02, 0x67, {3, 0x00, 3, 0, 2},
   {0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0, 0x4F, 0x0D, 0x0E, 0, 0, 0, 0, 0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF},
   {0, 0, 0, 0, 0, 0x10, 0x0E, 0x0F, 0xFF},
   {0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x0C, 0, 0x0F, 8, 0}, 0},
  {0x04, 0x63, {3, 0x09, 3, 0, 2},
   {0x2D, 0x27, 0x28, 0x90, 0x2B, 0x80, 0xBF, 0x1F, 0, 0xC1, 0, 0, 0, 0, 0, 0, 0x9C, 0x8E, 0x8F, 0x14, 0, 0x96, 0xB9, 0xA2, 0xFF},
   {0, 0, 0, 0, 0, 0x30, 0x0F, 0x0F, 0xFF},
   {0, 0x13, 0x15, 0x17, 2, 4, 6, 7, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 1, 0, 3, 0, 0}, 0},
  {0x06, 0x63, {3, 0x01, 1, 0, 6},
   {0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0, 0xC1, 0, 0, 0, 0, 0, 0, 0x9C, 0x8E, 0x8F, 0x28, 0, 0x96, 0xB9, 0xC2, 0xFF},
   {0, 0, 0, 0, 0, 0, 0x0D, 0x0F, 0xFF},
   {0, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 1, 0, 1, 0, 0}, 0},
  {0x07, 0x66, {3, 0x00, 3, 0, 2},
   {0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0, 0x4F, 0x0D, 0x0E, 0, 0, 0, 0, 0x9C, 0x8E, 0x8F, 0x28, 0x0F, 0x96, 0xB9, 0xA3, 0xFF},
   {0, 0, 0, 0, 0, 0x10, 0x0A, 0x0F, 0xFF},
   {0, 8, 8, 8, 8, 8, 8, 8, 0x10, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x0E, 0, 0x0F, 8, 0}, 0},
  {0x0D, 0x63, {3, 0x09, 0x0F, 0, 6},
   {0x2D, 0x27, 0x28, 0x90, 0x2B, 0x80, 0xBF, 0x1F, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x9C, 0x8E, 0x8F, 0x14, 0, 0x96, 0xB9, 0xE3, 0xFF},
   {0, 0, 0, 0, 0, 0, 5, 0x0F, 0xFF},
   {0, 1, 2, 3, 4, 5, 6, 7, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 1, 0, 0x0F, 0, 0}, 0},
  {0x0E, 0x63, {3, 0x01, 0x0F, 0, 6},
   {0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x9C, 0x8E, 0x8F, 0x28, 0, 0x96, 0xB9, 0xE3, 0xFF},
   {0, 0, 0, 0, 0, 0, 5, 0x0F, 0xFF},
   {0, 1, 2, 3, 4, 5, 6, 7, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 1, 0, 0x0F, 0, 0}, 0},
  {0x0F, 0xA2, {3, 0x01, 0x0F, 0, 6},
   {0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0, 0x40, 0, 0, 0, 0, 0, 0, 0x83, 0x85, 0x5D, 0x28, 0x0F, 0x63, 0xBA, 0xE3, 0xFF},
   {0, 0, 0, 0, 0, 0, 5, 0x0F, 0xFF},
   {0, 8, 0, 0, 0x18, 0x18, 0, 0, 0, 8, 0, 0, 0, 0x18, 0, 0, 0x0B, 0, 5, 0, 0}, 0},
  {0x10, 0xA3, {3, 0x01, 0x0F, 0, 6},
   {0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0, 0x40, 0, 0, 0, 0, 0, 0, 0x83, 0x85, 0x5D, 0x28, 0x0F, 0x63, 0xBA, 0xE3, 0xFF},
   {0, 0, 0, 0, 0, 0, 5, 0x0F, 0xFF},
   {0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 1, 0, 0x0F, 0, 0}, 0},
  {0x11, 0xE3, {3, 0x01, 0x0F, 0, 6},
   {0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E, 0, 0x40, 0, 0, 0, 0, 0, 0, 0xEA, 0x8C, 0xDF, 0x28, 0, 0xE7, 0x04, 0xC3, 0xFF},
   {0, 0, 0, 0, 0, 0, 5, 0x0F, 0xFF},
   {0, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 1, 0, 0x0F, 0, 0}, 0},
  {0x12, 0xE3, {3, 0x01, 0x0F, 0, 6},
   {0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E, 0, 0x40, 0, 0, 0, 0, 0, 0, 0xEA, 0x8C, 0xDF, 0x28, 0, 0xE7, 0x04, 0xE3, 0xFF},
   {0, 0, 0, 0, 0, 0, 5, 0x0F, 0xFF},
   {0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 1, 0, 0x0F, 0, 0}, 0},
  {0x13, 0x63, {3, 0x01, 0x0F, 0, 0x0E},
   {0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0, 0x41, 0, 0, 0, 0, 0, 0, 0x9C, 0x8E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF},
   {0, 0, 0, 0, 0, 0x40, 5, 0x0F, 0xFF},
   {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x41, 0, 0x0F, 0, 0}, 1},
};

static const ModeRegs *find_mode(int mode) {
  int m = mode & 0x7F;
  if (m == 1) m = 0;
  if (m == 3) m = 2;
  if (m == 5) m = 4;
  for (int i = 0; i < ARRAY_LEN(mode_tab); i++)
    if (mode_tab[i].mode == m) return &mode_tab[i];
  return NULL;
}

void vga_set_mode(int mode) {
  const ModeRegs *t = find_mode(mode);
  if (!t) t = &mode_tab[1];
  vga.misc_out = t->misc;
  for (int i = 0; i < 5; i++) vga.seq[i] = t->seq[i];
  vga.crtc[0x11] = 0; /* unlock */
  for (int i = 0; i < 25; i++) vga.crtc[i] = t->crtc[i];
  for (int i = 0; i < 9; i++) vga.gc[i] = t->gc[i];
  for (int i = 0; i < 21; i++) vga.attr[i] = t->attr[i];
  vga.attr_pas = 1;
  vga.attr_flip = 0;
  vga.dac_mask = 0xFF;
  load_default_dac(t->colors256);
  if (!(mode & 0x80)) {
    for (int p = 0; p < 4; p++) dm_memset(vga.plane[p], 0, 65536);
    if (!(t->attr[0x10] & 1)) { /* text: blank with attribute 07 */
      for (int i = 0; i < 65536; i += 2) { vga.plane[1][i] = 0x07; }
    }
  }
  update_timing();
  vga.frame_start_ns = emu_ns;
  sched_set(EV_VGA, emu_ns + vga.frame_ns, vga_frame_event);
}

void vga_load_font(const u8 *font, int height, int first, int count, int block) {
  u8 *p2 = vga.plane[2];
  u32 base = (u32)block * 0x2000;
  for (int c = 0; c < count; c++) {
    u32 dst = base + (u32)(first + c) * 32;
    for (int r = 0; r < 32; r++) p2[(dst + (u32)r) & 0xFFFF] = r < height ? font[c * height + r] : 0;
  }
}

/* ---------------- rendering ---------------- */
u32 *vga_framebuffer(void) { return vga.fb; }
int vga_fb_width(void) { return vga.fb_w; }
int vga_fb_height(void) { return vga.fb_h; }
u32 vga_frame_id(void) { return vga.frame_id; }

static u32 attr_to_rgb(u8 index4) {
  u8 pal = vga.attr[index4 & 0xF] & 0x3F;
  u8 cs = vga.attr[0x14];
  u8 dacidx;
  if (vga.attr[0x10] & 0x80) dacidx = (u8)((pal & 0x0F) | ((cs & 3) << 4) | ((cs & 0xC) << 4));
  else dacidx = (u8)(pal | ((cs & 0xC) << 4));
  return vga.dac_rgb[dacidx & vga.dac_mask];
}

static u32 mem_addr(u32 ma) {
  u8 mode = vga.crtc[0x17];
  if (vga.crtc[0x14] & 0x40) return (ma << 2) & 0xFFFF;
  if (mode & 0x40) return ma & 0xFFFF;
  return ((ma << 1) | ((mode & 0x20) ? 0 : 0)) & 0xFFFF;
}

void vga_render_frame(void) {
  int text = !(vga.attr[0x10] & 1);
  int chars = (vga.crtc[1] + 1);
  int vis = vga.visible_lines;
  int max_sl = (vga.crtc[9] & 0x1F) + 1;
  int dsc = (vga.crtc[9] & 0x80) ? 2 : 1;
  u32 start = ((u32)vga.crtc[0xC] << 8) | vga.crtc[0xD];
  u32 pitch = (u32)vga.crtc[0x13] * 2;
  int line_compare = vga.crtc[0x18] | ((vga.crtc[7] & 0x10) << 4) | ((vga.crtc[9] & 0x40) << 3);
  int preset = vga.crtc[8] & 0x1F;
  u8 pan = vga.attr[0x13] & 7;
  int c256 = (vga.gc[5] & 0x40) != 0;
  int interleave = (vga.gc[5] & 0x20) != 0;
  int dot9 = text && !(vga.seq[1] & 1);
  int cw = text ? (dot9 ? 9 : 8) : (c256 ? 4 : 8);
  int w = chars * cw;
  /* graphics: a memory row is repeated for max_sl scanlines (twice that with double scan) unless the
   * CGA-compatibility address bits turn the repeats into distinct banks */
  int banks = (!(vga.crtc[0x17] & 1) ? 2 : 1) * (!(vga.crtc[0x17] & 2) ? 2 : 1);
  int repeat = max_sl * dsc / banks;
  if (repeat < 1) repeat = 1;
  int h = text ? vis : vis / repeat;
  if (w > FB_MAX_W) w = FB_MAX_W;
  if (h > FB_MAX_H) h = FB_MAX_H;
  if (w < 8 || h < 1) { vga.fb_w = 0; vga.fb_h = 0; return; }
  vga.fb_w = w;
  vga.fb_h = h;
  u32 *fb = vga.fb;
  u8 plane_enable = vga.attr[0x12] & 0xF;
  int blink_on = (vga.frame_count / 16) & 1;
  int cursor_on = (vga.frame_count / 8) & 1;
  int screen_off = (vga.seq[1] & 0x20) || !(vga.attr_pas);

  if (text) {
    int rows = vis / max_sl;
    u32 cursor = ((u32)vga.crtc[0xE] << 8) | vga.crtc[0xF];
    int cur_start = vga.crtc[0xA] & 0x1F, cur_end = vga.crtc[0xB] & 0x1F;
    int cur_off = (vga.crtc[0xA] & 0x20) != 0;
    int blink_en = (vga.attr[0x10] & 8) != 0;
    int linegfx = (vga.attr[0x10] & 4) != 0;
    u8 fontA = (u8)(((vga.seq[3] & 3)) | ((vga.seq[3] & 0x10) >> 2));
    u8 fontB = (u8)(((vga.seq[3] >> 2) & 3) | ((vga.seq[3] & 0x20) >> 3));
    int dual = fontA != fontB;
    u32 ma_row = start;
    int y = 0;
    for (int row = 0; row < rows && y < h; row++) {
      u32 ma = ma_row;
      if (line_compare < vis && row * max_sl > line_compare) {}
      for (int r = preset; r < max_sl && y < h; r++, y++) {
        u32 *line = fb + (u32)y * (u32)w;
        for (int col = 0; col < chars; col++) {
          u32 off = mem_addr(ma + (u32)col);
          u8 ch = vga.plane[0][off], at = vga.plane[1][off];
          u8 font_block = dual ? ((at & 8) ? fontA : fontB) : fontA;
          u8 glyph = vga.plane[2][((u32)font_block * 0x2000 + (u32)ch * 32 + (u32)r) & 0xFFFF];
          u8 fg = at & 0x0F, bg;
          if (blink_en) { bg = (at >> 4) & 7; if ((at & 0x80) && !blink_on) fg = bg; }
          else bg = (at >> 4) & 0xF;
          if (dual) fg &= 7;
          int is_cursor = !cur_off && cursor_on && (ma + (u32)col) == cursor && r >= cur_start && r <= cur_end;
          if (is_cursor) glyph = 0xFF;
          u32 fgc = attr_to_rgb(fg), bgc = attr_to_rgb(bg);
          u32 *px = line + col * cw;
          for (int b = 0; b < 8; b++) px[b] = (glyph & (0x80 >> b)) ? fgc : bgc;
          if (dot9) px[8] = (linegfx && ch >= 0xC0 && ch <= 0xDF) ? px[7] : bgc;
        }
      }
      preset = 0;
      ma_row += pitch;
    }
    mouse_overlay(fb, w, h, 1, cw, max_sl);
    if (screen_off) for (u32 i = 0; i < (u32)(w * h); i++) fb[i] = 0xFF000000u;
    return;
  }

  /* graphics */
  u32 ma_row = start;
  int rsc = preset;
  int scan = 0;
  for (int y = 0; y < h; y++) {
    if (scan == line_compare + 1 || (line_compare == 0 && y == 0 && 0)) {}
    u32 *line = fb + (u32)y * (u32)w;
    u32 ma_line = ma_row;
    u8 mode17 = vga.crtc[0x17];
    u32 bank = 0;
    if (!(mode17 & 1)) bank |= (u32)(rsc & 1) << 13;
    if (!(mode17 & 2)) bank |= (u32)(rsc & 2) << 13;
    if (c256) {
      u8 pp = pan >> 1;
      for (int x = 0; x < w; x++) {
        u32 px = (u32)x + pp;
        u32 off = (mem_addr(ma_line + (px >> 2)) | bank) & 0xFFFF;
        u8 v = vga.plane[px & 3][off];
        line[x] = vga.dac_rgb[v & vga.dac_mask];
      }
    } else if (interleave) {
      for (int x = 0; x < w; x++) {
        u32 px = (u32)x + pan;
        u32 off = (mem_addr(ma_line + (px >> 3)) | bank) & 0xFFFF;
        int pix = px & 7;
        u8 byte = pix < 4 ? vga.plane[0][off] : vga.plane[1][off];
        int sh = 6 - 2 * (pix & 3);
        u8 idx = (byte >> sh) & 3;
        line[x] = attr_to_rgb(idx & plane_enable);
      }
    } else {
      for (int x = 0; x < w; x++) {
        u32 px = (u32)x + pan;
        u32 off = (mem_addr(ma_line + (px >> 3)) | bank) & 0xFFFF;
        int bit = 7 - (px & 7);
        u8 idx = (u8)(((vga.plane[0][off] >> bit) & 1) | (((vga.plane[1][off] >> bit) & 1) << 1) |
                      (((vga.plane[2][off] >> bit) & 1) << 2) | (((vga.plane[3][off] >> bit) & 1) << 3));
        line[x] = attr_to_rgb(idx & plane_enable);
      }
    }
    /* advance the row scan counter / memory row */
    /* each output row stands for `repeat` scanlines */
    scan += repeat;
    rsc += repeat / dsc > 0 ? repeat / dsc : 1;
    if (rsc >= max_sl) { rsc = 0; ma_row += pitch; }
    if (line_compare < vis && scan > line_compare && scan - repeat <= line_compare) {
      ma_row = 0; rsc = 0;
      if (vga.attr[0x10] & 0x20) pan = 0;
    }
  }
  mouse_overlay(fb, w, h, 0, 8, 8);
  if (screen_off) for (u32 i = 0; i < (u32)(w * h); i++) fb[i] = 0xFF000000u;
}

void vga_reset(void) {
  dm_memset(vga.plane, 0, sizeof vga.plane);
  dm_memset(vga.seq, 0, sizeof vga.seq);
  dm_memset(vga.crtc, 0, sizeof vga.crtc);
  dm_memset(vga.gc, 0, sizeof vga.gc);
  dm_memset(vga.attr, 0, sizeof vga.attr);
  vga.misc_out = 0x67;
  vga.dac_mask = 0xFF;
  vga.attr_flip = 0;
  vga.frame_count = 0;
  vga_set_mode(3);
}

void vga_init(void) {
  dm_memset(&vga, 0, sizeof vga);
  vga.fb = (u32 *)dm_alloc(FB_MAX_W * FB_MAX_H * 4);
  vga_reset();
}

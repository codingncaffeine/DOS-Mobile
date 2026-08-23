/* HLE BIOS: ROM image, POST, BDA, interrupt services (except INT 13h/19h: bios_disk.c). */
#include "bios.h"
#include "cpu_int.h"
#include "io.h"
#include "vga.h"
#include "sched.h"
#include "pic.h"
#include "kbd.h"
#include "cmos.h"
#include "disk.h"
#include "machine.h"

BiosConfig bios_cfg;

/* ---------------- BDA helpers ---------------- */
INLINE u8 bda8(u32 off) { return mem_rd8(BDA + off); }
INLINE u16 bda16(u32 off) { return mem_rd16(BDA + off); }
INLINE u32 bda32(u32 off) { return mem_rd32(BDA + off); }
INLINE void bda8w(u32 off, u8 v) { mem_wr8(BDA + off, v); }
INLINE void bda16w(u32 off, u16 v) { mem_wr16(BDA + off, v); }
INLINE void bda32w(u32 off, u32 v) { mem_wr32(BDA + off, v); }

/* register shorthands */
#define AL reg8_get(0)
#define AH reg8_get(4)
#define BL reg8_get(3)
#define BH reg8_get(7)
#define CL reg8_get(1)
#define CH reg8_get(5)
#define DL reg8_get(2)
#define DH reg8_get(6)
#define AX reg16_get(REG_AX)
#define BX reg16_get(REG_BX)
#define CX reg16_get(REG_CX)
#define DX reg16_get(REG_DX)
#define SI reg16_get(REG_SI)
#define DI reg16_get(REG_DI)
#define BP reg16_get(REG_BP)
#define SET_AL(v) reg8_set(0, (u8)(v))
#define SET_AH(v) reg8_set(4, (u8)(v))
#define SET_BL(v) reg8_set(3, (u8)(v))
#define SET_BH(v) reg8_set(7, (u8)(v))
#define SET_CL(v) reg8_set(1, (u8)(v))
#define SET_CH(v) reg8_set(5, (u8)(v))
#define SET_DL(v) reg8_set(2, (u8)(v))
#define SET_DH(v) reg8_set(6, (u8)(v))
#define SET_AX(v) reg16_set(REG_AX, (u16)(v))
#define SET_BX(v) reg16_set(REG_BX, (u16)(v))
#define SET_CX(v) reg16_set(REG_CX, (u16)(v))
#define SET_DX(v) reg16_set(REG_DX, (u16)(v))
#define SET_SI(v) reg16_set(REG_SI, (u16)(v))
#define SET_DI(v) reg16_set(REG_DI, (u16)(v))
#define SET_BP(v) reg16_set(REG_BP, (u16)(v))
#define ES_SEL cpu.seg[SEG_ES].sel
#define DS_SEL cpu.seg[SEG_DS].sel

/* flags in the IRET frame (SS:SP -> IP, CS, FLAGS) */
static u32 frame_flags_addr(void) { return cpu.seg[SEG_SS].base + ((cpu.r[REG_SP] + 4) & sp_mask()); }
static void ret_cf(int v) {
  u32 a = frame_flags_addr();
  u16 f = mem_rd16(a);
  f = v ? (u16)(f | F_CF) : (u16)(f & ~F_CF);
  mem_wr16(a, f);
  set_cf(v);
}
static void ret_zf(int v) {
  u32 a = frame_flags_addr();
  u16 f = mem_rd16(a);
  f = v ? (u16)(f | F_ZF) : (u16)(f & ~F_ZF);
  mem_wr16(a, f);
  flags_sync();
  cpu.eflags = v ? (cpu.eflags | F_ZF) : (cpu.eflags & ~(u32)F_ZF);
}
/* re-execute the hook after the next interrupt (used for blocking services) */
static void hle_wait(void) {
  cpu.eip = cpu.eip_start;
  cpu.eflags |= F_IF;
  cpu.halted = 1;
}

/* ---------------- ROM image ---------------- */
static u8 *rom(u32 off) { return ram + BIOS_ROM_BASE + off; }

static void emit(u32 *p, u8 b) { *rom(*p) = b; (*p)++; }

static void build_stub(int vec) {
  u32 p = BIOS_STUB_BASE + (u32)vec * 16;
  switch (vec) {
    case 0x08: /* timer tick: HLE, INT 1Ch, EOI, IRET */
      emit(&p, 0x0F); emit(&p, 0xFF); emit(&p, 0x08);
      emit(&p, 0xCD); emit(&p, 0x1C);
      emit(&p, 0x50); emit(&p, 0xB0); emit(&p, 0x20); emit(&p, 0xE6); emit(&p, 0x20); emit(&p, 0x58);
      emit(&p, 0xCF);
      break;
    case 0x09: /* keyboard: HLE, EOI, IRET */
      emit(&p, 0x0F); emit(&p, 0xFF); emit(&p, 0x09);
      emit(&p, 0x50); emit(&p, 0xB0); emit(&p, 0x20); emit(&p, 0xE6); emit(&p, 0x20); emit(&p, 0x58);
      emit(&p, 0xCF);
      break;
    case 0x70: /* RTC: HLE then EOI both controllers */
      emit(&p, 0x0F); emit(&p, 0xFF); emit(&p, 0x70);
      emit(&p, 0x50); emit(&p, 0xB0); emit(&p, 0x20); emit(&p, 0xE6); emit(&p, 0xA0); emit(&p, 0xE6); emit(&p, 0x20); emit(&p, 0x58);
      emit(&p, 0xCF);
      break;
    case 0x18: /* no boot device: HLE (prints, waits for a key), then INT 19h again */
      emit(&p, 0x0F); emit(&p, 0xFF); emit(&p, 0x18);
      emit(&p, 0xCD); emit(&p, 0x19);
      emit(&p, 0xCF);
      break;
    case 0x19: /* bootstrap: HLE jumps to the boot sector; falls through to INT 18h */
      emit(&p, 0x0F); emit(&p, 0xFF); emit(&p, 0x19);
      emit(&p, 0xCD); emit(&p, 0x18);
      emit(&p, 0xCF);
      break;
    case 0x05: case 0x1B: case 0x1C: case 0x4A: /* user hooks: plain IRET */
    case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: /* unused IRQs: EOI + IRET */
    case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
      if ((vec >= 0x0A && vec <= 0x0F) || (vec >= 0x71 && vec <= 0x77)) {
        emit(&p, 0x50); emit(&p, 0xB0); emit(&p, 0x20);
        if (vec >= 0x71) { emit(&p, 0xE6); emit(&p, 0xA0); }
        emit(&p, 0xE6); emit(&p, 0x20); emit(&p, 0x58);
      }
      emit(&p, 0xCF);
      break;
    default:
      emit(&p, 0x0F); emit(&p, 0xFF); emit(&p, (u8)vec);
      emit(&p, 0xCF);
      break;
  }
}

static void build_fonts(void) {
  u8 *f16 = rom(BIOS_FONT16_OFF);
  dm_memcpy(f16, font8x16, 4096);
  /* 8x14: drop the first and last rows of the 8x16 glyphs */
  u8 *f14 = rom(BIOS_FONT14_OFF);
  for (int c = 0; c < 256; c++)
    for (int r = 0; r < 14; r++) f14[c * 14 + r] = font8x16[c * 16 + r + 1];
  /* 8x8: OR row pairs */
  u8 *f8lo = rom(BIOS_FONT8_OFF), *f8hi = rom(BIOS_FONT8HI_OFF);
  for (int c = 0; c < 256; c++) {
    u8 *dst = c < 128 ? f8lo + c * 8 : f8hi + (c - 128) * 8;
    for (int r = 0; r < 8; r++) dst[r] = (u8)(font8x16[c * 16 + r * 2] | font8x16[c * 16 + r * 2 + 1]);
  }
}

static void build_tables(void) {
  /* system configuration table (INT 15h/C0h) */
  u8 *cfg = rom(BIOS_SYSCFG_OFF);
  cfg[0] = 8; cfg[1] = 0;
  cfg[2] = 0xFC; cfg[3] = 0x01; cfg[4] = 0x00;
  cfg[5] = 0x70; cfg[6] = 0; cfg[7] = 0; cfg[8] = 0; cfg[9] = 0;
  /* diskette parameter table */
  static const u8 dpt[11] = {0xDF, 0x02, 0x25, 0x02, 0x12, 0x1B, 0xFF, 0x6C, 0xF6, 0x0F, 0x08};
  dm_memcpy(rom(BIOS_DPT_OFF), dpt, 11);
  /* INT 10h/1Bh static functionality table */
  u8 *sft = rom(BIOS_STATIC_FN_OFF);
  dm_memset(sft, 0, 16);
  sft[0] = 0xFF; sft[1] = 0xE0; sft[2] = 0x0F; /* modes 0-7, 0D-13 */
  sft[7] = 0x07; /* scanlines 200/350/400 */
  sft[8] = 0x02; /* character blocks */
  sft[9] = 0x08; /* max active blocks */
  sft[10] = 0xE7; sft[11] = 0x0C; sft[12] = 0; sft[13] = 0; sft[14] = 0; sft[15] = 0;
  /* save pointer table: entry 0 = video parameter table */
  u8 *sp = rom(BIOS_SAVE_PTR_OFF);
  dm_memset(sp, 0, 28);
  st16(sp, BIOS_VPT_OFF); st16(sp + 2, BIOS_SEG);
  /* video parameter table: 64 bytes per mode index; modes 0-7 then 0D-13 (indices 8..14 unused) */
  dm_memset(rom(BIOS_VPT_OFF), 0, 64 * 30);
  /* reset vector + identification */
  u8 *rv = rom(0xFFF0);
  rv[0] = 0xEA; rv[1] = (u8)BIOS_POST_ENTRY; rv[2] = (u8)(BIOS_POST_ENTRY >> 8); rv[3] = 0x00; rv[4] = 0xF0;
  const char *date = "08/23/26";
  dm_memcpy(rom(0xFFF5), date, 8);
  *rom(0xFFFE) = 0xFC;
  /* POST entry: HLE POST, INT 19h, halt loop */
  u32 p = BIOS_POST_ENTRY;
  emit(&p, 0x0F); emit(&p, 0xFF); emit(&p, HLE_POST);
  emit(&p, 0xCD); emit(&p, 0x19);
  emit(&p, 0xF4); emit(&p, 0xEB); emit(&p, 0xFD);
  const char *id = "DOS Mobile BIOS";
  dm_memcpy(rom(0x0000), id, dm_strlen(id) + 1);
}

void bios_init(void) {
  dm_memset(rom(0), 0, 0x10000);
  for (int v = 0; v < 256; v++) build_stub(v);
  build_fonts();
  build_tables();
}

/* ---------------- video services ---------------- */
static int text_mode(void) { return vga_mode_is_text(); }
static int cols(void) { return bda16(0x4A); }
static int rows(void) { return bda8(0x84) + 1; }
static u32 page_base(int page) { return 0xB8000u + (u32)page * bda16(0x4C); }
static u32 text_base(void) { return (bda8(0x49) == 7) ? 0xB0000u : 0xB8000u; }

static void hw_cursor_update(void) {
  int page = bda8(0x62);
  u16 pos = bda16(0x50 + page * 2);
  u16 addr = (u16)(bda16(0x4E) / 2 + (pos >> 8) * cols() + (pos & 0xFF));
  vga_write_crtc(0x0E, (u8)(addr >> 8));
  vga_write_crtc(0x0F, (u8)addr);
}

static void set_cursor_shape(u8 start, u8 end) {
  bda16w(0x60, (u16)((start << 8) | end));
  int h = bda8(0x85);
  if (h > 8 && !(bda8(0x87) & 1) && (end & 0x1F) < 8) { /* cursor emulation for tall fonts */
    if ((start & 0x1F) == 6 && (end & 0x1F) == 7) { start = (u8)((start & 0xE0) | (h - 3)); end = (u8)(h - 2); }
    else { start = (u8)((start & 0xE0) | ((start & 0x1F) * h / 8)); end = (u8)((end & 0x1F) * h / 8 + 1); }
  }
  vga_write_crtc(0x0A, start);
  vga_write_crtc(0x0B, end);
}

static u32 font_ptr(int height) {
  (void)height;
  return mem_rd32(0x43 * 4); /* INT 43h: seg:off of the active graphics font */
}

/* graphics-mode pixel access (direct plane layout, mode-aware) */
static void put_pixel(int x, int y, u8 color, int xor_mode) {
  int mode = bda8(0x49);
  int w = cols() * 8;
  if (x < 0 || y < 0 || x >= w) return;
  switch (mode) {
    case 0x13: {
      u32 a = (u32)y * 320 + (u32)x;
      u8 *p = vga_plane(a & 3) + (a & 0xFFFC);
      *p = xor_mode ? (u8)(*p ^ color) : color;
      break;
    }
    case 0x04: case 0x05: {
      u32 a = (u32)((y & 1) * 0x2000 + (y >> 1) * 80 + (x >> 2));
      u8 *p = vga_plane(a & 1) + (a & 0xFFFE);
      int sh = (3 - (x & 3)) * 2;
      u8 v = (u8)((color & 3) << sh);
      if (xor_mode) *p ^= v; else *p = (u8)((*p & ~(3 << sh)) | v);
      break;
    }
    case 0x06: {
      u32 a = (u32)((y & 1) * 0x2000 + (y >> 1) * 80 + (x >> 3));
      u8 *p = vga_plane(a & 1) + (a & 0xFFFE);
      u8 m = (u8)(0x80 >> (x & 7));
      if (xor_mode) { if (color & 1) *p ^= m; }
      else if (color & 1) *p |= m; else *p &= (u8)~m;
      break;
    }
    default: { /* planar 16-colour (0D-12) */
      u32 a = (u32)y * (u32)(w / 8) + (u32)(x >> 3);
      u8 m = (u8)(0x80 >> (x & 7));
      for (int p = 0; p < 4; p++) {
        u8 *b = vga_plane(p) + (a & 0xFFFF);
        int bit = (color >> p) & 1;
        if (xor_mode) { if (bit) *b ^= m; }
        else if (bit) *b |= m; else *b &= (u8)~m;
      }
      break;
    }
  }
}

static u8 get_pixel(int x, int y) {
  int mode = bda8(0x49);
  int w = cols() * 8;
  if (x < 0 || y < 0 || x >= w) return 0;
  switch (mode) {
    case 0x13: { u32 a = (u32)y * 320 + (u32)x; return vga_plane(a & 3)[a & 0xFFFC]; }
    case 0x04: case 0x05: {
      u32 a = (u32)((y & 1) * 0x2000 + (y >> 1) * 80 + (x >> 2));
      return (u8)((vga_plane(a & 1)[a & 0xFFFE] >> ((3 - (x & 3)) * 2)) & 3);
    }
    case 0x06: {
      u32 a = (u32)((y & 1) * 0x2000 + (y >> 1) * 80 + (x >> 3));
      return (u8)((vga_plane(a & 1)[a & 0xFFFE] >> (7 - (x & 7))) & 1);
    }
    default: {
      u32 a = (u32)y * (u32)(w / 8) + (u32)(x >> 3);
      u8 v = 0;
      for (int p = 0; p < 4; p++) v |= (u8)(((vga_plane(p)[a & 0xFFFF] >> (7 - (x & 7))) & 1) << p);
      return v;
    }
  }
}

static void draw_glyph(int row, int col, u8 ch, u8 color, int xor_mode) {
  int h = bda8(0x85);
  u32 fp = font_ptr(h);
  u32 fbase = ((fp >> 16) << 4) + (fp & 0xFFFF);
  int x0 = col * 8, y0 = row * h;
  for (int r = 0; r < h; r++) {
    u8 bits = mem_rd8(fbase + (u32)ch * (u32)h + (u32)r);
    for (int b = 0; b < 8; b++) {
      int on = (bits >> (7 - b)) & 1;
      if (xor_mode) { if (on) put_pixel(x0 + b, y0 + r, color & 0x7F, 1); }
      else put_pixel(x0 + b, y0 + r, on ? color : 0, 0);
    }
  }
}

static void write_cell(int page, int row, int col, u8 ch, u8 attr, int with_attr) {
  if (text_mode()) {
    u32 a = text_base() + (u32)page * bda16(0x4C) + ((u32)row * (u32)cols() + (u32)col) * 2;
    mem_wr8(a, ch);
    if (with_attr) mem_wr8(a + 1, attr);
  } else {
    draw_glyph(row, col, ch, attr, (attr & 0x80) != 0);
  }
}

static void scroll_window(int lines, int up, u8 attr, int r0, int c0, int r1, int c1) {
  int nc = cols(), nr = rows();
  if (r1 >= nr) r1 = nr - 1;
  if (c1 >= nc) c1 = nc - 1;
  if (r0 > r1 || c0 > c1) return;
  int height = r1 - r0 + 1;
  if (lines == 0 || lines > height) lines = height;
  if (text_mode()) {
    int page = bda8(0x62);
    u32 base = text_base() + (u32)page * bda16(0x4C);
    if (up) {
      for (int r = r0; r <= r1; r++) {
        int src = r + lines;
        for (int c = c0; c <= c1; c++) {
          u32 d = base + ((u32)r * (u32)nc + (u32)c) * 2;
          if (src <= r1) { u32 s = base + ((u32)src * (u32)nc + (u32)c) * 2; mem_wr16(d, mem_rd16(s)); }
          else mem_wr16(d, (u16)(0x20 | (attr << 8)));
        }
      }
    } else {
      for (int r = r1; r >= r0; r--) {
        int src = r - lines;
        for (int c = c0; c <= c1; c++) {
          u32 d = base + ((u32)r * (u32)nc + (u32)c) * 2;
          if (src >= r0) { u32 s = base + ((u32)src * (u32)nc + (u32)c) * 2; mem_wr16(d, mem_rd16(s)); }
          else mem_wr16(d, (u16)(0x20 | (attr << 8)));
        }
      }
    }
  } else {
    int h = bda8(0x85);
    int x0 = c0 * 8, x1 = (c1 + 1) * 8;
    if (up) {
      for (int y = r0 * h; y < (r1 + 1) * h; y++) {
        int sy = y + lines * h;
        for (int x = x0; x < x1; x++) put_pixel(x, y, sy < (r1 + 1) * h ? get_pixel(x, sy) : (bda8(0x49) == 0x13 ? attr : 0), 0);
      }
    } else {
      for (int y = (r1 + 1) * h - 1; y >= r0 * h; y--) {
        int sy = y - lines * h;
        for (int x = x0; x < x1; x++) put_pixel(x, y, sy >= r0 * h ? get_pixel(x, sy) : (bda8(0x49) == 0x13 ? attr : 0), 0);
      }
    }
  }
}

static void set_video_mode(int mode) {
  int m = mode & 0x7F;
  int noclear = mode & 0x80;
  if (!(m <= 7 || (m >= 0x0D && m <= 0x13))) m = 3;
  int ncols, nrows = 25, height, regen;
  u8 mctl, pal;
  switch (m) {
    case 0: case 1: ncols = 40; height = 16; regen = 0x800; mctl = m == 0 ? 0x2C : 0x28; pal = 0x30; break;
    case 2: case 3: ncols = 80; height = 16; regen = 0x1000; mctl = m == 2 ? 0x2D : 0x29; pal = 0x30; break;
    case 4: case 5: ncols = 40; height = 8; regen = 0x4000; mctl = m == 4 ? 0x2A : 0x2E; pal = m == 4 ? 0x30 : 0x3F; break;
    case 6: ncols = 80; height = 8; regen = 0x4000; mctl = 0x1E; pal = 0x3F; break;
    case 7: ncols = 80; height = 16; regen = 0x1000; mctl = 0x29; pal = 0x30; break;
    case 0x0D: ncols = 40; height = 8; regen = 0x2000; mctl = 0x29; pal = 0x30; break;
    case 0x0E: ncols = 80; height = 8; regen = 0x4000; mctl = 0x29; pal = 0x30; break;
    case 0x0F: case 0x10: ncols = 80; height = 14; regen = 0x8000; mctl = 0x29; pal = 0x30; break;
    case 0x11: case 0x12: ncols = 80; height = 16; regen = 0xA000; mctl = 0x29; pal = 0x30; nrows = 30; break;
    default: ncols = 40; height = 8; regen = 0x2000; mctl = 0x29; pal = 0x30; break; /* 13h */
  }
  if (m == 0x0F || m == 0x10) nrows = 25;
  bda8w(0x49, (u8)m);
  bda16w(0x4A, (u16)ncols);
  bda16w(0x4C, (u16)regen);
  bda16w(0x4E, 0);
  for (int i = 0; i < 8; i++) bda16w(0x50 + i * 2, 0);
  bda8w(0x62, 0);
  bda16w(0x63, m == 7 ? 0x3B4 : 0x3D4);
  bda8w(0x65, mctl);
  bda8w(0x66, pal);
  bda8w(0x84, (u8)(nrows - 1));
  bda16w(0x85, (u16)height);
  bda8w(0x87, (u8)((bda8(0x87) & 0x7F) | (noclear ? 0x80 : 0) | 0x60));
  bda8w(0x88, 0x09);
  bda8w(0x89, (u8)((m == 0x11 || m == 0x12) ? 0x01 : 0x11));
  bda8w(0x8A, 0x08);
  vga_set_mode(mode);
  /* fonts: text modes get 8x16, 200-line graphics 8x8, 350-line 8x14, 480-line 8x16 */
  if (height == 16) { vga_load_font(font8x16, 16, 0, 256, 0); mem_wr32(0x43 * 4, ((u32)BIOS_SEG << 16) | BIOS_FONT16_OFF); }
  else if (height == 14) { vga_load_font(rom(BIOS_FONT14_OFF), 14, 0, 256, 0); mem_wr32(0x43 * 4, ((u32)BIOS_SEG << 16) | BIOS_FONT14_OFF); }
  else {
    static u8 f8[2048];
    dm_memcpy(f8, rom(BIOS_FONT8_OFF), 1024);
    dm_memcpy(f8 + 1024, rom(BIOS_FONT8HI_OFF), 1024);
    vga_load_font(f8, 8, 0, 256, 0);
    mem_wr32(0x43 * 4, ((u32)BIOS_SEG << 16) | BIOS_FONT8_OFF);
  }
  if (m <= 3 || m == 7) set_cursor_shape(6, 7);
  else set_cursor_shape(0x20, 0);
  hw_cursor_update();
}

void bios_putc(u8 c) {
  int page = bda8(0x62);
  u16 pos = bda16(0x50 + page * 2);
  int row = pos >> 8, col = pos & 0xFF;
  int nc = cols(), nr = rows();
  switch (c) {
    case 7: break;
    case 8: if (col > 0) col--; break;
    case 10: row++; break;
    case 13: col = 0; break;
    default:
      write_cell(page, row, col, c, text_mode() ? 0x07 : 0x0F, !text_mode());
      col++;
      if (col >= nc) { col = 0; row++; }
      break;
  }
  if (row >= nr) {
    row = nr - 1;
    u8 attr = 0x07;
    if (text_mode()) attr = (u8)(mem_rd16(text_base() + (u32)page * bda16(0x4C) + ((u32)row * (u32)nc + (u32)(nc - 1)) * 2) >> 8);
    scroll_window(1, 1, attr, 0, 0, nr - 1, nc - 1);
  }
  bda16w(0x50 + page * 2, (u16)((row << 8) | col));
  hw_cursor_update();
}

void bios_puts(const char *s) { while (*s) bios_putc((u8)*s++); }

static void int10(void) {
  u8 ah = AH;
  switch (ah) {
    case 0x00: set_video_mode(AL); break;
    case 0x01: set_cursor_shape(CH, CL); break;
    case 0x02: {
      int page = BH & 7;
      bda16w(0x50 + page * 2, DX);
      if (page == bda8(0x62)) hw_cursor_update();
      break;
    }
    case 0x03: {
      int page = BH & 7;
      SET_DX(bda16(0x50 + page * 2));
      SET_CX(bda16(0x60));
      break;
    }
    case 0x04: SET_AH(0); break; /* light pen */
    case 0x05: {
      int page = AL & 7;
      bda8w(0x62, (u8)page);
      bda16w(0x4E, (u16)(page * bda16(0x4C)));
      u16 start = (u16)(bda16(0x4E) / 2);
      vga_write_crtc(0x0C, (u8)(start >> 8));
      vga_write_crtc(0x0D, (u8)start);
      hw_cursor_update();
      break;
    }
    case 0x06: scroll_window(AL, 1, BH, CH, CL, DH, DL); break;
    case 0x07: scroll_window(AL, 0, BH, CH, CL, DH, DL); break;
    case 0x08: {
      int page = BH & 7;
      u16 pos = bda16(0x50 + page * 2);
      if (text_mode()) {
        u32 a = text_base() + (u32)page * bda16(0x4C) + ((u32)(pos >> 8) * (u32)cols() + (u32)(pos & 0xFF)) * 2;
        SET_AX(mem_rd16(a));
      } else {
        SET_AX(0); /* character recognition in graphics modes: not supported */
      }
      break;
    }
    case 0x09: case 0x0A: {
      int page = BH & 7;
      u16 pos = bda16(0x50 + page * 2);
      int row = pos >> 8, col = pos & 0xFF;
      u32 n = CX;
      for (u32 i = 0; i < n; i++) {
        write_cell(page, row, col, AL, BL, ah == 0x09 || !text_mode());
        if (++col >= cols()) { col = 0; if (++row >= rows()) break; }
      }
      break;
    }
    case 0x0B:
      if (BH == 0) { vga_set_attr_reg(0x11, BL); bda8w(0x66, (u8)((bda8(0x66) & 0xE0) | (BL & 0x1F))); }
      else {
        int m = bda8(0x49);
        if (m == 4 || m == 5) {
          if (BL & 1) { vga_set_attr_reg(1, 0x13); vga_set_attr_reg(2, 0x15); vga_set_attr_reg(3, 0x17); }
          else { vga_set_attr_reg(1, 0x02); vga_set_attr_reg(2, 0x04); vga_set_attr_reg(3, 0x06); }
        }
        bda8w(0x66, (u8)((bda8(0x66) & ~0x20) | ((BL & 1) << 5)));
      }
      break;
    case 0x0C: if (!text_mode()) put_pixel(CX, DX, AL & 0x7F, AL & 0x80); break;
    case 0x0D: SET_AL(text_mode() ? 0 : get_pixel(CX, DX)); break;
    case 0x0E: {
      int page = BH & 7;
      if (page != bda8(0x62)) { bda8w(0x62, (u8)page); }
      if (text_mode()) bios_putc(AL);
      else {
        u16 pos = bda16(0x50 + page * 2);
        int row = pos >> 8, col = pos & 0xFF;
        u8 c = AL;
        if (c == 8 || c == 10 || c == 13 || c == 7) bios_putc(c);
        else {
          write_cell(page, row, col, c, BL, 1);
          if (++col >= cols()) { col = 0; row++; }
          if (row >= rows()) { row = rows() - 1; scroll_window(1, 1, 0, 0, 0, rows() - 1, cols() - 1); }
          bda16w(0x50 + page * 2, (u16)((row << 8) | col));
          hw_cursor_update();
        }
      }
      break;
    }
    case 0x0F: SET_AL(bda8(0x49) | (bda8(0x87) & 0x80)); SET_AH((u8)cols()); SET_BH(bda8(0x62)); break;
    case 0x10:
      switch (AL) {
        case 0x00: if (BL < 16) vga_set_attr_reg(BL, BH); break;
        case 0x01: vga_set_attr_reg(0x11, BH); break;
        case 0x02: {
          u32 a = ((u32)ES_SEL << 4) + DX;
          for (int i = 0; i < 17; i++) vga_set_attr_reg(i == 16 ? 0x11 : i, mem_rd8(a + (u32)i));
          break;
        }
        case 0x03: {
          u8 v = vga_get_attr_reg(0x10);
          vga_set_attr_reg(0x10, (u8)((v & ~8) | ((BL & 1) << 3)));
          break;
        }
        case 0x07: SET_BH(vga_get_attr_reg(BL & 0xF)); break;
        case 0x08: SET_BH(vga_get_attr_reg(0x11)); break;
        case 0x09: {
          u32 a = ((u32)ES_SEL << 4) + DX;
          for (int i = 0; i < 17; i++) mem_wr8(a + (u32)i, vga_get_attr_reg(i == 16 ? 0x11 : i));
          break;
        }
        case 0x10: vga_set_dac(BX, DH, CH, CL); break;
        case 0x12: {
          u32 a = ((u32)ES_SEL << 4) + DX;
          u32 n = CX, first = BX;
          for (u32 i = 0; i < n && i < 256; i++)
            vga_set_dac((int)(first + i), mem_rd8(a + i * 3), mem_rd8(a + i * 3 + 1), mem_rd8(a + i * 3 + 2));
          break;
        }
        case 0x13: {
          u8 v = vga_get_attr_reg(0x10);
          if (BL == 0) vga_set_attr_reg(0x10, (u8)((v & 0x7F) | ((BH & 1) << 7)));
          else vga_set_attr_reg(0x14, (v & 0x80) ? (u8)(BH & 0xF) : (u8)((BH & 3) << 2));
          break;
        }
        case 0x15: { u8 r, g, b; vga_get_dac(BX, &r, &g, &b); SET_DH(r); SET_CH(g); SET_CL(b); break; }
        case 0x17: {
          u32 a = ((u32)ES_SEL << 4) + DX;
          u32 n = CX, first = BX;
          for (u32 i = 0; i < n && i < 256; i++) {
            u8 r, g, b; vga_get_dac((int)(first + i), &r, &g, &b);
            mem_wr8(a + i * 3, r); mem_wr8(a + i * 3 + 1, g); mem_wr8(a + i * 3 + 2, b);
          }
          break;
        }
        case 0x18: io_wr8(0x3C6, BL); break;
        case 0x19: SET_BL(io_rd8(0x3C6)); break;
        case 0x1A: {
          u8 v = vga_get_attr_reg(0x10), cs = vga_get_attr_reg(0x14);
          SET_BL((v & 0x80) ? 1 : 0);
          SET_BH((v & 0x80) ? (u8)(cs & 0xF) : (u8)((cs >> 2) & 3));
          break;
        }
        case 0x1B: {
          u32 n = CX, first = BX;
          for (u32 i = 0; i < n && i < 256; i++) {
            u8 r, g, b; vga_get_dac((int)(first + i), &r, &g, &b);
            u8 gray = (u8)((r * 30 + g * 59 + b * 11) / 100);
            vga_set_dac((int)(first + i), gray, gray, gray);
          }
          break;
        }
        default: break;
      }
      break;
    case 0x11: {
      u8 al = AL;
      int reprogram = (al & 0x10) != 0;
      int height = 0;
      switch (al & 0x0F) {
        case 0x00: {
          u32 a = ((u32)ES_SEL << 4) + BP;
          int h = BH, count = CX, first = DX;
          static u8 buf[256 * 32];
          for (int i = 0; i < count * h && i < (int)sizeof buf; i++) buf[i] = mem_rd8(a + (u32)i);
          vga_load_font(buf, h, first, count, BL & 7);
          height = h;
          break;
        }
        case 0x01: vga_load_font(rom(BIOS_FONT14_OFF), 14, 0, 256, BL & 7); height = 14; break;
        case 0x02: {
          static u8 f8[2048];
          dm_memcpy(f8, rom(BIOS_FONT8_OFF), 1024);
          dm_memcpy(f8 + 1024, rom(BIOS_FONT8HI_OFF), 1024);
          vga_load_font(f8, 8, 0, 256, BL & 7);
          height = 8;
          break;
        }
        case 0x03: io_wr8(0x3C4, 3); io_wr8(0x3C5, BL); break;
        case 0x04: vga_load_font(font8x16, 16, 0, 256, BL & 7); height = 16; break;
        default: break;
      }
      if (reprogram && height && text_mode()) {
        int nrows = 400 / height;
        if (bda8(0x49) == 0x0F || bda8(0x49) == 0x10) nrows = 350 / height;
        bda8w(0x85, (u8)height);
        bda8w(0x84, (u8)(nrows - 1));
        bda16w(0x4C, (u16)(nrows * cols() * 2));
        vga_write_crtc(0x09, (u8)((vga_read_crtc(0x09) & 0xE0) | (height - 1)));
        vga_write_crtc(0x12, (u8)(nrows * height - 1));
        set_cursor_shape((u8)(height - 2), (u8)(height - 1));
      }
      if ((al & 0x0F) >= 0x20 && 0) {}
      if (al == 0x20) mem_wr32(0x1F * 4, ((u32)ES_SEL << 16) | BP);
      else if (al == 0x21) {
        mem_wr32(0x43 * 4, ((u32)ES_SEL << 16) | BP);
        int nrows = BL == 0 ? DL : BL == 1 ? 14 : BL == 2 ? 25 : 43;
        bda8w(0x84, (u8)(nrows - 1));
        bda16w(0x85, CX);
      } else if (al == 0x22) { mem_wr32(0x43 * 4, ((u32)BIOS_SEG << 16) | BIOS_FONT14_OFF); bda16w(0x85, 14); bda8w(0x84, (u8)((BL == 0 ? DL : BL == 1 ? 14 : BL == 2 ? 25 : 43) - 1)); }
      else if (al == 0x23) { mem_wr32(0x43 * 4, ((u32)BIOS_SEG << 16) | BIOS_FONT8_OFF); bda16w(0x85, 8); bda8w(0x84, (u8)((BL == 0 ? DL : BL == 1 ? 14 : BL == 2 ? 25 : 43) - 1)); }
      else if (al == 0x24) { mem_wr32(0x43 * 4, ((u32)BIOS_SEG << 16) | BIOS_FONT16_OFF); bda16w(0x85, 16); bda8w(0x84, (u8)((BL == 0 ? DL : BL == 1 ? 14 : BL == 2 ? 25 : 43) - 1)); }
      else if (al == 0x30) {
        u32 ptr;
        switch (BH) {
          case 0: ptr = mem_rd32(0x1F * 4); break;
          case 1: ptr = mem_rd32(0x43 * 4); break;
          case 2: case 5: ptr = ((u32)BIOS_SEG << 16) | BIOS_FONT14_OFF; break;
          case 3: ptr = ((u32)BIOS_SEG << 16) | BIOS_FONT8_OFF; break;
          case 4: ptr = ((u32)BIOS_SEG << 16) | BIOS_FONT8HI_OFF; break;
          default: ptr = ((u32)BIOS_SEG << 16) | BIOS_FONT16_OFF; break;
        }
        cpu_load_seg(SEG_ES, (u16)(ptr >> 16));
        SET_BP(ptr & 0xFFFF);
        SET_CX(bda16(0x85));
        SET_DL(bda8(0x84));
      }
      break;
    }
    case 0x12:
      switch (BL) {
        case 0x10: SET_BH(0); SET_BL(3); SET_CH(0); SET_CL(9); break;
        case 0x20: break;
        case 0x30: bda8w(0x89, (u8)((bda8(0x89) & 0x6F) | (AL == 0 ? 0x80 : AL == 1 ? 0x00 : 0x10))); SET_AL(0x12); break;
        case 0x31: bda8w(0x89, (u8)((bda8(0x89) & ~8) | ((AL & 1) << 3))); SET_AL(0x12); break;
        case 0x32: SET_AL(0x12); break;
        case 0x33: bda8w(0x89, (u8)((bda8(0x89) & ~2) | ((AL & 1) << 1))); SET_AL(0x12); break;
        case 0x34: bda8w(0x87, (u8)((bda8(0x87) & ~1) | (AL & 1))); SET_AL(0x12); break;
        case 0x35: SET_AL(0x12); break;
        case 0x36: { io_wr8(0x3C4, 1); u8 v = io_rd8(0x3C5); io_wr8(0x3C5, (u8)((v & ~0x20) | (AL ? 0x20 : 0))); SET_AL(0x12); break; }
        default: break;
      }
      break;
    case 0x13: {
      int page = BH & 7;
      int row = DH, col = DL;
      u8 mode = AL;
      u32 a = ((u32)ES_SEL << 4) + BP;
      u16 n = CX;
      u8 attr = BL;
      u16 save = bda16(0x50 + page * 2);
      for (u16 i = 0; i < n; i++) {
        u8 c = mem_rd8(a++);
        if (mode & 2) attr = mem_rd8(a++);
        if (c == 8 || c == 10 || c == 13 || c == 7) {
          bda16w(0x50 + page * 2, (u16)((row << 8) | col));
          bios_putc(c);
          u16 p = bda16(0x50 + page * 2);
          row = p >> 8; col = p & 0xFF;
          continue;
        }
        write_cell(page, row, col, c, attr, 1);
        if (++col >= cols()) { col = 0; if (++row >= rows()) { row = rows() - 1; scroll_window(1, 1, attr, 0, 0, rows() - 1, cols() - 1); } }
      }
      if (mode & 1) { bda16w(0x50 + page * 2, (u16)((row << 8) | col)); hw_cursor_update(); }
      else bda16w(0x50 + page * 2, save);
      break;
    }
    case 0x1A:
      if (AL == 0) { SET_BL(0x08); SET_BH(0x00); SET_AL(0x1A); }
      else if (AL == 1) SET_AL(0x1A);
      break;
    case 0x1B: {
      u32 a = ((u32)ES_SEL << 4) + DI;
      for (int i = 0; i < 64; i++) mem_wr8(a + (u32)i, 0);
      mem_wr32(a + 0, ((u32)BIOS_SEG << 16) | BIOS_STATIC_FN_OFF);
      mem_wr8(a + 4, bda8(0x49));
      mem_wr16(a + 5, (u16)cols());
      mem_wr16(a + 7, bda16(0x4C));
      mem_wr16(a + 9, bda16(0x4E));
      for (int i = 0; i < 8; i++) mem_wr16(a + 11 + (u32)i * 2, bda16(0x50 + i * 2));
      mem_wr16(a + 27, bda16(0x60));
      mem_wr8(a + 29, bda8(0x62));
      mem_wr16(a + 30, bda16(0x63));
      mem_wr8(a + 32, bda8(0x65));
      mem_wr8(a + 33, bda8(0x66));
      mem_wr8(a + 34, (u8)rows());
      mem_wr16(a + 35, bda16(0x85));
      mem_wr8(a + 37, 0x08);
      mem_wr8(a + 38, 0x00);
      {
        int m = bda8(0x49);
        u16 colors = m == 0x13 ? 0x100 : (m == 6 || m == 0x11) ? 2 : (m == 4 || m == 5) ? 4 : 16;
        mem_wr16(a + 39, colors);
        mem_wr8(a + 41, (u8)(m <= 3 ? 8 : m == 7 ? 8 : m == 0x13 ? 1 : 2));
        mem_wr8(a + 42, (u8)(bda8(0x85) == 8 ? 0 : bda8(0x85) == 14 ? 1 : bda8(0x85) == 16 && rows() == 30 ? 3 : 2));
      }
      mem_wr8(a + 43, 0);
      mem_wr8(a + 44, 0);
      mem_wr8(a + 45, 0x11);
      mem_wr8(a + 49, 3);
      SET_AL(0x1B);
      break;
    }
    case 0x1C:
      if (AL == 0) { SET_BX(1); SET_AL(0x1C); }
      else SET_AL(0x1C);
      break;
    case 0x4F: SET_AX(0x014F); break; /* VESA: not yet */
    case 0xFE: case 0xFF: break;
    default: break;
  }
}

/* ---------------- keyboard ---------------- */
/* [scan][normal, shift, ctrl, alt] — full AX values (AH = scan, AL = ASCII) */
static const u16 keymap[0x59][4] = {
  {0, 0, 0, 0},
  {0x011B, 0x011B, 0x011B, 0x0100},
  {0x0231, 0x0221, 0, 0x7800}, {0x0332, 0x0340, 0x0300, 0x7900}, {0x0433, 0x0423, 0, 0x7A00},
  {0x0534, 0x0524, 0, 0x7B00}, {0x0635, 0x0625, 0, 0x7C00}, {0x0736, 0x075E, 0x071E, 0x7D00},
  {0x0837, 0x0826, 0, 0x7E00}, {0x0938, 0x092A, 0, 0x7F00}, {0x0A39, 0x0A28, 0, 0x8000},
  {0x0B30, 0x0B29, 0, 0x8100}, {0x0C2D, 0x0C5F, 0x0C1F, 0x8200}, {0x0D3D, 0x0D2B, 0, 0x8300},
  {0x0E08, 0x0E08, 0x0E7F, 0x0E00}, {0x0F09, 0x0F00, 0x9400, 0xA500},
  {0x1071, 0x1051, 0x1011, 0x1000}, {0x1177, 0x1157, 0x1117, 0x1100}, {0x1265, 0x1245, 0x1205, 0x1200},
  {0x1372, 0x1352, 0x1312, 0x1300}, {0x1474, 0x1454, 0x1414, 0x1400}, {0x1579, 0x1559, 0x1519, 0x1500},
  {0x1675, 0x1655, 0x1615, 0x1600}, {0x1769, 0x1749, 0x1709, 0x1700}, {0x186F, 0x184F, 0x180F, 0x1800},
  {0x1970, 0x1950, 0x1910, 0x1900}, {0x1A5B, 0x1A7B, 0x1A1B, 0x1A00}, {0x1B5D, 0x1B7D, 0x1B1D, 0x1B00},
  {0x1C0D, 0x1C0D, 0x1C0A, 0x1C00}, {0, 0, 0, 0},
  {0x1E61, 0x1E41, 0x1E01, 0x1E00}, {0x1F73, 0x1F53, 0x1F13, 0x1F00}, {0x2064, 0x2044, 0x2004, 0x2000},
  {0x2166, 0x2146, 0x2106, 0x2100}, {0x2267, 0x2247, 0x2207, 0x2200}, {0x2368, 0x2348, 0x2308, 0x2300},
  {0x246A, 0x244A, 0x240A, 0x2400}, {0x256B, 0x254B, 0x250B, 0x2500}, {0x266C, 0x264C, 0x260C, 0x2600},
  {0x273B, 0x273A, 0, 0x2700}, {0x2827, 0x2822, 0, 0x2800}, {0x2960, 0x297E, 0, 0x2900},
  {0, 0, 0, 0}, {0x2B5C, 0x2B7C, 0x2B1C, 0x2B00},
  {0x2C7A, 0x2C5A, 0x2C1A, 0x2C00}, {0x2D78, 0x2D58, 0x2D18, 0x2D00}, {0x2E63, 0x2E43, 0x2E03, 0x2E00},
  {0x2F76, 0x2F56, 0x2F16, 0x2F00}, {0x3062, 0x3042, 0x3002, 0x3000}, {0x316E, 0x314E, 0x310E, 0x3100},
  {0x326D, 0x324D, 0x320D, 0x3200}, {0x332C, 0x333C, 0, 0x3300}, {0x342E, 0x343E, 0, 0x3400},
  {0x352F, 0x353F, 0, 0x3500}, {0, 0, 0, 0}, {0x372A, 0x372A, 0x9600, 0x3700},
  {0, 0, 0, 0}, {0x3920, 0x3920, 0x3920, 0x3920}, {0, 0, 0, 0},
  {0x3B00, 0x5400, 0x5E00, 0x6800}, {0x3C00, 0x5500, 0x5F00, 0x6900}, {0x3D00, 0x5600, 0x6000, 0x6A00},
  {0x3E00, 0x5700, 0x6100, 0x6B00}, {0x3F00, 0x5800, 0x6200, 0x6C00}, {0x4000, 0x5900, 0x6300, 0x6D00},
  {0x4100, 0x5A00, 0x6400, 0x6E00}, {0x4200, 0x5B00, 0x6500, 0x6F00}, {0x4300, 0x5C00, 0x6600, 0x7000},
  {0x4400, 0x5D00, 0x6700, 0x7100},
  {0, 0, 0, 0}, {0, 0, 0, 0},
  {0x4700, 0x4737, 0x7700, 0x0700}, {0x4800, 0x4838, 0x8D00, 0x0800}, {0x4900, 0x4939, 0x8400, 0x0900},
  {0x4A2D, 0x4A2D, 0x8E00, 0x4A00},
  {0x4B00, 0x4B34, 0x7300, 0x0400}, {0x4C00, 0x4C35, 0x8F00, 0x0500}, {0x4D00, 0x4D36, 0x7400, 0x0600},
  {0x4E2B, 0x4E2B, 0x9000, 0x4E00},
  {0x4F00, 0x4F31, 0x7500, 0x0100}, {0x5000, 0x5032, 0x9100, 0x0200}, {0x5100, 0x5133, 0x7600, 0x0300},
  {0x5200, 0x5230, 0x9200, 0x0000}, {0x5300, 0x532E, 0x9300, 0},
  {0, 0, 0, 0}, {0, 0, 0, 0}, {0x565C, 0x567C, 0, 0},
  {0x8500, 0x8700, 0x8900, 0x8B00}, {0x8600, 0x8800, 0x8A00, 0x8C00},
};

static int kbuf_push(u16 ax) {
  u16 head = bda16(0x1A), tail = bda16(0x1C);
  u16 start = bda16(0x80), end = bda16(0x82);
  u16 next = (u16)(tail + 2);
  if (next >= end) next = start;
  if (next == head) return 0; /* full */
  mem_wr16(BDA + tail, ax);
  bda16w(0x1C, next);
  return 1;
}

static int kbuf_empty(void) { return bda16(0x1A) == bda16(0x1C); }
static u16 kbuf_peek(void) { return mem_rd16(BDA + bda16(0x1A)); }
static u16 kbuf_pop(void) {
  u16 head = bda16(0x1A);
  u16 v = mem_rd16(BDA + head);
  head += 2;
  if (head >= bda16(0x82)) head = bda16(0x80);
  bda16w(0x1A, head);
  return v;
}

static struct { u8 e0, e1, e1_count; } kstate;

void bios_kbd_scancode(u8 code) {
  u8 flags = bda8(0x17), flags2 = bda8(0x18), flags3 = bda8(0x96);
  if (kstate.e1) { if (++kstate.e1_count >= 2) { kstate.e1 = 0; kstate.e1_count = 0; } return; }
  if (code == 0xE0) { kstate.e0 = 1; bda8w(0x96, flags3 | 2); return; }
  if (code == 0xE1) { kstate.e1 = 1; kstate.e1_count = 0; return; }
  int e0 = kstate.e0;
  kstate.e0 = 0;
  bda8w(0x96, flags3 & (u8)~3);
  int release = code & 0x80;
  code &= 0x7F;
  switch (code) {
    case 0x2A: if (!e0) flags = release ? (u8)(flags & ~2) : (u8)(flags | 2); bda8w(0x17, flags); return;
    case 0x36: flags = release ? (u8)(flags & ~1) : (u8)(flags | 1); bda8w(0x17, flags); return;
    case 0x1D:
      if (e0) flags3 = release ? (u8)(flags3 & ~4) : (u8)(flags3 | 4);
      else flags2 = release ? (u8)(flags2 & ~1) : (u8)(flags2 | 1);
      flags = ((flags2 & 1) || (flags3 & 4)) ? (u8)(flags | 4) : (u8)(flags & ~4);
      bda8w(0x17, flags); bda8w(0x18, flags2); bda8w(0x96, flags3 & (u8)~3 ? (u8)(flags3 & ~3) : flags3);
      return;
    case 0x38:
      if (e0) flags3 = release ? (u8)(flags3 & ~8) : (u8)(flags3 | 8);
      else flags2 = release ? (u8)(flags2 & ~2) : (u8)(flags2 | 2);
      flags = ((flags2 & 2) || (flags3 & 8)) ? (u8)(flags | 8) : (u8)(flags & ~8);
      bda8w(0x17, flags); bda8w(0x18, flags2); bda8w(0x96, (u8)(flags3 & ~3));
      return;
    case 0x3A:
      if (!release) { flags ^= 0x40; bda8w(0x18, flags2 | 0x40); } else bda8w(0x18, flags2 & (u8)~0x40);
      bda8w(0x17, flags);
      return;
    case 0x45:
      if (!release) { flags ^= 0x20; bda8w(0x18, flags2 | 0x20); } else bda8w(0x18, flags2 & (u8)~0x20);
      bda8w(0x17, flags);
      return;
    case 0x46:
      if (!e0 || (flags & 4)) {
        if (e0 && (flags & 4)) { /* Ctrl-Break */
          if (release) return;
          bda8w(0x71, 0x80);
          bda16w(0x1A, bda16(0x80)); bda16w(0x1C, bda16(0x80));
          kbuf_push(0x0000);
          cpu_sw_interrupt(0x1B);
          return;
        }
        if (!release) { flags ^= 0x10; bda8w(0x18, flags2 | 0x10); } else bda8w(0x18, flags2 & (u8)~0x10);
        bda8w(0x17, flags);
        return;
      }
      return;
    default: break;
  }
  if (release) {
    if (code == 0x52) bda8w(0x18, flags2 & (u8)~0x80);
    return;
  }
  int shift = flags & 3, ctrl = flags & 4, alt = flags & 8;
  if (ctrl && alt && code == 0x53) { /* Ctrl-Alt-Del */
    bda16w(0x72, 0x1234);
    machine_reset_request();
    return;
  }
  if (code == 0x37 && (e0 || shift)) { cpu_sw_interrupt(0x05); return; }
  if (code >= 0x59) return;
  u16 ax;
  if (alt) ax = keymap[code][3];
  else if (ctrl) ax = keymap[code][2];
  else if (shift) ax = keymap[code][1];
  else ax = keymap[code][0];
  /* letters follow caps lock; keypad follows num lock */
  int is_letter = (code >= 0x10 && code <= 0x19) || (code >= 0x1E && code <= 0x26) || (code >= 0x2C && code <= 0x32);
  if (is_letter && (flags & 0x40) && !ctrl && !alt) ax = shift ? keymap[code][0] : keymap[code][1];
  int keypad = code >= 0x47 && code <= 0x53 && code != 0x4A && code != 0x4E;
  if (keypad && !e0 && !ctrl && !alt) {
    int numlock = (flags & 0x20) != 0;
    ax = (numlock ^ (shift != 0)) ? keymap[code][1] : keymap[code][0];
    if (code == 0x52 && !(numlock ^ (shift != 0))) { flags ^= 0x80; bda8w(0x17, flags); bda8w(0x18, flags2 | 0x80); }
  }
  if (e0) {
    if (keypad) ax = (u16)((ax & 0xFF00) | 0xE0);   /* grey navigation keys */
    else if (code == 0x1C) ax = (u16)(0xE000 | (ax & 0xFF));
    else if (code == 0x35) ax = 0xE02F;
  }
  if (ax == 0) return;
  if (!kbuf_push(ax)) { /* buffer full: nothing to do but drop it */ }
}

static void int09(void) {
  u8 code = io_rd8(0x60);
  bios_kbd_scancode(code);
}

static void int16(void) {
  u8 ah = AH;
  switch (ah) {
    case 0x00: case 0x10: {
      if (kbuf_empty()) { hle_wait(); return; }
      u16 v = kbuf_pop();
      if (ah == 0x00 && (v & 0xFF) == 0xE0) v &= 0xFF00;
      if (ah == 0x00 && (v >> 8) >= 0x85 && (v & 0xFF) == 0) { /* F11/F12 invisible to the legacy API */ }
      SET_AX(v);
      break;
    }
    case 0x01: case 0x11: {
      if (kbuf_empty()) { ret_zf(1); cpu.eflags |= F_IF; break; }
      u16 v = kbuf_peek();
      if (ah == 0x01 && (v & 0xFF) == 0xE0) v &= 0xFF00;
      SET_AX(v);
      ret_zf(0);
      break;
    }
    case 0x02: SET_AL(bda8(0x17)); break;
    case 0x03: break;
    case 0x05: SET_AL(kbuf_push(CX) ? 0 : 1); break;
    case 0x09: SET_AL(0x20); break;
    case 0x0A: SET_BX(0x41AB); break;
    case 0x12: SET_AL(bda8(0x17)); SET_AH((u8)((bda8(0x18) & 0x73) | (bda8(0x96) & 0x0C))); break;
    case 0x92: SET_AH(0x80); break;
    default: break;
  }
}

/* ---------------- timer / clock ---------------- */
static void int08(void) {
  u32 ticks = bda32(0x6C) + 1;
  if (ticks >= 0x1800B0) { ticks = 0; bda8w(0x70, 1); }
  bda32w(0x6C, ticks);
  u8 motor = bda8(0x40);
  if (motor) { motor--; bda8w(0x40, motor); if (motor == 0) bda8w(0x3F, bda8(0x3F) & 0xF0); }
}

static u8 bcd(int v) { return (u8)(((v / 10) << 4) | (v % 10)); }

static void int1a(void) {
  switch (AH) {
    case 0x00: {
      u32 t = bda32(0x6C);
      SET_CX((u16)(t >> 16)); SET_DX((u16)t);
      SET_AL(bda8(0x70));
      bda8w(0x70, 0);
      break;
    }
    case 0x01: bda32w(0x6C, ((u32)CX << 16) | DX); bda8w(0x70, 0); break;
    case 0x02: {
      int y, mo, d, h, mi, s, wd;
      cmos_get_time(&y, &mo, &d, &h, &mi, &s, &wd);
      SET_CH(bcd(h)); SET_CL(bcd(mi)); SET_DH(bcd(s)); SET_DL(0);
      ret_cf(0);
      break;
    }
    case 0x03: ret_cf(0); break;
    case 0x04: {
      int y, mo, d, h, mi, s, wd;
      cmos_get_time(&y, &mo, &d, &h, &mi, &s, &wd);
      SET_CH(bcd(y / 100)); SET_CL(bcd(y % 100)); SET_DH(bcd(mo)); SET_DL(bcd(d));
      ret_cf(0);
      break;
    }
    case 0x05: ret_cf(0); break;
    default: ret_cf(1); break;
  }
}

/* ---------------- INT 15h ---------------- */
static s64 wait_until;
static int wait_done;
static void wait_event(void) { wait_done = 1; }

static void int15(void) {
  u8 ah = AH;
  switch (ah) {
    case 0x24:
      switch (AL) {
        case 0: mem_set_a20(0); SET_AH(0); ret_cf(0); break;
        case 1: mem_set_a20(1); SET_AH(0); ret_cf(0); break;
        case 2: SET_AL(a20_mask == 0xFFFFFFFFu ? 1 : 0); SET_AH(0); ret_cf(0); break;
        case 3: SET_BX(3); SET_AH(0); ret_cf(0); break;
        default: SET_AH(0x86); ret_cf(1); break;
      }
      break;
    case 0x41: case 0x4F: case 0x83: case 0x89: case 0xC1: case 0x53: case 0xD8:
      SET_AH(0x86); ret_cf(1); break;
    case 0x86: {
      if (wait_done) { wait_done = 0; SET_AH(0); ret_cf(0); break; }
      if (!sched_active(EV_BIOS_WAIT)) {
        u32 us = ((u32)CX << 16) | DX;
        wait_until = emu_ns + (s64)us * 1000;
        sched_set(EV_BIOS_WAIT, wait_until, wait_event);
      }
      hle_wait();
      break;
    }
    case 0x87: {
      u32 gdt = ((u32)ES_SEL << 4) + SI;
      u32 words = CX;
      u32 src = mem_rd16(gdt + 0x12) | ((u32)mem_rd8(gdt + 0x14) << 16) | ((u32)mem_rd8(gdt + 0x17) << 24);
      u32 dst = mem_rd16(gdt + 0x1A) | ((u32)mem_rd8(gdt + 0x1C) << 16) | ((u32)mem_rd8(gdt + 0x1F) << 24);
      u32 old = a20_mask;
      a20_mask = 0xFFFFFFFFu;
      for (u32 i = 0; i < words; i++) mem_wr16(dst + i * 2, mem_rd16(src + i * 2));
      a20_mask = old;
      SET_AH(0); ret_cf(0);
      break;
    }
    case 0x88: {
      u32 ext = (bios_cfg.ram_kb > 1024) ? bios_cfg.ram_kb - 1024 : 0;
      SET_AX(ext > 0xFFFF ? 0xFFFF : ext);
      ret_cf(0);
      break;
    }
    case 0xC0:
      cpu_load_seg(SEG_ES, BIOS_SEG);
      SET_BX(BIOS_SYSCFG_OFF);
      SET_AH(0); ret_cf(0);
      break;
    case 0xC2: SET_AH(0x05); ret_cf(1); break; /* no PS/2 mouse yet */
    case 0xE8:
      if (AL == 0x01) {
        u32 ext = (bios_cfg.ram_kb > 1024) ? bios_cfg.ram_kb - 1024 : 0;
        u32 low = ext > 15 * 1024 ? 15 * 1024 : ext;
        u32 high = ext > 16 * 1024 ? (ext - 16 * 1024) / 64 : 0;
        SET_AX(low); SET_BX(high); SET_CX(low); SET_DX(high);
        ret_cf(0);
      } else if (AL == 0x20 && cpu.r[REG_DX] == 0x534D4150u) {
        u32 idx = cpu.r[REG_BX];
        u32 a = ((u32)ES_SEL << 4) + DI;
        u64 base, len; u32 type;
        u32 total = (u64)bios_cfg.ram_kb * 1024 > 0xFFFFFFFFull ? 0xFFFFFFFFu : bios_cfg.ram_kb * 1024;
        switch (idx) {
          case 0: base = 0; len = 0x9FC00; type = 1; break;
          case 1: base = 0x9FC00; len = 0x400; type = 2; break;
          case 2: base = 0xF0000; len = 0x10000; type = 2; break;
          default: base = 0x100000; len = total - 0x100000; type = 1; break;
        }
        mem_wr32(a, (u32)base); mem_wr32(a + 4, (u32)(base >> 32));
        mem_wr32(a + 8, (u32)len); mem_wr32(a + 12, (u32)(len >> 32));
        mem_wr32(a + 16, type);
        cpu.r[REG_AX] = 0x534D4150u;
        cpu.r[REG_CX] = 20;
        cpu.r[REG_BX] = idx >= 3 ? 0 : idx + 1;
        ret_cf(0);
      } else { SET_AH(0x86); ret_cf(1); }
      break;
    default: SET_AH(0x86); ret_cf(1); break;
  }
}

/* ---------------- POST ---------------- */
static u16 equipment_word(void) {
  u16 eq = 0;
  if (bios_cfg.floppies) eq |= 1 | (u16)((bios_cfg.floppies - 1) << 6);
  if (bios_cfg.fpu) eq |= 2;
  eq |= 2 << 4; /* initial video: 80x25 colour */
  return eq;
}

static void post(int warm) {
  (void)warm;
  /* BDA */
  for (u32 i = 0; i < 0x100; i++) mem_wr8(BDA + i, 0);
  bda16w(0x10, equipment_word());
  bda16w(0x13, 640);
  bda16w(0x1A, 0x1E); bda16w(0x1C, 0x1E);
  bda16w(0x80, 0x1E); bda16w(0x82, 0x3E);
  bda8w(0x96, 0x10);
  for (int i = 0; i < 4; i++) bda8w(0x78 + i, 20);
  for (int i = 0; i < 4; i++) bda8w(0x7C + i, 1);
  bda8w(0x75, (u8)bios_cfg.hdds);
  bda8w(0x8F, (u8)((bios_cfg.floppies > 0 ? 0x04 : 0) | (bios_cfg.floppies > 1 ? 0x40 : 0)));
  bda8w(0x90, 0x00); bda8w(0x91, 0x00);
  mem_wr32(BDA + 0xA8, ((u32)BIOS_SEG << 16) | BIOS_SAVE_PTR_OFF);
  /* IVT */
  for (int v = 0; v < 256; v++) mem_wr32((u32)v * 4, ((u32)BIOS_SEG << 16) | (BIOS_STUB_BASE + (u32)v * 16));
  mem_wr32(0x1E * 4, ((u32)BIOS_SEG << 16) | BIOS_DPT_OFF);
  mem_wr32(0x1F * 4, ((u32)BIOS_SEG << 16) | BIOS_FONT8HI_OFF);
  mem_wr32(0x41 * 4, ((u32)BIOS_SEG << 16) | BIOS_FDPT0_OFF);
  mem_wr32(0x46 * 4, ((u32)BIOS_SEG << 16) | BIOS_FDPT1_OFF);
  mem_wr32(0x43 * 4, ((u32)BIOS_SEG << 16) | BIOS_FONT16_OFF);
  /* fixed disk parameter tables */
  for (int i = 0; i < 2; i++) {
    u8 *t = rom(i == 0 ? BIOS_FDPT0_OFF : BIOS_FDPT1_OFF);
    dm_memset(t, 0, 16);
    Disk *d = &disks[2 + i];
    if (d->present) {
      st16(t, d->cyls); t[2] = d->heads; st16(t + 5, 0xFFFF); t[8] = 0xC0 | (d->heads > 8 ? 8 : 0);
      st16(t + 12, d->cyls); t[14] = d->spt;
    }
  }
  /* PICs, PIT, keyboard controller */
  io_wr8(0x20, 0x11); io_wr8(0x21, 0x08); io_wr8(0x21, 0x04); io_wr8(0x21, 0x01);
  io_wr8(0xA0, 0x11); io_wr8(0xA1, 0x70); io_wr8(0xA1, 0x02); io_wr8(0xA1, 0x01);
  io_wr8(0x21, 0xB8); /* enable IRQ0, IRQ1, IRQ2 (cascade), IRQ6 */
  io_wr8(0xA1, 0x8F); /* enable IRQ12-14 */
  io_wr8(0x43, 0x36); io_wr8(0x40, 0x00); io_wr8(0x40, 0x00);
  io_wr8(0x43, 0x54); io_wr8(0x41, 0x12);
  io_wr8(0x64, 0xAA); io_rd8(0x60);
  io_wr8(0x64, 0x60); io_wr8(0x60, 0x45);
  /* CMOS */
  cmos_set_byte(0x10, (u8)((bios_cfg.floppies > 0 ? bios_cfg.floppy_type[0] << 4 : 0) | (bios_cfg.floppies > 1 ? bios_cfg.floppy_type[1] : 0)));
  cmos_set_byte(0x12, (u8)((bios_cfg.hdds > 0 ? 0xF0 : 0) | (bios_cfg.hdds > 1 ? 0x0F : 0)));
  cmos_set_byte(0x14, (u8)equipment_word());
  cmos_set_byte(0x15, 0x80); cmos_set_byte(0x16, 0x02);
  u32 ext = bios_cfg.ram_kb > 1024 ? bios_cfg.ram_kb - 1024 : 0;
  if (ext > 0xFFFF) ext = 0xFFFF;
  cmos_set_byte(0x17, (u8)ext); cmos_set_byte(0x18, (u8)(ext >> 8));
  cmos_set_byte(0x30, (u8)ext); cmos_set_byte(0x31, (u8)(ext >> 8));
  cmos_set_byte(0x0F, 0);
  /* video */
  set_video_mode(3);
  /* banner */
  char line[96];
  bios_puts("DOS Mobile BIOS 0.1\r\n");
  __builtin_va_list ap;
  (void)ap;
  {
    char buf[96];
    int n = 0;
    const char *gen = cpu_gen_name(cpu.gen);
    while (*gen) buf[n++] = *gen++;
    buf[n] = 0;
    bios_puts(buf);
  }
  {
    u32 mhz = cpu_khz / 1000, frac = (cpu_khz % 1000) / 10;
    int n = 0;
    line[n++] = ' ';
    char num[16]; int k = 0; u32 v = mhz; if (v == 0) num[k++] = '0'; while (v) { num[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) line[n++] = num[--k];
    if (frac) { line[n++] = '.'; line[n++] = (char)('0' + frac / 10); if (frac % 10) line[n++] = (char)('0' + frac % 10); }
    const char *s = " MHz, ";
    while (*s) line[n++] = *s++;
    v = bios_cfg.ram_kb; k = 0; if (v == 0) num[k++] = '0'; while (v) { num[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) line[n++] = num[--k];
    s = " KB RAM OK\r\n";
    while (*s) line[n++] = *s++;
    line[n] = 0;
    bios_puts(line);
  }
  for (int i = 0; i < 2; i++) {
    if (!disks[i].present && i >= bios_cfg.floppies) continue;
    static const char *const names[] = {"", "360 KB", "1.2 MB", "720 KB", "1.44 MB", "2.88 MB"};
    bios_puts(i == 0 ? "Floppy A: " : "Floppy B: ");
    bios_puts(disks[i].present ? names[disks[i].type] : "empty");
    bios_puts("\r\n");
  }
  for (int i = 0; i < 2; i++) {
    if (!disks[2 + i].present) continue;
    u32 mb = disks[2 + i].sectors / 2048;
    char num[16]; int k = 0; u32 v = mb; if (v == 0) num[k++] = '0'; while (v) { num[k++] = (char)('0' + v % 10); v /= 10; }
    int n = 0;
    const char *s = i == 0 ? "Hard disk C: " : "Hard disk D: ";
    while (*s) line[n++] = *s++;
    while (k) line[n++] = num[--k];
    s = " MB\r\n";
    while (*s) line[n++] = *s++;
    line[n] = 0;
    bios_puts(line);
  }
  bios_puts("\r\n");
  cpu.eflags |= F_IF;
}

void bios_post_reset(int warm) {
  dm_memset(&kstate, 0, sizeof kstate);
  wait_done = 0;
  (void)warm;
}

/* ---------------- dispatcher ---------------- */
void int13(void);
void int19(void);
void int18(void);

void bios_hle(u8 fn) {
  switch (fn) {
    case HLE_POST: post(bda16(0x72) == 0x1234); break;
    case 0x08: int08(); break;
    case 0x09: int09(); break;
    case 0x10: int10(); break;
    case 0x11: SET_AX(bda16(0x10)); break;
    case 0x12: SET_AX(bda16(0x13)); break;
    case 0x13: int13(); break;
    case 0x14: SET_AX(0x0000); if (AH == 0 || AH == 3) SET_AH(0x80); break;
    case 0x15: int15(); break;
    case 0x16: int16(); break;
    case 0x17: SET_AH(0x90); break;
    case 0x18: int18(); break;
    case 0x19: int19(); break;
    case 0x1A: int1a(); break;
    case 0x70: { io_wr8(0x70, 0x0C); (void)io_rd8(0x71); break; }
    default: break;
  }
}

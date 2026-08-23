#include "mouse.h"
#include "cpu_int.h"
#include "bios.h"
#include "pic.h"
#include "sched.h"

typedef struct {
  int installed;
  int visible;       /* show/hide counter: >= 0 visible */
  s32 x, y;          /* virtual coordinates (already scaled) */
  s32 min_x, max_x, min_y, max_y;
  int buttons;
  s32 mick_x, mick_y;             /* accumulated mickeys since last read */
  int press_count[3], release_count[3];
  s32 press_x[3], press_y[3], release_x[3], release_y[3];
  u16 handler_mask, handler_seg, handler_off;
  int mick_per8_x, mick_per8_y;
  int double_speed;
  int mode_shift_x;               /* mode 13h reports x doubled */
  int text_mode, cell_w, cell_h, cols, rows;
  u16 gcursor_screen[16], gcursor_cursor[16];
  int hot_x, hot_y;
  int textcursor_type; u16 text_and, text_xor;
  int hide_x1, hide_y1, hide_x2, hide_y2, hide_active;
} Mouse;

static Mouse ms;
extern int cpu_trace_faults;

typedef struct { s16 dx, dy; u8 buttons; } MouseEvent;
#define MQ 64
static MouseEvent mq[MQ];
static u32 mq_head, mq_tail;

int mouse_present(void) { return 1; }

static void set_defaults(void) {
  ms.visible = -1;
  ms.handler_mask = 0;
  ms.mick_per8_x = 8;
  ms.mick_per8_y = 16;
  ms.double_speed = 64;
  ms.hide_active = 0;
  ms.textcursor_type = 0;
  ms.text_and = 0x77FF;
  ms.text_xor = 0x7700;
  ms.hot_x = 0; ms.hot_y = 0;
  static const u16 def_screen[16] = {0x3FFF, 0x1FFF, 0x0FFF, 0x07FF, 0x03FF, 0x01FF, 0x00FF, 0x007F,
                                     0x003F, 0x001F, 0x01FF, 0x00FF, 0x30FF, 0xF87F, 0xF87F, 0xFCFF};
  static const u16 def_cursor[16] = {0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00,
                                     0x7F80, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000};
  for (int i = 0; i < 16; i++) { ms.gcursor_screen[i] = def_screen[i]; ms.gcursor_cursor[i] = def_cursor[i]; }
  ms.x = (ms.min_x + ms.max_x + 1) / 2;
  ms.y = (ms.min_y + ms.max_y + 1) / 2;
  for (int b = 0; b < 3; b++) ms.press_count[b] = ms.release_count[b] = 0;
  ms.mick_x = ms.mick_y = 0;
}

void mouse_on_mode_change(int mode, int cols, int rows) {
  ms.text_mode = mode <= 3 || mode == 7;
  ms.cols = cols;
  ms.rows = rows;
  ms.cell_w = 8;
  ms.cell_h = 8;
  ms.mode_shift_x = mode == 0x13 ? 1 : 0;
  ms.min_x = 0;
  ms.min_y = 0;
  switch (mode) {
    case 0x00: case 0x01: ms.max_x = 639; ms.max_y = 199; break; /* 40 col: x granularity 16 */
    case 0x02: case 0x03: case 0x07: ms.max_x = 639; ms.max_y = 199; break;
    case 0x04: case 0x05: case 0x0D: case 0x13: ms.max_x = 639; ms.max_y = 199; break;
    case 0x06: case 0x0E: ms.max_x = 639; ms.max_y = 199; break;
    case 0x0F: case 0x10: ms.max_x = 639; ms.max_y = 349; break;
    case 0x11: case 0x12: ms.max_x = 639; ms.max_y = 479; break;
    default: ms.max_x = 639; ms.max_y = 199; break;
  }
  ms.x = (ms.max_x + 1) / 2;
  ms.y = (ms.max_y + 1) / 2;
  ms.visible = -1;
}

void mouse_init(void) {
  dm_memset(&ms, 0, sizeof ms);
  mq_head = mq_tail = 0;
  ms.installed = 1;
  mouse_on_mode_change(3, 80, 25);
  set_defaults();
}

void mouse_reset_state(void) {
  mouse_on_mode_change(3, 80, 25);
  set_defaults();
}

void mouse_host_event(int dx, int dy, int buttons) {
  if (mq_tail - mq_head >= MQ) return;
  MouseEvent *e = &mq[mq_tail++ % MQ];
  e->dx = (s16)(dx < -32000 ? -32000 : dx > 32000 ? 32000 : dx);
  e->dy = (s16)(dy < -32000 ? -32000 : dy > 32000 ? 32000 : dy);
  e->buttons = (u8)buttons;
  pic_raise_irq(12);
}

static void clamp(void) {
  if (ms.x < ms.min_x) ms.x = ms.min_x;
  if (ms.x > ms.max_x) ms.x = ms.max_x;
  if (ms.y < ms.min_y) ms.y = ms.min_y;
  if (ms.y > ms.max_y) ms.y = ms.max_y;
}

/* Process one queued host event; returns the INT 33h event mask. */
static int apply_event(void) {
  if (mq_head == mq_tail) return 0;
  if (cpu_trace_faults >= 2) dm_log("mouse apply dx=%d dy=%d b=%d -> x=%d y=%d", mq[mq_head % MQ].dx, mq[mq_head % MQ].dy, mq[mq_head % MQ].buttons, ms.x, ms.y);
  MouseEvent e = mq[mq_head++ % MQ];
  int mask = 0;
  if (e.dx || e.dy) {
    ms.mick_x += e.dx;
    ms.mick_y += e.dy;
    ms.x += (e.dx * 8) / ms.mick_per8_x;
    ms.y += (e.dy * 8) / ms.mick_per8_y;
    clamp();
    mask |= 0x01;
  }
  int changed = e.buttons ^ ms.buttons;
  for (int b = 0; b < 3; b++) {
    if (!(changed & (1 << b))) continue;
    if (e.buttons & (1 << b)) {
      ms.press_count[b]++;
      ms.press_x[b] = ms.x; ms.press_y[b] = ms.y;
      mask |= 2 << (b * 2);
    } else {
      ms.release_count[b]++;
      ms.release_x[b] = ms.x; ms.release_y[b] = ms.y;
      mask |= 4 << (b * 2);
    }
  }
  ms.buttons = e.buttons;
  return mask;
}

/* INT 74h (IRQ12) HLE body: runs inside the ROM stub. May redirect execution to the user handler;
 * the stub's following bytes do EOI + IRET and are used as the far-return target. */
void mouse_hle_irq(void) {
  int mask = apply_event();
  if (mq_head != mq_tail) pic_raise_irq(12); /* more queued */
  if (!mask) return;
  u16 use_mask = (u16)(mask & ms.handler_mask);
  if (!use_mask || !(ms.handler_seg | ms.handler_off)) return;
  /* far call into the guest handler; return lands after the HLE opcode (EOI+IRET follow) */
  u32 sp0 = sp_get();
  push16(cpu.seg[SEG_CS].sel);
  push16((u16)cpu.eip);
  if (cpu.fault_pending) { sp_set(sp0); return; }
  reg16_set(REG_AX, (u16)mask);
  reg16_set(REG_BX, (u16)ms.buttons);
  reg16_set(REG_CX, (u16)(ms.x << ms.mode_shift_x));
  reg16_set(REG_DX, (u16)ms.y);
  reg16_set(REG_SI, (u16)ms.mick_x);
  reg16_set(REG_DI, (u16)ms.mick_y);
  load_seg(SEG_DS, BIOS_SEG);
  load_seg(SEG_CS, ms.handler_seg);
  cpu.eip = ms.handler_off;
}

/* ---------------- INT 33h dispatch ---------------- */
#define AX reg16_get(REG_AX)
#define BX reg16_get(REG_BX)
#define CX reg16_get(REG_CX)
#define DX reg16_get(REG_DX)

void mouse_hle_int33(void) {
  u16 fn = AX;
  if (cpu_trace_faults >= 2) dm_log("INT33 fn=%04x x=%d y=%d btn=%d maxx=%d maxy=%d", fn, ms.x, ms.y, ms.buttons, ms.max_x, ms.max_y);
  switch (fn) {
    case 0x00: /* reset */
      mouse_reset_state();
      reg16_set(REG_AX, 0xFFFF);
      reg16_set(REG_BX, 3);
      break;
    case 0x01: if (ms.visible < 0) ms.visible++; break;
    case 0x02: ms.visible--; break;
    case 0x03:
      reg16_set(REG_BX, (u16)ms.buttons);
      reg16_set(REG_CX, (u16)(ms.x << ms.mode_shift_x));
      reg16_set(REG_DX, (u16)ms.y);
      break;
    case 0x04:
      ms.x = (s16)CX >> ms.mode_shift_x;
      ms.y = (s16)DX;
      clamp();
      break;
    case 0x05: {
      int b = BX < 3 ? BX : 0;
      reg16_set(REG_AX, (u16)ms.buttons);
      reg16_set(REG_BX, (u16)ms.press_count[b]);
      reg16_set(REG_CX, (u16)(ms.press_x[b] << ms.mode_shift_x));
      reg16_set(REG_DX, (u16)ms.press_y[b]);
      ms.press_count[b] = 0;
      break;
    }
    case 0x06: {
      int b = BX < 3 ? BX : 0;
      reg16_set(REG_AX, (u16)ms.buttons);
      reg16_set(REG_BX, (u16)ms.release_count[b]);
      reg16_set(REG_CX, (u16)(ms.release_x[b] << ms.mode_shift_x));
      reg16_set(REG_DX, (u16)ms.release_y[b]);
      ms.release_count[b] = 0;
      break;
    }
    case 0x07:
      ms.min_x = (s16)CX >> ms.mode_shift_x;
      ms.max_x = (s16)DX >> ms.mode_shift_x;
      if (ms.min_x > ms.max_x) { s32 t = ms.min_x; ms.min_x = ms.max_x; ms.max_x = t; }
      clamp();
      break;
    case 0x08:
      ms.min_y = (s16)CX;
      ms.max_y = (s16)DX;
      if (ms.min_y > ms.max_y) { s32 t = ms.min_y; ms.min_y = ms.max_y; ms.max_y = t; }
      clamp();
      break;
    case 0x09: { /* graphics cursor: ES:DX -> 32 words */
      u32 a = ((u32)cpu.seg[SEG_ES].sel << 4) + DX;
      ms.hot_x = (s16)BX;
      ms.hot_y = (s16)CX;
      for (int i = 0; i < 16; i++) ms.gcursor_screen[i] = lin_rd16(a + (u32)i * 2);
      for (int i = 0; i < 16; i++) ms.gcursor_cursor[i] = lin_rd16(a + 32 + (u32)i * 2);
      break;
    }
    case 0x0A:
      ms.textcursor_type = BX;
      ms.text_and = CX;
      ms.text_xor = DX;
      break;
    case 0x0B:
      reg16_set(REG_CX, (u16)ms.mick_x);
      reg16_set(REG_DX, (u16)ms.mick_y);
      ms.mick_x = ms.mick_y = 0;
      break;
    case 0x0C:
      ms.handler_mask = CX;
      ms.handler_off = DX;
      ms.handler_seg = cpu.seg[SEG_ES].sel;
      break;
    case 0x0F:
      ms.mick_per8_x = CX ? CX : 8;
      ms.mick_per8_y = DX ? DX : 16;
      break;
    case 0x10: ms.hide_active = 0; break; /* conditional off: treated as a no-op region */
    case 0x13: ms.double_speed = DX ? DX : 64; break;
    case 0x14: { /* exchange handlers */
      u16 om = ms.handler_mask, os = ms.handler_seg, oo = ms.handler_off;
      ms.handler_mask = CX;
      ms.handler_off = DX;
      ms.handler_seg = cpu.seg[SEG_ES].sel;
      reg16_set(REG_CX, om);
      reg16_set(REG_DX, oo);
      load_seg(SEG_ES, os);
      break;
    }
    case 0x15: reg16_set(REG_BX, 0x100); break;
    case 0x16: { /* save state */
      u32 a = ((u32)cpu.seg[SEG_ES].sel << 4) + DX;
      for (u32 i = 0; i < sizeof(Mouse) && i < 0x100; i++) lin_wr8(a + i, ((u8 *)&ms)[i]);
      break;
    }
    case 0x17: { /* restore state */
      u32 a = ((u32)cpu.seg[SEG_ES].sel << 4) + DX;
      for (u32 i = 0; i < sizeof(Mouse) && i < 0x100; i++) ((u8 *)&ms)[i] = lin_rd8(a + i);
      break;
    }
    case 0x1A:
      ms.mick_per8_x = BX ? BX : 8;
      ms.mick_per8_y = CX ? CX : 16;
      ms.double_speed = DX ? DX : 64;
      break;
    case 0x1B:
      reg16_set(REG_BX, (u16)ms.mick_per8_x);
      reg16_set(REG_CX, (u16)ms.mick_per8_y);
      reg16_set(REG_DX, (u16)ms.double_speed);
      break;
    case 0x1C: case 0x1D: case 0x1E: break;
    case 0x1F: /* disable driver */
      reg16_set(REG_AX, 0x001F);
      load_seg(SEG_ES, 0);
      reg16_set(REG_BX, 0);
      break;
    case 0x20: break; /* enable driver */
    case 0x21:
      set_defaults();
      reg16_set(REG_AX, 0xFFFF);
      reg16_set(REG_BX, 3);
      break;
    case 0x24:
      reg16_set(REG_BX, 0x0805); /* version 8.05 */
      reg16_set(REG_CX, 0x0400); /* PS/2 */
      break;
    default:
      break;
  }
}

/* ---------------- cursor overlay ---------------- */
void mouse_overlay(u32 *fb, int w, int h, int text_mode, int cell_w, int cell_h) {
  if (ms.visible < 0 || !ms.installed) return;
  if (text_mode) {
    int col = ms.x / 8, row = ms.y / 8;
    if (col < 0 || row < 0 || col >= ms.cols || row >= ms.rows) return;
    int x0 = col * cell_w, y0 = row * cell_h;
    for (int y = y0; y < y0 + cell_h && y < h; y++)
      for (int x = x0; x < x0 + cell_w && x < w; x++) fb[y * w + x] ^= 0x00FFFFFFu;
    return;
  }
  int sx = (ms.x - ms.min_x) * w / (ms.max_x - ms.min_x + 1);
  int sy = (ms.y - ms.min_y) * h / (ms.max_y - ms.min_y + 1);
  sx -= ms.hot_x;
  sy -= ms.hot_y;
  for (int r = 0; r < 16; r++) {
    int y = sy + r;
    if (y < 0 || y >= h) continue;
    for (int c = 0; c < 16; c++) {
      int x = sx + c;
      if (x < 0 || x >= w) continue;
      u16 bit = (u16)(0x8000 >> c);
      u32 *px = &fb[y * w + x];
      if (!(ms.gcursor_screen[r] & bit)) *px &= 0xFF000000u;
      if (ms.gcursor_cursor[r] & bit) *px |= 0x00FFFFFFu;
    }
  }
}

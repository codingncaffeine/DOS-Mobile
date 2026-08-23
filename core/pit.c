#include "pit.h"
#include "io.h"
#include "sched.h"
#include "pic.h"

typedef struct {
  u8 mode;        /* 0-5 */
  u8 access;      /* 1 = lo, 2 = hi, 3 = lo/hi */
  u8 bcd;
  u8 flip;        /* next write/read selects hi byte */
  u8 latched;     /* count latched for reading */
  u16 latch;
  u16 reload;     /* 0 means 65536 */
  u8 loaded;      /* a count has been written since programming */
  u8 gate;
  u8 out;
  s64 load_tick;  /* PIT tick when the current count started */
  u8 wlo;         /* partial write buffer */
} PitChan;

static PitChan ch[3];
static u8 port61;

static s64 pit_now(void) { return ns_to_rate(emu_ns, PIT_HZ); }
static s64 tick_to_ns(s64 tick) { return rate_to_ns(tick, PIT_HZ); }
static u32 period(PitChan *c) { return c->reload ? c->reload : 65536; }

static void ch0_event(void);

static void schedule_irq0(void) {
  PitChan *c = &ch[0];
  if (!c->loaded) { sched_cancel(EV_PIT0); return; }
  s64 at;
  switch (c->mode) {
    case 0: case 1: case 4: case 5:
      at = c->load_tick + period(c);
      break;
    default: { /* modes 2/3: periodic */
      s64 now = pit_now();
      s64 elapsed = now - c->load_tick;
      u32 p = period(c);
      s64 n = elapsed < 0 ? 0 : elapsed / p + 1;
      at = c->load_tick + n * p;
      break;
    }
  }
  sched_set(EV_PIT0, tick_to_ns(at), ch0_event);
}

static void ch0_event(void) {
  PitChan *c = &ch[0];
  pic_raise_irq(0);
  c->out = 1;
  if (c->mode == 2 || c->mode == 3) schedule_irq0();
}

/* Current counter value as the guest would read it. */
static u16 current_count(PitChan *c) {
  if (!c->loaded) return 0;
  s64 elapsed = pit_now() - c->load_tick;
  if (elapsed < 0) elapsed = 0;
  u32 p = period(c);
  switch (c->mode) {
    case 3: {
      s64 e2 = (elapsed * 2) % p;
      return (u16)(p - e2);
    }
    case 2: return (u16)(p - (elapsed % p));
    default: return (u16)((p - elapsed) & 0xFFFF);
  }
}

static int chan_output(PitChan *c) {
  if (!c->loaded) return c->mode == 0 ? 0 : 1;
  s64 elapsed = pit_now() - c->load_tick;
  if (elapsed < 0) elapsed = 0;
  u32 p = period(c);
  switch (c->mode) {
    case 0: return elapsed >= p;
    case 2: return (elapsed % p) != p - 1;
    case 3: return (elapsed % p) < (p + 1) / 2;
    default: return 1;
  }
}

int pit_ch2_output(void) { return ch[2].gate ? chan_output(&ch[2]) : 1; }
int pit_speaker_output(void) { return (port61 & 3) == 3 && chan_output(&ch[2]); }
u32 pit_ch2_reload(void) { return period(&ch[2]); }

static void pit_write_count(PitChan *c, u16 v) {
  c->reload = v;
  c->loaded = 1;
  c->load_tick = pit_now();
  if (c == &ch[0]) schedule_irq0();
}

static void wr_pit(u16 port, u8 v) {
  int idx = port & 3;
  if (idx == 3) { /* control word */
    int sel = v >> 6;
    if (sel == 3) { /* 8254 read-back */
      for (int i = 0; i < 3; i++) {
        if (!(v & (2 << i))) continue;
        if (!(v & 0x20) && !ch[i].latched) { ch[i].latch = current_count(&ch[i]); ch[i].latched = 1; }
      }
      return;
    }
    PitChan *c = &ch[sel];
    int access = (v >> 4) & 3;
    if (access == 0) { /* latch */
      if (!c->latched) { c->latch = current_count(c); c->latched = 1; }
      return;
    }
    c->access = (u8)access;
    c->mode = (v >> 1) & 7;
    if (c->mode > 5) c->mode -= 4;
    c->bcd = v & 1;
    c->flip = 0;
    c->loaded = 0;
    c->latched = 0;
    if (sel == 0) sched_cancel(EV_PIT0);
    return;
  }
  PitChan *c = &ch[idx];
  switch (c->access) {
    case 1: pit_write_count(c, v); break;
    case 2: pit_write_count(c, (u16)(v << 8)); break;
    default:
      if (!c->flip) { c->wlo = v; c->flip = 1; }
      else { c->flip = 0; pit_write_count(c, (u16)(c->wlo | (v << 8))); }
      break;
  }
}

static u8 rd_pit(u16 port) {
  int idx = port & 3;
  if (idx == 3) return 0xFF;
  PitChan *c = &ch[idx];
  u16 val = c->latched ? c->latch : current_count(c);
  switch (c->access) {
    case 1: c->latched = 0; return (u8)val;
    case 2: c->latched = 0; return (u8)(val >> 8);
    default:
      if (!c->flip) { c->flip = 1; return (u8)val; }
      c->flip = 0;
      c->latched = 0;
      return (u8)(val >> 8);
  }
}

static u8 rd_61(u16 port) {
  (void)port;
  u8 v = port61 & 0x0F;
  if ((emu_ns / 15000) & 1) v |= 0x10; /* DRAM refresh toggle (~15 µs) */
  if (pit_ch2_output()) v |= 0x20;
  return v;
}
static void wr_61(u16 port, u8 v) {
  (void)port;
  port61 = v;
  ch[2].gate = v & 1;
}

void pit_init(void) {
  dm_memset(ch, 0, sizeof ch);
  port61 = 0;
  for (int i = 0; i < 3; i++) { ch[i].access = 3; ch[i].mode = 3; }
  ch[2].gate = 0;
}

void pit_register_ports(void) {
  io_register(0x40, 4, rd_pit, wr_pit);
  io_register(0x61, 1, rd_61, wr_61);
}

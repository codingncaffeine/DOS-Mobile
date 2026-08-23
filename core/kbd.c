#include "kbd.h"
#include "io.h"
#include "sched.h"
#include "pic.h"
#include "mem.h"
#include "machine.h"

#define QSIZE 256

static struct {
  u8 queue[QSIZE];
  u32 qhead, qtail;
  u8 out;          /* output buffer */
  u8 obf;          /* output buffer full */
  u8 status_cmd;   /* last write went to 64h */
  u8 cmdbyte;      /* controller command byte (20h/60h) */
  u8 pending_cmd;  /* controller command awaiting a data byte */
  u8 kbd_cmd;      /* keyboard command awaiting a data byte */
  u8 kbd_enabled;
  u8 leds;
  u8 scan_set;
  u8 aux_out;      /* byte in the output buffer came from the mouse */
} kc;

static u32 qcount(void) { return kc.qtail - kc.qhead; }

static void kbd_deliver(void);

static void queue_byte(u8 b) {
  if (qcount() >= QSIZE) return;
  kc.queue[kc.qtail++ % QSIZE] = b;
  if (!kc.obf && !sched_active(EV_KBD)) sched_set(EV_KBD, emu_ns + 1000, kbd_deliver);
}

/* Move the next queued byte into the output buffer and signal IRQ1. */
static void kbd_deliver(void) {
  if (kc.obf || qcount() == 0) return;
  kc.out = kc.queue[kc.qhead++ % QSIZE];
  kc.obf = 1;
  kc.aux_out = 0;
  if (kc.cmdbyte & 1) pic_raise_irq(1);
}

void kbd_push_scancode(u8 code) {
  if (!kc.kbd_enabled) return;
  queue_byte(code);
}

int kbd_queue_free(void) { return (int)(QSIZE - qcount()); }

static void kbd_device_cmd(u8 v) {
  if (kc.kbd_cmd) {
    u8 c = kc.kbd_cmd;
    kc.kbd_cmd = 0;
    switch (c) {
      case 0xED: kc.leds = v & 7; queue_byte(0xFA); return;
      case 0xF0:
        queue_byte(0xFA);
        if (v == 0) queue_byte(kc.scan_set);
        else kc.scan_set = v;
        return;
      case 0xF3: queue_byte(0xFA); return;
      default: queue_byte(0xFA); return;
    }
  }
  switch (v) {
    case 0xED: case 0xF0: case 0xF3: kc.kbd_cmd = v; queue_byte(0xFA); return;
    case 0xEE: queue_byte(0xEE); return;
    case 0xF2: queue_byte(0xFA); queue_byte(0xAB); queue_byte(0x83); return;
    case 0xF4: kc.kbd_enabled = 1; queue_byte(0xFA); return;
    case 0xF5: case 0xF6: queue_byte(0xFA); return;
    case 0xFF: queue_byte(0xFA); queue_byte(0xAA); return;
    default: queue_byte(0xFA); return;
  }
}

static void wr_60(u16 port, u8 v) {
  (void)port;
  kc.status_cmd = 0;
  if (kc.pending_cmd) {
    u8 c = kc.pending_cmd;
    kc.pending_cmd = 0;
    switch (c) {
      case 0x60: kc.cmdbyte = v; return;
      case 0xD1: /* output port: bit 1 = A20, bit 0 = 0 resets the CPU */
        mem_set_a20((v >> 1) & 1);
        if (!(v & 1)) machine_reset_request();
        return;
      case 0xD2: queue_byte(v); return;
      case 0xD3: return; /* mouse output buffer write (no mouse yet) */
      case 0xD4: return; /* command to the mouse (no mouse yet) */
      default: return;
    }
  }
  kbd_device_cmd(v);
}

static void wr_64(u16 port, u8 v) {
  (void)port;
  kc.status_cmd = 1;
  switch (v) {
    case 0x20: queue_byte(kc.cmdbyte); return;
    case 0x60: case 0xD1: case 0xD2: case 0xD3: case 0xD4: kc.pending_cmd = v; return;
    case 0xA7: case 0xA8: return;
    case 0xA9: queue_byte(0x00); return;
    case 0xAA: queue_byte(0x55); return;
    case 0xAB: queue_byte(0x00); return;
    case 0xAD: kc.cmdbyte |= 0x10; return;
    case 0xAE: kc.cmdbyte &= (u8)~0x10; return;
    case 0xC0: queue_byte(0xFF); return;
    case 0xD0: queue_byte((u8)(0x01 | (a20_mask == 0xFFFFFFFFu ? 0x02 : 0) | 0x10 | 0x20 | 0xC0)); return;
    case 0xE0: queue_byte(0xFF); return;
    case 0xFE: machine_reset_request(); return;
    default:
      if (v >= 0xF0) return; /* pulse output lines */
      return;
  }
}

static u8 rd_60(u16 port) {
  (void)port;
  u8 v = kc.out;
  if (kc.obf) {
    kc.obf = 0;
    pic_lower_irq(1);
    if (qcount() && !sched_active(EV_KBD)) sched_set(EV_KBD, emu_ns + 500000, kbd_deliver);
  }
  return v;
}

static u8 rd_64(u16 port) {
  (void)port;
  u8 s = 0x10 | 0x04; /* keyboard not inhibited, self test passed */
  if (kc.obf) s |= 0x01;
  if (kc.status_cmd) s |= 0x08;
  if (kc.aux_out) s |= 0x20;
  return s;
}

void kbd_init(void) {
  dm_memset(&kc, 0, sizeof kc);
  kc.cmdbyte = 0x45; /* IRQ1 enabled, system flag, translation */
  kc.kbd_enabled = 1;
  kc.scan_set = 2;
}

void kbd_register_ports(void) {
  io_register(0x60, 1, rd_60, wr_60);
  io_register(0x64, 1, rd_64, wr_64);
}

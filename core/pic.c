#include "pic.h"
#include "io.h"

typedef struct {
  u8 irr, isr, imr;
  u8 icw_step;   /* 0 = idle, 2/3/4 = waiting for ICWn */
  u8 icw4_needed;
  u8 vector_base;
  u8 auto_eoi;
  u8 read_isr;   /* OCW3 selection for reads of port 0 */
  u8 level;      /* level-triggered mode (ICW1 bit 3) */
} PIC;

static PIC pics[2];

void pic_init(void) {
  dm_memset(pics, 0, sizeof pics);
  pics[0].imr = 0xFF;
  pics[1].imr = 0xFF;
  pics[0].vector_base = 0x08;
  pics[1].vector_base = 0x70;
}

/* highest-priority pending IRQ on one controller, or -1 (priority = lowest number first) */
static int pic_highest(PIC *p) {
  u8 req = p->irr & (u8)~p->imr;
  if (!req) return -1;
  for (int i = 0; i < 8; i++) {
    if (p->isr & (1 << i)) return -1; /* an equal/higher priority ISR blocks */
    if (req & (1 << i)) return i;
  }
  return -1;
}

static void update_cascade(void) {
  if (pic_highest(&pics[1]) >= 0) pics[0].irr |= 1 << 2;
  else pics[0].irr &= (u8)~(1 << 2);
}

void pic_raise_irq(int irq) {
  if (irq < 8) pics[0].irr |= (u8)(1 << irq);
  else pics[1].irr |= (u8)(1 << (irq - 8));
}

void pic_lower_irq(int irq) {
  if (irq < 8) pics[0].irr &= (u8)~(1 << irq);
  else pics[1].irr &= (u8)~(1 << (irq - 8));
}

int pic_has_pending(void) {
  update_cascade();
  return pic_highest(&pics[0]) >= 0;
}

u8 pic_ack(void) {
  update_cascade();
  int irq = pic_highest(&pics[0]);
  if (irq < 0) return pics[0].vector_base + 7; /* spurious */
  if (irq == 2) {
    int s = pic_highest(&pics[1]);
    if (s < 0) return pics[0].vector_base + 7;
    pics[1].irr &= (u8)~(1 << s);
    if (!pics[1].auto_eoi) pics[1].isr |= (u8)(1 << s);
    if (!pics[0].auto_eoi) pics[0].isr |= 1 << 2;
    update_cascade();
    return (u8)(pics[1].vector_base + s);
  }
  pics[0].irr &= (u8)~(1 << irq);
  if (!pics[0].auto_eoi) pics[0].isr |= (u8)(1 << irq);
  return (u8)(pics[0].vector_base + irq);
}

static void pic_write(PIC *p, int reg, u8 v) {
  if (reg == 0) {
    if (v & 0x10) { /* ICW1 */
      p->icw_step = 2;
      p->icw4_needed = v & 1;
      p->level = (v >> 3) & 1;
      p->imr = 0;
      p->isr = 0;
      p->irr = 0;
      p->auto_eoi = 0;
      p->read_isr = 0;
      return;
    }
    if (v & 0x08) { /* OCW3 */
      if (v & 2) p->read_isr = v & 1;
      return;
    }
    /* OCW2 */
    int cmd = v >> 5;
    switch (cmd) {
      case 1: /* non-specific EOI */
      case 5: /* rotate on non-specific EOI */
        for (int i = 0; i < 8; i++)
          if (p->isr & (1 << i)) { p->isr &= (u8)~(1 << i); break; }
        break;
      case 3: /* specific EOI */
      case 7:
        p->isr &= (u8)~(1 << (v & 7));
        break;
      default:
        break;
    }
    return;
  }
  /* reg 1 */
  switch (p->icw_step) {
    case 2:
      p->vector_base = v & 0xF8;
      p->icw_step = 3;
      return;
    case 3:
      p->icw_step = p->icw4_needed ? 4 : 0;
      return;
    case 4:
      p->auto_eoi = (v >> 1) & 1;
      p->icw_step = 0;
      return;
    default:
      p->imr = v; /* OCW1 */
      return;
  }
}

static u8 pic_read(PIC *p, int reg) {
  if (reg == 1) return p->imr;
  return p->read_isr ? p->isr : p->irr;
}

static u8 rd_master(u16 port) { return pic_read(&pics[0], port & 1); }
static void wr_master(u16 port, u8 v) { pic_write(&pics[0], port & 1, v); }
static u8 rd_slave(u16 port) { return pic_read(&pics[1], port & 1); }
static void wr_slave(u16 port, u8 v) { pic_write(&pics[1], port & 1, v); }

void pic_register_ports(void) {
  io_register(0x20, 2, rd_master, wr_master);
  io_register(0xA0, 2, rd_slave, wr_slave);
}

/* INT 13h disk services, INT 19h bootstrap, INT 18h no-boot handler. */
#include "bios.h"
#include "cpu_int.h"
#include "disk.h"
#include "machine.h"

#define AL reg8_get(0)
#define AH reg8_get(4)
#define DL reg8_get(2)
#define DH reg8_get(6)
#define CL reg8_get(1)
#define CH reg8_get(5)
#define BX reg16_get(REG_BX)
#define SI reg16_get(REG_SI)
#define DS_SEL cpu.seg[SEG_DS].sel
#define ES_SEL cpu.seg[SEG_ES].sel

static u32 frame_flags_addr(void) { return cpu.seg[SEG_SS].base + ((cpu.r[REG_SP] + 4) & sp_mask()); }

static void done(int slot, u8 status) {
  reg8_set(4, status);
  u32 a = frame_flags_addr();
  u16 f = lin_rd16(a);
  f = status ? (u16)(f | F_CF) : (u16)(f & ~F_CF);
  lin_wr16(a, f);
  set_cf(status != 0);
  if (slot >= 2) lin_wr8(BDA + 0x74, status);
  else lin_wr8(BDA + 0x41, status);
}

static int chs_to_lba(Disk *d, int cyl, int head, int sector, u32 *lba) {
  if (sector < 1 || sector > d->spt || head >= d->heads || cyl >= d->cyls) return 0;
  *lba = ((u32)cyl * d->heads + (u32)head) * d->spt + (u32)(sector - 1);
  return 1;
}

extern int cpu_trace_faults;
void int13(void) {
  u8 fn = AH, drive = DL;
  if (cpu_trace_faults >= 2) dm_log("INT13 fn=%02x dl=%02x ch=%02x cl=%02x dh=%02x al=%02x es:bx=%04x:%04x", fn, drive, CH, CL, DH, AL, ES_SEL, BX);
  int slot = disk_slot_for_bios(drive);
  Disk *d = slot >= 0 ? &disks[slot] : NULL;
  int floppy = drive < 0x80;
  int drive_exists = floppy ? (drive < (u8)bios_cfg.floppies) : (d && d->present);

  switch (fn) {
    case 0x00: case 0x0D: case 0x11: case 0x14: case 0x0C: case 0x09:
      if (slot < 0) { done(0, 0x01); return; }
      done(slot, 0);
      return;
    case 0x01:
      reg8_set(0, lin_rd8(BDA + (floppy ? 0x41 : 0x74)));
      done(slot < 0 ? 0 : slot, 0);
      reg8_set(4, lin_rd8(BDA + (floppy ? 0x41 : 0x74)));
      return;
    case 0x02: case 0x03: case 0x04: {
      if (!drive_exists) { done(slot < 0 ? 0 : slot, 0x01); return; }
      if (!d->present) { done(slot, 0x80); return; }
      if (floppy && d->changed && fn != 0x04) { d->changed = 0; done(slot, 0x06); return; }
      int cyl = CH | ((CL & 0xC0) << 2), sector = CL & 0x3F, head = DH;
      u32 count = AL, lba;
      if (!chs_to_lba(d, cyl, head, sector, &lba)) { done(slot, 0x04); return; }
      if (count == 0 || count > 128) { done(slot, 0x01); return; }
      u32 phys = ((u32)ES_SEL << 4) + BX;
      int st;
      if (fn == 0x02) st = disk_read(slot, lba, count, phys);
      else if (fn == 0x03) st = disk_write(slot, lba, count, phys);
      else st = (lba + count <= d->sectors) ? 0 : 0x04;
      if (st == DISK_ST_PENDING) { /* async sectors in flight: rerun this INT 13h after a wake */
        cpu.eip = cpu.eip_start;
        cpu.eflags |= F_IF;
        cpu.halted = 1;
        return;
      }
      if (st == 0) reg8_set(0, (u8)count);
      done(slot, (u8)st);
      return;
    }
    case 0x05: /* format track: the image is already laid out */
      if (!drive_exists || !d->present) { done(slot < 0 ? 0 : slot, 0x80); return; }
      done(slot, 0);
      return;
    case 0x08: {
      if (!drive_exists) {
        reg8_set(2, floppy ? (u8)bios_cfg.floppies : (u8)bios_cfg.hdds);
        done(slot < 0 ? 0 : slot, floppy ? 0x01 : 0x07);
        return;
      }
      if (floppy) {
        int type = d->present ? d->type : bios_cfg.floppy_type[drive];
        u16 cyls = d->present ? d->cyls : 80, spt = d->present ? d->spt : 18;
        u8 heads = d->present ? d->heads : 2;
        reg8_set(3, (u8)type);
        reg8_set(5, (u8)((cyls - 1) & 0xFF));
        reg8_set(1, (u8)(spt | (((cyls - 1) >> 2) & 0xC0)));
        reg8_set(6, (u8)(heads - 1));
        reg8_set(2, (u8)bios_cfg.floppies);
        reg8_set(0, 0);
        cpu_load_seg(SEG_ES, BIOS_SEG);
        reg16_set(REG_DI, BIOS_DPT_OFF);
        done(slot, 0);
      } else {
        u16 maxc = (u16)(d->cyls - 1);
        if (maxc > 1023) maxc = 1023;
        reg8_set(5, (u8)(maxc & 0xFF));
        reg8_set(1, (u8)(d->spt | ((maxc >> 2) & 0xC0)));
        reg8_set(6, (u8)(d->heads - 1));
        reg8_set(2, (u8)bios_cfg.hdds);
        reg8_set(0, 0);
        done(slot, 0);
      }
      return;
    }
    case 0x0A: case 0x0B: done(slot < 0 ? 0 : slot, 0x01); return;
    case 0x10: done(slot < 0 ? 0 : slot, (d && d->present) ? 0 : 0xAA); return;
    case 0x15: {
      if (!drive_exists) { reg8_set(4, 0); done(slot < 0 ? 0 : slot, 0); reg8_set(4, 0); return; }
      if (floppy) { reg8_set(4, 0x02); done(slot, 0); reg8_set(4, 0x02); }
      else {
        reg16_set(REG_CX, (u16)(d->sectors >> 16));
        reg16_set(REG_DX, (u16)d->sectors);
        done(slot, 0);
        reg8_set(4, 0x03);
      }
      return;
    }
    case 0x16: {
      if (!floppy) { done(slot < 0 ? 0 : slot, 0x01); return; }
      if (!drive_exists) { done(0, 0x80); return; }
      if (!d->present) { done(slot, 0x80); return; }
      if (d->changed) { d->changed = 0; done(slot, 0x06); return; }
      done(slot, 0);
      return;
    }
    case 0x17: case 0x18: {
      if (!floppy || !drive_exists) { done(slot < 0 ? 0 : slot, 0x01); return; }
      cpu_load_seg(SEG_ES, BIOS_SEG);
      reg16_set(REG_DI, BIOS_DPT_OFF);
      done(slot, 0);
      return;
    }
    case 0x41: { /* EDD installation check */
      if (floppy || !drive_exists) { done(slot < 0 ? 0 : slot, 0x01); return; }
      if (BX != 0x55AA) { done(slot, 0x01); return; }
      reg16_set(REG_BX, 0xAA55);
      reg8_set(4, 0x01);
      reg16_set(REG_CX, 0x0001);
      done(slot, 0);
      reg8_set(4, 0x01);
      return;
    }
    case 0x42: case 0x43: { /* extended read/write via disk address packet at DS:SI */
      if (floppy || !drive_exists) { done(slot < 0 ? 0 : slot, 0x01); return; }
      u32 pkt = ((u32)DS_SEL << 4) + SI;
      u32 count = lin_rd16(pkt + 2);
      u32 off = lin_rd16(pkt + 4), seg = lin_rd16(pkt + 6);
      u32 lba = lin_rd32(pkt + 8);
      u32 lba_hi = lin_rd32(pkt + 12);
      if (lba_hi || count > 127) { done(slot, 0x01); return; }
      int st = fn == 0x42 ? disk_read(slot, lba, count, (seg << 4) + off) : disk_write(slot, lba, count, (seg << 4) + off);
      if (st == DISK_ST_PENDING) { /* async sectors in flight: rerun this INT 13h after a wake */
        cpu.eip = cpu.eip_start;
        cpu.eflags |= F_IF;
        cpu.halted = 1;
        return;
      }
      if (st) lin_wr16(pkt + 2, 0);
      done(slot, (u8)st);
      return;
    }
    case 0x48: {
      if (floppy || !drive_exists) { done(slot < 0 ? 0 : slot, 0x01); return; }
      u32 buf = ((u32)DS_SEL << 4) + SI;
      u16 size = lin_rd16(buf);
      if (size < 26) { done(slot, 0x01); return; }
      lin_wr16(buf, 26);
      lin_wr16(buf + 2, 0x0002); /* CHS info valid */
      lin_wr32(buf + 4, d->cyls);
      lin_wr32(buf + 8, d->heads);
      lin_wr32(buf + 12, d->spt);
      lin_wr32(buf + 16, d->sectors);
      lin_wr32(buf + 20, 0);
      lin_wr16(buf + 24, 512);
      done(slot, 0);
      return;
    }
    default:
      done(slot < 0 ? 0 : slot, 0x01);
      return;
  }
}

static int try_boot(int slot, u8 dl) {
  if (!disks[slot].present) return 0;
  if (disk_read(slot, 0, 1, 0x7C00) != 0) return 0;
  if (slot >= 2 && lin_rd16(0x7DFE) != 0xAA55) return 0;
  if (slot < 2) disks[slot].changed = 0;
  /* hand over to the boot sector */
  cpu_load_seg(SEG_CS, 0);
  cpu.eip = 0x7C00;
  cpu_load_seg(SEG_DS, 0);
  cpu_load_seg(SEG_ES, 0);
  cpu_load_seg(SEG_SS, 0);
  cpu.r[REG_SP] = 0x7C00;
  reg16_set(REG_DX, dl);
  cpu.eflags |= F_IF;
  return 1;
}

void int19(void) {
  /* boot order: A:, then C: */
  if (try_boot(0, 0x00)) return;
  if (try_boot(2, 0x80)) return;
  /* nothing bootable: fall through to INT 18h in the stub */
}

static int waiting_key;

void int18(void) {
  if (!waiting_key) {
    bios_puts("\r\nNo bootable disk. Insert a disk image and press any key to retry.\r\n");
    waiting_key = 1;
  }
  /* wait for a keystroke, then retry the boot */
  u16 head = lin_rd16(BDA + 0x1A), tail = lin_rd16(BDA + 0x1C);
  if (head == tail) {
    cpu.eip = cpu.eip_start;
    cpu.eflags |= F_IF;
    cpu.halted = 1;
    return;
  }
  lin_wr16(BDA + 0x1A, tail); /* drain */
  waiting_key = 0;
}

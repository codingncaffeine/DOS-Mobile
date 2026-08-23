#include "disk.h"
#include "mem.h"
#include "cpu_int.h"

Disk disks[DISK_SLOTS];
static u8 bounce[512 * 128];

void disk_init(void) { dm_memset(disks, 0, sizeof disks); }

static int floppy_geometry(Disk *d, u32 sectors) {
  switch (sectors) {
    case 320: d->cyls = 40; d->heads = 1; d->spt = 8; d->type = 1; break;   /* 160K */
    case 360: d->cyls = 40; d->heads = 1; d->spt = 9; d->type = 1; break;   /* 180K */
    case 640: d->cyls = 40; d->heads = 2; d->spt = 8; d->type = 1; break;   /* 320K */
    case 720: d->cyls = 40; d->heads = 2; d->spt = 9; d->type = 1; break;   /* 360K */
    case 1440: d->cyls = 80; d->heads = 2; d->spt = 9; d->type = 3; break;  /* 720K */
    case 2400: d->cyls = 80; d->heads = 2; d->spt = 15; d->type = 2; break; /* 1.2M */
    case 2880: d->cyls = 80; d->heads = 2; d->spt = 18; d->type = 4; break; /* 1.44M */
    case 3360: d->cyls = 80; d->heads = 2; d->spt = 21; d->type = 4; break; /* 1.68M */
    case 5760: d->cyls = 80; d->heads = 2; d->spt = 36; d->type = 5; break; /* 2.88M */
    default: return 0;
  }
  return 1;
}

int disk_attach(int slot, u32 sectors, int readonly) {
  if (slot < 0 || slot >= DISK_SLOTS) return -1;
  Disk *d = &disks[slot];
  dm_memset(d, 0, sizeof *d);
  d->sectors = sectors;
  d->readonly = readonly;
  if (slot < 2) {
    if (!floppy_geometry(d, sectors)) return -2;
    d->changed = 1;
  } else {
    d->is_hdd = 1;
    d->spt = 63;
    d->heads = sectors > 1024u * 16 * 63 ? 255 : 16;
    u32 cyls = sectors / (d->heads * d->spt);
    if (cyls > 1024 && d->heads == 16) { d->heads = 255; cyls = sectors / (255u * 63); }
    if (cyls > 1024) cyls = 1024;
    if (cyls == 0) cyls = 1;
    d->cyls = (u16)cyls;
  }
  d->present = 1;
  return 0;
}

void disk_detach(int slot) {
  if (slot < 0 || slot >= DISK_SLOTS) return;
  disks[slot].present = 0;
  if (slot < 2) disks[slot].changed = 1;
}

int disk_slot_for_bios(u8 dl) {
  if (dl < 2) return dl;
  if (dl >= 0x80 && dl < 0x80 + (DISK_SLOTS - 2)) return 2 + (dl - 0x80);
  return -1;
}

int disk_read(int slot, u32 lba, u32 count, u32 phys) {
  Disk *d = &disks[slot];
  if (!d->present) return 0x80;
  if (lba + count > d->sectors || count == 0) return 0x04;
  while (count) {
    u32 n = count > 128 ? 128 : count;
    int r = host_disk_read(slot, lba, n, bounce);
    if (r == 2) return DISK_ST_PENDING; /* async source still loading; caller retries */
    if (r != 0) return 0x04;
    lin_copy_in(phys, bounce, n * 512);
    lba += n;
    phys += n * 512;
    count -= n;
  }
  return 0;
}

int disk_write(int slot, u32 lba, u32 count, u32 phys) {
  Disk *d = &disks[slot];
  if (!d->present) return 0x80;
  if (d->readonly) return 0x03;
  if (lba + count > d->sectors || count == 0) return 0x04;
  while (count) {
    u32 n = count > 128 ? 128 : count;
    lin_copy_out(bounce, phys, n * 512);
    if (host_disk_write(slot, lba, n, bounce) != 0) return 0x04;
    lba += n;
    phys += n * 512;
    count -= n;
  }
  return 0;
}

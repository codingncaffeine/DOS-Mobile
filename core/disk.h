/* Disk images served by the host; the BIOS INT 13h layer addresses them by CHS or LBA. */
#pragma once
#include "platform.h"

typedef struct {
  int present;
  int is_hdd;
  int readonly;
  u32 sectors;
  u16 cyls;
  u8 heads, spt;
  u8 type;      /* floppy: BIOS drive type 1-5; hdd: 0 */
  int changed;  /* floppy change line pending */
} Disk;

/* slots: 0,1 = A:,B:  2,3 = first/second hard disk */
#define DISK_SLOTS 4
extern Disk disks[DISK_SLOTS];

/* disk_read result when the host has the sectors in flight (async source, e.g. the local
 * folder drive): the BIOS rewinds the INT 13h stub and halts, then retries on the next wake. */
#define DISK_ST_PENDING (-1)

void disk_init(void);
int disk_attach(int slot, u32 sectors, int readonly);   /* geometry derived from size */
void disk_detach(int slot);
int disk_slot_for_bios(u8 dl);                           /* -1 if no such drive */
/* returns BIOS status code (0 = ok) or DISK_ST_PENDING */
int disk_read(int slot, u32 lba, u32 count, u32 phys);
int disk_write(int slot, u32 lba, u32 count, u32 phys);

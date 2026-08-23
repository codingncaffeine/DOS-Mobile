/* High-level-emulation BIOS: the ROM image at F000:0000 holds tiny stubs that trap into
 * C through the 0F FF nn hook opcode; the service routines live here. */
#pragma once
#include "platform.h"

#define BIOS_ROM_BASE 0xF0000u
#define BIOS_SEG 0xF000
#define BIOS_STUB_BASE 0x0100      /* F000:0100 + vector*16 */
#define BIOS_POST_ENTRY 0x0010
#define BIOS_FONT16_OFF 0xE000     /* 4096 bytes */
#define BIOS_FONT8_OFF 0xFA6E      /* classic 8x8 lower-half location (1024 bytes) */
#define BIOS_FONT8HI_OFF 0xF000    /* 8x8 upper half (1024 bytes) */
#define BIOS_FONT14_OFF 0xD000     /* 3584 bytes */
#define BIOS_SYSCFG_OFF 0xE6F5
#define BIOS_DPT_OFF 0xEFC7
#define BIOS_FDPT0_OFF 0xE401
#define BIOS_FDPT1_OFF 0xE411
#define BIOS_VPT_OFF 0xC000        /* video parameter table */
#define BIOS_SAVE_PTR_OFF 0xBFE0   /* VGA save pointer table (7 dwords) */
#define BIOS_STATIC_FN_OFF 0xBFC0  /* INT 10h/1Bh static functionality table */

#define BDA 0x400u

/* HLE hook function numbers: 0x00-0xFF = INT vector services, 0xF0 = POST */
#define HLE_POST 0xF0

void bios_init(void);            /* build the ROM image (once) */
void bios_post_reset(int warm);  /* state cleared on reset */
void bios_hle(u8 fn);

/* text output used by the BIOS and by the POST banner */
void bios_putc(u8 c);
void bios_puts(const char *s);

/* keyboard: translate a raw scancode stream into BDA keystrokes */
void bios_kbd_scancode(u8 code);

extern const u8 font8x16[256 * 16];

/* machine description the BIOS reports */
typedef struct {
  u32 ram_kb;        /* total RAM in KB */
  int fpu;
  int floppies;      /* 0-2 */
  int floppy_type[2];/* 1=360K 2=1.2M 3=720K 4=1.44M 5=2.88M */
  int hdds;          /* 0-8 */
  int video;         /* 0 = VGA (only option in P0) */
} BiosConfig;
extern BiosConfig bios_cfg;

#include "machine.h"
#include "cpu.h"
#include "mem.h"
#include "io.h"
#include "sched.h"
#include "pic.h"
#include "pit.h"
#include "kbd.h"
#include "cmos.h"
#include "vga.h"
#include "bios.h"
#include "disk.h"
#include "mouse.h"
#include "dma.h"
#include "sb.h"
#include "audio.h"

MachineConfig machine_cfg;
static int reset_pending;
static int initialized;

void pic_register_ports(void);

static u8 rd_misc(u16 port) {
  switch (port) {
    case 0x92: return (u8)(a20_mask == 0xFFFFFFFFu ? 0x02 : 0x00);
    default: return 0xFF;
  }
}
static void wr_misc(u16 port, u8 v) {
  switch (port) {
    case 0x92:
      mem_set_a20((v >> 1) & 1);
      if (v & 1) machine_reset_request();
      break;
    case 0xE9: { char c = (char)v; host_log(&c, 1); break; }
    default: break;
  }
}

void machine_init(const MachineConfig *cfg) {
  machine_cfg = *cfg;
  if (!initialized) {
    mem_init(cfg->ram_kb * 1024);
    io_init();
    vga_init();
    disk_init();
    initialized = 1;
  }
  bios_cfg.ram_kb = cfg->ram_kb;
  bios_cfg.fpu = cfg->fpu;
  bios_cfg.floppies = cfg->floppies;
  bios_cfg.floppy_type[0] = cfg->floppy_type[0];
  bios_cfg.floppy_type[1] = cfg->floppy_type[1];
  bios_cfg.video = 0;
  machine_reset(0);
}

void machine_reset(int warm) {
  int ty, tmo, td, th, tmi, ts, twd;
  cmos_get_time(&ty, &tmo, &td, &th, &tmi, &ts, &twd);
  sched_init(machine_cfg.cpu_khz);
  pic_init();
  pit_init();
  kbd_init();
  dma_init();
  sb_init();
  audio_init();
  if (!warm) cmos_init(); else cmos_set_time(ty, tmo, td, th, tmi, ts);
  io_init();
  pic_register_ports();
  pit_register_ports();
  kbd_register_ports();
  cmos_register_ports();
  vga_register_ports();
  dma_register_ports();
  sb_register_ports();
  io_register(0x92, 1, rd_misc, wr_misc);
  io_register(0xE9, 1, rd_misc, wr_misc);
  io_register(0x80, 1, rd_misc, wr_misc);
  mem_set_a20(0);
  bios_cfg.hdds = 0; /* BIOS drive numbers must be contiguous from 80h */
  for (int hd = 2; hd < DISK_SLOTS && disks[hd].present; hd++) bios_cfg.hdds++;
  mouse_init();
  cpu_init(machine_cfg.cpu_gen, machine_cfg.fpu);
  bios_init();
  bios_post_reset(warm);
  vga_reset();
  audio_start();
  reset_pending = 0;
}

void machine_reset_request(void) { reset_pending = 1; cpu.halted = 0; }

void machine_fatal(void) { cpu.fatal = 1; }

int machine_run_ns(s64 ns) {
  if (cpu.fatal) return 1;
  int r = sched_run_ns(ns);
  if (reset_pending) {
    int warm = mem_rd16(BDA + 0x72) == 0x1234;
    machine_reset(warm);
    return 2;
  }
  return r;
}

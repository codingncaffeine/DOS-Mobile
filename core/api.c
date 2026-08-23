/* Exported C API consumed by the JS worker (and the Deno test harness). */
#include "platform.h"
#include "machine.h"
#include "cpu.h"
#include "mem.h"
#include "sched.h"
#include "vga.h"
#include "kbd.h"
#include "cmos.h"
#include "disk.h"
#include "bios.h"

EXPORT("core_version") u32 core_version(void) { return 0x000001; }

EXPORT("core_init")
int core_init(int gen, u32 khz, u32 ram_kb, int fpu, int floppies, int ftype0, int ftype1) {
  MachineConfig cfg;
  cfg.cpu_gen = gen;
  cfg.cpu_khz = khz;
  cfg.ram_kb = ram_kb < 1024 ? 1024 : ram_kb;
  cfg.fpu = fpu;
  cfg.floppies = floppies;
  cfg.floppy_type[0] = ftype0;
  cfg.floppy_type[1] = ftype1;
  machine_init(&cfg);
  return 0;
}

EXPORT("core_reset") void core_reset(int warm) { machine_reset(warm); }
EXPORT("core_set_khz") void core_set_khz(u32 khz) { sched_set_khz(khz); machine_cfg.cpu_khz = khz; }
EXPORT("core_get_khz") u32 core_get_khz(void) { return cpu_khz; }

/* Run `us` microseconds of emulated time. Returns 0 ok, 1 fatal, 2 reset happened. */
EXPORT("core_run_us") int core_run_us(u32 us) { return machine_run_ns((s64)us * 1000); }

EXPORT("core_key") void core_key(u32 scancode) { kbd_push_scancode((u8)scancode); }
EXPORT("core_key_space") int core_key_space(void) { return kbd_queue_free(); }

EXPORT("core_disk_attach") int core_disk_attach(int slot, u32 sectors, int readonly) {
  int r = disk_attach(slot, sectors, readonly);
  bios_cfg.hdds = (disks[2].present ? 1 : 0) + (disks[3].present ? 1 : 0);
  return r;
}
EXPORT("core_disk_detach") void core_disk_detach(int slot) { disk_detach(slot); }

EXPORT("core_fb_ptr") u32 *core_fb_ptr(void) { return vga_framebuffer(); }
EXPORT("core_fb_width") int core_fb_width(void) { return vga_fb_width(); }
EXPORT("core_fb_height") int core_fb_height(void) { return vga_fb_height(); }
EXPORT("core_frame_id") u32 core_frame_id(void) { return vga_frame_id(); }

EXPORT("core_set_time") void core_set_time(int y, int mo, int d, int h, int mi, int s) { cmos_set_time(y, mo, d, h, mi, s); }

/* Debug / test helpers */
EXPORT("core_halted") int core_halted(void) { return cpu.halted; }
EXPORT("core_fatal") int core_fatal(void) { return cpu.fatal; }
EXPORT("core_insns") u64 core_insns(void) { return cpu.insn_count; }
EXPORT("core_emu_ns") s64 core_emu_ns(void) { return emu_ns; }
EXPORT("core_reg") u32 core_reg(int i) {
  switch (i) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: return cpu.r[i];
    case 8: return cpu.eip;
    case 9: return cpu_get_eflags();
    case 10: return cpu.seg[SEG_ES].sel;
    case 11: return cpu.seg[SEG_CS].sel;
    case 12: return cpu.seg[SEG_SS].sel;
    case 13: return cpu.seg[SEG_DS].sel;
    default: return 0;
  }
}
EXPORT("core_mem_ptr") u8 *core_mem_ptr(void) { return ram; }
EXPORT("core_mem_size") u32 core_mem_size(void) { return ram_size; }
EXPORT("core_text_plane") u8 *core_text_plane(int p) { return vga_plane(p); }
EXPORT("core_text_cols") int core_text_cols(void) { return mem_rd16(BDA + 0x4A); }
EXPORT("core_text_rows") int core_text_rows(void) { return mem_rd8(BDA + 0x84) + 1; }
EXPORT("core_text_is_text") int core_text_is_text(void) { return vga_mode_is_text(); }
EXPORT("core_alloc") u8 *core_alloc(u32 size) { return (u8 *)dm_alloc(size); }

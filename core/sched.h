/* Emulated time and the device event scheduler.
 * Time is kept in nanoseconds of emulated machine time. The CPU clock (kHz) converts
 * instruction cycles into time; devices schedule events at absolute ns timestamps. */
#pragma once
#include "platform.h"

enum {
  EV_PIT0, EV_PIT1, EV_PIT2, EV_KBD, EV_VGA, EV_AUDIO, EV_RTC, EV_MOUSE, EV_FDC, EV_DSP, EV_BIOS_WAIT,
  EV_COUNT
};

typedef void (*sched_fn)(void);

extern s64 emu_ns;
extern u32 cpu_khz;
/* Fine-grained now: emu_ns plus the cycles the CPU has executed inside the current slice.
 * Use this for anything guest-visible (timer counters, status flags, retrace bits). */
s64 emu_now_ns(void);

void sched_init(u32 khz);
void sched_set_khz(u32 khz);
void sched_set(int id, s64 at_ns, sched_fn fn);
void sched_cancel(int id);
int sched_active(int id);
s64 sched_when(int id);

/* Advance the machine by ns of emulated time (CPU + devices). Returns 0, or 1 if fatal. */
int sched_run_ns(s64 ns);

/* Charge the CPU for an ISA bus I/O access (~1 µs). */
void sched_io_cost(void);

s64 ns_to_cycles(s64 ns);
s64 cycles_to_ns(s64 cycles);
/* Exact rate conversion without overflow: value * rate / 1e9 for a counter running at `rate` Hz. */
INLINE s64 ns_to_rate(s64 ns, s64 rate) {
  return (ns / 1000000000LL) * rate + ((ns % 1000000000LL) * rate) / 1000000000LL;
}
INLINE s64 rate_to_ns(s64 ticks, s64 rate) {
  return (ticks / rate) * 1000000000LL + ((ticks % rate) * 1000000000LL) / rate;
}

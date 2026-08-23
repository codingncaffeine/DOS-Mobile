#include "sched.h"
#include "cpu.h"
#include "pic.h"

s64 emu_ns;
u32 cpu_khz = 66000;

static struct { s64 at; sched_fn fn; int active; } events[EV_COUNT];
static s64 ns_rem; /* fractional carry for cycles→ns */

void sched_init(u32 khz) {
  emu_ns = 0;
  ns_rem = 0;
  cpu_khz = khz ? khz : 1;
  for (int i = 0; i < EV_COUNT; i++) events[i].active = 0;
}

void sched_set_khz(u32 khz) { cpu_khz = khz ? khz : 1; }

void sched_set(int id, s64 at_ns, sched_fn fn) {
  events[id].at = at_ns;
  events[id].fn = fn;
  events[id].active = 1;
}
void sched_cancel(int id) { events[id].active = 0; }
int sched_active(int id) { return events[id].active; }
s64 sched_when(int id) { return events[id].active ? events[id].at : -1; }

static s64 next_event(void) {
  s64 best = (s64)0x7FFFFFFFFFFFFFFFLL;
  for (int i = 0; i < EV_COUNT; i++)
    if (events[i].active && events[i].at < best) best = events[i].at;
  return best;
}

s64 ns_to_cycles(s64 ns) { return (ns * (s64)cpu_khz) / 1000000LL; }
s64 cycles_to_ns(s64 cycles) { return (cycles * 1000000LL) / (s64)cpu_khz; }

void sched_io_cost(void) { cpu.cycles += cpu_khz / 1000 + 1; }

static void fire_due(void) {
  for (int i = 0; i < EV_COUNT; i++) {
    if (events[i].active && events[i].at <= emu_ns) {
      events[i].active = 0;
      events[i].fn();
    }
  }
}

int sched_run_ns(s64 ns) {
  s64 end = emu_ns + ns;
  while (emu_ns < end) {
    if (cpu.fatal) return 1;
    if (cpu.halted && (cpu.eflags & F_IF) && pic_has_pending()) cpu.halted = 0;
    s64 next = next_event();
    if (next > end) next = end;
    if (cpu.halted) {
      emu_ns = next; /* nothing to execute: let time flow to the next event */
    } else {
      s64 budget = ns_to_cycles(next - emu_ns);
      if (budget < 1) budget = 1;
      u64 done = cpu_run(cpu.cycles + (u64)budget);
      s64 total = (s64)done * 1000000LL + ns_rem;
      emu_ns += total / (s64)cpu_khz;
      ns_rem = total % (s64)cpu_khz;
    }
    fire_due();
  }
  return 0;
}

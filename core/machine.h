/* Machine assembly: configuration, reset, and the run loop entry used by the API. */
#pragma once
#include "platform.h"

typedef struct {
  int cpu_gen;      /* GEN_* */
  u32 cpu_khz;
  u32 ram_kb;
  int fpu;
  int floppies;
  int floppy_type[2];
} MachineConfig;

extern MachineConfig machine_cfg;

void machine_init(const MachineConfig *cfg);
void machine_reset(int warm);
void machine_reset_request(void);   /* from the keyboard controller / Ctrl-Alt-Del */
void machine_fatal(void);
int machine_run_ns(s64 ns);         /* 0 ok, 1 fatal, 2 reset happened */

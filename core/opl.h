/* OPL3 (YMF262) FM synthesiser — an original implementation.
 * Register-compatible: 18 two-operator channels (6 pairable into 4-op), 8 waveforms,
 * tremolo/vibrato, rhythm mode, stereo output, the two OPL2-style timers. */
#pragma once
#include "platform.h"

void opl_init(void);
void opl_register_ports(void);      /* 388-38B and the 220/228 mirrors are wired by sb.c */
u8 opl_status(void);                /* status with lazily evaluated timer flags */
void opl_addr(int bank, u8 v);
void opl_data(u8 v);
/* Render `frames` stereo samples (s32 accumulate; the mixer scales). */
void opl_render(s32 *dst, u32 frames);

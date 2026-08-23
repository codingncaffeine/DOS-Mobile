/* Sound Blaster 16 (DSP 4.05 + mixer) with SB2/Pro compatibility, plus the OPL3 port wiring
 * (220h-22Fh, 388h-38Bh) and the MPU-401 UART detection stub (330h/331h). */
#pragma once
#include "platform.h"

void sb_init(void);
void sb_register_ports(void);
/* Mix `frames` stereo samples of the DMA voice into dst (s32 accumulate). */
void sb_render(s32 *dst, u32 frames);

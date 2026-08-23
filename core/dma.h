/* Dual 8237 DMA controllers (ports 00-0F, C0-DF, page registers 80-8F). */
#pragma once
#include "platform.h"

void dma_init(void);
void dma_register_ports(void);
/* Device side: read bytes/words from the channel's current block. Returns bytes actually
 * transferred (0 if the channel is masked). Sets *tc when the terminal count was reached. */
u32 dma_device_read(int channel, u8 *dst, u32 bytes, int *tc);
int dma_channel_masked(int channel);
u32 dma_channel_remaining(int channel);
int dma_channel_autoinit(int channel);

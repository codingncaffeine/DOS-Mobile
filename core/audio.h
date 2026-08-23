/* Audio mixing: a 1 ms event renders PC speaker + OPL3 + Sound Blaster into a ring buffer
 * of stereo s16 frames at 48 kHz; the host drains it. */
#pragma once
#include "platform.h"

#define AUDIO_RATE 48000

void audio_init(void);
void audio_start(void);
/* Drain up to max_frames stereo frames into dst (interleaved s16). Returns frames. */
u32 audio_read(s16 *dst, u32 max_frames);
u32 audio_available(void);

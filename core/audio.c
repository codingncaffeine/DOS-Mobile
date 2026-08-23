#include "audio.h"
#include "sched.h"
#include "pit.h"
#include "opl.h"
#include "sb.h"

#define RING_FRAMES 16384

static s16 ring[RING_FRAMES * 2];
static u32 ring_head, ring_tail; /* frame indices */
static s32 mixbuf[64 * 2];
static s64 last_ns;
static s32 spk_level, spk_dc; /* low-passed speaker level + DC blocker */

static void audio_event(void);

/* Soft-knee limiter: hard clipping of a hot mix (music + digital sfx) sounds like crackle;
 * compress the top quarter of the range instead and only then saturate. */
static s32 soften(s32 v) {
  if (v > 24576) v = 24576 + ((v - 24576) >> 2);
  else if (v < -24576) v = -24576 + ((v + 24576) >> 2);
  if (v > 32767) v = 32767;
  if (v < -32768) v = -32768;
  return v;
}

void audio_init(void) {
  ring_head = ring_tail = 0;
  last_ns = 0;
  spk_level = 0;
}

void audio_start(void) {
  last_ns = emu_ns;
  sched_set(EV_AUDIO, emu_ns + 1000000, audio_event);
}

static void audio_event(void) {
  /* render 1 ms = 48 frames */
  u32 frames = 48;
  dm_memset(mixbuf, 0, frames * 2 * sizeof(s32));
  /* PC speaker: sample the output state across the millisecond (square wave with a soft edge) */
  for (u32 f = 0; f < frames; f++) {
    int on = pit_speaker_output();
    /* pit_speaker_output() is evaluated at the current emulated instant; sub-ms detail comes from
     * the low-pass so fast toggling (PWM audio) still produces a usable level */
    s32 target = on ? 6000 : 0;
    spk_level += (target - spk_level) >> 2;
    spk_dc += (spk_level - spk_dc) >> 9;
    s32 s = spk_level - spk_dc;
    mixbuf[f * 2] += s;
    mixbuf[f * 2 + 1] += s;
  }
  opl_render(mixbuf, frames);
  sb_render(mixbuf, frames);
  for (u32 f = 0; f < frames; f++) {
    if (ring_tail - ring_head >= RING_FRAMES) break; /* full: drop */
    s32 l = soften(mixbuf[f * 2]), r = soften(mixbuf[f * 2 + 1]);
    u32 idx = (ring_tail % RING_FRAMES) * 2;
    ring[idx] = (s16)l;
    ring[idx + 1] = (s16)r;
    ring_tail++;
  }
  sched_set(EV_AUDIO, emu_ns + 1000000, audio_event);
}

u32 audio_available(void) { return ring_tail - ring_head; }

u32 audio_read(s16 *dst, u32 max_frames) {
  u32 n = audio_available();
  if (n > max_frames) n = max_frames;
  for (u32 i = 0; i < n; i++) {
    u32 idx = (ring_head % RING_FRAMES) * 2;
    dst[i * 2] = ring[idx];
    dst[i * 2 + 1] = ring[idx + 1];
    ring_head++;
  }
  return n;
}

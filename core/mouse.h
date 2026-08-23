/* INT 33h mouse services (HLE driver) + IRQ12 event delivery + cursor overlay. */
#pragma once
#include "platform.h"

void mouse_init(void);
void mouse_reset_state(void);
/* Host events: dx/dy in mickeys (~pixels), buttons bit0 left bit1 right bit2 middle. */
void mouse_host_event(int dx, int dy, int buttons);
void mouse_hle_int33(void);
void mouse_hle_irq(void);      /* INT 74h body: process one queued event, maybe call the user handler */
void mouse_on_mode_change(int mode, int cols, int rows);
/* Renderer overlay: composite the cursor into the frame buffer (called by vga_render_frame). */
void mouse_overlay(u32 *fb, int w, int h, int text_mode, int cell_w, int cell_h);
int mouse_present(void);

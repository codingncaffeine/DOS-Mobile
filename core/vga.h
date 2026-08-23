/* VGA: 256 KB planar video memory, full register set, per-frame renderer. */
#pragma once
#include "platform.h"

void vga_init(void);
void vga_register_ports(void);
void vga_reset(void);

/* BIOS helpers */
void vga_set_mode(int mode);            /* program registers + DAC for a standard BIOS mode */
int vga_mode_is_text(void);
u8 *vga_plane(int p);                   /* direct plane access for BIOS font loading */
void vga_load_font(const u8 *font, int height, int first, int count, int block);
void vga_set_dac(int index, u8 r, u8 g, u8 b);
void vga_get_dac(int index, u8 *r, u8 *g, u8 *b);
void vga_set_attr_reg(int idx, u8 v);
u8 vga_get_attr_reg(int idx);
u8 vga_read_crtc(int idx);
void vga_write_crtc(int idx, u8 v);
u16 vga_crtc_port(void);

/* Frame output */
u32 *vga_framebuffer(void);
int vga_fb_width(void);
int vga_fb_height(void);
u32 vga_frame_id(void);                 /* increments at every vertical retrace */
void vga_render_frame(void);            /* render the current register/memory state */

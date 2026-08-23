/* 8042 keyboard controller (60h/64h), keyboard device, A20 gate and reset lines. */
#pragma once
#include "platform.h"

void kbd_init(void);
void kbd_register_ports(void);
/* Host side: queue a raw scancode byte (set 1; E0/E1 prefixes are just bytes). */
void kbd_push_scancode(u8 code);
int kbd_queue_free(void);

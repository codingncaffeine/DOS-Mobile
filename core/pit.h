/* 8254 programmable interval timer (40h-43h) + PPI port 61h speaker/gate bits. */
#pragma once
#include "platform.h"

#define PIT_HZ 1193182LL

void pit_init(void);
void pit_register_ports(void);
int pit_speaker_output(void);   /* current speaker drive level (gate && data && ch2 out) */
int pit_ch2_output(void);
u32 pit_ch2_reload(void);

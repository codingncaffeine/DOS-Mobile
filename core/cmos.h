/* MC146818 real-time clock + CMOS RAM (70h/71h). */
#pragma once
#include "platform.h"

void cmos_init(void);
void cmos_register_ports(void);
void cmos_set_byte(int reg, u8 v);
u8 cmos_get_byte(int reg);
/* Host wall clock at power-on; the RTC advances with emulated time from there. */
void cmos_set_time(int year, int month, int day, int hour, int min, int sec);
void cmos_get_time(int *year, int *month, int *day, int *hour, int *min, int *sec, int *wday);

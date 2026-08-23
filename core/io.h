/* I/O port space. Devices register byte handlers for port ranges; word/dword accesses are
 * split into bytes unless a device registers wide handlers (none yet). */
#pragma once
#include "platform.h"

typedef u8 (*io_rd_fn)(u16 port);
typedef void (*io_wr_fn)(u16 port, u8 v);

void io_init(void);
void io_register(u16 base, u16 count, io_rd_fn rd, io_wr_fn wr);

u8 io_rd8(u16 port);
u16 io_rd16(u16 port);
u32 io_rd32(u16 port);
void io_wr8(u16 port, u8 v);
void io_wr16(u16 port, u16 v);
void io_wr32(u16 port, u32 v);

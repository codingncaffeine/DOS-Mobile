/* Dual 8259A programmable interrupt controllers (master 20h/21h, slave A0h/A1h). */
#pragma once
#include "platform.h"

void pic_init(void);
void pic_raise_irq(int irq);  /* latch an edge on IRQ 0-15 */
void pic_lower_irq(int irq);  /* level devices: drop the request if not yet acknowledged */
int pic_has_pending(void);
u8 pic_ack(void);             /* INTA cycle: returns the vector of the highest pending request */

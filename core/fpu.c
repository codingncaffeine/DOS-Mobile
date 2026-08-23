/* x87 FPU — P2 work item. Until then an attached coprocessor behaves like an absent one:
 * the operand is decoded and the instruction is a NOP. FNSTSW/FNINIT style probes therefore
 * see "no FPU", which the BIOS equipment word also reports. */
#include "cpu_int.h"

void fpu_exec(u8 op) {
  (void)op;
  decode_modrm();
}

/* Segment loading, interrupts, exceptions and far control transfers.
 * Real mode is complete; descriptor-based protected mode is the P2 work item and currently
 * stops the machine with a diagnostic instead of silently misbehaving. */
#include "cpu_int.h"

static void unimplemented_pmode(const char *what) {
  dm_log("CPU: protected-mode %s not implemented yet (CS=%04x EIP=%08x)", what,
         cpu.seg[SEG_CS].sel, cpu.eip_start);
  cpu.fatal = 1;
}

void load_seg_real(int s, u16 sel) {
  if (in_pmode() && !in_v86()) { unimplemented_pmode("segment load"); return; }
  cpu.seg[s].sel = sel;
  cpu.seg[s].base = (u32)sel << 4;
  cpu.seg[s].valid = 1;
  /* limit/attributes deliberately untouched: matches real hardware (and "unreal mode"). */
}

/* Push FLAGS/CS/IP and vector through the real-mode IVT. */
static void interrupt_real(u8 vec, u32 ret_eip) {
  u32 entry = cpu.idtr.base + (u32)vec * 4;
  if (cpu.gen >= GEN_286 && (u32)vec * 4 + 3 > cpu.idtr.limit) {
    raise_fault(EXC_GP, 1, (u32)vec * 8 + 2);
    return;
  }
  u16 ip = mem_rd16(entry);
  u16 cs = mem_rd16(entry + 2);
  u32 f = cpu_get_eflags();
  push16((u16)f);
  push16(cpu.seg[SEG_CS].sel);
  push16((u16)ret_eip);
  if (cpu.fault_pending) return;
  cpu.eflags &= ~(u32)(F_IF | F_TF | F_RF | F_AC);
  cpu.lf_type = LF_NONE;
  load_seg_real(SEG_CS, cs);
  cpu.eip = ip;
  cpu.cycles += 25;
}

void cpu_interrupt(u8 vec, int is_sw, int has_err, u32 err, u32 ret_eip) {
  (void)is_sw; (void)has_err; (void)err;
  if (in_pmode() && !in_v86()) { unimplemented_pmode("interrupt"); return; }
  if (in_v86()) { unimplemented_pmode("V86 interrupt"); return; }
  interrupt_real(vec, ret_eip);
}

void cpu_deliver_fault(void) {
  u8 vec = cpu.fault_vec;
  int has_err = cpu.fault_has_err;
  u32 err = cpu.fault_err;
  cpu.fault_pending = 0;
  /* Faults return to the faulting instruction; the 8086 divide error is a trap (next IP). */
  u32 ret = cpu.eip_start;
  if (vec == EXC_DE && cpu.gen <= GEN_186) ret = cpu.eip;
  if (vec == EXC_UD || vec == EXC_GP) {
    u32 a = cpu.seg[SEG_CS].base + cpu.eip_start;
    dm_log("CPU: #%s at %04x:%04x bytes %02x %02x %02x %02x", vec == EXC_UD ? "UD" : "GP",
           cpu.seg[SEG_CS].sel, cpu.eip_start, mem_rd8(a), mem_rd8(a + 1), mem_rd8(a + 2), mem_rd8(a + 3));
  }
  cpu.eip = ret;
  cpu_interrupt(vec, 0, has_err, err, ret);
  if (cpu.fault_pending) { /* double fault while delivering: stop rather than loop */
    cpu.fault_pending = 0;
    dm_log("CPU: fault while delivering fault %d", vec);
    cpu.fatal = 1;
  }
}

void cpu_far_jump(u16 sel, u32 off) {
  if (in_pmode() && !in_v86()) { unimplemented_pmode("far jump"); return; }
  load_seg_real(SEG_CS, sel);
  cpu.eip = off & eip_mask();
  cpu.cycles += 12;
}

void cpu_far_call(u16 sel, u32 off) {
  if (in_pmode() && !in_v86()) { unimplemented_pmode("far call"); return; }
  pushv(cpu.seg[SEG_CS].sel);
  pushv(cpu.eip);
  if (cpu.fault_pending) return;
  load_seg_real(SEG_CS, sel);
  cpu.eip = off & eip_mask();
  cpu.cycles += 18;
}

void cpu_far_ret(int pop_bytes) {
  if (in_pmode() && !in_v86()) { unimplemented_pmode("far return"); return; }
  u32 ip = popv();
  u32 cs = popv();
  if (cpu.fault_pending) return;
  load_seg_real(SEG_CS, (u16)cs);
  cpu.eip = ip & eip_mask();
  sp_add(pop_bytes);
  cpu.cycles += 15;
}

void cpu_iret(void) {
  if (in_pmode() && !in_v86()) { unimplemented_pmode("iret"); return; }
  u32 ip, cs, f;
  if (cpu.osize32) { ip = pop32(); cs = pop32(); f = pop32(); }
  else { ip = pop16(); cs = pop16(); f = pop16(); }
  if (cpu.fault_pending) return;
  load_seg_real(SEG_CS, (u16)cs);
  cpu.eip = ip & eip_mask();
  u32 cur = cpu_get_eflags();
  if (cpu.osize32) cpu_set_eflags((f & ~(u32)(F_VM | F_VIF | F_VIP)) | (cur & (F_VM | F_VIF | F_VIP)));
  else cpu_set_eflags((cur & 0xFFFF0000u) | (f & 0xFFFF));
  cpu.cycles += 20;
}

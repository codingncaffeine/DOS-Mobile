/* Internal CPU helpers shared by cpu.c / cpu_0f.c / cpu_pm.c / fpu.c */
#pragma once
#include "cpu.h"
#include "mem.h"
#include "paging.h"

extern const u8 parity_tab[256];

/* ---------------- registers ---------------- */
INLINE u8 reg8_get(int i) { return i < 4 ? (u8)cpu.r[i] : (u8)(cpu.r[i - 4] >> 8); }
INLINE void reg8_set(int i, u8 v) {
  if (i < 4) cpu.r[i] = (cpu.r[i] & ~0xFFu) | v;
  else cpu.r[i - 4] = (cpu.r[i - 4] & ~0xFF00u) | ((u32)v << 8);
}
INLINE u16 reg16_get(int i) { return (u16)cpu.r[i]; }
INLINE void reg16_set(int i, u16 v) { cpu.r[i] = (cpu.r[i] & 0xFFFF0000u) | v; }
INLINE u32 reg32_get(int i) { return cpu.r[i]; }
INLINE void reg32_set(int i, u32 v) { cpu.r[i] = v; }
INLINE u32 regv_get(int i) { return cpu.osize32 ? cpu.r[i] : (u16)cpu.r[i]; }
INLINE void regv_set(int i, u32 v) { if (cpu.osize32) cpu.r[i] = v; else reg16_set(i, (u16)v); }

/* ---------------- lazy flags ---------------- */
INLINE u32 size_mask(int bits) { return bits == 8 ? 0xFFu : bits == 16 ? 0xFFFFu : 0xFFFFFFFFu; }
INLINE u32 sign_bit(int bits) { return 1u << (bits - 1); }

INLINE void lf_set(int type, int bits, u32 a, u32 b, u32 res, u32 cin) {
  cpu.lf_type = (u8)type;
  cpu.lf_bits = (u8)bits;
  cpu.lf_a = a;
  cpu.lf_b = b;
  cpu.lf_res = res;
  cpu.lf_cin = cin;
}

INLINE int flag_zf(void) { return cpu.lf_type == LF_NONE ? !!(cpu.eflags & F_ZF) : cpu.lf_res == 0; }
INLINE int flag_sf(void) {
  return cpu.lf_type == LF_NONE ? !!(cpu.eflags & F_SF) : !!(cpu.lf_res & sign_bit(cpu.lf_bits));
}
INLINE int flag_pf(void) {
  return cpu.lf_type == LF_NONE ? !!(cpu.eflags & F_PF) : parity_tab[cpu.lf_res & 0xFF];
}
INLINE int flag_cf(void) {
  switch (cpu.lf_type) {
    case LF_ADD: return cpu.lf_res < cpu.lf_a;
    case LF_ADC: return cpu.lf_res < cpu.lf_a || (cpu.lf_cin && cpu.lf_res == cpu.lf_a);
    case LF_SUB: return cpu.lf_a < cpu.lf_b;
    case LF_SBB: return cpu.lf_a < cpu.lf_b || (cpu.lf_cin && cpu.lf_a == cpu.lf_b);
    case LF_LOGIC: return 0;
    case LF_INC:
    case LF_DEC: return (int)cpu.lf_cin;
    case LF_NEG: return cpu.lf_res != 0;
    default: return cpu.eflags & F_CF;
  }
}
INLINE int flag_af(void) {
  switch (cpu.lf_type) {
    case LF_ADD: case LF_ADC: case LF_SUB: case LF_SBB:
      return ((cpu.lf_a ^ cpu.lf_b ^ cpu.lf_res) & 0x10) != 0;
    case LF_LOGIC: return 0;
    case LF_INC: return (cpu.lf_res & 0xF) == 0;
    case LF_DEC: return (cpu.lf_res & 0xF) == 0xF;
    case LF_NEG: return (cpu.lf_res & 0xF) != 0;
    default: return !!(cpu.eflags & F_AF);
  }
}
INLINE int flag_of(void) {
  u32 s = sign_bit(cpu.lf_bits);
  switch (cpu.lf_type) {
    case LF_ADD: case LF_ADC: return ((cpu.lf_a ^ cpu.lf_res) & (cpu.lf_b ^ cpu.lf_res) & s) != 0;
    case LF_SUB: case LF_SBB: return ((cpu.lf_a ^ cpu.lf_b) & (cpu.lf_a ^ cpu.lf_res) & s) != 0;
    case LF_LOGIC: return 0;
    case LF_INC: return cpu.lf_res == s;
    case LF_DEC: return cpu.lf_res == s - 1;
    case LF_NEG: return cpu.lf_res == s;
    default: return !!(cpu.eflags & F_OF);
  }
}

INLINE void flags_sync(void) {
  if (cpu.lf_type == LF_NONE) return;
  u32 f = cpu.eflags & ~F_ARITH;
  if (flag_cf()) f |= F_CF;
  if (flag_pf()) f |= F_PF;
  if (flag_af()) f |= F_AF;
  if (flag_zf()) f |= F_ZF;
  if (flag_sf()) f |= F_SF;
  if (flag_of()) f |= F_OF;
  cpu.eflags = f;
  cpu.lf_type = LF_NONE;
}

INLINE void lf_zsp(int bits, u32 res, int cf, int of) {
  flags_sync();
  cpu.eflags = (cpu.eflags & ~(u32)(F_CF | F_OF)) | (cf ? F_CF : 0) | (of ? F_OF : 0);
  cpu.lf_type = LF_ZSP;
  cpu.lf_bits = (u8)bits;
  cpu.lf_res = res;
}
INLINE void set_cf(int v) { flags_sync(); cpu.eflags = (cpu.eflags & ~(u32)F_CF) | (v ? F_CF : 0); }
INLINE void set_of(int v) { flags_sync(); cpu.eflags = (cpu.eflags & ~(u32)F_OF) | (v ? F_OF : 0); }

INLINE int cond_true(int cc) {
  int r;
  switch (cc >> 1) {
    case 0: r = flag_of(); break;
    case 1: r = flag_cf(); break;
    case 2: r = flag_zf(); break;
    case 3: r = flag_cf() || flag_zf(); break;
    case 4: r = flag_sf(); break;
    case 5: r = flag_pf(); break;
    case 6: r = flag_sf() != flag_of(); break;
    default: r = flag_zf() || (flag_sf() != flag_of()); break;
  }
  return (cc & 1) ? !r : r;
}

/* ---------------- faults ---------------- */
INLINE void raise_fault(u8 vec, int has_err, u32 err) {
  if (!cpu.fault_pending) {
    cpu.fault_pending = 1;
    cpu.fault_vec = vec;
    cpu.fault_has_err = (u8)has_err;
    cpu.fault_err = err;
  }
}
#define FAULT_CHECK() do { if (UNLIKELY(cpu.fault_pending)) return; } while (0)
#define FAULT_CHECK_RET(v) do { if (UNLIKELY(cpu.fault_pending)) return (v); } while (0)

/* ---------------- mode helpers ---------------- */
INLINE int in_pmode(void) { return (cpu.cr0 & 1) != 0; }
INLINE int in_v86(void) { return (cpu.eflags & F_VM) != 0; }
INLINE int in_prot(void) { return in_pmode() && !in_v86(); } /* descriptor-based protected mode */
INLINE int paging_on(void) { return (cpu.cr0 & 0x80000000u) != 0; }
INLINE int osize_bits(void) { return cpu.osize32 ? 32 : 16; }
INLINE int iopl(void) { return (int)((cpu.eflags >> 12) & 3); }

/* ---------------- linear memory (paging-aware) ---------------- */
INLINE u8 lin_rd8(u32 lin) { return LIKELY(!paging_on()) ? mem_rd8(lin) : lin_rd8_slow(lin); }
INLINE u16 lin_rd16(u32 lin) { return LIKELY(!paging_on()) ? mem_rd16(lin) : lin_rd16_slow(lin); }
INLINE u32 lin_rd32(u32 lin) { return LIKELY(!paging_on()) ? mem_rd32(lin) : lin_rd32_slow(lin); }
INLINE void lin_wr8(u32 lin, u8 v) { if (LIKELY(!paging_on())) mem_wr8(lin, v); else lin_wr8_slow(lin, v); }
INLINE void lin_wr16(u32 lin, u16 v) { if (LIKELY(!paging_on())) mem_wr16(lin, v); else lin_wr16_slow(lin, v); }
INLINE void lin_wr32(u32 lin, u32 v) { if (LIKELY(!paging_on())) mem_wr32(lin, v); else lin_wr32_slow(lin, v); }

/* ---------------- segment-relative access with limit / access checks ---------------- */
INLINE int seg_check(int s, u32 off, u32 size, int write) {
  const Seg *sg = &cpu.seg[s];
  if (UNLIKELY(!sg->valid)) { raise_fault(EXC_GP, 1, 0); return 0; }
  u32 last = off + size - 1;
  if (sg->flags & SEGF_EXPDOWN) {
    u32 top = sg->db ? 0xFFFFFFFFu : 0xFFFFu;
    if (UNLIKELY(off <= sg->limit || last > top || last < off)) { raise_fault(s == SEG_SS ? EXC_SS : EXC_GP, 1, 0); return 0; }
  } else if (UNLIKELY(last > sg->limit || last < off)) {
    raise_fault(s == SEG_SS ? EXC_SS : EXC_GP, 1, 0);
    return 0;
  }
  if (in_prot()) {
    if (write ? !(sg->flags & SEGF_WRITE) : !(sg->flags & SEGF_READ)) { raise_fault(EXC_GP, 1, 0); return 0; }
  }
  return 1;
}
INLINE u8 rd8s(int s, u32 off) { if (!seg_check(s, off, 1, 0)) return 0; return lin_rd8(cpu.seg[s].base + off); }
INLINE u16 rd16s(int s, u32 off) { if (!seg_check(s, off, 2, 0)) return 0; return lin_rd16(cpu.seg[s].base + off); }
INLINE u32 rd32s(int s, u32 off) { if (!seg_check(s, off, 4, 0)) return 0; return lin_rd32(cpu.seg[s].base + off); }
INLINE void wr8s(int s, u32 off, u8 v) { if (!seg_check(s, off, 1, 1)) return; lin_wr8(cpu.seg[s].base + off, v); }
INLINE void wr16s(int s, u32 off, u16 v) { if (!seg_check(s, off, 2, 1)) return; lin_wr16(cpu.seg[s].base + off, v); }
INLINE void wr32s(int s, u32 off, u32 v) { if (!seg_check(s, off, 4, 1)) return; lin_wr32(cpu.seg[s].base + off, v); }
INLINE u32 rdvs(int s, u32 off) { return cpu.osize32 ? rd32s(s, off) : rd16s(s, off); }
INLINE void wrvs(int s, u32 off, u32 v) { if (cpu.osize32) wr32s(s, off, v); else wr16s(s, off, (u16)v); }
/* Verify a write will succeed (used before read-modify-write so faults leave no partial state). */
INLINE int probe_write(int s, u32 off, u32 size) {
  if (!seg_check(s, off, size, 1)) return 0;
  if (paging_on()) return lin_probe_write_slow(cpu.seg[s].base + off, size);
  return 1;
}

/* ---------------- instruction fetch ---------------- */
INLINE u32 eip_mask(void) { return cpu.seg[SEG_CS].db ? 0xFFFFFFFFu : 0xFFFFu; }
INLINE u8 fetch8(void) {
  u32 lin = cpu.seg[SEG_CS].base + cpu.eip;
  u8 v;
  if (LIKELY((lin & ~0xFFFu) == cpu.fetch_page_lin && cpu.fetch_page_ptr)) v = cpu.fetch_page_ptr[lin & 0xFFF];
  else {
    if ((lin & ~0xFFFu) != cpu.fetch_page_lin && !fetch_page_prepare(lin)) return 0;
    v = cpu.fetch_page_ptr ? cpu.fetch_page_ptr[lin & 0xFFF] : lin_rd8(lin);
  }
  cpu.eip = (cpu.eip + 1) & eip_mask();
  return v;
}
INLINE u16 fetch16(void) {
  u32 lin = cpu.seg[SEG_CS].base + cpu.eip;
  if (LIKELY((lin & 0xFFF) <= 0xFFE && (lin & ~0xFFFu) == cpu.fetch_page_lin && cpu.fetch_page_ptr)) {
    u16 v = ld16(cpu.fetch_page_ptr + (lin & 0xFFF));
    cpu.eip = (cpu.eip + 2) & eip_mask();
    return v;
  }
  u16 lo = fetch8();
  return (u16)(lo | (fetch8() << 8));
}
INLINE u32 fetch32(void) {
  u32 lin = cpu.seg[SEG_CS].base + cpu.eip;
  if (LIKELY((lin & 0xFFF) <= 0xFFC && (lin & ~0xFFFu) == cpu.fetch_page_lin && cpu.fetch_page_ptr)) {
    u32 v = ld32(cpu.fetch_page_ptr + (lin & 0xFFF));
    cpu.eip = (cpu.eip + 4) & eip_mask();
    return v;
  }
  u32 lo = fetch16();
  return lo | ((u32)fetch16() << 16);
}
INLINE u32 fetchv(void) { return cpu.osize32 ? fetch32() : fetch16(); }
INLINE u32 fetchv_sx(void) { return cpu.osize32 ? fetch32() : (u32)(s32)(s16)fetch16(); }

/* ---------------- stack (fault-safe: SP moves only after the access succeeded) ---------------- */
INLINE u32 sp_mask(void) { return cpu.seg[SEG_SS].db ? 0xFFFFFFFFu : 0xFFFFu; }
INLINE void sp_set(u32 v) {
  if (cpu.seg[SEG_SS].db) cpu.r[REG_SP] = v;
  else cpu.r[REG_SP] = (cpu.r[REG_SP] & 0xFFFF0000u) | (v & 0xFFFFu);
}
INLINE void sp_add(s32 d) { sp_set((cpu.r[REG_SP] + (u32)d) & sp_mask()); }
INLINE u32 sp_get(void) { return cpu.r[REG_SP] & sp_mask(); }
INLINE void push16(u16 v) { u32 nsp = (sp_get() - 2) & sp_mask(); wr16s(SEG_SS, nsp, v); if (!cpu.fault_pending) sp_set(nsp); }
INLINE void push32(u32 v) { u32 nsp = (sp_get() - 4) & sp_mask(); wr32s(SEG_SS, nsp, v); if (!cpu.fault_pending) sp_set(nsp); }
INLINE void pushv(u32 v) { if (cpu.osize32) push32(v); else push16((u16)v); }
INLINE u16 pop16(void) { u16 v = rd16s(SEG_SS, sp_get()); if (!cpu.fault_pending) sp_add(2); return v; }
INLINE u32 pop32(void) { u32 v = rd32s(SEG_SS, sp_get()); if (!cpu.fault_pending) sp_add(4); return v; }
INLINE u32 popv(void) { return cpu.osize32 ? pop32() : pop16(); }
/* peek without moving SP (for instructions that must not commit before their write) */
INLINE u32 peekv(u32 depth) { return cpu.osize32 ? rd32s(SEG_SS, (sp_get() + depth) & sp_mask()) : rd16s(SEG_SS, (sp_get() + depth) & sp_mask()); }

/* ---------------- ModRM ---------------- */
void decode_modrm(void);
INLINE int rm_is_reg(void) { return cpu.modrm_mod == 3; }
INLINE u8 rm_rd8(void) { return rm_is_reg() ? reg8_get(cpu.modrm_rm) : rd8s(cpu.ea_seg, cpu.ea); }
INLINE u16 rm_rd16(void) { return rm_is_reg() ? reg16_get(cpu.modrm_rm) : rd16s(cpu.ea_seg, cpu.ea); }
INLINE u32 rm_rd32(void) { return rm_is_reg() ? reg32_get(cpu.modrm_rm) : rd32s(cpu.ea_seg, cpu.ea); }
INLINE u32 rm_rdv(void) { return cpu.osize32 ? rm_rd32() : rm_rd16(); }
INLINE void rm_wr8(u8 v) { if (rm_is_reg()) reg8_set(cpu.modrm_rm, v); else wr8s(cpu.ea_seg, cpu.ea, v); }
INLINE void rm_wr16(u16 v) { if (rm_is_reg()) reg16_set(cpu.modrm_rm, v); else wr16s(cpu.ea_seg, cpu.ea, v); }
INLINE void rm_wr32(u32 v) { if (rm_is_reg()) reg32_set(cpu.modrm_rm, v); else wr32s(cpu.ea_seg, cpu.ea, v); }
INLINE void rm_wrv(u32 v) { if (cpu.osize32) rm_wr32(v); else rm_wr16((u16)v); }
/* for read-modify-write memory operands: check the write first */
INLINE int rm_probe(int bits) { return rm_is_reg() ? 1 : probe_write(cpu.ea_seg, cpu.ea, (u32)bits / 8); }

/* ---------------- ALU ---------------- */
INLINE u32 alu_op(int op, int bits, u32 a, u32 b) {
  u32 m = size_mask(bits), res;
  a &= m;
  b &= m;
  switch (op) {
    case 0: res = (a + b) & m; lf_set(LF_ADD, bits, a, b, res, 0); break;
    case 1: res = a | b; lf_set(LF_LOGIC, bits, a, b, res, 0); break;
    case 2: { u32 c = (u32)flag_cf(); res = (a + b + c) & m; lf_set(LF_ADC, bits, a, b, res, c); break; }
    case 3: { u32 c = (u32)flag_cf(); res = (a - b - c) & m; lf_set(LF_SBB, bits, a, b, res, c); break; }
    case 4: res = a & b; lf_set(LF_LOGIC, bits, a, b, res, 0); break;
    case 6: res = a ^ b; lf_set(LF_LOGIC, bits, a, b, res, 0); break;
    default: res = (a - b) & m; lf_set(LF_SUB, bits, a, b, res, 0); break; /* SUB, CMP */
  }
  return res;
}
INLINE u32 alu_inc(int bits, u32 a) {
  u32 cf = (u32)flag_cf(), res = (a + 1) & size_mask(bits);
  lf_set(LF_INC, bits, a, 1, res, cf);
  return res;
}
INLINE u32 alu_dec(int bits, u32 a) {
  u32 cf = (u32)flag_cf(), res = (a - 1) & size_mask(bits);
  lf_set(LF_DEC, bits, a, 1, res, cf);
  return res;
}

/* ---------------- privilege helpers ---------------- */
INLINE int io_allowed(u16 port, int size);     /* cpu_pm.c */
INLINE int require_cpl0(void) { if (in_pmode() && (cpu.cpl != 0 || in_v86())) { raise_fault(EXC_GP, 1, 0); return 0; } return 1; }

/* segment loading / control transfers (cpu_pm.c) */
void load_seg_real(int s, u16 sel);
void load_seg(int s, u16 sel);        /* mode-aware: real, V86 or protected */
void cpu_interrupt(u8 vec, int is_sw, int has_err, u32 err, u32 ret_eip);
void cpu_far_jump(u16 sel, u32 off);
void cpu_far_call(u16 sel, u32 off);
void cpu_far_ret(int pop_bytes);
void cpu_iret(void);
void cpu_deliver_fault(void);
void cpu_update_cpl(void);
int io_allowed_slow(u16 port, int size);
INLINE int io_allowed(u16 port, int size) {
  if (LIKELY(!in_pmode())) return 1;
  if (!in_v86() && cpu.cpl <= iopl()) return 1;
  return io_allowed_slow(port, size);
}

/* two-byte opcodes (cpu_0f.c) */
void cpu_exec_0f(void);
void cpu_sys_0f00(void);
void cpu_sys_lar_lsl(int is_lsl);
void cpu_ltr_lldt(int is_tr, u16 sel);

/* misc */
void cpu_ud(void);
u32 cpu_cycles_add(u32 n);

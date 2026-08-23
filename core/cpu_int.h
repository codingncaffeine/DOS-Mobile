/* Internal CPU helpers shared by cpu.c / cpu_0f.c / cpu_pm.c */
#pragma once
#include "cpu.h"
#include "mem.h"

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

/* Materialise the lazy flags into EFLAGS. */
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

/* Result-only flags (ZF/SF/PF from res; CF/OF/AF written explicitly by the caller). */
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

/* ---------------- linear memory through segments ---------------- */
/* Segment limit checks apply only to 32-bit offsets in real mode (P0); descriptors in P2. */
INLINE u32 seg_lin(int s, u32 off) {
  if (UNLIKELY(off > cpu.seg[s].limit)) raise_fault(s == SEG_SS ? EXC_SS : EXC_GP, 1, 0);
  return cpu.seg[s].base + off;
}
INLINE u8 rd8s(int s, u32 off) { return mem_rd8(seg_lin(s, off)); }
INLINE u16 rd16s(int s, u32 off) { return mem_rd16(seg_lin(s, off)); }
INLINE u32 rd32s(int s, u32 off) { return mem_rd32(seg_lin(s, off)); }
INLINE void wr8s(int s, u32 off, u8 v) { mem_wr8(seg_lin(s, off), v); }
INLINE void wr16s(int s, u32 off, u16 v) { mem_wr16(seg_lin(s, off), v); }
INLINE void wr32s(int s, u32 off, u32 v) { mem_wr32(seg_lin(s, off), v); }
INLINE u32 rdvs(int s, u32 off) { return cpu.osize32 ? rd32s(s, off) : rd16s(s, off); }
INLINE void wrvs(int s, u32 off, u32 v) { if (cpu.osize32) wr32s(s, off, v); else wr16s(s, off, (u16)v); }

/* ---------------- instruction fetch ---------------- */
INLINE u32 eip_mask(void) { return cpu.seg[SEG_CS].db ? 0xFFFFFFFFu : 0xFFFFu; }
INLINE u8 fetch8(void) {
  u8 v = mem_rd8(cpu.seg[SEG_CS].base + cpu.eip);
  cpu.eip = (cpu.eip + 1) & eip_mask();
  return v;
}
INLINE u16 fetch16(void) {
  u16 v = mem_rd16(cpu.seg[SEG_CS].base + cpu.eip);
  cpu.eip = (cpu.eip + 2) & eip_mask();
  return v;
}
INLINE u32 fetch32(void) {
  u32 v = mem_rd32(cpu.seg[SEG_CS].base + cpu.eip);
  cpu.eip = (cpu.eip + 4) & eip_mask();
  return v;
}
INLINE u32 fetchv(void) { return cpu.osize32 ? fetch32() : fetch16(); }
INLINE u32 fetchv_sx(void) { return cpu.osize32 ? fetch32() : (u32)(s32)(s16)fetch16(); }

/* ---------------- stack ---------------- */
INLINE u32 sp_mask(void) { return cpu.seg[SEG_SS].db ? 0xFFFFFFFFu : 0xFFFFu; }
INLINE void sp_add(s32 d) {
  if (cpu.seg[SEG_SS].db) cpu.r[REG_SP] += (u32)d;
  else cpu.r[REG_SP] = (cpu.r[REG_SP] & 0xFFFF0000u) | ((cpu.r[REG_SP] + (u32)d) & 0xFFFFu);
}
INLINE u32 sp_get(void) { return cpu.r[REG_SP] & sp_mask(); }
INLINE void push16(u16 v) { sp_add(-2); wr16s(SEG_SS, sp_get(), v); }
INLINE void push32(u32 v) { sp_add(-4); wr32s(SEG_SS, sp_get(), v); }
INLINE void pushv(u32 v) { if (cpu.osize32) push32(v); else push16((u16)v); }
INLINE u16 pop16(void) { u16 v = rd16s(SEG_SS, sp_get()); sp_add(2); return v; }
INLINE u32 pop32(void) { u32 v = rd32s(SEG_SS, sp_get()); sp_add(4); return v; }
INLINE u32 popv(void) { return cpu.osize32 ? pop32() : pop16(); }

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

/* ---------------- ALU ---------------- */
/* op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP */
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

/* ---------------- mode helpers ---------------- */
INLINE int in_pmode(void) { return (cpu.cr0 & 1) != 0; }
INLINE int in_v86(void) { return (cpu.eflags & F_VM) != 0; }
INLINE int osize_bits(void) { return cpu.osize32 ? 32 : 16; }

/* segment loading / control transfers (cpu_pm.c) */
void load_seg_real(int s, u16 sel);
void cpu_interrupt(u8 vec, int is_sw, int has_err, u32 err, u32 ret_eip);
void cpu_far_jump(u16 sel, u32 off);
void cpu_far_call(u16 sel, u32 off);
void cpu_far_ret(int pop_bytes);
void cpu_iret(void);
void cpu_deliver_fault(void);

/* two-byte opcodes (cpu_0f.c) */
void cpu_exec_0f(void);

/* string / misc helpers used by both files */
void cpu_ud(void);
u32 cpu_cycles_add(u32 n);

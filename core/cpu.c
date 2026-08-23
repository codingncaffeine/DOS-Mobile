/* x86 interpreter: main loop, prefixes, ModRM, and the one-byte opcode map. */
#include "cpu_int.h"
#include "io.h"
#include "pic.h"
#include "bios.h"

CPU cpu;

const u8 parity_tab[256] = {
#define P2(n) n, n ^ 1, n ^ 1, n
#define P4(n) P2(n), P2(n ^ 1), P2(n ^ 1), P2(n)
#define P6(n) P4(n), P4(n ^ 1), P4(n ^ 1), P4(n)
  P6(1), P6(0), P6(0), P6(1)
#undef P2
#undef P4
#undef P6
};

static const char *const gen_names[] = {"8088", "8086", "80186", "80286", "80386", "80486", "Pentium", "Pentium II"};
static int itrace_left; /* one-shot instruction trace armed by the IRQ0 probe at trace>=4 */
static int itrace_burst; /* consecutive deliveries still to trace */
static u32 itrace_entry_count; /* executions of the traced program's timer-handler entry */
extern int cpu_trace_faults;
const char *cpu_gen_name(int gen) { return gen_names[gen & 7]; }

void cpu_set_generation(int gen) { cpu.gen = (u8)gen; }

void cpu_init(int gen, int fpu) {
  dm_memset(&cpu, 0, sizeof cpu);
  cpu.gen = (u8)gen;
  cpu.fpu_present = (u8)fpu;
  cpu_reset();
}

void cpu_reset(void) {
  for (int i = 0; i < 8; i++) cpu.r[i] = 0;
  cpu.eflags = 0x2;
  cpu.lf_type = LF_NONE;
  cpu.cr0 = 0;
  cpu.cr2 = cpu.cr3 = cpu.cr4 = 0;
  cpu.halted = 0;
  cpu.inhibit = 0;
  cpu.fault_pending = 0;
  cpu.cpl = 0;
  cpu.fatal = 0;
  for (int s = 0; s < SEG_COUNT; s++) {
    cpu.seg[s].sel = 0;
    cpu.seg[s].base = 0;
    cpu.seg[s].limit = 0xFFFF;
    cpu.seg[s].access = s == SEG_CS ? 0x9B : 0x93;
    cpu.seg[s].flags = (u8)(SEGF_READ | SEGF_WRITE | (s == SEG_CS ? SEGF_CODE : 0));
    cpu.seg[s].db = 0;
    cpu.seg[s].valid = 1;
    cpu.seg[s].dpl = 0;
  }
  cpu.ldtr.valid = 0; cpu.tr.valid = 0; cpu.tss_is32 = 0;
  cpu.in_fault_delivery = 0;
  tlb_flush();
  /* Reset vector: F000:FFF0 (the BIOS ROM lives at F0000 for every generation here). */
  cpu.seg[SEG_CS].sel = 0xF000;
  cpu.seg[SEG_CS].base = 0xF0000;
  cpu.eip = 0xFFF0;
  cpu.gdtr.base = 0; cpu.gdtr.limit = 0xFFFF;
  cpu.idtr.base = 0; cpu.idtr.limit = 0xFFFF;
  cpu.r[REG_DX] = cpu.gen >= GEN_386 ? 0x0300u + (cpu.gen - GEN_386) * 0x100u : 0;
}

void cpu_load_seg(int s, u16 sel) { load_seg_real(s, sel); }

u32 cpu_get_eflags(void) {
  flags_sync();
  u32 f = cpu.eflags | 2;
  if (cpu.gen <= GEN_186) f |= 0xF000;          /* 8086/186: bits 12-15 read as 1 */
  else if (cpu.gen == GEN_286 && !in_pmode()) f &= 0x0FFF; /* 286 real mode: 12-15 read as 0 */
  return f;
}

void cpu_set_eflags(u32 v) {
  u32 keep_mask;
  switch (cpu.gen) {
    case GEN_8088: case GEN_8086: case GEN_186: keep_mask = 0x0FD5; break;
    case GEN_286: keep_mask = in_pmode() ? 0x7FD5 : 0x0FD5; break;
    default: keep_mask = 0x00277FD5; break; /* up to ID (bit 21), AC, VM handled by IRET/pmode */
  }
  cpu.lf_type = LF_NONE;
  cpu.eflags = (v & keep_mask) | 2;
}

void cpu_flags_sync(void) { flags_sync(); }

int cpu_interrupts_enabled(void) { return (cpu.eflags & F_IF) != 0; }

void cpu_ud(void) { raise_fault(EXC_UD, 0, 0); }

u32 cpu_cycles_add(u32 n) { cpu.cycles += n; return n; }

/* ---------------- ModRM ---------------- */
void decode_modrm(void) {
  u8 m = fetch8();
  cpu.modrm_mod = m >> 6;
  cpu.modrm_reg = (m >> 3) & 7;
  cpu.modrm_rm = m & 7;
  if (cpu.modrm_mod == 3) return;
  int seg = SEG_DS;
  u32 ea;
  if (cpu.asize32) {
    u32 base = 0, index = 0;
    int rm = cpu.modrm_rm;
    if (rm == 4) {
      u8 sib = fetch8();
      int scale = sib >> 6, idx = (sib >> 3) & 7, b = sib & 7;
      if (idx != 4) index = cpu.r[idx] << scale;
      if (b == 5 && cpu.modrm_mod == 0) base = fetch32();
      else {
        base = cpu.r[b];
        if (b == 4 || b == 5) seg = SEG_SS;
      }
    } else if (rm == 5 && cpu.modrm_mod == 0) {
      base = fetch32();
    } else {
      base = cpu.r[rm];
      if (rm == 5) seg = SEG_SS;
    }
    ea = base + index;
    if (cpu.modrm_mod == 1) ea += (u32)(s32)(s8)fetch8();
    else if (cpu.modrm_mod == 2) ea += fetch32();
  } else {
    switch (cpu.modrm_rm) {
      case 0: ea = cpu.r[REG_BX] + cpu.r[REG_SI]; break;
      case 1: ea = cpu.r[REG_BX] + cpu.r[REG_DI]; break;
      case 2: ea = cpu.r[REG_BP] + cpu.r[REG_SI]; seg = SEG_SS; break;
      case 3: ea = cpu.r[REG_BP] + cpu.r[REG_DI]; seg = SEG_SS; break;
      case 4: ea = cpu.r[REG_SI]; break;
      case 5: ea = cpu.r[REG_DI]; break;
      case 6:
        if (cpu.modrm_mod == 0) ea = fetch16();
        else { ea = cpu.r[REG_BP]; seg = SEG_SS; }
        break;
      default: ea = cpu.r[REG_BX]; break;
    }
    if (cpu.modrm_mod == 1) ea += (u32)(s32)(s8)fetch8();
    else if (cpu.modrm_mod == 2) ea += fetch16();
    ea &= 0xFFFF;
  }
  cpu.ea = ea;
  cpu.ea_seg = cpu.seg_override != SEG_NONE ? cpu.seg_override : (u8)seg;
}

/* ---------------- shifts and rotates (group 2) ---------------- */
static void shift_op(int sub, int bits, u32 v, u32 count) {
  u32 m = size_mask(bits);
  v &= m;
  if (cpu.gen >= GEN_186) count &= 0x1F;
  else count &= 0xFF;
  if (count == 0) { rm_wrv(v); return; }
  u32 res, cf, of;
  int msb = bits - 1;
  switch (sub) {
    case 0: { /* ROL */
      u32 c = count % bits;
      res = c ? ((v << c) | (v >> (bits - c))) & m : v;
      cf = res & 1;
      of = ((res >> msb) & 1) ^ cf;
      flags_sync();
      cpu.eflags = (cpu.eflags & ~(u32)(F_CF | F_OF)) | (cf ? F_CF : 0) | (count == 1 && of ? F_OF : 0);
      break;
    }
    case 1: { /* ROR */
      u32 c = count % bits;
      res = c ? ((v >> c) | (v << (bits - c))) & m : v;
      cf = (res >> msb) & 1;
      of = cf ^ ((res >> (msb - 1)) & 1);
      flags_sync();
      cpu.eflags = (cpu.eflags & ~(u32)(F_CF | F_OF)) | (cf ? F_CF : 0) | (count == 1 && of ? F_OF : 0);
      break;
    }
    case 2: { /* RCL */
      u32 c = count % (bits + 1);
      cf = (u32)flag_cf();
      res = v;
      for (u32 i = 0; i < c; i++) {
        u32 nc = (res >> msb) & 1;
        res = ((res << 1) | cf) & m;
        cf = nc;
      }
      of = ((res >> msb) & 1) ^ cf;
      flags_sync();
      cpu.eflags = (cpu.eflags & ~(u32)(F_CF | F_OF)) | (cf ? F_CF : 0) | (count == 1 && of ? F_OF : 0);
      break;
    }
    case 3: { /* RCR */
      u32 c = count % (bits + 1);
      cf = (u32)flag_cf();
      of = ((v >> msb) & 1) ^ cf;
      res = v;
      for (u32 i = 0; i < c; i++) {
        u32 nc = res & 1;
        res = (res >> 1) | (cf << msb);
        cf = nc;
      }
      flags_sync();
      cpu.eflags = (cpu.eflags & ~(u32)(F_CF | F_OF)) | (cf ? F_CF : 0) | (count == 1 && of ? F_OF : 0);
      break;
    }
    case 4:
    case 6: { /* SHL / SAL */
      if (count > (u32)bits) { res = 0; cf = 0; }
      else { res = (v << count) & m; cf = (v >> (bits - count)) & 1; }
      of = ((res >> msb) & 1) ^ cf;
      lf_zsp(bits, res, (int)cf, count == 1 ? (int)of : flag_of());
      break;
    }
    case 5: { /* SHR */
      if (count > (u32)bits) { res = 0; cf = 0; }
      else { cf = (v >> (count - 1)) & 1; res = v >> count; }
      of = (v >> msb) & 1;
      lf_zsp(bits, res, (int)cf, count == 1 ? (int)of : flag_of());
      break;
    }
    default: { /* SAR */
      s32 sv = bits == 8 ? (s32)(s8)v : bits == 16 ? (s32)(s16)v : (s32)v;
      if (count >= (u32)bits) { res = sv < 0 ? m : 0; cf = sv < 0; }
      else { cf = ((u32)sv >> (count - 1)) & 1; res = (u32)(sv >> count) & m; }
      lf_zsp(bits, res, (int)cf, 0);
      break;
    }
  }
  if (bits == 8) rm_wr8((u8)res);
  else rm_wrv(res);
}

/* ---------------- group 3: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV ---------------- */
static void group3(int bits) {
  int sub = cpu.modrm_reg;
  u32 m = size_mask(bits);
  u32 v = bits == 8 ? rm_rd8() : rm_rdv();
  FAULT_CHECK();
  switch (sub) {
    case 0:
    case 1: {
      u32 imm = bits == 8 ? fetch8() : fetchv();
      alu_op(4, bits, v, imm);
      break;
    }
    case 2:
      if (bits == 8) rm_wr8((u8)~v);
      else rm_wrv(~v & m);
      break;
    case 3: {
      u32 res = (0u - v) & m;
      lf_set(LF_NEG, bits, 0, v, res, 0);
      if (bits == 8) rm_wr8((u8)res);
      else rm_wrv(res);
      break;
    }
    case 4: { /* MUL */
      cpu.cycles += bits == 8 ? 12 : bits == 16 ? 20 : 36;
      if (bits == 8) {
        u32 r = (u32)reg8_get(0) * v;
        reg16_set(REG_AX, (u16)r);
        lf_zsp(8, r & 0xFF, r > 0xFF, r > 0xFF);
      } else if (bits == 16) {
        u32 r = (u32)reg16_get(REG_AX) * v;
        reg16_set(REG_AX, (u16)r);
        reg16_set(REG_DX, (u16)(r >> 16));
        lf_zsp(16, r & 0xFFFF, r > 0xFFFF, r > 0xFFFF);
      } else {
        u64 r = (u64)cpu.r[REG_AX] * v;
        cpu.r[REG_AX] = (u32)r;
        cpu.r[REG_DX] = (u32)(r >> 32);
        lf_zsp(32, (u32)r, r > 0xFFFFFFFFull, r > 0xFFFFFFFFull);
      }
      break;
    }
    case 5: { /* IMUL */
      cpu.cycles += bits == 8 ? 12 : bits == 16 ? 20 : 36;
      if (bits == 8) {
        s32 r = (s32)(s8)reg8_get(0) * (s32)(s8)v;
        reg16_set(REG_AX, (u16)r);
        int ov = r != (s32)(s8)r;
        lf_zsp(8, (u32)r & 0xFF, ov, ov);
      } else if (bits == 16) {
        s32 r = (s32)(s16)reg16_get(REG_AX) * (s32)(s16)v;
        reg16_set(REG_AX, (u16)r);
        reg16_set(REG_DX, (u16)((u32)r >> 16));
        int ov = r != (s32)(s16)r;
        lf_zsp(16, (u32)r & 0xFFFF, ov, ov);
      } else {
        s64 r = (s64)(s32)cpu.r[REG_AX] * (s64)(s32)v;
        cpu.r[REG_AX] = (u32)r;
        cpu.r[REG_DX] = (u32)((u64)r >> 32);
        int ov = r != (s64)(s32)r;
        lf_zsp(32, (u32)r, ov, ov);
      }
      break;
    }
    case 6: { /* DIV */
      cpu.cycles += bits == 8 ? 16 : bits == 16 ? 24 : 40;
      if (v == 0) { raise_fault(EXC_DE, 0, 0); return; }
      if (bits == 8) {
        u32 d = reg16_get(REG_AX), q = d / v;
        if (q > 0xFF) { raise_fault(EXC_DE, 0, 0); return; }
        reg8_set(0, (u8)q);
        reg8_set(4, (u8)(d % v));
      } else if (bits == 16) {
        u32 d = ((u32)reg16_get(REG_DX) << 16) | reg16_get(REG_AX), q = d / v;
        if (q > 0xFFFF) { raise_fault(EXC_DE, 0, 0); return; }
        reg16_set(REG_AX, (u16)q);
        reg16_set(REG_DX, (u16)(d % v));
      } else {
        u64 d = ((u64)cpu.r[REG_DX] << 32) | cpu.r[REG_AX], q = d / v;
        if (q > 0xFFFFFFFFull) { raise_fault(EXC_DE, 0, 0); return; }
        cpu.r[REG_AX] = (u32)q;
        cpu.r[REG_DX] = (u32)(d % v);
      }
      break;
    }
    default: { /* IDIV */
      cpu.cycles += bits == 8 ? 19 : bits == 16 ? 27 : 43;
      if (v == 0) { raise_fault(EXC_DE, 0, 0); return; }
      if (bits == 8) {
        s32 d = (s32)(s16)reg16_get(REG_AX), dv = (s32)(s8)v;
        s32 q = d / dv, r = d % dv;
        if (q != (s32)(s8)q) { raise_fault(EXC_DE, 0, 0); return; }
        reg8_set(0, (u8)q);
        reg8_set(4, (u8)r);
      } else if (bits == 16) {
        s32 d = (s32)(((u32)reg16_get(REG_DX) << 16) | reg16_get(REG_AX)), dv = (s32)(s16)v;
        if (d == (s32)0x80000000 && dv == -1) { raise_fault(EXC_DE, 0, 0); return; }
        s32 q = d / dv, r = d % dv;
        if (q != (s32)(s16)q) { raise_fault(EXC_DE, 0, 0); return; }
        reg16_set(REG_AX, (u16)q);
        reg16_set(REG_DX, (u16)r);
      } else {
        s64 d = (s64)(((u64)cpu.r[REG_DX] << 32) | cpu.r[REG_AX]), dv = (s64)(s32)v;
        if (d == (s64)0x8000000000000000ull && dv == -1) { raise_fault(EXC_DE, 0, 0); return; }
        s64 q = d / dv, r = d % dv;
        if (q != (s64)(s32)q) { raise_fault(EXC_DE, 0, 0); return; }
        cpu.r[REG_AX] = (u32)q;
        cpu.r[REG_DX] = (u32)r;
      }
      break;
    }
  }
}

/* ---------------- string instructions ---------------- */
static void string_op(u8 op) {
  int bits = (op & 1) ? osize_bits() : 8;
  u32 sz = (u32)bits / 8;
  u32 amask = cpu.asize32 ? 0xFFFFFFFFu : 0xFFFFu;
  int seg = cpu.seg_override != SEG_NONE ? cpu.seg_override : SEG_DS;
  u32 delta = (cpu.eflags & F_DF) ? (u32)-(s32)sz : sz;
  int iterations = 0;
  for (;;) {
    if (cpu.rep && (cpu.r[REG_CX] & amask) == 0) return;
    u32 si = cpu.r[REG_SI] & amask, di = cpu.r[REG_DI] & amask;
    int cmp_done = 0;
    switch (op & 0xFE) {
      case 0xA4: { /* MOVS */
        if (bits == 8) wr8s(SEG_ES, di, rd8s(seg, si));
        else if (bits == 16) wr16s(SEG_ES, di, rd16s(seg, si));
        else wr32s(SEG_ES, di, rd32s(seg, si));
        si += delta; di += delta;
        break;
      }
      case 0xA6: { /* CMPS */
        u32 a = bits == 8 ? rd8s(seg, si) : bits == 16 ? rd16s(seg, si) : rd32s(seg, si);
        u32 b = bits == 8 ? rd8s(SEG_ES, di) : bits == 16 ? rd16s(SEG_ES, di) : rd32s(SEG_ES, di);
        alu_op(7, bits, a, b);
        si += delta; di += delta;
        cmp_done = 1;
        break;
      }
      case 0xAA: { /* STOS */
        if (bits == 8) wr8s(SEG_ES, di, reg8_get(0));
        else if (bits == 16) wr16s(SEG_ES, di, reg16_get(REG_AX));
        else wr32s(SEG_ES, di, cpu.r[REG_AX]);
        di += delta;
        break;
      }
      case 0xAC: { /* LODS */
        if (bits == 8) reg8_set(0, rd8s(seg, si));
        else if (bits == 16) reg16_set(REG_AX, rd16s(seg, si));
        else cpu.r[REG_AX] = rd32s(seg, si);
        si += delta;
        break;
      }
      case 0xAE: { /* SCAS */
        u32 a = bits == 8 ? reg8_get(0) : bits == 16 ? reg16_get(REG_AX) : cpu.r[REG_AX];
        u32 b = bits == 8 ? rd8s(SEG_ES, di) : bits == 16 ? rd16s(SEG_ES, di) : rd32s(SEG_ES, di);
        alu_op(7, bits, a, b);
        di += delta;
        cmp_done = 1;
        break;
      }
      case 0x6C: { /* INS */
        u16 port = reg16_get(REG_DX);
        if (!io_allowed(port, bits / 8)) return;
        if (bits == 8) wr8s(SEG_ES, di, io_rd8(port));
        else if (bits == 16) wr16s(SEG_ES, di, io_rd16(port));
        else wr32s(SEG_ES, di, io_rd32(port));
        di += delta;
        break;
      }
      default: { /* OUTS */
        u16 port = reg16_get(REG_DX);
        if (!io_allowed(port, bits / 8)) return;
        if (bits == 8) io_wr8(port, rd8s(seg, si));
        else if (bits == 16) io_wr16(port, rd16s(seg, si));
        else io_wr32(port, rd32s(seg, si));
        si += delta;
        break;
      }
    }
    if (cpu.fault_pending) return;
    if (cpu.asize32) { cpu.r[REG_SI] = si; cpu.r[REG_DI] = di; }
    else { reg16_set(REG_SI, (u16)si); reg16_set(REG_DI, (u16)di); }
    cpu.cycles += 3;
    if (!cpu.rep) return;
    u32 cx = (cpu.r[REG_CX] - 1) & amask;
    if (cpu.asize32) cpu.r[REG_CX] = cx;
    else reg16_set(REG_CX, (u16)cx);
    if (cx == 0) return;
    if (cmp_done) {
      int z = flag_zf();
      if ((cpu.rep == 0xF3 && !z) || (cpu.rep == 0xF2 && z)) return;
    }
    if (++iterations >= 128 || (pic_has_pending() && (cpu.eflags & F_IF))) {
      cpu.eip = cpu.eip_start; /* resume the instruction after interrupts are serviced */
      return;
    }
  }
}

/* ---------------- BCD helpers ---------------- */
static void op_daa(void) {
  u32 al = reg8_get(0), old_al = al;
  int cf = flag_cf(), af = flag_af();
  int ncf = 0;
  if ((al & 0xF) > 9 || af) { al += 6; ncf = cf || (old_al > 0xF9); af = 1; } else af = 0;
  if (old_al > 0x99 || cf) { al += 0x60; ncf = 1; }
  al &= 0xFF;
  reg8_set(0, (u8)al);
  lf_zsp(8, al, ncf, 0);
  cpu.eflags = (cpu.eflags & ~(u32)F_AF) | (af ? F_AF : 0);
}
static void op_das(void) {
  u32 al = reg8_get(0), old_al = al;
  int cf = flag_cf(), af = flag_af();
  int ncf = 0;
  if ((al & 0xF) > 9 || af) { al -= 6; ncf = cf || (old_al < 6); af = 1; } else af = 0;
  if (old_al > 0x99 || cf) { al -= 0x60; ncf = 1; }
  al &= 0xFF;
  reg8_set(0, (u8)al);
  lf_zsp(8, al, ncf, 0);
  cpu.eflags = (cpu.eflags & ~(u32)F_AF) | (af ? F_AF : 0);
}
static void op_aaa(void) {
  u32 al = reg8_get(0), ah = reg8_get(4);
  int af = flag_af(), c;
  if ((al & 0xF) > 9 || af) { al += 6; ah += 1; c = 1; } else c = 0;
  al &= 0xF;
  reg8_set(0, (u8)al);
  reg8_set(4, (u8)ah);
  lf_zsp(8, al, c, 0);
  cpu.eflags = (cpu.eflags & ~(u32)F_AF) | (c ? F_AF : 0);
}
static void op_aas(void) {
  u32 al = reg8_get(0), ah = reg8_get(4);
  int af = flag_af(), c;
  if ((al & 0xF) > 9 || af) { al -= 6; ah -= 1; c = 1; } else c = 0;
  al &= 0xF;
  reg8_set(0, (u8)al);
  reg8_set(4, (u8)ah);
  lf_zsp(8, al, c, 0);
  cpu.eflags = (cpu.eflags & ~(u32)F_AF) | (c ? F_AF : 0);
}

/* ---------------- the one-byte opcode map ---------------- */
static void exec_primary(u8 op) {
  switch (op) {
    /* ALU r/m,r ; r,r/m ; AL,imm8 ; eAX,imm */
    case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38: {
      decode_modrm();
      if (op != 0x38 && !rm_probe(8)) break;
      u32 a = rm_rd8();
      FAULT_CHECK();
      u32 r = alu_op(op >> 3, 8, a, reg8_get(cpu.modrm_reg));
      if ((op >> 3) != 7) rm_wr8((u8)r);
      break;
    }
    case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x31: case 0x39: {
      decode_modrm();
      if (op != 0x39 && !rm_probe(osize_bits())) break;
      u32 a = rm_rdv();
      FAULT_CHECK();
      u32 r = alu_op(op >> 3, osize_bits(), a, regv_get(cpu.modrm_reg));
      if ((op >> 3) != 7) rm_wrv(r);
      break;
    }
    case 0x02: case 0x0A: case 0x12: case 0x1A: case 0x22: case 0x2A: case 0x32: case 0x3A: {
      decode_modrm();
      u32 b = rm_rd8();
      FAULT_CHECK();
      u32 r = alu_op(op >> 3, 8, reg8_get(cpu.modrm_reg), b);
      if ((op >> 3) != 7) reg8_set(cpu.modrm_reg, (u8)r);
      break;
    }
    case 0x03: case 0x0B: case 0x13: case 0x1B: case 0x23: case 0x2B: case 0x33: case 0x3B: {
      decode_modrm();
      u32 b = rm_rdv();
      FAULT_CHECK();
      u32 r = alu_op(op >> 3, osize_bits(), regv_get(cpu.modrm_reg), b);
      if ((op >> 3) != 7) regv_set(cpu.modrm_reg, r);
      break;
    }
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: {
      u32 imm = fetch8();
      u32 r = alu_op(op >> 3, 8, reg8_get(0), imm);
      if ((op >> 3) != 7) reg8_set(0, (u8)r);
      break;
    }
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: {
      u32 imm = fetchv();
      u32 r = alu_op(op >> 3, osize_bits(), regv_get(REG_AX), imm);
      if ((op >> 3) != 7) regv_set(REG_AX, r);
      break;
    }

    case 0x06: pushv(cpu.seg[SEG_ES].sel); break;
    case 0x07: { u32 v = popv(); FAULT_CHECK(); load_seg(SEG_ES, (u16)v); break; }
    case 0x0E: pushv(cpu.seg[SEG_CS].sel); break;
    case 0x0F:
      /* the BIOS ROM's HLE hook (0F FF) must work on every generation; elsewhere 8086/186 treat 0F as POP CS */
      if (cpu.gen >= GEN_286 || cpu.seg[SEG_CS].base == BIOS_ROM_BASE) cpu_exec_0f();
      else { u32 v = popv(); FAULT_CHECK(); load_seg_real(SEG_CS, (u16)v); }
      break;
    case 0x16: pushv(cpu.seg[SEG_SS].sel); break;
    case 0x17: { u32 v = popv(); FAULT_CHECK(); load_seg(SEG_SS, (u16)v); cpu.inhibit = 1; break; }
    case 0x1E: pushv(cpu.seg[SEG_DS].sel); break;
    case 0x1F: { u32 v = popv(); FAULT_CHECK(); load_seg(SEG_DS, (u16)v); break; }

    case 0x27: op_daa(); break;
    case 0x2F: op_das(); break;
    case 0x37: op_aaa(); break;
    case 0x3F: op_aas(); break;

    /* INC/DEC r */
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
      regv_set(op & 7, alu_inc(osize_bits(), regv_get(op & 7)));
      break;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
      regv_set(op & 7, alu_dec(osize_bits(), regv_get(op & 7)));
      break;

    /* PUSH/POP r */
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x55: case 0x56: case 0x57:
      pushv(regv_get(op & 7));
      break;
    case 0x54: /* PUSH SP: 8086/186 push the decremented value */
      if (cpu.gen <= GEN_186) { u32 v = (regv_get(REG_SP) - 2) & 0xFFFF; pushv(v); }
      else pushv(regv_get(REG_SP));
      break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
      u32 v = popv();
      FAULT_CHECK();
      regv_set(op & 7, v);
      break;
    }

    case 0x60: /* PUSHA */
      if (cpu.gen < GEN_186) goto jcc_alias;
      {
        u32 sp = regv_get(REG_SP);
        pushv(regv_get(REG_AX)); pushv(regv_get(REG_CX)); pushv(regv_get(REG_DX)); pushv(regv_get(REG_BX));
        pushv(sp); pushv(regv_get(REG_BP)); pushv(regv_get(REG_SI)); pushv(regv_get(REG_DI));
      }
      break;
    case 0x61: /* POPA */
      if (cpu.gen < GEN_186) goto jcc_alias;
      {
        u32 di = popv(), si = popv(), bp = popv();
        popv();
        u32 bx = popv(), dx = popv(), cx = popv(), ax = popv();
        FAULT_CHECK();
        regv_set(REG_DI, di); regv_set(REG_SI, si); regv_set(REG_BP, bp);
        regv_set(REG_BX, bx); regv_set(REG_DX, dx); regv_set(REG_CX, cx); regv_set(REG_AX, ax);
      }
      break;
    case 0x62: /* BOUND */
      if (cpu.gen < GEN_186) goto jcc_alias;
      decode_modrm();
      if (rm_is_reg()) { cpu_ud(); break; }
      if (cpu.osize32) {
        s32 idx = (s32)cpu.r[cpu.modrm_reg];
        s32 lo = (s32)rd32s(cpu.ea_seg, cpu.ea), hi = (s32)rd32s(cpu.ea_seg, cpu.ea + 4);
        FAULT_CHECK();
        if (idx < lo || idx > hi) raise_fault(EXC_BR, 0, 0);
      } else {
        s32 idx = (s32)(s16)cpu.r[cpu.modrm_reg];
        s32 lo = (s16)rd16s(cpu.ea_seg, cpu.ea), hi = (s16)rd16s(cpu.ea_seg, cpu.ea + 2);
        FAULT_CHECK();
        if (idx < lo || idx > hi) raise_fault(EXC_BR, 0, 0);
      }
      break;
    case 0x63: /* ARPL (protected mode only) */
      if (cpu.gen < GEN_186) goto jcc_alias;
      if (!in_pmode() || in_v86()) { cpu_ud(); break; }
      decode_modrm();
      {
        u32 d = rm_rd16();
        FAULT_CHECK();
        u32 s = reg16_get(cpu.modrm_reg);
        flags_sync();
        if ((d & 3) < (s & 3)) { d = (d & ~3u) | (s & 3); rm_wr16((u16)d); cpu.eflags |= F_ZF; }
        else cpu.eflags &= ~(u32)F_ZF;
      }
      break;
    case 0x68: /* PUSH imm */
      if (cpu.gen < GEN_186) goto jcc_alias;
      pushv(fetchv());
      break;
    case 0x69: /* IMUL r, r/m, imm */
      if (cpu.gen < GEN_186) goto jcc_alias;
      decode_modrm();
      {
        u32 v = rm_rdv();
        FAULT_CHECK();
        u32 imm = fetchv();
        if (cpu.osize32) {
          s64 r = (s64)(s32)v * (s64)(s32)imm;
          cpu.r[cpu.modrm_reg] = (u32)r;
          int ov = r != (s64)(s32)r;
          lf_zsp(32, (u32)r, ov, ov);
        } else {
          s32 r = (s32)(s16)v * (s32)(s16)imm;
          reg16_set(cpu.modrm_reg, (u16)r);
          int ov = r != (s32)(s16)r;
          lf_zsp(16, (u32)r & 0xFFFF, ov, ov);
        }
        cpu.cycles += 12;
      }
      break;
    case 0x6A: /* PUSH imm8 */
      if (cpu.gen < GEN_186) goto jcc_alias;
      pushv((u32)(s32)(s8)fetch8());
      break;
    case 0x6B: /* IMUL r, r/m, imm8 */
      if (cpu.gen < GEN_186) goto jcc_alias;
      decode_modrm();
      {
        u32 v = rm_rdv();
        FAULT_CHECK();
        s32 imm = (s32)(s8)fetch8();
        if (cpu.osize32) {
          s64 r = (s64)(s32)v * imm;
          cpu.r[cpu.modrm_reg] = (u32)r;
          int ov = r != (s64)(s32)r;
          lf_zsp(32, (u32)r, ov, ov);
        } else {
          s32 r = (s32)(s16)v * imm;
          reg16_set(cpu.modrm_reg, (u16)r);
          int ov = r != (s32)(s16)r;
          lf_zsp(16, (u32)r & 0xFFFF, ov, ov);
        }
        cpu.cycles += 12;
      }
      break;
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
      if (cpu.gen < GEN_186) goto jcc_alias;
      string_op(op);
      break;
    case 0x64: case 0x65: case 0x66: case 0x67: /* only reached on < 386 (prefixes otherwise) */
    jcc_alias:
      if (cpu.gen < GEN_186) { /* 8086: 60-6F alias 70-7F */
        s32 rel = (s8)fetch8();
        if (cond_true(op & 0xF)) cpu.eip = (cpu.eip + (u32)rel) & eip_mask();
      } else cpu_ud();
      break;

    /* Jcc rel8 */
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
      s32 rel = (s8)fetch8();
      if (cond_true(op & 0xF)) { cpu.eip = (cpu.eip + (u32)rel) & eip_mask(); cpu.cycles += 3; }
      break;
    }

    /* group 1 */
    case 0x80: case 0x82: {
      decode_modrm();
      if (cpu.modrm_reg != 7 && !rm_probe(8)) break;
      u32 a = rm_rd8();
      FAULT_CHECK();
      u32 imm = fetch8();
      u32 r = alu_op(cpu.modrm_reg, 8, a, imm);
      if (cpu.modrm_reg != 7) rm_wr8((u8)r);
      break;
    }
    case 0x81: {
      decode_modrm();
      if (cpu.modrm_reg != 7 && !rm_probe(osize_bits())) break;
      u32 a = rm_rdv();
      FAULT_CHECK();
      u32 imm = fetchv();
      u32 r = alu_op(cpu.modrm_reg, osize_bits(), a, imm);
      if (cpu.modrm_reg != 7) rm_wrv(r);
      break;
    }
    case 0x83: {
      decode_modrm();
      if (cpu.modrm_reg != 7 && !rm_probe(osize_bits())) break;
      u32 a = rm_rdv();
      FAULT_CHECK();
      u32 imm = (u32)(s32)(s8)fetch8();
      u32 r = alu_op(cpu.modrm_reg, osize_bits(), a, imm);
      if (cpu.modrm_reg != 7) rm_wrv(r);
      break;
    }
    case 0x84: { decode_modrm(); u32 a = rm_rd8(); FAULT_CHECK(); alu_op(4, 8, a, reg8_get(cpu.modrm_reg)); break; }
    case 0x85: { decode_modrm(); u32 a = rm_rdv(); FAULT_CHECK(); alu_op(4, osize_bits(), a, regv_get(cpu.modrm_reg)); break; }
    case 0x86: {
      decode_modrm();
      if (!rm_probe(8)) break;
      u32 a = rm_rd8();
      FAULT_CHECK();
      rm_wr8(reg8_get(cpu.modrm_reg));
      FAULT_CHECK();
      reg8_set(cpu.modrm_reg, (u8)a);
      break;
    }
    case 0x87: {
      decode_modrm();
      if (!rm_probe(osize_bits())) break;
      u32 a = rm_rdv();
      FAULT_CHECK();
      rm_wrv(regv_get(cpu.modrm_reg));
      FAULT_CHECK();
      regv_set(cpu.modrm_reg, a);
      break;
    }
    case 0x88: decode_modrm(); rm_wr8(reg8_get(cpu.modrm_reg)); break;
    case 0x89: decode_modrm(); rm_wrv(regv_get(cpu.modrm_reg)); break;
    case 0x8A: { decode_modrm(); u32 v = rm_rd8(); FAULT_CHECK(); reg8_set(cpu.modrm_reg, (u8)v); break; }
    case 0x8B: { decode_modrm(); u32 v = rm_rdv(); FAULT_CHECK(); regv_set(cpu.modrm_reg, v); break; }
    case 0x8C: /* MOV r/m16, Sreg */
      decode_modrm();
      if (cpu.modrm_reg >= SEG_COUNT || (cpu.modrm_reg >= SEG_FS && cpu.gen < GEN_386)) { cpu_ud(); break; }
      if (rm_is_reg()) regv_set(cpu.modrm_rm, cpu.seg[cpu.modrm_reg].sel);
      else rm_wr16(cpu.seg[cpu.modrm_reg].sel);
      break;
    case 0x8D: /* LEA */
      decode_modrm();
      if (rm_is_reg()) { cpu_ud(); break; }
      regv_set(cpu.modrm_reg, cpu.osize32 ? cpu.ea : (cpu.ea & 0xFFFF));
      break;
    case 0x8E: { /* MOV Sreg, r/m16 */
      decode_modrm();
      int s = cpu.modrm_reg;
      if (s >= SEG_COUNT || (s >= SEG_FS && cpu.gen < GEN_386) || (s == SEG_CS && cpu.gen >= GEN_286)) { cpu_ud(); break; }
      u32 v = rm_rd16();
      FAULT_CHECK();
      load_seg(s, (u16)v);
      if (s == SEG_SS) cpu.inhibit = 1;
      break;
    }
    case 0x8F: { /* POP r/m: the write must succeed before SP moves */
      u32 v = peekv(0);
      FAULT_CHECK();
      u32 sp_before = sp_get();
      sp_add(cpu.osize32 ? 4 : 2); /* the effective address may use (E)SP after the pop */
      decode_modrm();
      rm_wrv(v);
      if (cpu.fault_pending) sp_set(sp_before);
      break;
    }

    case 0x90: break; /* NOP */
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
      u32 t = regv_get(REG_AX);
      regv_set(REG_AX, regv_get(op & 7));
      regv_set(op & 7, t);
      break;
    }
    case 0x98: /* CBW / CWDE */
      if (cpu.osize32) cpu.r[REG_AX] = (u32)(s32)(s16)cpu.r[REG_AX];
      else reg16_set(REG_AX, (u16)(s16)(s8)reg8_get(0));
      break;
    case 0x99: /* CWD / CDQ */
      if (cpu.osize32) cpu.r[REG_DX] = (cpu.r[REG_AX] & 0x80000000u) ? 0xFFFFFFFFu : 0;
      else reg16_set(REG_DX, (reg16_get(REG_AX) & 0x8000) ? 0xFFFF : 0);
      break;
    case 0x9A: { /* CALL far imm */
      u32 off = fetchv();
      u16 sel = fetch16();
      cpu_far_call(sel, off);
      break;
    }
    case 0x9B: break; /* WAIT */
    case 0x9C: { /* PUSHF */
      if (in_v86() && iopl() < 3) { raise_fault(EXC_GP, 1, 0); break; }
      u32 f = cpu_get_eflags();
      if (cpu.osize32) push32(f & ~(u32)(F_VM | F_RF));
      else push16((u16)f);
      break;
    }
    case 0x9D: { /* POPF */
      if (in_v86() && iopl() < 3) { raise_fault(EXC_GP, 1, 0); break; }
      u32 v = popv();
      FAULT_CHECK();
      u32 cur = cpu_get_eflags();
      u32 mask = cpu.osize32 ? 0x00257FD5u : 0x7FD5u;
      if (in_pmode()) {
        if (cpu.cpl > 0 || in_v86()) mask &= ~(u32)F_IOPL;
        if (cpu.cpl > iopl()) mask &= ~(u32)F_IF;
      }
      mask &= ~(u32)(F_VM | F_RF | F_VIP | F_VIF);
      cpu_set_eflags((cur & ~mask) | (v & mask));
      break;
    }
    case 0x9E: { /* SAHF */
      flags_sync();
      cpu.eflags = (cpu.eflags & ~0xD5u) | (reg8_get(4) & 0xD5);
      break;
    }
    case 0x9F: reg8_set(4, (u8)((cpu_get_eflags() & 0xD5) | 2)); break; /* LAHF */

    case 0xA0: { u32 a = cpu.asize32 ? fetch32() : fetch16(); int s = cpu.seg_override != SEG_NONE ? cpu.seg_override : SEG_DS; u32 v = rd8s(s, a); FAULT_CHECK(); reg8_set(0, (u8)v); break; }
    case 0xA1: { u32 a = cpu.asize32 ? fetch32() : fetch16(); int s = cpu.seg_override != SEG_NONE ? cpu.seg_override : SEG_DS; u32 v = rdvs(s, a); FAULT_CHECK(); regv_set(REG_AX, v); break; }
    case 0xA2: { u32 a = cpu.asize32 ? fetch32() : fetch16(); int s = cpu.seg_override != SEG_NONE ? cpu.seg_override : SEG_DS; wr8s(s, a, reg8_get(0)); break; }
    case 0xA3: { u32 a = cpu.asize32 ? fetch32() : fetch16(); int s = cpu.seg_override != SEG_NONE ? cpu.seg_override : SEG_DS; wrvs(s, a, regv_get(REG_AX)); break; }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
      string_op(op);
      break;
    case 0xA8: alu_op(4, 8, reg8_get(0), fetch8()); break;
    case 0xA9: alu_op(4, osize_bits(), regv_get(REG_AX), fetchv()); break;

    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
      reg8_set(op & 7, fetch8());
      break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
      regv_set(op & 7, fetchv());
      break;

    case 0xC0: /* group 2 r/m8, imm8 (186+) */
      if (cpu.gen < GEN_186) goto ret_imm;
      decode_modrm();
      if (!rm_probe(8)) break;
      { u32 v = rm_rd8(); FAULT_CHECK(); u32 c = fetch8(); shift_op(cpu.modrm_reg, 8, v, c); }
      break;
    case 0xC1:
      if (cpu.gen < GEN_186) goto ret_near;
      decode_modrm();
      if (!rm_probe(osize_bits())) break;
      { u32 v = rm_rdv(); FAULT_CHECK(); u32 c = fetch8(); shift_op(cpu.modrm_reg, osize_bits(), v, c); }
      break;
    case 0xC2: ret_imm: {
      u32 n = fetch16();
      u32 ip = popv();
      FAULT_CHECK();
      cpu.eip = ip & eip_mask();
      sp_add((s32)n);
      cpu.cycles += 5;
      break;
    }
    case 0xC3: ret_near: {
      u32 ip = popv();
      FAULT_CHECK();
      cpu.eip = ip & eip_mask();
      cpu.cycles += 5;
      break;
    }
    case 0xC4: case 0xC5: { /* LES / LDS */
      decode_modrm();
      if (rm_is_reg()) { cpu_ud(); break; }
      u32 off = rdvs(cpu.ea_seg, cpu.ea);
      u16 sel = rd16s(cpu.ea_seg, cpu.ea + (cpu.osize32 ? 4 : 2));
      FAULT_CHECK();
      load_seg(op == 0xC4 ? SEG_ES : SEG_DS, sel);
      FAULT_CHECK();
      regv_set(cpu.modrm_reg, off);
      break;
    }
    case 0xC6: decode_modrm(); rm_wr8(fetch8()); break;
    case 0xC7: decode_modrm(); rm_wrv(fetchv()); break;
    case 0xC8: { /* ENTER */
      if (cpu.gen < GEN_186) goto retf_imm;
      u32 size = fetch16();
      u32 level = fetch8() & 0x1F;
      pushv(regv_get(REG_BP));
      u32 frame = sp_get();
      if (level > 0) {
        for (u32 i = 1; i < level; i++) {
          u32 bp = cpu.seg[SEG_SS].db ? cpu.r[REG_BP] - i * (cpu.osize32 ? 4 : 2) : (cpu.r[REG_BP] - i * (cpu.osize32 ? 4 : 2)) & 0xFFFF;
          pushv(rdvs(SEG_SS, bp));
        }
        pushv(frame);
      }
      FAULT_CHECK();
      if (cpu.osize32) cpu.r[REG_BP] = frame; else reg16_set(REG_BP, (u16)frame);
      sp_add(-(s32)size);
      break;
    }
    case 0xC9: { /* LEAVE */
      if (cpu.gen < GEN_186) goto retf;
      if (cpu.seg[SEG_SS].db) cpu.r[REG_SP] = cpu.r[REG_BP];
      else reg16_set(REG_SP, reg16_get(REG_BP));
      u32 v = popv();
      FAULT_CHECK();
      regv_set(REG_BP, v);
      break;
    }
    case 0xCA: retf_imm: { u32 n = fetch16(); cpu_far_ret((int)n); break; }
    case 0xCB: retf: cpu_far_ret(0); break;
    case 0xCC: cpu_interrupt(3, 1, 0, 0, cpu.eip); break;
    case 0xCD: { u8 n = fetch8(); cpu_interrupt(n, 1, 0, 0, cpu.eip); break; }
    case 0xCE: if (flag_of()) cpu_interrupt(4, 1, 0, 0, cpu.eip); break;
    case 0xCF: cpu_iret(); break;

    case 0xD0: decode_modrm(); if (!rm_probe(8)) break; { u32 v = rm_rd8(); FAULT_CHECK(); shift_op(cpu.modrm_reg, 8, v, 1); } break;
    case 0xD1: decode_modrm(); if (!rm_probe(osize_bits())) break; { u32 v = rm_rdv(); FAULT_CHECK(); shift_op(cpu.modrm_reg, osize_bits(), v, 1); } break;
    case 0xD2: decode_modrm(); if (!rm_probe(8)) break; { u32 v = rm_rd8(); FAULT_CHECK(); shift_op(cpu.modrm_reg, 8, v, reg8_get(1)); } break;
    case 0xD3: decode_modrm(); if (!rm_probe(osize_bits())) break; { u32 v = rm_rdv(); FAULT_CHECK(); shift_op(cpu.modrm_reg, osize_bits(), v, reg8_get(1)); } break;
    case 0xD4: { /* AAM */
      u32 imm = fetch8();
      if (imm == 0) { raise_fault(EXC_DE, 0, 0); break; }
      u32 al = reg8_get(0);
      reg8_set(4, (u8)(al / imm));
      reg8_set(0, (u8)(al % imm));
      lf_zsp(8, al % imm, 0, 0);
      break;
    }
    case 0xD5: { /* AAD */
      u32 imm = fetch8();
      u32 al = (reg8_get(4) * imm + reg8_get(0)) & 0xFF;
      reg8_set(0, (u8)al);
      reg8_set(4, 0);
      lf_zsp(8, al, 0, 0);
      break;
    }
    case 0xD6: reg8_set(0, flag_cf() ? 0xFF : 0); break; /* SALC */
    case 0xD7: { /* XLAT */
      int s = cpu.seg_override != SEG_NONE ? cpu.seg_override : SEG_DS;
      u32 a = cpu.asize32 ? cpu.r[REG_BX] + reg8_get(0) : ((cpu.r[REG_BX] + reg8_get(0)) & 0xFFFF);
      u32 v = rd8s(s, a);
      FAULT_CHECK();
      reg8_set(0, (u8)v);
      break;
    }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
      /* FPU escape: without a coprocessor the operand is fetched and the instruction is a NOP. */
      extern void fpu_exec(u8 op);
      if (cpu.fpu_present) fpu_exec(op);
      else decode_modrm();
      break;
    }

    case 0xE0: case 0xE1: case 0xE2: { /* LOOPNE / LOOPE / LOOP */
      s32 rel = (s8)fetch8();
      u32 cx = cpu.asize32 ? cpu.r[REG_CX] - 1 : ((cpu.r[REG_CX] - 1) & 0xFFFF);
      if (cpu.asize32) cpu.r[REG_CX] = cx; else reg16_set(REG_CX, (u16)cx);
      int take = cx != 0;
      if (op == 0xE0) take = take && !flag_zf();
      else if (op == 0xE1) take = take && flag_zf();
      if (take) cpu.eip = (cpu.eip + (u32)rel) & eip_mask();
      cpu.cycles += 2;
      break;
    }
    case 0xE3: { /* JCXZ / JECXZ */
      s32 rel = (s8)fetch8();
      u32 cx = cpu.asize32 ? cpu.r[REG_CX] : (cpu.r[REG_CX] & 0xFFFF);
      if (cx == 0) cpu.eip = (cpu.eip + (u32)rel) & eip_mask();
      break;
    }
    case 0xE4: { u16 p = fetch8(); if (!io_allowed(p, 1)) break; reg8_set(0, io_rd8(p)); break; }
    case 0xE5: { u16 p = fetch8(); if (!io_allowed(p, cpu.osize32 ? 4 : 2)) break; regv_set(REG_AX, cpu.osize32 ? io_rd32(p) : io_rd16(p)); break; }
    case 0xE6: { u16 p = fetch8(); if (!io_allowed(p, 1)) break; io_wr8(p, reg8_get(0)); break; }
    case 0xE7: { u16 p = fetch8(); if (!io_allowed(p, cpu.osize32 ? 4 : 2)) break; if (cpu.osize32) io_wr32(p, cpu.r[REG_AX]); else io_wr16(p, reg16_get(REG_AX)); break; }
    case 0xE8: { /* CALL rel */
      u32 rel = fetchv_sx();
      pushv(cpu.eip);
      FAULT_CHECK();
      cpu.eip = (cpu.eip + rel) & eip_mask();
      cpu.cycles += 4;
      break;
    }
    case 0xE9: { u32 rel = fetchv_sx(); cpu.eip = (cpu.eip + rel) & eip_mask(); cpu.cycles += 3; break; }
    case 0xEA: { u32 off = fetchv(); u16 sel = fetch16(); cpu_far_jump(sel, off); break; }
    case 0xEB: { s32 rel = (s8)fetch8(); cpu.eip = (cpu.eip + (u32)rel) & eip_mask(); cpu.cycles += 3; break; }
    case 0xEC: if (!io_allowed(reg16_get(REG_DX), 1)) break; reg8_set(0, io_rd8(reg16_get(REG_DX))); break;
    case 0xED: if (!io_allowed(reg16_get(REG_DX), cpu.osize32 ? 4 : 2)) break; regv_set(REG_AX, cpu.osize32 ? io_rd32(reg16_get(REG_DX)) : io_rd16(reg16_get(REG_DX))); break;
    case 0xEE: if (!io_allowed(reg16_get(REG_DX), 1)) break; io_wr8(reg16_get(REG_DX), reg8_get(0)); break;
    case 0xEF: if (!io_allowed(reg16_get(REG_DX), cpu.osize32 ? 4 : 2)) break; if (cpu.osize32) io_wr32(reg16_get(REG_DX), cpu.r[REG_AX]); else io_wr16(reg16_get(REG_DX), reg16_get(REG_AX)); break;

    case 0xF1: cpu_interrupt(1, 1, 0, 0, cpu.eip); break; /* ICEBP */
    case 0xF4: if (!require_cpl0()) break; cpu.halted = 1; break;
    case 0xF5: set_cf(!flag_cf()); break;
    case 0xF6: decode_modrm(); if ((cpu.modrm_reg == 2 || cpu.modrm_reg == 3) && !rm_probe(8)) break; group3(8); break;
    case 0xF7: decode_modrm(); if ((cpu.modrm_reg == 2 || cpu.modrm_reg == 3) && !rm_probe(osize_bits())) break; group3(osize_bits()); break;
    case 0xF8: set_cf(0); break;
    case 0xF9: set_cf(1); break;
    case 0xFA: if (in_pmode() && (in_v86() ? iopl() < 3 : cpu.cpl > iopl())) { raise_fault(EXC_GP, 1, 0); break; } cpu.eflags &= ~(u32)F_IF; break;
    case 0xFB: if (in_pmode() && (in_v86() ? iopl() < 3 : cpu.cpl > iopl())) { raise_fault(EXC_GP, 1, 0); break; } if (!(cpu.eflags & F_IF)) cpu.inhibit = 1; cpu.eflags |= F_IF; break;
    case 0xFC: cpu.eflags &= ~(u32)F_DF; break;
    case 0xFD: cpu.eflags |= F_DF; break;
    case 0xFE: { /* group 4 */
      decode_modrm();
      if (cpu.modrm_reg <= 1 && !rm_probe(8)) break;
      u32 v = rm_rd8();
      FAULT_CHECK();
      if (cpu.modrm_reg == 0) rm_wr8((u8)alu_inc(8, v));
      else if (cpu.modrm_reg == 1) rm_wr8((u8)alu_dec(8, v));
      else cpu_ud();
      break;
    }
    case 0xFF: { /* group 5 */
      decode_modrm();
      if (cpu.modrm_reg <= 1 && !rm_probe(osize_bits())) break;
      switch (cpu.modrm_reg) {
        case 0: { u32 v = rm_rdv(); FAULT_CHECK(); rm_wrv(alu_inc(osize_bits(), v)); break; }
        case 1: { u32 v = rm_rdv(); FAULT_CHECK(); rm_wrv(alu_dec(osize_bits(), v)); break; }
        case 2: { /* CALL near r/m */
          u32 t = rm_rdv();
          FAULT_CHECK();
          pushv(cpu.eip);
          FAULT_CHECK();
          cpu.eip = t & eip_mask();
          cpu.cycles += 4;
          break;
        }
        case 3: { /* CALL far m16:16/32 */
          if (rm_is_reg()) { cpu_ud(); break; }
          u32 off = rdvs(cpu.ea_seg, cpu.ea);
          u16 sel = rd16s(cpu.ea_seg, cpu.ea + (cpu.osize32 ? 4 : 2));
          FAULT_CHECK();
          cpu_far_call(sel, off);
          break;
        }
        case 4: { u32 t = rm_rdv(); FAULT_CHECK(); cpu.eip = t & eip_mask(); cpu.cycles += 3; break; }
        case 5: { /* JMP far */
          if (rm_is_reg()) { cpu_ud(); break; }
          u32 off = rdvs(cpu.ea_seg, cpu.ea);
          u16 sel = rd16s(cpu.ea_seg, cpu.ea + (cpu.osize32 ? 4 : 2));
          FAULT_CHECK();
          cpu_far_jump(sel, off);
          break;
        }
        case 6: { u32 v = rm_rdv(); FAULT_CHECK(); pushv(v); break; }
        default: cpu_ud(); break;
      }
      break;
    }
    default:
      cpu_ud();
      break;
  }
}

/* ---------------- one instruction ---------------- */
static void cpu_step(void) {
  cpu.eip_start = cpu.eip;
  cpu.osize32 = cpu.seg[SEG_CS].db;
  cpu.asize32 = cpu.seg[SEG_CS].db;
  cpu.seg_override = SEG_NONE;
  cpu.rep = 0;
  cpu.lock = 0;
  u8 op;
  for (;;) {
    op = fetch8();
    switch (op) {
      case 0x26: cpu.seg_override = SEG_ES; continue;
      case 0x2E: cpu.seg_override = SEG_CS; continue;
      case 0x36: cpu.seg_override = SEG_SS; continue;
      case 0x3E: cpu.seg_override = SEG_DS; continue;
      case 0x64: if (cpu.gen >= GEN_386) { cpu.seg_override = SEG_FS; continue; } break;
      case 0x65: if (cpu.gen >= GEN_386) { cpu.seg_override = SEG_GS; continue; } break;
      case 0x66: if (cpu.gen >= GEN_386) { cpu.osize32 = !cpu.seg[SEG_CS].db; continue; } break;
      case 0x67: if (cpu.gen >= GEN_386) { cpu.asize32 = !cpu.seg[SEG_CS].db; continue; } break;
      case 0xF0: cpu.lock = 1; continue;
      case 0xF2: case 0xF3: cpu.rep = op; continue;
      default: break;
    }
    break;
  }
  exec_primary(op);
  cpu.cycles += 1;
  cpu.insn_count++;
  if (UNLIKELY(cpu.fault_pending)) cpu_deliver_fault();
}

u64 cpu_run(u64 target_cycles) {
  u64 start = cpu.cycles;
  while (cpu.cycles < target_cycles && !cpu.fatal) {
    if (UNLIKELY(cpu.halted)) break;
    int can_int = !cpu.inhibit;
    cpu.inhibit = 0;
    if (can_int && (cpu.eflags & F_IF) && pic_has_pending()) {
      u8 vec = pic_ack();
      cpu_hw_interrupt(vec);
      { extern int cpu_trace_faults; extern s64 emu_now_ns(void); static u32 nreal, npm; static s64 lastns;
        if (UNLIKELY(cpu_trace_faults >= 4) && vec == 8) {
          static u32 total;
          total++;
          if (total == 1000 || total == 8000) { /* control burst (working) + menu burst (broken) */
            itrace_burst = 60;
            dm_log("ITRACE burst armed at irq0 #%d", (int)total);
          }
          if (itrace_burst > 0) { itrace_burst--; itrace_left = 250; dm_log("ITRACE tick %d", 60 - itrace_burst); }
        }
        if (UNLIKELY(cpu_trace_faults >= 1) && vec == 8) {
          if (cpu.cr0 & 1) {
            npm++;
            static int once;
            if (!once) { once = 1; dm_log("HWINT8 pm target cs=%x base=%x eip=%x lin=%x", cpu.seg[SEG_CS].sel, cpu.seg[SEG_CS].base, cpu.eip, cpu.seg[SEG_CS].base + cpu.eip); }
          } else nreal++;
          s64 now = emu_now_ns();
          if (now - lastns >= 1000000000LL) {
            dm_log("HWINT8 rate real=%d pm=%d entries=%d per %dms", (int)nreal, (int)npm, (int)itrace_entry_count, (int)((now - lastns) / 1000000));
            nreal = npm = 0;
            itrace_entry_count = 0;
            lastns = now;
          }
        } }
      continue;
    }
    int tf = (cpu.eflags & F_TF) != 0;
    if (UNLIKELY(cpu_trace_faults >= 4) && cpu.seg[SEG_CS].base + cpu.eip == 0x1F84C4) itrace_entry_count++;
    if (UNLIKELY(itrace_left > 0)) {
      itrace_left--;
      u32 lin = cpu.seg[SEG_CS].base + cpu.eip;
      dm_log("T %04x:%08x %02x %02x %02x %02x %02x %02x", cpu.seg[SEG_CS].sel, cpu.eip,
             lin_rd8(lin), lin_rd8(lin + 1), lin_rd8(lin + 2), lin_rd8(lin + 3), lin_rd8(lin + 4), lin_rd8(lin + 5));
    }
    cpu_step();
    if (UNLIKELY(tf) && !cpu.fault_pending && !cpu.halted && !cpu.in_fault_delivery) cpu_interrupt(1, 0, 0, 0, cpu.eip);
  }
  u64 done = cpu.cycles - start;
  cpu.tsc += done;
  return done;
}

void cpu_hw_interrupt(u8 vector) {
  cpu.halted = 0;
  cpu_interrupt(vector, 0, 0, 0, cpu.eip);
  cpu.cycles += 30;
}

void cpu_sw_interrupt(u8 vector) { cpu_interrupt(vector, 1, 0, 0, cpu.eip); }

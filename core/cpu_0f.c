/* Two-byte (0F xx) opcode map: 286/386/486/Pentium system and extended instructions. */
#include "cpu_int.h"
#include "bios.h"

static void bit_op(int kind, u32 bitoff, int from_reg) {
  int bits = osize_bits();
  u32 v;
  if (!rm_is_reg() && from_reg) {
    /* register bit offsets index beyond the addressed unit */
    s32 off = bits == 32 ? (s32)bitoff : (s32)(s16)bitoff;
    s32 unit = off >> (bits == 32 ? 5 : 4);
    cpu.ea = cpu.asize32 ? cpu.ea + (u32)(unit * (bits / 8)) : ((cpu.ea + (u32)(unit * (bits / 8))) & 0xFFFF);
  }
  u32 bit = bitoff & (u32)(bits - 1);
  v = rm_rdv();
  FAULT_CHECK();
  int cf = (v >> bit) & 1;
  set_cf(cf);
  switch (kind) {
    case 1: v |= 1u << bit; rm_wrv(v); break;  /* BTS */
    case 2: v &= ~(1u << bit); rm_wrv(v); break; /* BTR */
    case 3: v ^= 1u << bit; rm_wrv(v); break;  /* BTC */
    default: break;
  }
}

static void cpuid(void) {
  u32 leaf = cpu.r[REG_AX];
  switch (leaf) {
    case 0:
      cpu.r[REG_AX] = 1;
      cpu.r[REG_BX] = 0x4D534F44; /* "DOSM" */
      cpu.r[REG_DX] = 0x6C69626F; /* "obil" */
      cpu.r[REG_CX] = 0x55504365; /* "eCPU" */
      break;
    case 1: {
      u32 family = cpu.gen == GEN_486 ? 4 : cpu.gen == GEN_P5 ? 5 : 6;
      u32 model = cpu.gen == GEN_486 ? 3 : cpu.gen == GEN_P5 ? 2 : 5;
      cpu.r[REG_AX] = (family << 8) | (model << 4) | 1;
      cpu.r[REG_BX] = 0;
      cpu.r[REG_CX] = 0;
      u32 f = cpu.fpu_present ? 1 : 0;
      if (cpu.gen >= GEN_P5) f |= (1 << 4) | (1 << 8); /* TSC, CX8 */
      if (cpu.gen >= GEN_P6) f |= 1 << 15;             /* CMOV */
      cpu.r[REG_DX] = f;
      break;
    }
    default:
      cpu.r[REG_AX] = cpu.r[REG_BX] = cpu.r[REG_CX] = cpu.r[REG_DX] = 0;
      break;
  }
}

static void shld_shrd(int left, u32 count) {
  int bits = osize_bits();
  u32 m = size_mask(bits);
  count &= 0x1F;
  u32 dst = rm_rdv() & m;
  FAULT_CHECK();
  if (count == 0) return;
  u32 src = regv_get(cpu.modrm_reg) & m;
  u32 res, cf, of;
  if (count > (u32)bits) { /* undefined on 16-bit; keep it well-defined here */
    res = dst; cf = 0; of = 0;
  } else if (left) {
    res = ((dst << count) | (count == (u32)bits ? src : src >> (bits - count))) & m;
    cf = (dst >> (bits - count)) & 1;
    of = ((res ^ dst) >> (bits - 1)) & 1;
  } else {
    res = ((dst >> count) | (count == (u32)bits ? src : src << (bits - count))) & m;
    cf = (dst >> (count - 1)) & 1;
    of = ((res ^ dst) >> (bits - 1)) & 1;
  }
  lf_zsp(bits, res, (int)cf, count == 1 ? (int)of : flag_of());
  rm_wrv(res);
}

void cpu_exec_0f(void) {
  u8 op = fetch8();
  switch (op) {
    case 0x00: /* group 6 (pmode only) */
      if (!in_pmode() || in_v86()) { cpu_ud(); return; }
      dm_log("CPU: group 6 op not implemented");
      cpu.fatal = 1;
      return;
    case 0x01: { /* group 7 */
      decode_modrm();
      switch (cpu.modrm_reg) {
        case 0: case 1: { /* SGDT / SIDT */
          if (rm_is_reg()) { cpu_ud(); return; }
          u32 base = cpu.modrm_reg == 0 ? cpu.gdtr.base : cpu.idtr.base;
          u32 limit = cpu.modrm_reg == 0 ? cpu.gdtr.limit : cpu.idtr.limit;
          wr16s(cpu.ea_seg, cpu.ea, (u16)limit);
          wr32s(cpu.ea_seg, cpu.ea + 2, cpu.osize32 ? base : (base & 0x00FFFFFF));
          return;
        }
        case 2: case 3: { /* LGDT / LIDT */
          if (rm_is_reg()) { cpu_ud(); return; }
          u32 limit = rd16s(cpu.ea_seg, cpu.ea);
          u32 base = rd32s(cpu.ea_seg, cpu.ea + 2);
          FAULT_CHECK();
          if (!cpu.osize32) base &= 0x00FFFFFF;
          if (cpu.modrm_reg == 2) { cpu.gdtr.base = base; cpu.gdtr.limit = limit; }
          else { cpu.idtr.base = base; cpu.idtr.limit = limit; }
          return;
        }
        case 4: /* SMSW */
          if (rm_is_reg()) regv_set(cpu.modrm_rm, cpu.cr0 & 0xFFFF);
          else rm_wr16((u16)cpu.cr0);
          return;
        case 6: { /* LMSW */
          u32 v = rm_rd16();
          FAULT_CHECK();
          u32 ncr0 = (cpu.cr0 & ~0xEu) | (v & 0xF) | (cpu.cr0 & 1);
          if ((v & 1) && !(cpu.cr0 & 1)) dm_log("CPU: LMSW set PE (protected mode requested)");
          cpu.cr0 = ncr0 | (v & 1);
          return;
        }
        case 7: /* INVLPG */
          if (rm_is_reg() || cpu.gen < GEN_486) { cpu_ud(); return; }
          return;
        default: cpu_ud(); return;
      }
    }
    case 0x02: case 0x03: /* LAR / LSL */
      if (!in_pmode() || in_v86()) { cpu_ud(); return; }
      dm_log("CPU: LAR/LSL not implemented");
      cpu.fatal = 1;
      return;
    case 0x06: cpu.cr0 &= ~8u; return; /* CLTS */
    case 0x08: case 0x09: if (cpu.gen < GEN_486) cpu_ud(); return; /* INVD / WBINVD */
    case 0x0B: cpu_ud(); return;
    case 0x20: { /* MOV r32, CRn */
      if (cpu.gen < GEN_386) { cpu_ud(); return; }
      decode_modrm();
      u32 v = cpu.modrm_reg == 0 ? cpu.cr0 : cpu.modrm_reg == 2 ? cpu.cr2 : cpu.modrm_reg == 3 ? cpu.cr3 : cpu.modrm_reg == 4 ? cpu.cr4 : 0;
      cpu.r[cpu.modrm_rm] = v;
      return;
    }
    case 0x21: { if (cpu.gen < GEN_386) { cpu_ud(); return; } decode_modrm(); cpu.r[cpu.modrm_rm] = cpu.dr[cpu.modrm_reg]; return; }
    case 0x22: { /* MOV CRn, r32 */
      if (cpu.gen < GEN_386) { cpu_ud(); return; }
      decode_modrm();
      u32 v = cpu.r[cpu.modrm_rm];
      switch (cpu.modrm_reg) {
        case 0:
          if ((v & 1) != (cpu.cr0 & 1)) dm_log("CPU: CR0.PE -> %d", v & 1);
          if ((v & 0x80000000u) != (cpu.cr0 & 0x80000000u)) dm_log("CPU: CR0.PG -> %d", v >> 31);
          cpu.cr0 = v;
          break;
        case 2: cpu.cr2 = v; break;
        case 3: cpu.cr3 = v; break;
        case 4: cpu.cr4 = v; break;
        default: cpu_ud(); break;
      }
      return;
    }
    case 0x23: { if (cpu.gen < GEN_386) { cpu_ud(); return; } decode_modrm(); cpu.dr[cpu.modrm_reg] = cpu.r[cpu.modrm_rm]; return; }
    case 0x30: if (cpu.gen < GEN_P5) { cpu_ud(); return; } return; /* WRMSR: ignored */
    case 0x31: /* RDTSC */
      if (cpu.gen < GEN_P5) { cpu_ud(); return; }
      cpu.r[REG_AX] = (u32)cpu.tsc;
      cpu.r[REG_DX] = (u32)(cpu.tsc >> 32);
      return;
    case 0x32: if (cpu.gen < GEN_P5) { cpu_ud(); return; } cpu.r[REG_AX] = cpu.r[REG_DX] = 0; return;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
      if (cpu.gen < GEN_P6) { cpu_ud(); return; }
      decode_modrm();
      u32 v = rm_rdv();
      FAULT_CHECK();
      if (cond_true(op & 0xF)) regv_set(cpu.modrm_reg, v);
      return;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
      if (cpu.gen < GEN_386) { cpu_ud(); return; }
      u32 rel = fetchv_sx();
      if (cond_true(op & 0xF)) { cpu.eip = (cpu.eip + rel) & eip_mask(); cpu.cycles += 3; }
      return;
    }
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
      if (cpu.gen < GEN_386) { cpu_ud(); return; }
      decode_modrm();
      rm_wr8(cond_true(op & 0xF) ? 1 : 0);
      return;
    case 0xA0: if (cpu.gen < GEN_386) { cpu_ud(); return; } pushv(cpu.seg[SEG_FS].sel); return;
    case 0xA1: { if (cpu.gen < GEN_386) { cpu_ud(); return; } u32 v = popv(); FAULT_CHECK(); load_seg_real(SEG_FS, (u16)v); return; }
    case 0xA8: if (cpu.gen < GEN_386) { cpu_ud(); return; } pushv(cpu.seg[SEG_GS].sel); return;
    case 0xA9: { if (cpu.gen < GEN_386) { cpu_ud(); return; } u32 v = popv(); FAULT_CHECK(); load_seg_real(SEG_GS, (u16)v); return; }
    case 0xA2: if (cpu.gen < GEN_486) { cpu_ud(); return; } cpuid(); return;
    case 0xA3: if (cpu.gen < GEN_386) { cpu_ud(); return; } decode_modrm(); bit_op(0, regv_get(cpu.modrm_reg), 1); return;
    case 0xAB: if (cpu.gen < GEN_386) { cpu_ud(); return; } decode_modrm(); bit_op(1, regv_get(cpu.modrm_reg), 1); return;
    case 0xB3: if (cpu.gen < GEN_386) { cpu_ud(); return; } decode_modrm(); bit_op(2, regv_get(cpu.modrm_reg), 1); return;
    case 0xBB: if (cpu.gen < GEN_386) { cpu_ud(); return; } decode_modrm(); bit_op(3, regv_get(cpu.modrm_reg), 1); return;
    case 0xBA: { /* group 8 */
      if (cpu.gen < GEN_386) { cpu_ud(); return; }
      decode_modrm();
      if (cpu.modrm_reg < 4) { cpu_ud(); return; }
      u32 imm = fetch8();
      bit_op(cpu.modrm_reg - 4, imm, 0);
      return;
    }
    case 0xA4: if (cpu.gen < GEN_386) { cpu_ud(); return; } decode_modrm(); { u32 c = fetch8(); shld_shrd(1, c); } return;
    case 0xA5: if (cpu.gen < GEN_386) { cpu_ud(); return; } decode_modrm(); shld_shrd(1, reg8_get(1)); return;
    case 0xAC: if (cpu.gen < GEN_386) { cpu_ud(); return; } decode_modrm(); { u32 c = fetch8(); shld_shrd(0, c); } return;
    case 0xAD: if (cpu.gen < GEN_386) { cpu_ud(); return; } decode_modrm(); shld_shrd(0, reg8_get(1)); return;
    case 0xAF: { /* IMUL r, r/m */
      if (cpu.gen < GEN_386) { cpu_ud(); return; }
      decode_modrm();
      u32 v = rm_rdv();
      FAULT_CHECK();
      if (cpu.osize32) {
        s64 r = (s64)(s32)cpu.r[cpu.modrm_reg] * (s64)(s32)v;
        cpu.r[cpu.modrm_reg] = (u32)r;
        int ov = r != (s64)(s32)r;
        lf_zsp(32, (u32)r, ov, ov);
      } else {
        s32 r = (s32)(s16)reg16_get(cpu.modrm_reg) * (s32)(s16)v;
        reg16_set(cpu.modrm_reg, (u16)r);
        int ov = r != (s32)(s16)r;
        lf_zsp(16, (u32)r & 0xFFFF, ov, ov);
      }
      cpu.cycles += 12;
      return;
    }
    case 0xB0: { /* CMPXCHG r/m8, r8 */
      if (cpu.gen < GEN_486) { cpu_ud(); return; }
      decode_modrm();
      u32 v = rm_rd8();
      FAULT_CHECK();
      u32 acc = reg8_get(0);
      alu_op(7, 8, acc, v);
      if (acc == v) rm_wr8(reg8_get(cpu.modrm_reg));
      else reg8_set(0, (u8)v);
      return;
    }
    case 0xB1: {
      if (cpu.gen < GEN_486) { cpu_ud(); return; }
      decode_modrm();
      u32 v = rm_rdv();
      FAULT_CHECK();
      u32 acc = regv_get(REG_AX);
      alu_op(7, osize_bits(), acc, v);
      if (acc == v) rm_wrv(regv_get(cpu.modrm_reg));
      else regv_set(REG_AX, v);
      return;
    }
    case 0xB2: case 0xB4: case 0xB5: { /* LSS / LFS / LGS */
      if (cpu.gen < GEN_386) { cpu_ud(); return; }
      decode_modrm();
      if (rm_is_reg()) { cpu_ud(); return; }
      u32 off = rdvs(cpu.ea_seg, cpu.ea);
      u16 sel = rd16s(cpu.ea_seg, cpu.ea + (cpu.osize32 ? 4 : 2));
      FAULT_CHECK();
      int s = op == 0xB2 ? SEG_SS : op == 0xB4 ? SEG_FS : SEG_GS;
      load_seg_real(s, sel);
      FAULT_CHECK();
      regv_set(cpu.modrm_reg, off);
      if (s == SEG_SS) cpu.inhibit = 1;
      return;
    }
    case 0xB6: { if (cpu.gen < GEN_386) { cpu_ud(); return; } decode_modrm(); u32 v = rm_rd8(); FAULT_CHECK(); regv_set(cpu.modrm_reg, v); return; }
    case 0xB7: { if (cpu.gen < GEN_386) { cpu_ud(); return; } decode_modrm(); u32 v = rm_rd16(); FAULT_CHECK(); regv_set(cpu.modrm_reg, v); return; }
    case 0xBE: { if (cpu.gen < GEN_386) { cpu_ud(); return; } decode_modrm(); u32 v = rm_rd8(); FAULT_CHECK(); regv_set(cpu.modrm_reg, (u32)(s32)(s8)v); return; }
    case 0xBF: { if (cpu.gen < GEN_386) { cpu_ud(); return; } decode_modrm(); u32 v = rm_rd16(); FAULT_CHECK(); regv_set(cpu.modrm_reg, (u32)(s32)(s16)v); return; }
    case 0xBC: case 0xBD: { /* BSF / BSR */
      if (cpu.gen < GEN_386) { cpu_ud(); return; }
      decode_modrm();
      u32 v = rm_rdv();
      FAULT_CHECK();
      flags_sync();
      if (v == 0) { cpu.eflags |= F_ZF; return; }
      cpu.eflags &= ~(u32)F_ZF;
      u32 idx = op == 0xBC ? (u32)__builtin_ctz(v) : 31u - (u32)__builtin_clz(v);
      regv_set(cpu.modrm_reg, idx);
      return;
    }
    case 0xC0: { /* XADD r/m8, r8 */
      if (cpu.gen < GEN_486) { cpu_ud(); return; }
      decode_modrm();
      u32 d = rm_rd8();
      FAULT_CHECK();
      u32 s = reg8_get(cpu.modrm_reg);
      u32 r = alu_op(0, 8, d, s);
      reg8_set(cpu.modrm_reg, (u8)d);
      rm_wr8((u8)r);
      return;
    }
    case 0xC1: {
      if (cpu.gen < GEN_486) { cpu_ud(); return; }
      decode_modrm();
      u32 d = rm_rdv();
      FAULT_CHECK();
      u32 s = regv_get(cpu.modrm_reg);
      u32 r = alu_op(0, osize_bits(), d, s);
      regv_set(cpu.modrm_reg, d);
      rm_wrv(r);
      return;
    }
    case 0xC7: { /* CMPXCHG8B */
      if (cpu.gen < GEN_P5) { cpu_ud(); return; }
      decode_modrm();
      if (rm_is_reg() || cpu.modrm_reg != 1) { cpu_ud(); return; }
      u32 lo = rd32s(cpu.ea_seg, cpu.ea), hi = rd32s(cpu.ea_seg, cpu.ea + 4);
      FAULT_CHECK();
      flags_sync();
      if (lo == cpu.r[REG_AX] && hi == cpu.r[REG_DX]) {
        wr32s(cpu.ea_seg, cpu.ea, cpu.r[REG_BX]);
        wr32s(cpu.ea_seg, cpu.ea + 4, cpu.r[REG_CX]);
        cpu.eflags |= F_ZF;
      } else {
        cpu.r[REG_AX] = lo;
        cpu.r[REG_DX] = hi;
        cpu.eflags &= ~(u32)F_ZF;
      }
      return;
    }
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
      if (cpu.gen < GEN_486) { cpu_ud(); return; }
      u32 v = cpu.r[op & 7];
      cpu.r[op & 7] = __builtin_bswap32(v);
      return;
    }
    case 0xFF: { /* DOS Mobile HLE hook: only valid from the BIOS ROM segment */
      if (cpu.seg[SEG_CS].base == BIOS_ROM_BASE) {
        u8 fn = fetch8();
        bios_hle(fn);
        return;
      }
      cpu_ud();
      return;
    }
    default:
      cpu_ud();
      return;
  }
}

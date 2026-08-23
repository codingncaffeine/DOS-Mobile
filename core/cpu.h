/* x86 CPU core — 8086 through P6 instruction set, real/protected/V86 modes.
 * P0 implements the real-mode machine completely (8086/186 ISA plus the 386 32-bit
 * operand/address forms); descriptor-based protected mode and paging arrive in P2. */
#pragma once
#include "platform.h"

enum { REG_AX, REG_CX, REG_DX, REG_BX, REG_SP, REG_BP, REG_SI, REG_DI };
enum { SEG_ES, SEG_CS, SEG_SS, SEG_DS, SEG_FS, SEG_GS, SEG_COUNT, SEG_NONE = 0xFF };

/* CPU generations — gate the ISA, CPUID identity and the instruction cost table. */
enum { GEN_8088 = 0, GEN_8086, GEN_186, GEN_286, GEN_386, GEN_486, GEN_P5, GEN_P6 };

/* EFLAGS bits */
enum {
  F_CF = 1 << 0, F_PF = 1 << 2, F_AF = 1 << 4, F_ZF = 1 << 6, F_SF = 1 << 7, F_TF = 1 << 8,
  F_IF = 1 << 9, F_DF = 1 << 10, F_OF = 1 << 11, F_IOPL = 3 << 12, F_NT = 1 << 14,
  F_RF = 1 << 16, F_VM = 1 << 17, F_AC = 1 << 18, F_VIF = 1 << 19, F_VIP = 1 << 20, F_ID = 1 << 21,
  F_ARITH = F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF,
};

/* Exception vectors */
enum { EXC_DE = 0, EXC_DB = 1, EXC_NMI = 2, EXC_BP = 3, EXC_OF = 4, EXC_BR = 5, EXC_UD = 6, EXC_NM = 7,
       EXC_DF = 8, EXC_TS = 10, EXC_NP = 11, EXC_SS = 12, EXC_GP = 13, EXC_PF = 14, EXC_MF = 16 };

/* Segment cache flags */
enum { SEGF_READ = 1, SEGF_WRITE = 2, SEGF_CODE = 4, SEGF_EXPDOWN = 8, SEGF_CONFORMING = 16 };

typedef struct {
  u16 sel;
  u32 base;
  u32 limit;  /* byte-granular effective limit (for expand-down: the lower bound) */
  u8 access;  /* descriptor access byte: P DPL S Type */
  u8 flags;   /* SEGF_* */
  u8 db;      /* default operand/address size: 1 = 32-bit (D/B bit) */
  u8 valid;   /* 0 = null selector loaded (any access faults) */
  u8 dpl;
} Seg;

/* Lazy flag record: the last arithmetic result + operands; flags are derived on demand. */
enum { LF_NONE = 0, LF_ADD, LF_ADC, LF_SUB, LF_SBB, LF_LOGIC, LF_INC, LF_DEC, LF_NEG, LF_ZSP };

typedef struct CPU {
  u32 r[8]; /* EAX ECX EDX EBX ESP EBP ESI EDI */
  u32 eip;
  u32 eflags; /* authoritative for everything outside the lazy arithmetic flags */
  Seg seg[SEG_COUNT];

  u32 lf_res, lf_a, lf_b, lf_cin;
  u8 lf_type, lf_bits; /* bits = 8/16/32 */

  u32 cr0, cr2, cr3, cr4;
  u32 dr[8];
  struct { u32 base; u32 limit; } gdtr, idtr;
  Seg ldtr, tr;  /* ldtr/tr: base/limit/access of the loaded LDT / TSS */
  u8 tss_is32;

  /* instruction-fetch page cache (linear page → host pointer), invalidated with the TLB */
  u32 fetch_page_lin;
  const u8 *fetch_page_ptr;

  u8 gen;         /* GEN_* */
  u8 fpu_present;
  u8 halted;
  u8 inhibit;     /* interrupt shadow after STI / MOV SS / POP SS */
  u8 cpl;
  u8 a20_enabled;
  u8 fatal;       /* machine stopped on an unrecoverable condition */

  u64 cycles;     /* instruction cost accumulator; the scheduler converts to time */
  u64 tsc;

  /* per-instruction decode state */
  u32 eip_start;
  u8 osize32, asize32;
  u8 seg_override;  /* SEG_* or SEG_NONE */
  u8 rep;           /* 0, 0xF2, 0xF3 */
  u8 lock;
  u8 modrm_mod, modrm_reg, modrm_rm;
  u32 ea;          /* effective address (offset, not linear) when mod != 3 */
  u8 ea_seg;       /* segment used for the memory operand */

  /* pending fault raised by a helper; delivered at the end of the step */
  u8 fault_pending;
  u8 fault_vec;
  u8 fault_has_err;
  u32 fault_err;
  u32 fault_cr2;
  u8 in_fault_delivery; /* nested fault detection → #DF / triple fault */

  /* statistics */
  u64 insn_count;
} CPU;

extern CPU cpu;

void cpu_init(int gen, int fpu);
void cpu_reset(void);
/* Run until cpu.cycles >= target_cycles, a halt, or a fatal error. Returns executed cycles. */
u64 cpu_run(u64 target_cycles);
void cpu_hw_interrupt(u8 vector);
int cpu_interrupts_enabled(void);

/* Service for the HLE BIOS: raise a software interrupt as if INT n executed at the current EIP. */
void cpu_sw_interrupt(u8 vector);
/* Load a segment register in the current mode (real mode: base = sel << 4). */
void cpu_load_seg(int s, u16 sel);
void cpu_set_eflags(u32 v);
u32 cpu_get_eflags(void);
void cpu_flags_sync(void);

/* Cost model hooks (P7 refines per generation). */
void cpu_set_generation(int gen);
const char *cpu_gen_name(int gen);

/* HLE hook: executed when the CPU hits 0F FF imm8 while CS points at the BIOS ROM. */
void bios_hle(u8 fn);

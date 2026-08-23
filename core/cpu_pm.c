/* Segment loading, descriptors, gates, interrupts/exceptions, task switching and far control
 * transfers for real mode, virtual-8086 mode and protected mode. */
#include "cpu_int.h"

/* ---------------- descriptors ---------------- */
typedef struct {
  u32 base, limit;
  u8 access, flags; /* access byte; flags = G D/B L AVL */
  u32 lo, hi;
  u32 addr;         /* linear address of the descriptor */
} Desc;

#define DESC_P(d) ((d)->access & 0x80)
#define DESC_DPL(d) (((d)->access >> 5) & 3)
#define DESC_S(d) ((d)->access & 0x10)
#define DESC_TYPE(d) ((d)->access & 0x0F)
#define DESC_CODE(d) (DESC_S(d) && ((d)->access & 0x08))
#define DESC_DATA(d) (DESC_S(d) && !((d)->access & 0x08))
#define DESC_CONFORMING(d) (DESC_CODE(d) && ((d)->access & 0x04))
#define DESC_READABLE_CODE(d) (DESC_CODE(d) && ((d)->access & 0x02))
#define DESC_WRITABLE(d) (DESC_DATA(d) && ((d)->access & 0x02))
#define DESC_EXPDOWN(d) (DESC_DATA(d) && ((d)->access & 0x04))

enum { ST_TSS16_AVAIL = 1, ST_LDT = 2, ST_TSS16_BUSY = 3, ST_CALLGATE16 = 4, ST_TASKGATE = 5, ST_INTGATE16 = 6,
       ST_TRAPGATE16 = 7, ST_TSS32_AVAIL = 9, ST_TSS32_BUSY = 11, ST_CALLGATE32 = 12, ST_INTGATE32 = 14, ST_TRAPGATE32 = 15 };

static void decode_desc(Desc *d, u32 lo, u32 hi, u32 addr) {
  d->lo = lo; d->hi = hi; d->addr = addr;
  d->base = (lo >> 16) | ((hi & 0xFF) << 16) | (hi & 0xFF000000u);
  d->limit = (lo & 0xFFFF) | (hi & 0x000F0000u);
  if (hi & 0x00800000u) d->limit = (d->limit << 12) | 0xFFF;
  d->access = (u8)(hi >> 8);
  d->flags = (u8)((hi >> 20) & 0xF);
}

/* Read a descriptor by selector. On error raises fault `vec` with the selector error code. */
static int read_desc(u16 sel, Desc *d, u8 vec) {
  u32 base, limit;
  if (sel & 4) {
    if (!cpu.ldtr.valid) { raise_fault(vec, 1, sel & 0xFFFC); return 0; }
    base = cpu.ldtr.base; limit = cpu.ldtr.limit;
  } else { base = cpu.gdtr.base; limit = cpu.gdtr.limit; }
  u32 idx = sel & 0xFFF8;
  if (idx + 7 > limit) { raise_fault(vec, 1, sel & 0xFFFC); return 0; }
  u32 lo = lin_rd32(base + idx);
  u32 hi = lin_rd32(base + idx + 4);
  if (cpu.fault_pending) return 0;
  decode_desc(d, lo, hi, base + idx);
  return 1;
}

static void mark_accessed(Desc *d) {
  if (!(d->hi & 0x100)) { d->hi |= 0x100; lin_wr32(d->addr + 4, d->hi); }
}

static void commit_seg(int s, u16 sel, const Desc *d) {
  Seg *sg = &cpu.seg[s];
  sg->sel = sel;
  sg->base = d->base;
  sg->limit = d->limit;
  sg->access = d->access;
  sg->dpl = (u8)DESC_DPL(d);
  sg->db = (d->flags & 4) ? 1 : 0;
  sg->valid = 1;
  u8 f = 0;
  if (DESC_CODE(d)) { f |= SEGF_CODE; if (d->access & 2) f |= SEGF_READ; if (d->access & 4) f |= SEGF_CONFORMING; }
  else { f |= SEGF_READ; if (d->access & 2) f |= SEGF_WRITE; if (d->access & 4) f |= SEGF_EXPDOWN; }
  sg->flags = f;
}

/* CPL follows CS loads in protected mode (load_cs); mode switches start at ring 0. */
void cpu_update_cpl(void) {
  if (in_v86()) cpu.cpl = 3;
  else if (!in_pmode()) cpu.cpl = 0;
}

/* ---------------- real / V86 mode segment loads ---------------- */
void load_seg_real(int s, u16 sel) {
  Seg *sg = &cpu.seg[s];
  sg->sel = sel;
  sg->base = (u32)sel << 4;
  sg->valid = 1;
  sg->flags = (u8)(SEGF_READ | SEGF_WRITE | (s == SEG_CS ? SEGF_CODE : 0));
  sg->access = s == SEG_CS ? 0x9B : 0x93;
  sg->dpl = 0;
  if (in_v86()) { sg->limit = 0xFFFF; sg->db = 0; }
  /* real mode keeps limit and D bit from the last descriptor (unreal mode) */
  if (s == SEG_CS) { cpu_update_cpl(); cpu.fetch_page_lin = 0xFFFFFFFFu; }
}

/* ---------------- protected-mode data segment loads (ES/DS/FS/GS/SS) ---------------- */
static void load_seg_prot(int s, u16 sel) {
  if ((sel & 0xFFFC) == 0) {
    if (s == SEG_SS) { raise_fault(EXC_GP, 1, 0); return; }
    cpu.seg[s].sel = sel; cpu.seg[s].valid = 0; cpu.seg[s].base = 0; cpu.seg[s].limit = 0; cpu.seg[s].flags = 0;
    return;
  }
  Desc d;
  if (!read_desc(sel, &d, EXC_GP)) return;
  int rpl = sel & 3, dpl = DESC_DPL(&d);
  if (!DESC_S(&d)) { raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
  if (s == SEG_SS) {
    if (!DESC_WRITABLE(&d) || rpl != cpu.cpl || dpl != cpu.cpl) { raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
    if (!DESC_P(&d)) { raise_fault(EXC_SS, 1, sel & 0xFFFC); return; }
  } else {
    if (DESC_CODE(&d) && !(d.access & 2)) { raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
    if (!DESC_CONFORMING(&d) && (rpl > dpl || cpu.cpl > dpl)) { raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
    if (!DESC_P(&d)) { raise_fault(EXC_NP, 1, sel & 0xFFFC); return; }
  }
  mark_accessed(&d);
  commit_seg(s, sel, &d);
}

void load_seg(int s, u16 sel) {
  if (in_prot()) load_seg_prot(s, sel);
  else load_seg_real(s, sel);
}

/* Load CS from a code descriptor already validated; cpl becomes `cpl`. */
static void load_cs(u16 sel, const Desc *d, int cpl) {
  commit_seg(SEG_CS, (u16)((sel & 0xFFFC) | cpl), d);
  cpu.cpl = (u8)cpl;
  cpu.fetch_page_lin = 0xFFFFFFFFu;
}

/* After a privilege change, data segments that are no longer accessible are nulled. */
static void validate_data_segs(void) {
  static const int segs[4] = {SEG_ES, SEG_DS, SEG_FS, SEG_GS};
  for (int i = 0; i < 4; i++) {
    Seg *sg = &cpu.seg[segs[i]];
    if (!sg->valid) continue;
    int conforming_code = (sg->flags & SEGF_CODE) && (sg->flags & SEGF_CONFORMING);
    if (!conforming_code && sg->dpl < cpu.cpl) { sg->valid = 0; sg->sel = 0; }
  }
}

/* ---------------- TSS ---------------- */
static int tss_stack(int level, u16 *ss, u32 *esp) {
  u32 base = cpu.tr.base, limit = cpu.tr.limit;
  if (cpu.tss_is32) {
    u32 off = 4 + (u32)level * 8;
    if (off + 7 > limit) { raise_fault(EXC_TS, 1, cpu.tr.sel & 0xFFFC); return 0; }
    *esp = lin_rd32(base + off);
    *ss = lin_rd16(base + off + 4);
  } else {
    u32 off = 2 + (u32)level * 4;
    if (off + 3 > limit) { raise_fault(EXC_TS, 1, cpu.tr.sel & 0xFFFC); return 0; }
    *esp = lin_rd16(base + off);
    *ss = lin_rd16(base + off + 2);
  }
  return !cpu.fault_pending;
}

int io_allowed_slow(u16 port, int size) {
  if (!cpu.tr.valid || !cpu.tss_is32) { raise_fault(EXC_GP, 1, 0); return 0; }
  u32 limit = cpu.tr.limit;
  if (0x67 > limit) { raise_fault(EXC_GP, 1, 0); return 0; }
  u32 map = lin_rd16(cpu.tr.base + 0x66);
  for (int i = 0; i < size; i++) {
    u32 p = (u32)port + (u32)i;
    u32 byte = map + (p >> 3);
    if (byte > limit) { raise_fault(EXC_GP, 1, 0); return 0; }
    u8 b = lin_rd8(cpu.tr.base + byte);
    if (b & (1 << (p & 7))) { raise_fault(EXC_GP, 1, 0); return 0; }
  }
  return 1;
}

/* Load SS:ESP for a stack switch (descriptor must be a writable data segment at `cpl`). */
static int load_stack(u16 ss, u32 esp, int cpl) {
  Desc d;
  if ((ss & 0xFFFC) == 0) { raise_fault(EXC_TS, 1, ss & 0xFFFC); return 0; }
  if (!read_desc(ss, &d, EXC_TS)) return 0;
  if ((ss & 3) != cpl || DESC_DPL(&d) != cpl || !DESC_WRITABLE(&d)) { raise_fault(EXC_TS, 1, ss & 0xFFFC); return 0; }
  if (!DESC_P(&d)) { raise_fault(EXC_SS, 1, ss & 0xFFFC); return 0; }
  mark_accessed(&d);
  commit_seg(SEG_SS, ss, &d);
  if (cpu.seg[SEG_SS].db) cpu.r[REG_SP] = esp; else reg16_set(REG_SP, (u16)esp);
  return 1;
}

/* ---------------- task switch ---------------- */
enum { TS_JMP, TS_CALL, TS_IRET, TS_INT };

static void task_switch(u16 sel, Desc *nd, int kind) {
  int new32 = DESC_TYPE(nd) == ST_TSS32_AVAIL || DESC_TYPE(nd) == ST_TSS32_BUSY;
  u32 nbase = nd->base;
  if (nd->limit < (u32)(new32 ? 0x67 : 0x2B)) { raise_fault(EXC_TS, 1, sel & 0xFFFC); return; }
  if (!DESC_P(nd)) { raise_fault(EXC_NP, 1, sel & 0xFFFC); return; }
  /* save outgoing state */
  flags_sync();
  u32 obase = cpu.tr.base;
  u32 ef = cpu_get_eflags();
  if (kind == TS_IRET) ef &= ~(u32)F_NT;
  if (cpu.tss_is32) {
    lin_wr32(obase + 0x20, cpu.eip); lin_wr32(obase + 0x24, ef);
    for (int i = 0; i < 8; i++) lin_wr32(obase + 0x28 + (u32)i * 4, cpu.r[i]);
    lin_wr16(obase + 0x48, cpu.seg[SEG_ES].sel); lin_wr16(obase + 0x4C, cpu.seg[SEG_CS].sel);
    lin_wr16(obase + 0x50, cpu.seg[SEG_SS].sel); lin_wr16(obase + 0x54, cpu.seg[SEG_DS].sel);
    lin_wr16(obase + 0x58, cpu.seg[SEG_FS].sel); lin_wr16(obase + 0x5C, cpu.seg[SEG_GS].sel);
  } else {
    lin_wr16(obase + 0x0E, (u16)cpu.eip); lin_wr16(obase + 0x10, (u16)ef);
    for (int i = 0; i < 8; i++) lin_wr16(obase + 0x12 + (u32)i * 2, (u16)cpu.r[i]);
    lin_wr16(obase + 0x22, cpu.seg[SEG_ES].sel); lin_wr16(obase + 0x24, cpu.seg[SEG_CS].sel);
    lin_wr16(obase + 0x26, cpu.seg[SEG_SS].sel); lin_wr16(obase + 0x28, cpu.seg[SEG_DS].sel);
  }
  if (cpu.fault_pending) return;
  /* busy bits */
  if (kind == TS_JMP || kind == TS_IRET) { /* old task becomes available */
    u32 hi = lin_rd32(cpu.tr.base ? 0 : 0); (void)hi;
    Desc od;
    if (read_desc(cpu.tr.sel, &od, EXC_TS)) { od.hi &= ~0x200u; lin_wr32(od.addr + 4, od.hi); }
  }
  if (kind != TS_IRET) { nd->hi |= 0x200; lin_wr32(nd->addr + 4, nd->hi); }
  if (kind == TS_CALL || kind == TS_INT) lin_wr16(nbase + 0, cpu.tr.sel); /* back link */
  /* load incoming state */
  cpu.tr.sel = sel; cpu.tr.base = nbase; cpu.tr.limit = nd->limit; cpu.tr.access = nd->access; cpu.tr.valid = 1;
  cpu.tss_is32 = (u8)new32;
  cpu.cr0 |= 8; /* TS */
  u32 neip, nef, regs[8];
  u16 segs[6], ldt;
  if (new32) {
    cpu.cr3 = lin_rd32(nbase + 0x1C); tlb_flush();
    neip = lin_rd32(nbase + 0x20); nef = lin_rd32(nbase + 0x24);
    for (int i = 0; i < 8; i++) regs[i] = lin_rd32(nbase + 0x28 + (u32)i * 4);
    segs[SEG_ES] = lin_rd16(nbase + 0x48); segs[SEG_CS] = lin_rd16(nbase + 0x4C); segs[SEG_SS] = lin_rd16(nbase + 0x50);
    segs[SEG_DS] = lin_rd16(nbase + 0x54); segs[SEG_FS] = lin_rd16(nbase + 0x58); segs[SEG_GS] = lin_rd16(nbase + 0x5C);
    ldt = lin_rd16(nbase + 0x60);
  } else {
    neip = lin_rd16(nbase + 0x0E); nef = lin_rd16(nbase + 0x10);
    for (int i = 0; i < 8; i++) regs[i] = lin_rd16(nbase + 0x12 + (u32)i * 2);
    segs[SEG_ES] = lin_rd16(nbase + 0x22); segs[SEG_CS] = lin_rd16(nbase + 0x24); segs[SEG_SS] = lin_rd16(nbase + 0x26);
    segs[SEG_DS] = lin_rd16(nbase + 0x28); segs[SEG_FS] = 0; segs[SEG_GS] = 0;
    ldt = lin_rd16(nbase + 0x2A);
  }
  if (cpu.fault_pending) return;
  for (int i = 0; i < 8; i++) cpu.r[i] = regs[i];
  cpu.eip = neip;
  if (kind == TS_CALL || kind == TS_INT) nef |= F_NT;
  cpu_set_eflags(nef);
  cpu.eflags = (cpu.eflags & ~(u32)F_VM) | (nef & F_VM);
  /* LDT */
  if ((ldt & 0xFFFC) == 0) { cpu.ldtr.valid = 0; cpu.ldtr.sel = ldt; }
  else {
    Desc ld;
    if (!read_desc((u16)(ldt & ~4), &ld, EXC_TS)) return;
    if (DESC_S(&ld) || DESC_TYPE(&ld) != ST_LDT) { raise_fault(EXC_TS, 1, ldt & 0xFFFC); return; }
    cpu.ldtr.sel = ldt; cpu.ldtr.base = ld.base; cpu.ldtr.limit = ld.limit; cpu.ldtr.valid = 1;
  }
  if (in_v86()) {
    cpu.cpl = 3;
    for (int s = 0; s < SEG_COUNT; s++) load_seg_real(s, segs[s]);
    cpu.cpl = 3;
  } else {
    cpu.cpl = segs[SEG_CS] & 3;
    Desc cd;
    if (!read_desc(segs[SEG_CS], &cd, EXC_TS)) return;
    if (!DESC_CODE(&cd)) { raise_fault(EXC_TS, 1, segs[SEG_CS] & 0xFFFC); return; }
    if (!DESC_P(&cd)) { raise_fault(EXC_NP, 1, segs[SEG_CS] & 0xFFFC); return; }
    load_cs(segs[SEG_CS], &cd, segs[SEG_CS] & 3);
    if (!load_stack(segs[SEG_SS], cpu.r[REG_SP], cpu.cpl)) return;
    static const int ds[4] = {SEG_ES, SEG_DS, SEG_FS, SEG_GS};
    for (int i = 0; i < 4; i++) { load_seg_prot(ds[i], segs[ds[i]]); if (cpu.fault_pending) return; }
  }
  cpu.fetch_page_lin = 0xFFFFFFFFu;
}

/* ---------------- interrupts / exceptions ---------------- */
static void interrupt_real(u8 vec, u32 ret_eip) {
  u32 entry = cpu.idtr.base + (u32)vec * 4;
  if (cpu.gen >= GEN_286 && (u32)vec * 4 + 3 > cpu.idtr.limit) { raise_fault(EXC_GP, 1, (u32)vec * 8 + 2); return; }
  u16 ip = lin_rd16(entry);
  u16 cs = lin_rd16(entry + 2);
  if (cpu.fault_pending) return;
  u32 f = cpu_get_eflags();
  u32 sp0 = sp_get();
  push16((u16)f);
  push16(cpu.seg[SEG_CS].sel);
  push16((u16)ret_eip);
  if (cpu.fault_pending) { sp_set(sp0); return; }
  cpu.eflags &= ~(u32)(F_IF | F_TF | F_RF | F_AC);
  cpu.lf_type = LF_NONE;
  load_seg_real(SEG_CS, cs);
  cpu.eip = ip;
  cpu.cycles += 25;
}

static void push_frame(int is32, u32 v) { if (is32) push32(v); else push16((u16)v); }

static void interrupt_prot(u8 vec, int is_sw, int has_err, u32 err, u32 ret_eip) {
  u32 off = (u32)vec * 8;
  if (off + 7 > cpu.idtr.limit) { raise_fault(EXC_GP, 1, off + 2 + (is_sw ? 0 : 1)); return; }
  u32 lo = lin_rd32(cpu.idtr.base + off), hi = lin_rd32(cpu.idtr.base + off + 4);
  if (cpu.fault_pending) return;
  Desc g; decode_desc(&g, lo, hi, cpu.idtr.base + off);
  int type = DESC_TYPE(&g);
  if (DESC_S(&g) || !(type == ST_TASKGATE || type == ST_INTGATE16 || type == ST_TRAPGATE16 || type == ST_INTGATE32 || type == ST_TRAPGATE32)) {
    raise_fault(EXC_GP, 1, off + 2 + (is_sw ? 0 : 1)); return;
  }
  if (is_sw && DESC_DPL(&g) < cpu.cpl) { raise_fault(EXC_GP, 1, off + 2); return; }
  if (!DESC_P(&g)) { raise_fault(EXC_NP, 1, off + 2 + (is_sw ? 0 : 1)); return; }
  u16 sel = (u16)(lo >> 16);
  u32 gate_off = (lo & 0xFFFF) | (type >= ST_INTGATE32 ? (hi & 0xFFFF0000u) : 0);
  if (type == ST_TASKGATE) {
    Desc td;
    if (!read_desc(sel, &td, EXC_GP)) return;
    if (DESC_S(&td) || (DESC_TYPE(&td) != ST_TSS16_AVAIL && DESC_TYPE(&td) != ST_TSS32_AVAIL)) { raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
    cpu.eip = ret_eip;
    task_switch(sel, &td, TS_INT);
    if (has_err && !cpu.fault_pending) push_frame(cpu.tss_is32, err);
    return;
  }
  int is32 = type >= ST_INTGATE32;
  int intgate = type == ST_INTGATE16 || type == ST_INTGATE32;
  Desc cd;
  if (!read_desc(sel, &cd, EXC_GP)) return;
  if (!DESC_CODE(&cd) || DESC_DPL(&cd) > cpu.cpl) { raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
  if (!DESC_P(&cd)) { raise_fault(EXC_NP, 1, sel & 0xFFFC); return; }
  int dpl = DESC_DPL(&cd);
  u32 ef = cpu_get_eflags();
  u32 old_cs = cpu.seg[SEG_CS].sel, old_ss = cpu.seg[SEG_SS].sel, old_esp = cpu.r[REG_SP];
  Seg saved_ss = cpu.seg[SEG_SS];
  u32 saved_sp = cpu.r[REG_SP];

  if (in_v86()) {
    if (dpl != 0) { raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
    u16 nss; u32 nesp;
    if (!tss_stack(0, &nss, &nesp)) return;
    u16 es = cpu.seg[SEG_ES].sel, ds = cpu.seg[SEG_DS].sel, fs = cpu.seg[SEG_FS].sel, gs = cpu.seg[SEG_GS].sel;
    cpu.eflags &= ~(u32)F_VM; /* leave V86 so the stack load uses descriptors */
    cpu.cpl = 0;
    if (!load_stack(nss, nesp, 0)) { cpu.eflags |= F_VM; cpu.cpl = 3; cpu.seg[SEG_SS] = saved_ss; cpu.r[REG_SP] = saved_sp; return; }
    push_frame(is32, gs); push_frame(is32, fs); push_frame(is32, ds); push_frame(is32, es);
    push_frame(is32, old_ss); push_frame(is32, old_esp);
    push_frame(is32, ef); push_frame(is32, old_cs); push_frame(is32, ret_eip);
    if (has_err) push_frame(is32, err);
    if (cpu.fault_pending) { cpu.eflags |= F_VM; cpu.cpl = 3; cpu.seg[SEG_SS] = saved_ss; cpu.r[REG_SP] = saved_sp; return; }
    for (int s = 0; s < SEG_COUNT; s++) if (s != SEG_CS && s != SEG_SS) { cpu.seg[s].valid = 0; cpu.seg[s].sel = 0; }
    load_cs(sel, &cd, 0);
  } else if (!DESC_CONFORMING(&cd) && dpl < cpu.cpl) {
    u16 nss; u32 nesp;
    if (!tss_stack(dpl, &nss, &nesp)) return;
    if (!load_stack(nss, nesp, dpl)) return;
    push_frame(is32, old_ss); push_frame(is32, old_esp);
    push_frame(is32, ef); push_frame(is32, old_cs); push_frame(is32, ret_eip);
    if (has_err) push_frame(is32, err);
    if (cpu.fault_pending) { cpu.seg[SEG_SS] = saved_ss; cpu.r[REG_SP] = saved_sp; return; }
    load_cs(sel, &cd, dpl);
    validate_data_segs();
  } else {
    u32 sp0 = sp_get();
    push_frame(is32, ef); push_frame(is32, old_cs); push_frame(is32, ret_eip);
    if (has_err) push_frame(is32, err);
    if (cpu.fault_pending) { sp_set(sp0); return; }
    load_cs(sel, &cd, cpu.cpl);
  }
  cpu.eip = is32 ? gate_off : (gate_off & 0xFFFF);
  cpu.eflags &= ~(u32)(F_TF | F_RF | F_NT | F_VM);
  if (intgate) cpu.eflags &= ~(u32)F_IF;
  cpu.lf_type = LF_NONE;
  cpu.cycles += 40;
}

void cpu_interrupt(u8 vec, int is_sw, int has_err, u32 err, u32 ret_eip) {
  if (in_v86() && is_sw && iopl() < 3) { /* INT n in V86 with IOPL < 3 traps to the monitor */
    cpu.eip = cpu.eip_start;
    raise_fault(EXC_GP, 1, 0);
    return;
  }
  if (in_pmode()) interrupt_prot(vec, is_sw, has_err, err, ret_eip);
  else interrupt_real(vec, ret_eip);
}

int cpu_trace_faults;

void cpu_deliver_fault(void) {
  u8 vec = cpu.fault_vec;
  int has_err = cpu.fault_has_err;
  u32 err = cpu.fault_err;
  cpu.fault_pending = 0;
  if (cpu_trace_faults && (vec != EXC_PF || cpu_trace_faults > 1))
    dm_log("CPU: fault %d err=%x at %04x:%08x cr0=%x cpl=%d vm=%d lin=%08x nest=%d", vec, err, cpu.seg[SEG_CS].sel, cpu.eip_start,
           cpu.cr0, cpu.cpl, in_v86(), vec == EXC_PF ? cpu.fault_cr2 : 0, cpu.in_fault_delivery);
  if (!in_pmode()) has_err = 0; /* real mode pushes no error codes */
  if (vec == EXC_PF) cpu.cr2 = cpu.fault_cr2;
  u32 ret = cpu.eip_start;
  if (vec == EXC_DE && cpu.gen <= GEN_186) ret = cpu.eip;
  if (cpu.in_fault_delivery) {
    /* a fault while delivering a fault: double fault; a third one resets the machine */
    if (cpu.in_fault_delivery >= 2 || vec == EXC_DF) {
      dm_log("CPU: triple fault at %04x:%08x — reset", cpu.seg[SEG_CS].sel, cpu.eip_start);
      extern void machine_reset_request(void);
      machine_reset_request();
      cpu.in_fault_delivery = 0;
      return;
    }
    cpu.in_fault_delivery++;
    vec = EXC_DF; has_err = 1; err = 0;
  } else cpu.in_fault_delivery = 1;
  if (vec == EXC_UD || (vec == EXC_GP && !in_pmode())) {
    u32 a = cpu.seg[SEG_CS].base + cpu.eip_start;
    dm_log("CPU: #%s at %04x:%08x bytes %02x %02x %02x %02x", vec == EXC_UD ? "UD" : "GP",
           cpu.seg[SEG_CS].sel, cpu.eip_start, lin_rd8(a), lin_rd8(a + 1), lin_rd8(a + 2), lin_rd8(a + 3));
  }
  cpu.eip = ret;
  cpu.halted = 0;
  cpu_interrupt(vec, 0, has_err, err, ret);
  if (cpu.fault_pending) { cpu_deliver_fault(); return; }
  cpu.in_fault_delivery = 0;
}

/* ---------------- far transfers ---------------- */
static void far_jump_prot(u16 sel, u32 off) {
  Desc d;
  if ((sel & 0xFFFC) == 0) { raise_fault(EXC_GP, 1, 0); return; }
  if (!read_desc(sel, &d, EXC_GP)) return;
  if (DESC_CODE(&d)) {
    int rpl = sel & 3, dpl = DESC_DPL(&d);
    if (DESC_CONFORMING(&d)) { if (dpl > cpu.cpl) { raise_fault(EXC_GP, 1, sel & 0xFFFC); return; } }
    else if (rpl > cpu.cpl || dpl != cpu.cpl) { raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
    if (!DESC_P(&d)) { raise_fault(EXC_NP, 1, sel & 0xFFFC); return; }
    if (off > d.limit) { raise_fault(EXC_GP, 1, 0); return; }
    mark_accessed(&d);
    load_cs(sel, &d, cpu.cpl);
    cpu.eip = off;
    return;
  }
  int type = DESC_TYPE(&d);
  if (type == ST_CALLGATE16 || type == ST_CALLGATE32) {
    if (DESC_DPL(&d) < cpu.cpl || DESC_DPL(&d) < (sel & 3)) { raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
    if (!DESC_P(&d)) { raise_fault(EXC_NP, 1, sel & 0xFFFC); return; }
    u16 tsel = (u16)(d.lo >> 16);
    u32 toff = (d.lo & 0xFFFF) | (type == ST_CALLGATE32 ? (d.hi & 0xFFFF0000u) : 0);
    Desc cd;
    if (!read_desc(tsel, &cd, EXC_GP)) return;
    if (!DESC_CODE(&cd) || DESC_DPL(&cd) > cpu.cpl || (!DESC_CONFORMING(&cd) && DESC_DPL(&cd) != cpu.cpl)) { raise_fault(EXC_GP, 1, tsel & 0xFFFC); return; }
    if (!DESC_P(&cd)) { raise_fault(EXC_NP, 1, tsel & 0xFFFC); return; }
    load_cs(tsel, &cd, cpu.cpl);
    cpu.eip = toff;
    return;
  }
  if (type == ST_TASKGATE) {
    u16 tsel = (u16)(d.lo >> 16);
    Desc td;
    if (!read_desc(tsel, &td, EXC_GP)) return;
    task_switch(tsel, &td, TS_JMP);
    return;
  }
  if (type == ST_TSS16_AVAIL || type == ST_TSS32_AVAIL) {
    if (DESC_DPL(&d) < cpu.cpl || DESC_DPL(&d) < (sel & 3)) { raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
    task_switch(sel, &d, TS_JMP);
    return;
  }
  raise_fault(EXC_GP, 1, sel & 0xFFFC);
}

void cpu_far_jump(u16 sel, u32 off) {
  if (in_prot()) { far_jump_prot(sel, off); cpu.cycles += 20; return; }
  load_seg_real(SEG_CS, sel);
  cpu.eip = off & eip_mask();
  cpu.cycles += 12;
}

static void far_call_prot(u16 sel, u32 off) {
  Desc d;
  if ((sel & 0xFFFC) == 0) { raise_fault(EXC_GP, 1, 0); return; }
  if (!read_desc(sel, &d, EXC_GP)) return;
  u32 sp0 = sp_get();
  if (DESC_CODE(&d)) {
    int rpl = sel & 3, dpl = DESC_DPL(&d);
    if (DESC_CONFORMING(&d)) { if (dpl > cpu.cpl) { raise_fault(EXC_GP, 1, sel & 0xFFFC); return; } }
    else if (rpl > cpu.cpl || dpl != cpu.cpl) { raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
    if (!DESC_P(&d)) { raise_fault(EXC_NP, 1, sel & 0xFFFC); return; }
    if (off > d.limit) { raise_fault(EXC_GP, 1, 0); return; }
    pushv(cpu.seg[SEG_CS].sel);
    pushv(cpu.eip);
    if (cpu.fault_pending) { sp_set(sp0); return; }
    mark_accessed(&d);
    load_cs(sel, &d, cpu.cpl);
    cpu.eip = off;
    return;
  }
  int type = DESC_TYPE(&d);
  if (type == ST_CALLGATE16 || type == ST_CALLGATE32) {
    if (DESC_DPL(&d) < cpu.cpl || DESC_DPL(&d) < (sel & 3)) { raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
    if (!DESC_P(&d)) { raise_fault(EXC_NP, 1, sel & 0xFFFC); return; }
    int is32 = type == ST_CALLGATE32;
    u16 tsel = (u16)(d.lo >> 16);
    u32 toff = (d.lo & 0xFFFF) | (is32 ? (d.hi & 0xFFFF0000u) : 0);
    int count = d.hi & 0x1F;
    Desc cd;
    if ((tsel & 0xFFFC) == 0) { raise_fault(EXC_GP, 1, 0); return; }
    if (!read_desc(tsel, &cd, EXC_GP)) return;
    if (!DESC_CODE(&cd) || DESC_DPL(&cd) > cpu.cpl) { raise_fault(EXC_GP, 1, tsel & 0xFFFC); return; }
    if (!DESC_P(&cd)) { raise_fault(EXC_NP, 1, tsel & 0xFFFC); return; }
    int dpl = DESC_DPL(&cd);
    if (!DESC_CONFORMING(&cd) && dpl < cpu.cpl) { /* more privileged: switch stacks */
      u16 nss; u32 nesp;
      if (!tss_stack(dpl, &nss, &nesp)) return;
      u32 old_ss = cpu.seg[SEG_SS].sel, old_esp = cpu.r[REG_SP];
      Seg saved_ss = cpu.seg[SEG_SS];
      u32 params[32];
      for (int i = 0; i < count; i++) params[i] = is32 ? rd32s(SEG_SS, (old_esp + (u32)i * 4) & sp_mask()) : rd16s(SEG_SS, (old_esp + (u32)i * 2) & sp_mask());
      if (cpu.fault_pending) return;
      if (!load_stack(nss, nesp, dpl)) return;
      push_frame(is32, old_ss); push_frame(is32, old_esp);
      for (int i = count - 1; i >= 0; i--) push_frame(is32, params[i]);
      push_frame(is32, cpu.seg[SEG_CS].sel); push_frame(is32, cpu.eip);
      if (cpu.fault_pending) { cpu.seg[SEG_SS] = saved_ss; cpu.r[REG_SP] = old_esp; return; }
      load_cs(tsel, &cd, dpl);
    } else {
      push_frame(is32, cpu.seg[SEG_CS].sel); push_frame(is32, cpu.eip);
      if (cpu.fault_pending) { sp_set(sp0); return; }
      load_cs(tsel, &cd, cpu.cpl);
    }
    cpu.eip = is32 ? toff : (toff & 0xFFFF);
    return;
  }
  if (type == ST_TASKGATE) {
    u16 tsel = (u16)(d.lo >> 16);
    Desc td;
    if (!read_desc(tsel, &td, EXC_GP)) return;
    task_switch(tsel, &td, TS_CALL);
    return;
  }
  if (type == ST_TSS16_AVAIL || type == ST_TSS32_AVAIL) {
    if (DESC_DPL(&d) < cpu.cpl || DESC_DPL(&d) < (sel & 3)) { raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
    task_switch(sel, &d, TS_CALL);
    return;
  }
  raise_fault(EXC_GP, 1, sel & 0xFFFC);
}

void cpu_far_call(u16 sel, u32 off) {
  if (in_prot()) { far_call_prot(sel, off); cpu.cycles += 25; return; }
  u32 sp0 = sp_get();
  pushv(cpu.seg[SEG_CS].sel);
  pushv(cpu.eip);
  if (cpu.fault_pending) { sp_set(sp0); return; }
  load_seg_real(SEG_CS, sel);
  cpu.eip = off & eip_mask();
  cpu.cycles += 18;
}

static void far_ret_prot(int pop_bytes) {
  u32 sp0 = sp_get();
  u32 ip = popv();
  u32 cs = popv();
  if (cpu.fault_pending) { sp_set(sp0); return; }
  u16 sel = (u16)cs;
  int rpl = sel & 3;
  if ((sel & 0xFFFC) == 0 || rpl < cpu.cpl) { sp_set(sp0); raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
  Desc d;
  if (!read_desc(sel, &d, EXC_GP)) { sp_set(sp0); return; }
  if (!DESC_CODE(&d)) { sp_set(sp0); raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
  int dpl = DESC_DPL(&d);
  if (DESC_CONFORMING(&d) ? dpl > rpl : dpl != rpl) { sp_set(sp0); raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
  if (!DESC_P(&d)) { sp_set(sp0); raise_fault(EXC_NP, 1, sel & 0xFFFC); return; }
  if (rpl > cpu.cpl) { /* return to an outer level */
    sp_add(pop_bytes);
    u32 nesp = popv();
    u32 nss = popv();
    if (cpu.fault_pending) { sp_set(sp0); return; }
    load_cs(sel, &d, rpl);
    cpu.eip = ip;
    Seg saved = cpu.seg[SEG_SS];
    if (!load_stack((u16)nss, nesp, rpl)) { cpu.seg[SEG_SS] = saved; return; }
    validate_data_segs();
  } else {
    load_cs(sel, &d, cpu.cpl);
    cpu.eip = ip;
    sp_add(pop_bytes);
  }
}

void cpu_far_ret(int pop_bytes) {
  if (in_prot()) { far_ret_prot(pop_bytes); cpu.cycles += 20; return; }
  u32 sp0 = sp_get();
  u32 ip = popv();
  u32 cs = popv();
  if (cpu.fault_pending) { sp_set(sp0); return; }
  load_seg_real(SEG_CS, (u16)cs);
  cpu.eip = ip & eip_mask();
  sp_add(pop_bytes);
  cpu.cycles += 15;
}

static void iret_prot(void) {
  if (cpu.eflags & F_NT) { /* nested task return */
    u16 back = lin_rd16(cpu.tr.base);
    Desc td;
    if (!read_desc(back, &td, EXC_TS)) return;
    if (DESC_S(&td) || (DESC_TYPE(&td) != ST_TSS16_BUSY && DESC_TYPE(&td) != ST_TSS32_BUSY)) { raise_fault(EXC_TS, 1, back & 0xFFFC); return; }
    task_switch(back, &td, TS_IRET);
    return;
  }
  u32 sp0 = sp_get();
  u32 ip, cs, f;
  if (cpu.osize32) { ip = pop32(); cs = pop32(); f = pop32(); }
  else { ip = pop16(); cs = pop16(); f = pop16(); }
  if (cpu.fault_pending) { sp_set(sp0); return; }
  if (cpu.osize32 && (f & F_VM) && cpu.cpl == 0) { /* back to virtual-8086 */
    u32 nesp = pop32(), nss = pop32(), es = pop32(), ds = pop32(), fs = pop32(), gs = pop32();
    if (cpu.fault_pending) { sp_set(sp0); return; }
    cpu_set_eflags(f);
    cpu.eflags |= F_VM;
    cpu.cpl = 3;
    load_seg_real(SEG_CS, (u16)cs);
    cpu.eip = ip & 0xFFFF;
    load_seg_real(SEG_SS, (u16)nss);
    cpu.r[REG_SP] = nesp;
    load_seg_real(SEG_ES, (u16)es); load_seg_real(SEG_DS, (u16)ds);
    load_seg_real(SEG_FS, (u16)fs); load_seg_real(SEG_GS, (u16)gs);
    cpu.cpl = 3;
    return;
  }
  u16 sel = (u16)cs;
  int rpl = sel & 3;
  if ((sel & 0xFFFC) == 0 || rpl < cpu.cpl) { sp_set(sp0); raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
  Desc d;
  if (!read_desc(sel, &d, EXC_GP)) { sp_set(sp0); return; }
  if (!DESC_CODE(&d)) { sp_set(sp0); raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
  int dpl = DESC_DPL(&d);
  if (DESC_CONFORMING(&d) ? dpl > rpl : dpl != rpl) { sp_set(sp0); raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
  if (!DESC_P(&d)) { sp_set(sp0); raise_fault(EXC_NP, 1, sel & 0xFFFC); return; }
  u32 cur = cpu_get_eflags();
  u32 mask = cpu.osize32 ? 0x00257FD5u : 0x7FD5u; /* writable bits */
  if (cpu.cpl > 0) mask &= ~(u32)(F_IOPL | F_VM | F_VIF | F_VIP);
  if (cpu.cpl > iopl()) mask &= ~(u32)F_IF;
  u32 nf = (cur & ~mask) | (f & mask);
  if (rpl > cpu.cpl) {
    u32 nesp = popv();
    u32 nss = popv();
    if (cpu.fault_pending) { sp_set(sp0); return; }
    load_cs(sel, &d, rpl);
    cpu.eip = ip;
    Seg saved = cpu.seg[SEG_SS];
    if (!load_stack((u16)nss, nesp, rpl)) { cpu.seg[SEG_SS] = saved; return; }
    validate_data_segs();
  } else {
    load_cs(sel, &d, cpu.cpl);
    cpu.eip = ip;
  }
  cpu_set_eflags(nf);
  cpu.eflags &= ~(u32)F_VM;
}

void cpu_iret(void) {
  if (in_v86() && iopl() < 3) { raise_fault(EXC_GP, 1, 0); return; }
  if (in_prot()) { iret_prot(); cpu.cycles += 30; return; }
  u32 sp0 = sp_get();
  u32 ip, cs, f;
  if (cpu.osize32) { ip = pop32(); cs = pop32(); f = pop32(); }
  else { ip = pop16(); cs = pop16(); f = pop16(); }
  if (cpu.fault_pending) { sp_set(sp0); return; }
  load_seg_real(SEG_CS, (u16)cs);
  cpu.eip = ip & eip_mask();
  u32 cur = cpu_get_eflags();
  if (cpu.osize32) cpu_set_eflags((f & ~(u32)(F_VM | F_VIF | F_VIP)) | (cur & (F_VM | F_VIF | F_VIP)));
  else cpu_set_eflags((cur & 0xFFFF0000u) | (f & 0xFFFF));
  if (in_v86()) { /* IOPL is not writable from V86 */
    cpu.eflags = (cpu.eflags & ~(u32)F_IOPL) | (cur & F_IOPL);
  }
  cpu.cycles += 20;
}

/* ---------------- system instructions (called from cpu_0f.c) ---------------- */
void cpu_ltr_lldt(int is_tr, u16 sel) {
  if ((sel & 0xFFFC) == 0) {
    if (is_tr) { raise_fault(EXC_GP, 1, 0); return; }
    cpu.ldtr.valid = 0; cpu.ldtr.sel = sel;
    return;
  }
  if (sel & 4) { raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
  Desc d;
  if (!read_desc(sel, &d, EXC_GP)) return;
  if (is_tr) {
    if (DESC_S(&d) || (DESC_TYPE(&d) != ST_TSS16_AVAIL && DESC_TYPE(&d) != ST_TSS32_AVAIL)) { raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
    if (!DESC_P(&d)) { raise_fault(EXC_NP, 1, sel & 0xFFFC); return; }
    d.hi |= 0x200; lin_wr32(d.addr + 4, d.hi); /* busy */
    cpu.tr.sel = sel; cpu.tr.base = d.base; cpu.tr.limit = d.limit; cpu.tr.access = d.access; cpu.tr.valid = 1;
    cpu.tss_is32 = DESC_TYPE(&d) == ST_TSS32_AVAIL;
  } else {
    if (DESC_S(&d) || DESC_TYPE(&d) != ST_LDT) { raise_fault(EXC_GP, 1, sel & 0xFFFC); return; }
    if (!DESC_P(&d)) { raise_fault(EXC_NP, 1, sel & 0xFFFC); return; }
    cpu.ldtr.sel = sel; cpu.ldtr.base = d.base; cpu.ldtr.limit = d.limit; cpu.ldtr.access = d.access; cpu.ldtr.valid = 1;
  }
}

/* LAR (is_lsl = 0) / LSL (is_lsl = 1): ZF set on success */
void cpu_sys_lar_lsl(int is_lsl) {
  decode_modrm();
  u32 sel = rm_rd16();
  FAULT_CHECK();
  flags_sync();
  cpu.eflags &= ~(u32)F_ZF;
  if ((sel & 0xFFFC) == 0) return;
  Desc d;
  u32 base, limit;
  if (sel & 4) { if (!cpu.ldtr.valid) return; base = cpu.ldtr.base; limit = cpu.ldtr.limit; } else { base = cpu.gdtr.base; limit = cpu.gdtr.limit; }
  u32 idx = sel & 0xFFF8;
  if (idx + 7 > limit) return;
  u32 lo = lin_rd32(base + idx), hi = lin_rd32(base + idx + 4);
  FAULT_CHECK();
  decode_desc(&d, lo, hi, base + idx);
  int type = DESC_TYPE(&d);
  if (DESC_S(&d)) {
    if (!DESC_CONFORMING(&d) && (DESC_DPL(&d) < cpu.cpl || DESC_DPL(&d) < (int)(sel & 3))) return;
  } else {
    static const u8 ok_lar[16] = {0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0};
    static const u8 ok_lsl[16] = {0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0};
    if (!(is_lsl ? ok_lsl[type] : ok_lar[type])) return;
    if (DESC_DPL(&d) < cpu.cpl || DESC_DPL(&d) < (int)(sel & 3)) return;
  }
  cpu.eflags |= F_ZF;
  if (is_lsl) regv_set(cpu.modrm_reg, d.limit);
  else regv_set(cpu.modrm_reg, cpu.osize32 ? (hi & 0x00FFFF00u) : (hi & 0xFF00));
}

/* group 6: SLDT STR LLDT LTR VERR VERW */
void cpu_sys_0f00(void) {
  decode_modrm();
  switch (cpu.modrm_reg) {
    case 0: if (rm_is_reg()) regv_set(cpu.modrm_rm, cpu.ldtr.sel); else rm_wr16(cpu.ldtr.sel); return;
    case 1: if (rm_is_reg()) regv_set(cpu.modrm_rm, cpu.tr.sel); else rm_wr16(cpu.tr.sel); return;
    case 2: case 3: {
      if (!require_cpl0()) return;
      u32 sel = rm_rd16();
      FAULT_CHECK();
      cpu_ltr_lldt(cpu.modrm_reg == 3, (u16)sel);
      return;
    }
    case 4: case 5: { /* VERR / VERW */
      u32 sel = rm_rd16();
      FAULT_CHECK();
      flags_sync();
      cpu.eflags &= ~(u32)F_ZF;
      if ((sel & 0xFFFC) == 0) return;
      Desc d;
      u32 base, limit;
      if (sel & 4) { if (!cpu.ldtr.valid) return; base = cpu.ldtr.base; limit = cpu.ldtr.limit; } else { base = cpu.gdtr.base; limit = cpu.gdtr.limit; }
      u32 idx = sel & 0xFFF8;
      if (idx + 7 > limit) return;
      u32 lo = lin_rd32(base + idx), hi = lin_rd32(base + idx + 4);
      FAULT_CHECK();
      decode_desc(&d, lo, hi, base + idx);
      if (!DESC_S(&d)) return;
      int ok;
      if (cpu.modrm_reg == 4) ok = DESC_DATA(&d) || DESC_READABLE_CODE(&d);
      else ok = DESC_WRITABLE(&d);
      if (ok && !DESC_CONFORMING(&d) && (DESC_DPL(&d) < cpu.cpl || DESC_DPL(&d) < (int)(sel & 3))) ok = 0;
      if (ok) cpu.eflags |= F_ZF;
      return;
    }
    default: cpu_ud(); return;
  }
}

#include "paging.h"
#include "cpu_int.h"
#include "mem.h"

#define TLB_SIZE 1024

typedef struct {
  u32 tag;       /* (lin >> 12) + 1, 0 = empty */
  u32 phys_page; /* physical page base (after A20) */
  u32 pte_addr;  /* physical address of the PTE (0 for 4 MB pages: pde_addr) */
  u8 w, u, d;    /* writable, user, dirty-already-set */
} TlbEntry;

static TlbEntry tlb[TLB_SIZE];

void tlb_flush(void) {
  for (int i = 0; i < TLB_SIZE; i++) tlb[i].tag = 0;
  cpu.fetch_page_lin = 0xFFFFFFFFu;
  cpu.fetch_page_ptr = NULL;
}

void tlb_flush_page(u32 lin) {
  tlb[(lin >> 12) & (TLB_SIZE - 1)].tag = 0;
  if ((lin & ~0xFFFu) == cpu.fetch_page_lin) { cpu.fetch_page_lin = 0xFFFFFFFFu; cpu.fetch_page_ptr = NULL; }
}

static void page_fault(u32 lin, int present, int write, int user) {
  u32 err = (present ? 1u : 0u) | (write ? 2u : 0u) | (user ? 4u : 0u);
  raise_fault(EXC_PF, 1, err);
  cpu.fault_cr2 = lin;
}

/* Walk the page tables. */
static int walk(u32 lin, int write, int user, TlbEntry *e) {
  u32 pde_addr = (cpu.cr3 & ~0xFFFu) + ((lin >> 22) << 2);
  u32 pde = mem_rd32(pde_addr);
  if (!(pde & 1)) { page_fault(lin, 0, write, user); return 0; }
  u32 phys, pte_addr, pte;
  int w, u;
  if ((cpu.cr4 & 0x10) && (pde & 0x80)) { /* 4 MB page */
    phys = (pde & 0xFFC00000u) | (lin & 0x003FF000u);
    w = (pde >> 1) & 1; u = (pde >> 2) & 1;
    pte_addr = pde_addr; pte = pde;
  } else {
    pte_addr = (pde & ~0xFFFu) + (((lin >> 12) & 0x3FF) << 2);
    pte = mem_rd32(pte_addr);
    if (!(pte & 1)) { page_fault(lin, 0, write, user); return 0; }
    phys = pte & ~0xFFFu;
    w = ((pde & pte) >> 1) & 1;
    u = ((pde & pte) >> 2) & 1;
    if (!(pde & 0x20)) mem_wr32(pde_addr, pde | 0x20);
  }
  if (user && !u) { page_fault(lin, 1, write, user); return 0; }
  if (write && !w && (user || (cpu.cr0 & 0x10000))) { page_fault(lin, 1, write, user); return 0; }
  u32 npte = pte | 0x20 | (write ? 0x40 : 0);
  if (npte != pte) mem_wr32(pte_addr, npte);
  e->tag = (lin >> 12) + 1;
  e->phys_page = phys & a20_mask;
  e->pte_addr = pte_addr;
  e->w = (u8)w; e->u = (u8)u; e->d = (u8)((npte >> 6) & 1);
  return 1;
}

int lin_translate(u32 lin, int write, int user, u32 *phys) {
  if (!(cpu.cr0 & 0x80000000u)) { *phys = lin; return 1; }
  TlbEntry *e = &tlb[(lin >> 12) & (TLB_SIZE - 1)];
  if (e->tag == (lin >> 12) + 1) {
    if (user && !e->u) { page_fault(lin, 1, write, user); return 0; }
    if (write) {
      if (!e->w && (user || (cpu.cr0 & 0x10000))) { page_fault(lin, 1, write, user); return 0; }
      if (!e->d) { u32 pte = mem_rd32(e->pte_addr); mem_wr32(e->pte_addr, pte | 0x60); e->d = 1; }
    }
    *phys = e->phys_page | (lin & 0xFFF);
    return 1;
  }
  if (!walk(lin, write, user, e)) { e->tag = 0; return 0; }
  *phys = e->phys_page | (lin & 0xFFF);
  return 1;
}

static int user_mode(void) { return cpu.cpl == 3; }

u8 lin_rd8_slow(u32 lin) { u32 p; if (!lin_translate(lin, 0, user_mode(), &p)) return 0; return mem_rd8(p); }
void lin_wr8_slow(u32 lin, u8 v) { u32 p; if (!lin_translate(lin, 1, user_mode(), &p)) return; mem_wr8(p, v); }

u16 lin_rd16_slow(u32 lin) {
  if ((lin & 0xFFF) <= 0xFFE) { u32 p; if (!lin_translate(lin, 0, user_mode(), &p)) return 0; return mem_rd16(p); }
  u32 lo = lin_rd8_slow(lin);
  if (cpu.fault_pending) return 0;
  return (u16)(lo | (lin_rd8_slow(lin + 1) << 8));
}
u32 lin_rd32_slow(u32 lin) {
  if ((lin & 0xFFF) <= 0xFFC) { u32 p; if (!lin_translate(lin, 0, user_mode(), &p)) return 0; return mem_rd32(p); }
  u32 v = 0;
  for (int i = 0; i < 4; i++) { v |= (u32)lin_rd8_slow(lin + (u32)i) << (8 * i); if (cpu.fault_pending) return 0; }
  return v;
}
void lin_wr16_slow(u32 lin, u16 v) {
  if ((lin & 0xFFF) <= 0xFFE) { u32 p; if (!lin_translate(lin, 1, user_mode(), &p)) return; mem_wr16(p, v); return; }
  u32 p0, p1;
  if (!lin_translate(lin, 1, user_mode(), &p0) || !lin_translate(lin + 1, 1, user_mode(), &p1)) return;
  mem_wr8(p0, (u8)v); mem_wr8(p1, (u8)(v >> 8));
}
void lin_wr32_slow(u32 lin, u32 v) {
  if ((lin & 0xFFF) <= 0xFFC) { u32 p; if (!lin_translate(lin, 1, user_mode(), &p)) return; mem_wr32(p, v); return; }
  u32 p[4];
  for (int i = 0; i < 4; i++) if (!lin_translate(lin + (u32)i, 1, user_mode(), &p[i])) return;
  for (int i = 0; i < 4; i++) mem_wr8(p[i], (u8)(v >> (8 * i)));
}
int lin_probe_write_slow(u32 lin, u32 size) {
  u32 p;
  if (!lin_translate(lin, 1, user_mode(), &p)) return 0;
  if (((lin & 0xFFF) + size - 1) > 0xFFF && !lin_translate(lin + size - 1, 1, user_mode(), &p)) return 0;
  return 1;
}

int fetch_page_prepare(u32 lin) {
  u32 p;
  if (!lin_translate(lin, 0, user_mode(), &p)) return 0;
  u32 page = p & ~0xFFFu;
  cpu.fetch_page_lin = lin & ~0xFFFu;
  /* only plain RAM pages are cached; MMIO (VGA window) goes through mem_rd8 */
  if (page + 0xFFF < ram_size && !(page >= 0xA0000 && page < 0xC0000)) cpu.fetch_page_ptr = ram + page;
  else cpu.fetch_page_ptr = NULL;
  return 1;
}

void lin_copy_in(u32 lin, const void *src, u32 n) {
  const u8 *s = (const u8 *)src;
  if (!paging_on()) { mem_copy_in(lin, src, n); return; }
  for (u32 i = 0; i < n; i++) { lin_wr8_slow(lin + i, s[i]); if (cpu.fault_pending) return; }
}
void lin_copy_out(void *dst, u32 lin, u32 n) {
  u8 *d = (u8 *)dst;
  if (!paging_on()) { mem_copy_out(dst, lin, n); return; }
  for (u32 i = 0; i < n; i++) { d[i] = lin_rd8_slow(lin + i); if (cpu.fault_pending) return; }
}

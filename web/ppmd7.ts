// PPMd variant H decoder (used by RAR3 "text" compression blocks).
//
// Ported from Igor Pavlov's public-domain Ppmd7 implementation (2010-03-12,
// itself based on Dmitry Shkarin's public-domain PPMd var.H, 2001), with the
// carryless RAR range decoder. Structure kept close to the original so it can
// be audited against it; memory pool is a Uint8Array with u32 byte-offset
// "references" (offset 0 = null).

const PPMD_INT_BITS = 7;
const PPMD_PERIOD_BITS = 7;
const PPMD_BIN_SCALE = 1 << (PPMD_INT_BITS + PPMD_PERIOD_BITS);
const MAX_FREQ = 124;
const UNIT_SIZE = 12;
const N_INDEXES = 4 + 4 + 4 + ((128 + 3 - 1 * 4 - 2 * 4 - 3 * 4) >> 2); // 38
const MAX_ORDER = 64;
const K_TOP = 1 << 24;

const K_INIT_BIN_ESC = [0x3CDD, 0x1F3F, 0x59BF, 0x48F3, 0x64A1, 0x5ABC, 0x6632, 0x6051];
const K_EXP_ESCAPE = [25, 14, 9, 7, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 2];

function getMean(summ: number): number {
  return (summ + (1 << (PPMD_PERIOD_BITS - 2))) >> PPMD_PERIOD_BITS;
}

interface See { summ: number; shift: number; count: number; }

/*
 * Layout inside the pool:
 *   State  (6 bytes): Symbol u8 @0, Freq u8 @1, SuccessorLow u16 @2, SuccessorHigh u16 @4
 *   Context(12 bytes): NumStats u16 @0, SummFreq u16 @2, Stats u32 @4, Suffix u32 @8
 *                      (its "one state" when NumStats==1 lives at offset 2)
 *   Node   (12 bytes): Stamp u16 @0, NU u16 @2, Next u32 @4, Prev u32 @8
 */
export class Ppmd7 {
  mem: Uint8Array;
  dv: DataView;
  size: number;
  alignOffset: number;

  indx2Units = new Uint8Array(N_INDEXES);
  units2Indx = new Uint8Array(128);
  ns2Indx = new Uint8Array(256);
  ns2BSIndx = new Uint8Array(256);
  hb2Flag = new Uint8Array(256);
  freeList = new Uint32Array(N_INDEXES);
  see: See[][] = [];
  dummySee: See = { summ: 0, shift: PPMD_PERIOD_BITS, count: 64 };
  binSumm = new Uint16Array(128 * 64);

  minContext = 0;
  maxContext = 0;
  foundState = 0;
  orderFall = 0;
  initEsc = 0;
  prevSuccess = 0;
  maxOrder = 0;
  hiBitsFlag = 0;
  runLength = 0;
  initRL = 0;
  glueCount = 0;
  text = 0;
  unitsStart = 0;
  loUnit = 0;
  hiUnit = 0;

  // range decoder (RAR carryless variant)
  low = 0;
  code = 0;
  range = 0;
  byteIn: () => number = () => 0;

  private charMask = new Int8Array(256);
  private ps = new Int32Array(256);
  private psStack = new Int32Array(MAX_ORDER + 1);

  constructor(size: number) {
    if (size < UNIT_SIZE) throw new Error("ppmd size too small");
    this.size = size;
    this.alignOffset = 4 - (size & 3);
    this.mem = new Uint8Array(this.alignOffset + size + UNIT_SIZE);
    this.dv = new DataView(this.mem.buffer);
    // Construct
    for (let i = 0, k = 0; i < N_INDEXES; i++) {
      let step = i >= 12 ? 4 : (i >> 2) + 1;
      do { this.units2Indx[k++] = i; } while (--step);
      this.indx2Units[i] = k;
    }
    this.ns2BSIndx[0] = 0;
    this.ns2BSIndx[1] = 2;
    this.ns2BSIndx.fill(4, 2, 11);
    this.ns2BSIndx.fill(6, 11);
    for (let i = 0; i < 3; i++) this.ns2Indx[i] = i;
    for (let i = 3, m = 3, k = 1; i < 256; i++) {
      this.ns2Indx[i] = m;
      if (--k === 0) k = (++m) - 2;
    }
    this.hb2Flag.fill(0, 0, 0x40);
    this.hb2Flag.fill(8, 0x40);
    for (let i = 0; i < 25; i++) {
      const row: See[] = [];
      for (let k = 0; k < 16; k++) row.push({ summ: 0, shift: 0, count: 0 });
      this.see.push(row);
    }
  }

  /* ---- field accessors ---- */
  private nS(c: number) { return this.dv.getUint16(c, true); }               // NumStats
  private setNS(c: number, v: number) { this.dv.setUint16(c, v, true); }
  private sF(c: number) { return this.dv.getUint16(c + 2, true); }           // SummFreq
  private setSF(c: number, v: number) { this.dv.setUint16(c + 2, v & 0xFFFF, true); }
  private stats(c: number) { return this.dv.getUint32(c + 4, true); }
  private setStats(c: number, v: number) { this.dv.setUint32(c + 4, v, true); }
  private suffix(c: number) { return this.dv.getUint32(c + 8, true); }
  private setSuffix(c: number, v: number) { this.dv.setUint32(c + 8, v, true); }
  private oneState(c: number) { return c + 2; }
  private sym(s: number) { return this.mem[s]; }
  private setSym(s: number, v: number) { this.mem[s] = v; }
  private freq(s: number) { return this.mem[s + 1]; }
  private setFreq(s: number, v: number) { this.mem[s + 1] = v & 0xFF; }
  private succ(s: number) {
    return this.dv.getUint16(s + 2, true) | (this.dv.getUint16(s + 4, true) << 16);
  }
  private setSucc(s: number, v: number) {
    this.dv.setUint16(s + 2, v & 0xFFFF, true);
    this.dv.setUint16(s + 4, (v >>> 16) & 0xFFFF, true);
  }
  private copyState(dst: number, src: number) { this.mem.copyWithin(dst, src, src + 6); }

  /* ---- suballocator ---- */
  private U2B(nu: number) { return nu * UNIT_SIZE; }
  private U2I(nu: number) { return this.units2Indx[nu - 1]; }
  private I2U(indx: number) { return this.indx2Units[indx]; }

  private insertNode(node: number, indx: number) {
    this.dv.setUint32(node, this.freeList[indx], true);
    this.freeList[indx] = node;
  }

  private removeNode(indx: number): number {
    const node = this.freeList[indx];
    this.freeList[indx] = this.dv.getUint32(node, true);
    return node;
  }

  private splitBlock(ptr: number, oldIndx: number, newIndx: number) {
    const nu = this.I2U(oldIndx) - this.I2U(newIndx);
    ptr += this.U2B(this.I2U(newIndx));
    let i = this.U2I(nu);
    if (this.I2U(i) !== nu) {
      const k = this.I2U(--i);
      this.insertNode(ptr + this.U2B(k), nu - k - 1);
    }
    this.insertNode(ptr, i);
  }

  private nodeStamp(n: number) { return this.dv.getUint16(n, true); }
  private setNodeStamp(n: number, v: number) { this.dv.setUint16(n, v, true); }
  private nodeNU(n: number) { return this.dv.getUint16(n + 2, true); }
  private setNodeNU(n: number, v: number) { this.dv.setUint16(n + 2, v, true); }
  private nodeNext(n: number) { return this.dv.getUint32(n + 4, true); }
  private setNodeNext(n: number, v: number) { this.dv.setUint32(n + 4, v, true); }
  private nodePrev(n: number) { return this.dv.getUint32(n + 8, true); }
  private setNodePrev(n: number, v: number) { this.dv.setUint32(n + 8, v, true); }

  private glueFreeBlocks() {
    const head = this.alignOffset + this.size;
    let n = head;
    this.glueCount = 255;
    // create doubly-linked list of free blocks
    for (let i = 0; i < N_INDEXES; i++) {
      const nu = this.I2U(i);
      let next = this.freeList[i];
      this.freeList[i] = 0;
      while (next !== 0) {
        const node = next;
        next = this.dv.getUint32(node, true);
        this.setNodeNext(node, n);
        this.setNodePrev(n, node);
        n = node;
        this.setNodeStamp(node, 0);
        this.setNodeNU(node, nu);
      }
    }
    this.setNodeStamp(head, 1);
    this.setNodeNext(head, n);
    this.setNodePrev(n, head);
    if (this.loUnit !== this.hiUnit) this.setNodeStamp(this.loUnit, 1);
    // glue adjacent free blocks
    n = this.nodeNext(head);
    while (n !== head) {
      const node = n;
      let nu = this.nodeNU(node);
      for (;;) {
        const node2 = node + nu * UNIT_SIZE;
        nu += this.nodeNU(node2);
        if (this.nodeStamp(node2) !== 0 || nu >= 0x10000) break;
        this.setNodeNext(this.nodePrev(node2), this.nodeNext(node2));
        this.setNodePrev(this.nodeNext(node2), this.nodePrev(node2));
        this.setNodeNU(node, nu);
      }
      n = this.nodeNext(node);
    }
    // fill free lists
    n = this.nodeNext(head);
    while (n !== head) {
      let node = n;
      let nu = this.nodeNU(node);
      const next = this.nodeNext(node);
      for (; nu > 128; nu -= 128, node += 128 * UNIT_SIZE) {
        this.insertNode(node, N_INDEXES - 1);
      }
      let i = this.U2I(nu);
      if (this.I2U(i) !== nu) {
        const k = this.I2U(--i);
        this.insertNode(node + this.U2B(k), nu - k - 1);
      }
      this.insertNode(node, i);
      n = next;
    }
  }

  private allocUnitsRare(indx: number): number {
    if (this.glueCount === 0) {
      this.glueFreeBlocks();
      if (this.freeList[indx] !== 0) return this.removeNode(indx);
    }
    let i = indx;
    do {
      if (++i === N_INDEXES) {
        const numBytes = this.U2B(this.I2U(indx));
        this.glueCount--;
        if (this.unitsStart - this.text > numBytes) {
          this.unitsStart -= numBytes;
          return this.unitsStart;
        }
        return 0;
      }
    } while (this.freeList[i] === 0);
    const ret = this.removeNode(i);
    this.splitBlock(ret, i, indx);
    return ret;
  }

  private allocUnits(indx: number): number {
    if (this.freeList[indx] !== 0) return this.removeNode(indx);
    const numBytes = this.U2B(this.I2U(indx));
    if (numBytes <= this.hiUnit - this.loUnit) {
      const ret = this.loUnit;
      this.loUnit += numBytes;
      return ret;
    }
    return this.allocUnitsRare(indx);
  }

  private mem12Cpy(dst: number, src: number, nu: number) {
    this.mem.copyWithin(dst, src, src + nu * UNIT_SIZE);
  }

  private shrinkUnits(oldPtr: number, oldNU: number, newNU: number): number {
    const i0 = this.U2I(oldNU), i1 = this.U2I(newNU);
    if (i0 === i1) return oldPtr;
    if (this.freeList[i1] !== 0) {
      const ptr = this.removeNode(i1);
      this.mem12Cpy(ptr, oldPtr, newNU);
      this.insertNode(oldPtr, i0);
      return ptr;
    }
    this.splitBlock(oldPtr, i0, i1);
    return oldPtr;
  }

  /* ---- model ---- */

  private restartModel() {
    this.freeList.fill(0);
    this.text = this.alignOffset;
    this.hiUnit = this.alignOffset + this.size;
    this.loUnit = this.unitsStart =
      this.hiUnit - Math.floor(Math.floor(this.size / 8) / UNIT_SIZE) * 7 * UNIT_SIZE;
    this.glueCount = 0;

    this.orderFall = this.maxOrder;
    this.initRL = -(this.maxOrder < 12 ? this.maxOrder : 12) - 1;
    this.runLength = this.initRL;
    this.prevSuccess = 0;

    this.hiUnit -= UNIT_SIZE;
    this.minContext = this.maxContext = this.hiUnit;
    this.setSuffix(this.minContext, 0);
    this.setNS(this.minContext, 256);
    this.setSF(this.minContext, 256 + 1);
    this.foundState = this.loUnit;
    this.loUnit += this.U2B(256 / 2);
    this.setStats(this.minContext, this.foundState);
    for (let i = 0; i < 256; i++) {
      const s = this.foundState + i * 6;
      this.setSym(s, i);
      this.setFreq(s, 1);
      this.setSucc(s, 0);
    }

    for (let i = 0; i < 128; i++) {
      for (let k = 0; k < 8; k++) {
        const val = (PPMD_BIN_SCALE - Math.floor(K_INIT_BIN_ESC[k] / (i + 2))) & 0xFFFF;
        for (let m = 0; m < 64; m += 8) this.binSumm[i * 64 + k + m] = val;
      }
    }

    for (let i = 0; i < 25; i++) {
      for (let k = 0; k < 16; k++) {
        const s = this.see[i][k];
        s.shift = PPMD_PERIOD_BITS - 4;
        s.summ = ((5 * i + 10) << s.shift) & 0xFFFF;
        s.count = 4;
      }
    }
  }

  init(maxOrder: number, byteIn: () => number) {
    this.maxOrder = maxOrder;
    this.restartModel();
    this.initRange(byteIn);
  }

  initRange(byteIn: () => number) {
    this.byteIn = byteIn;
    this.low = 0;
    this.range = 0xFFFFFFFF >>> 0;
    this.code = 0;
    for (let i = 0; i < 4; i++) this.code = ((this.code << 8) | (byteIn() & 0xFF)) >>> 0;
  }

  private createSuccessors(skip: boolean): number {
    let c = this.minContext;
    const upBranch = this.succ(this.foundState);
    const ps = this.psStack;
    let numPs = 0;

    if (!skip) ps[numPs++] = this.foundState;

    let broke = false;
    while (this.suffix(c) !== 0) {
      c = this.suffix(c);
      let s: number;
      if (this.nS(c) !== 1) {
        s = this.stats(c);
        while (this.sym(s) !== this.sym(this.foundState)) s += 6;
      } else {
        s = this.oneState(c);
      }
      const successor = this.succ(s);
      if (successor !== upBranch) {
        c = successor;
        if (numPs === 0) return c;
        broke = true;
        break;
      }
      ps[numPs++] = s;
    }
    void broke;

    const upSymbol = this.mem[upBranch];
    const upSuccessor = upBranch + 1;
    let upFreq: number;

    if (this.nS(c) === 1) {
      upFreq = this.freq(this.oneState(c));
    } else {
      let s = this.stats(c);
      while (this.sym(s) !== upSymbol) s += 6;
      const cf = this.freq(s) - 1;
      const s0 = this.sF(c) - this.nS(c) - cf;
      upFreq = 1 + ((2 * cf <= s0) ? (5 * cf > s0 ? 1 : 0) : Math.floor((2 * cf + 3 * s0 - 1) / (2 * s0)));
    }

    while (numPs !== 0) {
      let c1: number;
      if (this.hiUnit !== this.loUnit) {
        this.hiUnit -= UNIT_SIZE;
        c1 = this.hiUnit;
      } else if (this.freeList[0] !== 0) {
        c1 = this.removeNode(0);
      } else {
        c1 = this.allocUnitsRare(0);
        if (!c1) return 0;
      }
      this.setNS(c1, 1);
      const os = this.oneState(c1);
      this.setSym(os, upSymbol);
      this.setFreq(os, upFreq);
      this.setSucc(os, upSuccessor);
      this.setSuffix(c1, c);
      this.setSucc(ps[--numPs], c1);
      c = c1;
    }
    return c;
  }

  private swapStates(a: number, b: number) {
    for (let i = 0; i < 6; i++) {
      const t = this.mem[a + i];
      this.mem[a + i] = this.mem[b + i];
      this.mem[b + i] = t;
    }
  }

  private updateModel() {
    let fSuccessor = this.succ(this.foundState);
    let c: number;

    if (this.freq(this.foundState) < MAX_FREQ / 4 && this.suffix(this.minContext) !== 0) {
      c = this.suffix(this.minContext);
      if (this.nS(c) === 1) {
        const s = this.oneState(c);
        if (this.freq(s) < 32) this.setFreq(s, this.freq(s) + 1);
      } else {
        let s = this.stats(c);
        if (this.sym(s) !== this.sym(this.foundState)) {
          do { s += 6; } while (this.sym(s) !== this.sym(this.foundState));
          if (this.freq(s) >= this.freq(s - 6)) {
            this.swapStates(s, s - 6);
            s -= 6;
          }
        }
        if (this.freq(s) < MAX_FREQ - 9) {
          this.setFreq(s, this.freq(s) + 2);
          this.setSF(c, this.sF(c) + 2);
        }
      }
    }

    if (this.orderFall === 0) {
      const nc = this.createSuccessors(true);
      if (nc === 0) { this.restartModel(); return; }
      this.minContext = this.maxContext = nc;
      this.setSucc(this.foundState, nc);
      return;
    }

    this.mem[this.text++] = this.sym(this.foundState);
    let successor = this.text;
    if (this.text >= this.unitsStart) { this.restartModel(); return; }

    if (fSuccessor) {
      if (fSuccessor <= successor) {
        const cs = this.createSuccessors(false);
        if (cs === 0) { this.restartModel(); return; }
        fSuccessor = cs;
      }
      if (--this.orderFall === 0) {
        successor = fSuccessor;
        this.text -= (this.maxContext !== this.minContext) ? 1 : 0;
      }
    } else {
      this.setSucc(this.foundState, successor);
      fSuccessor = this.minContext;
    }

    const ns = this.nS(this.minContext);
    const s0 = this.sF(this.minContext) - ns - (this.freq(this.foundState) - 1);

    for (c = this.maxContext; c !== this.minContext; c = this.suffix(c)) {
      const ns1 = this.nS(c);
      if (ns1 !== 1) {
        if ((ns1 & 1) === 0) {
          const oldNU = ns1 >> 1;
          const i = this.U2I(oldNU);
          if (i !== this.U2I(oldNU + 1)) {
            const ptr = this.allocUnits(i + 1);
            if (!ptr) { this.restartModel(); return; }
            const oldPtr = this.stats(c);
            this.mem12Cpy(ptr, oldPtr, oldNU);
            this.insertNode(oldPtr, i);
            this.setStats(c, ptr);
          }
        }
        const add = (2 * ns1 < ns ? 1 : 0) + 2 * (((4 * ns1 <= ns ? 1 : 0) & (this.sF(c) <= 8 * ns1 ? 1 : 0)));
        this.setSF(c, this.sF(c) + add);
      } else {
        const s = this.allocUnits(0);
        if (!s) { this.restartModel(); return; }
        this.copyState(s, this.oneState(c));
        this.setStats(c, s);
        let f = this.freq(s);
        if (f < MAX_FREQ / 4 - 1) f <<= 1;
        else f = MAX_FREQ - 4;
        this.setFreq(s, f);
        this.setSF(c, f + this.initEsc + (ns > 3 ? 1 : 0));
      }
      let cf = 2 * this.freq(this.foundState) * (this.sF(c) + 6);
      const sf = s0 + this.sF(c);
      if (cf < 6 * sf) {
        cf = 1 + (cf > sf ? 1 : 0) + (cf >= 4 * sf ? 1 : 0);
        this.setSF(c, this.sF(c) + 3);
      } else {
        cf = 4 + (cf >= 9 * sf ? 1 : 0) + (cf >= 12 * sf ? 1 : 0) + (cf >= 15 * sf ? 1 : 0);
        this.setSF(c, this.sF(c) + cf);
      }
      {
        const s = this.stats(c) + ns1 * 6;
        this.setSucc(s, successor);
        this.setSym(s, this.sym(this.foundState));
        this.setFreq(s, cf);
        this.setNS(c, ns1 + 1);
      }
    }
    this.maxContext = this.minContext = fSuccessor;
  }

  private rescale() {
    const stats = this.stats(this.minContext);
    let s = this.foundState;
    // move found state to front
    if (s !== stats) {
      const tmp = this.mem.slice(s, s + 6);
      for (; s !== stats; s -= 6) this.copyState(s, s - 6);
      this.mem.set(tmp, stats);
    }
    s = stats;
    let escFreq = this.sF(this.minContext) - this.freq(s);
    this.setFreq(s, this.freq(s) + 4);
    const adder = this.orderFall !== 0 ? 1 : 0;
    this.setFreq(s, (this.freq(s) + adder) >> 1);
    let sumFreq = this.freq(s);

    let i = this.nS(this.minContext) - 1;
    do {
      s += 6;
      escFreq -= this.freq(s);
      this.setFreq(s, (this.freq(s) + adder) >> 1);
      sumFreq += this.freq(s);
      if (this.freq(s) > this.freq(s - 6)) {
        let s1 = s;
        const tmp = this.mem.slice(s1, s1 + 6);
        const tmpFreq = tmp[1];
        do {
          this.copyState(s1, s1 - 6);
          s1 -= 6;
        } while (s1 !== stats && tmpFreq > this.freq(s1 - 6));
        this.mem.set(tmp, s1);
      }
    } while (--i);

    if (this.freq(s) === 0) {
      const numStats = this.nS(this.minContext);
      i = 0;
      do { i++; s -= 6; } while (this.freq(s) === 0);
      escFreq += i;
      this.setNS(this.minContext, numStats - i);
      if (this.nS(this.minContext) === 1) {
        const tmp = this.mem.slice(stats, stats + 6);
        let f = tmp[1];
        do {
          f -= f >> 1;
          escFreq >>= 1;
        } while (escFreq > 1);
        tmp[1] = f;
        this.insertNode(stats, this.U2I((numStats + 1) >> 1));
        this.foundState = this.oneState(this.minContext);
        this.mem.set(tmp, this.foundState);
        return;
      }
      const n0 = (numStats + 1) >> 1;
      const n1 = (this.nS(this.minContext) + 1) >> 1;
      if (n0 !== n1) {
        this.setStats(this.minContext, this.shrinkUnits(stats, n0, n1));
      }
    }
    this.setSF(this.minContext, sumFreq + escFreq - (escFreq >> 1));
    this.foundState = this.stats(this.minContext);
  }

  private makeEscFreq(numMasked: number): { see: See; escFreq: number } {
    const mc = this.minContext;
    const numStats = this.nS(mc);
    const nonMasked = numStats - numMasked;
    if (numStats !== 256) {
      const row = this.ns2Indx[nonMasked - 1];
      const col = (nonMasked < this.nS(this.suffix(mc)) - numStats ? 1 : 0) +
        2 * (this.sF(mc) < 11 * numStats ? 1 : 0) +
        4 * (numMasked > nonMasked ? 1 : 0) +
        this.hiBitsFlag;
      const see = this.see[row][col];
      const r = see.summ >> see.shift;
      see.summ = (see.summ - r) & 0xFFFF;
      return { see, escFreq: r + (r === 0 ? 1 : 0) };
    }
    return { see: this.dummySee, escFreq: 1 };
  }

  private seeUpdate(see: See) {
    if (see.shift < PPMD_PERIOD_BITS && --see.count === 0) {
      see.summ = (see.summ << 1) & 0xFFFF;
      see.count = 3 << see.shift++;
    }
  }

  private nextContext() {
    const c = this.succ(this.foundState);
    if (this.orderFall === 0 && c > this.text) {
      this.minContext = this.maxContext = c;
    } else {
      this.updateModel();
    }
  }

  private update1() {
    let s = this.foundState;
    this.setFreq(s, this.freq(s) + 4);
    this.setSF(this.minContext, this.sF(this.minContext) + 4);
    if (this.freq(s) > this.freq(s - 6)) {
      this.swapStates(s, s - 6);
      this.foundState = s = s - 6;
      if (this.freq(s) > MAX_FREQ) this.rescale();
    }
    this.nextContext();
  }

  private update1_0() {
    this.prevSuccess = (2 * this.freq(this.foundState) > this.sF(this.minContext)) ? 1 : 0;
    this.runLength += this.prevSuccess;
    this.setSF(this.minContext, this.sF(this.minContext) + 4);
    this.setFreq(this.foundState, this.freq(this.foundState) + 4);
    if (this.freq(this.foundState) > MAX_FREQ) this.rescale();
    this.nextContext();
  }

  private updateBin() {
    const s = this.foundState;
    this.setFreq(s, this.freq(s) + (this.freq(s) < 128 ? 1 : 0));
    this.prevSuccess = 1;
    this.runLength++;
    this.nextContext();
  }

  private update2() {
    this.setSF(this.minContext, this.sF(this.minContext) + 4);
    this.setFreq(this.foundState, this.freq(this.foundState) + 4);
    if (this.freq(this.foundState) > MAX_FREQ) this.rescale();
    this.runLength = this.initRL;
    this.updateModel();
  }

  /* ---- range decoder (RAR carryless) ---- */

  private getThreshold(total: number): number {
    this.range = Math.floor((this.range >>> 0) / total) >>> 0;
    return Math.floor((((this.code - this.low) >>> 0)) / this.range);
  }

  private rcDecode(start: number, size: number) {
    this.low = (this.low + Math.imul(start, this.range)) >>> 0;
    this.range = Math.imul(this.range, size) >>> 0;
    this.normalize();
  }

  private normalize() {
    for (;;) {
      if ((((this.low ^ ((this.low + this.range) >>> 0)) >>> 0) >= K_TOP)) {
        if ((this.range >>> 0) >= 0x8000) break;
        this.range = ((0 - this.low) >>> 0) & 0x7FFF;
      }
      this.code = ((this.code << 8) | (this.byteIn() & 0xFF)) >>> 0;
      this.range = (this.range << 8) >>> 0;
      this.low = (this.low << 8) >>> 0;
    }
  }

  private decodeBit(size0: number): number {
    const value = this.getThreshold(PPMD_BIN_SCALE);
    if (value < size0) {
      this.rcDecode(0, size0);
      return 0;
    }
    this.rcDecode(size0, PPMD_BIN_SCALE - size0);
    return 1;
  }

  /* ---- symbol decode ---- */

  decodeSymbol(): number {
    const mask = this.charMask;
    if (this.nS(this.minContext) !== 1) {
      let s = this.stats(this.minContext);
      const count = this.getThreshold(this.sF(this.minContext));
      let hiCnt = this.freq(s);
      if (count < hiCnt) {
        this.rcDecode(0, hiCnt);
        this.foundState = s;
        const symbol = this.sym(s);
        this.update1_0();
        return symbol;
      }
      this.prevSuccess = 0;
      let i = this.nS(this.minContext) - 1;
      let found = false;
      do {
        s += 6;
        hiCnt += this.freq(s);
        if (hiCnt > count) { found = true; break; }
      } while (--i);
      if (found) {
        this.rcDecode(hiCnt - this.freq(s), this.freq(s));
        this.foundState = s;
        const symbol = this.sym(s);
        this.update1();
        return symbol;
      }
      if (count >= this.sF(this.minContext)) return -2;
      this.hiBitsFlag = this.hb2Flag[this.sym(this.foundState)];
      this.rcDecode(hiCnt, this.sF(this.minContext) - hiCnt);
      mask.fill(-1);
      mask[this.sym(s)] = 0;
      i = this.nS(this.minContext) - 1;
      do { s -= 6; mask[this.sym(s)] = 0; } while (--i);
    } else {
      const os = this.oneState(this.minContext);
      this.hiBitsFlag = this.hb2Flag[this.sym(this.foundState)];
      const probIdx = (this.freq(os) - 1) * 64 +
        this.prevSuccess +
        this.ns2BSIndx[this.nS(this.suffix(this.minContext)) - 1] +
        this.hiBitsFlag +
        2 * this.hb2Flag[this.sym(os)] +
        ((this.runLength >> 26) & 0x20);
      const prob = this.binSumm[probIdx];
      if (this.decodeBit(prob) === 0) {
        this.binSumm[probIdx] = (prob + (1 << PPMD_INT_BITS) - getMean(prob)) & 0xFFFF;
        this.foundState = os;
        const symbol = this.sym(os);
        this.updateBin();
        return symbol;
      }
      this.binSumm[probIdx] = (prob - getMean(prob)) & 0xFFFF;
      this.initEsc = K_EXP_ESCAPE[this.binSumm[probIdx] >> 10];
      mask.fill(-1);
      mask[this.sym(os)] = 0;
      this.prevSuccess = 0;
    }
    for (;;) {
      let numMasked = this.nS(this.minContext);
      do {
        this.orderFall++;
        if (this.suffix(this.minContext) === 0) return -1;
        this.minContext = this.suffix(this.minContext);
      } while (this.nS(this.minContext) === numMasked);
      let hiCnt = 0;
      let s = this.stats(this.minContext);
      const ps = this.ps;
      let i = 0;
      const num = this.nS(this.minContext) - numMasked;
      do {
        const k = mask[this.sym(s)];
        hiCnt += this.freq(s) & k;
        ps[i] = s;
        s += 6;
        i -= k;
      } while (i !== num);

      const { see, escFreq } = this.makeEscFreq(numMasked);
      let freqSum = escFreq + hiCnt;
      const count = this.getThreshold(freqSum);

      if (count < hiCnt) {
        let j = 0;
        let acc = this.freq(ps[0]);
        while (acc <= count) { j++; acc += this.freq(ps[j]); }
        const st = ps[j];
        this.rcDecode(acc - this.freq(st), this.freq(st));
        this.seeUpdate(see);
        this.foundState = st;
        const symbol = this.sym(st);
        this.update2();
        return symbol;
      }
      if (count >= freqSum) return -2;
      this.rcDecode(hiCnt, freqSum - hiCnt);
      see.summ = (see.summ + freqSum) & 0xFFFF;
      do { mask[this.sym(ps[--i])] = 0; } while (i !== 0);
    }
  }
}

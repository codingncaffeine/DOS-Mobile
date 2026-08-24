// RAR reader for importing archives into the virtual disk.
//
// Supports the RAR4 container (unpack v2.9 LZ + PPMd + the standard WinRAR
// filters, and stored entries of any version) and the RAR5 container
// (unpack v5.0 LZ + delta/e8/e8e9/arm filters), solid archives, multi-volume
// sets and SFX archives. Encrypted archives are rejected.
//
// Clean-room notes: container layout from the public RAR technote; the
// decompression algorithms were ported from the BSD-2-Clause libarchive
// readers (archive_read_support_format_rar{,5}.c); the PPMd var.H model in
// ppmd7.ts derives from Igor Pavlov's public-domain Ppmd7 implementation.

import { Ppmd7 } from "./ppmd7.ts";

export interface RarEntry {
  path: string;
  isDir: boolean;
  size: number;
}

export interface RarResult {
  files: number;
  warnings: string[];
}

export class RarError extends Error {}

/** Decode-path counters (diagnostics). */
export const rarStats = { ppmBlocks: 0, lzBlocks: 0, filters: 0 };

/* ---------------- CRC32 ---------------- */

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    t[i] = c >>> 0;
  }
  return t;
})();

export function crc32(crc: number, buf: Uint8Array, len = buf.length): number {
  let c = (crc ^ 0xFFFFFFFF) >>> 0;
  for (let i = 0; i < len; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

/* ---------------- bit reader (MSB first) ---------------- */

class BitIn {
  buf: Uint8Array;
  pos = 0;      // next byte index
  bit = 0;      // bits already consumed from buf[pos] (0..7)
  overrun = false;

  constructor(buf: Uint8Array, pos = 0) {
    this.buf = buf;
    this.pos = pos;
  }

  peek16(): number {
    const b = this.buf, p = this.pos;
    let b0 = 0, b1 = 0, b2 = 0;
    if (p < b.length) b0 = b[p]; else this.overrun = true;
    if (p + 1 < b.length) b1 = b[p + 1];
    if (p + 2 < b.length) b2 = b[p + 2];
    return ((b0 << 16 | b1 << 8 | b2) >>> (8 - this.bit)) & 0xFFFF;
  }

  consume(n: number) {
    this.bit += n;
    this.pos += this.bit >> 3;
    this.bit &= 7;
  }

  bits(n: number): number {
    if (n === 0) return 0;
    const v = this.peek16() >>> (16 - n);
    this.consume(n);
    return v;
  }

  /** MSB-first read of up to 32 bits. */
  bitsLong(n: number): number {
    if (n <= 16) return this.bits(n);
    const hi = this.bits(n - 16);
    return (hi * 0x10000 + this.bits(16)) >>> 0;
  }

  alignByte() {
    if (this.bit) { this.bit = 0; this.pos++; }
  }

  readByte(): number {
    this.alignByte();
    if (this.pos >= this.buf.length) { this.overrun = true; return 0; }
    return this.buf[this.pos++];
  }
}

/* ---------------- canonical Huffman ---------------- */

const MAX_CODE_LEN = 15;

class Huff {
  count = new Int32Array(MAX_CODE_LEN + 1);
  first = new Int32Array(MAX_CODE_LEN + 1);
  base = new Int32Array(MAX_CODE_LEN + 1);
  syms: Int32Array;
  minLen = 1;
  maxLen = 1;

  constructor(lengths: Uint8Array, n: number) {
    for (let i = 0; i < n; i++) {
      const l = lengths[i];
      if (l) this.count[l]++;
    }
    let total = 0;
    for (let l = 1; l <= MAX_CODE_LEN; l++) { this.base[l] = total; total += this.count[l]; }
    this.syms = new Int32Array(total);
    const next = this.base.slice();
    for (let i = 0; i < n; i++) {
      const l = lengths[i];
      if (l) this.syms[next[l]++] = i;
    }
    let code = 0;
    for (let l = 1; l <= MAX_CODE_LEN; l++) {
      this.first[l] = code;
      code = (code + this.count[l]) << 1;
    }
    this.minLen = 1;
    while (this.minLen <= MAX_CODE_LEN && !this.count[this.minLen]) this.minLen++;
    this.maxLen = MAX_CODE_LEN;
    while (this.maxLen >= 1 && !this.count[this.maxLen]) this.maxLen--;
    if (this.maxLen < 1) { this.minLen = this.maxLen = 1; }
  }

  decode(br: BitIn): number {
    const v = br.peek16() >>> 1; // 15 bits
    for (let l = this.minLen; l <= this.maxLen; l++) {
      if (!this.count[l]) continue;
      const c = v >>> (15 - l);
      const off = c - this.first[l];
      if (off >= 0 && off < this.count[l]) {
        br.consume(l);
        return this.syms[this.base[l] + off];
      }
    }
    throw new RarError("invalid prefix code");
  }
}

/* ---------------- output sink ---------------- */

class FileSink {
  chunks: Uint8Array[] = [];
  written = 0;
  crc = 0;

  emit(bytes: Uint8Array) {
    if (!bytes.length) return;
    this.crc = crc32(this.crc, bytes);
    this.chunks.push(bytes);
    this.written += bytes.length;
  }

  finish(): Uint8Array {
    if (this.chunks.length === 1) return this.chunks[0];
    const out = new Uint8Array(this.written);
    let p = 0;
    for (const c of this.chunks) { out.set(c, p); p += c.length; }
    this.chunks = [out];
    return out;
  }
}

/* ================= container parsing ================= */

const RAR4_SIG = [0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00];
const RAR5_SIG = [0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00];

function matchAt(b: Uint8Array, off: number, sig: number[]): boolean {
  if (off + sig.length > b.length) return false;
  for (let i = 0; i < sig.length; i++) if (b[off + i] !== sig[i]) return false;
  return true;
}

/** Find a RAR signature (supports SFX: scans up to 1 MB). Returns null if none. */
export function findRarSignature(b: Uint8Array): { offset: number; v5: boolean } | null {
  const limit = Math.min(b.length, 1 << 20);
  for (let i = 0; i + 7 <= limit; i++) {
    if (b[i] !== 0x52) continue;
    if (matchAt(b, i, RAR5_SIG)) return { offset: i, v5: true };
    if (matchAt(b, i, RAR4_SIG)) return { offset: i, v5: false };
  }
  return null;
}

interface DataSeg { vol: number; off: number; len: number; }

interface ParsedFile {
  path: string;
  isDir: boolean;
  unpSize: number;
  method: number;      // 0x30..0x35 (rar4) or 0..5 (rar5)
  unpVer: number;      // 15/20/26/29/36 (rar4) or 50+ (rar5)
  solid: boolean;
  splitBefore: boolean;
  splitAfter: boolean;
  crc: number;
  hasCrc: boolean;
  dictBits: number;    // rar5 only
  segs: DataSeg[];
  encrypted: boolean;
}

interface ParsedVolume {
  files: ParsedFile[];
  hasNextVolume: boolean;
  isVolume: boolean;
}

function rd16(b: Uint8Array, o: number) { return b[o] | (b[o + 1] << 8); }
function rd32(b: Uint8Array, o: number) { return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0; }

function decodeRar4Name(raw: Uint8Array, unicode: boolean): string {
  if (unicode) {
    const zero = raw.indexOf(0);
    if (zero >= 0) {
      // RAR's custom unicode encoding after the zero byte → UTF-16 code units
      const out: number[] = [];
      const end = raw.length;
      let offset = zero + 1;
      const fnEnd = raw.length * 2;
      const highbyte = offset >= end ? 0 : raw[offset++];
      let flagbits = 0, flagbyte = 0;
      let size = 0;
      while (offset < end && size < fnEnd) {
        if (!flagbits) { flagbyte = raw[offset++]; flagbits = 8; }
        flagbits -= 2;
        switch ((flagbyte >> flagbits) & 3) {
          case 0:
            if (offset >= end) continue;
            out.push(raw[offset++]); size += 2;
            break;
          case 1:
            if (offset >= end) continue;
            out.push((highbyte << 8) | raw[offset++]); size += 2;
            break;
          case 2:
            if (offset >= end - 1) { offset = end; continue; }
            out.push((raw[offset + 1] << 8) | raw[offset]); offset += 2; size += 2;
            break;
          case 3: {
            if (offset >= end) continue;
            let length = raw[offset++];
            let extra = 0, high = 0;
            if (length & 0x80) {
              if (offset >= end) continue;
              extra = raw[offset++];
              high = highbyte;
            }
            length = (length & 0x7F) + 2;
            while (length && size < fnEnd) {
              const cp = size >> 1;
              out.push((high << 8) | ((raw[cp] + extra) & 0xFF));
              size += 2; length--;
            }
            break;
          }
        }
      }
      return String.fromCharCode(...out);
    }
  }
  let s = "";
  for (const c of raw) s += String.fromCharCode(c);
  return s;
}

function parseRar4Volume(vol: number, b: Uint8Array, start: number): ParsedVolume {
  const out: ParsedVolume = { files: [], hasNextVolume: false, isVolume: false };
  let p = start + 7; // skip marker block (the signature is itself a block)
  while (p + 7 <= b.length) {
    const type = b[p + 2];
    const flags = rd16(b, p + 3);
    const size = rd16(b, p + 5);
    let addSize = 0;
    if ((flags & 0x8000) && type !== 0x74) {
      if (p + 11 > b.length) break;
      addSize = rd32(b, p + 7);
    }
    if (size < 7) break;
    if (type === 0x73) { // MAIN
      out.isVolume = !!(flags & 0x0001);
      if (flags & 0x0080) throw new RarError("archive has encrypted headers (password-protected)");
      p += size + addSize;
    } else if (type === 0x74) { // FILE
      const h = p + 7;
      if (h + 25 > b.length) break;
      let packSize = rd32(b, h);
      let unpSize = rd32(b, h + 4);
      const fileCrc = rd32(b, h + 9);
      const unpVer = b[h + 17];
      const method = b[h + 18];
      const nameSize = rd16(b, h + 19);
      let q = h + 25;
      if (flags & 0x0100) { // LARGE: high 32 bits follow the fixed header
        packSize = rd32(b, q) * 0x100000000 + packSize;
        unpSize = rd32(b, q + 4) * 0x100000000 + unpSize;
        q += 8;
      }
      const rawName = b.subarray(q, q + nameSize);
      const name = decodeRar4Name(rawName, !!(flags & 0x0200));
      const isDir = (flags & 0xE0) === 0xE0;
      out.files.push({
        path: name.replace(/\\/g, "/"),
        isDir,
        unpSize,
        method,
        unpVer,
        solid: !!(flags & 0x0010),
        splitBefore: !!(flags & 0x0001),
        splitAfter: !!(flags & 0x0002),
        crc: fileCrc,
        hasCrc: true,
        dictBits: 0,
        segs: [{ vol, off: p + size, len: packSize }],
        encrypted: !!(flags & 0x0004),
      });
      p += size + packSize;
    } else if (type === 0x7B) { // ENDARC
      out.hasNextVolume = !!(flags & 0x0001);
      break;
    } else {
      // MARK/COMM/AV/SUB/PROTECT/SIGN/NEWSUB/unknown: skip block + data
      p += size + addSize;
    }
  }
  return out;
}

class Vint {
  b: Uint8Array; p: number;
  constructor(b: Uint8Array, p: number) { this.b = b; this.p = p; }
  next(): number {
    let v = 0, shift = 1;
    for (let i = 0; i < 10; i++) {
      if (this.p >= this.b.length) throw new RarError("truncated RAR5 header");
      const byte = this.b[this.p++];
      v += (byte & 0x7F) * shift;
      if (!(byte & 0x80)) return v;
      shift *= 128;
    }
    throw new RarError("bad RAR5 varint");
  }
  u32(): number {
    if (this.p + 4 > this.b.length) throw new RarError("truncated RAR5 header");
    const v = rd32(this.b, this.p);
    this.p += 4;
    return v;
  }
}

function parseRar5Volume(vol: number, b: Uint8Array, start: number): ParsedVolume {
  const out: ParsedVolume = { files: [], hasNextVolume: false, isVolume: false };
  let p = start + 8;
  const td = new TextDecoder("utf-8");
  while (p + 7 <= b.length) {
    const v = new Vint(b, p + 4);
    let headSize: number;
    try { headSize = v.next(); } catch { break; }
    const headStart = v.p;
    if (headSize < 1 || headStart + headSize > b.length) break;
    let dataSize = 0;
    try {
      const type = v.next();
      const hflags = v.next();
      let extraSize = 0;
      if (hflags & 0x0001) extraSize = v.next();
      if (hflags & 0x0002) dataSize = v.next();
      if (type === 1) { // MAIN
        const aflags = v.next();
        out.isVolume = !!(aflags & 0x0001);
      } else if (type === 4) {
        throw new RarError("archive has encrypted headers (password-protected)");
      } else if (type === 2) { // FILE
        const fflags = v.next();
        const unpSize = v.next();
        v.next(); // attributes
        if (fflags & 0x0002) v.u32(); // mtime
        let fcrc = 0, hasCrc = false;
        if (fflags & 0x0004) { fcrc = v.u32(); hasCrc = true; }
        const compInfo = v.next();
        v.next(); // host os
        const nameSize = v.next();
        const rawName = b.subarray(v.p, v.p + nameSize);
        const name = td.decode(rawName);
        let encrypted = false;
        if (extraSize > 0) {
          const ev = new Vint(b, headStart + headSize - extraSize);
          try {
            while (ev.p < headStart + headSize) {
              const recSize = ev.next();
              const recStart = ev.p;
              const recId = ev.next();
              if (recId === 1) encrypted = true;
              ev.p = recStart + recSize;
            }
          } catch { /* tolerate malformed extra area */ }
        }
        out.files.push({
          path: name.replace(/\\/g, "/"),
          isDir: !!(fflags & 0x0001),
          unpSize: (fflags & 0x0008) ? -1 : unpSize,
          method: (compInfo >> 7) & 7,
          unpVer: 50 + (compInfo & 0x3F),
          solid: !!(compInfo & 0x0040),
          splitBefore: !!(hflags & 0x0008),
          splitAfter: !!(hflags & 0x0010),
          crc: fcrc,
          hasCrc,
          dictBits: (compInfo >> 10) & 0x0F,
          segs: [{ vol, off: headStart + headSize, len: dataSize }],
          encrypted,
        });
      } else if (type === 5) { // ENDARC
        const eflags = v.next();
        out.hasNextVolume = !!(eflags & 0x0001);
        break;
      }
      // type 3 (SERVICE: CMT/QO/RR…) and others: skip
    } catch (e) {
      if (e instanceof RarError && /password/.test(e.message)) throw e;
      break;
    }
    p = headStart + headSize + dataSize;
  }
  return out;
}

/* ================= unpack v2.9 (RAR3) ================= */

const NC29 = 299, DC29 = 60, LDC29 = 17, RC29 = 28, BC29 = 20;
const TABLESIZE29 = NC29 + DC29 + LDC29 + RC29;
const WINSIZE29 = 0x400000;
const WINMASK29 = WINSIZE29 - 1;
const MAXMATCH29 = 260;
const VM_MEMSIZE = 0x40000;

const LEN_BASES = [0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224];
const LEN_BITS = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5];
const OFF_BASES = [0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072,
  4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152, 65536, 98304, 131072, 196608, 262144, 327680, 393216, 458752,
  524288, 589824, 655360, 720896, 786432, 851968, 917504, 983040, 1048576, 1310720, 1572864, 1835008, 2097152, 2359296,
  2621440, 2883584, 3145728, 3407872, 3670016, 3932160];
const OFF_BITS = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14,
  15, 15, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18];
const SHORT_BASES = [0, 4, 8, 16, 32, 64, 128, 192];
const SHORT_BITS = [2, 2, 3, 4, 5, 6, 6, 6];

const FILTER_DELTA_FP = 0x1D * 0x100000000 + 0x0E06077D;
const FILTER_E8_FP = 0x35 * 0x100000000 + 0xAD576887;
const FILTER_E8E9_FP = 0x39 * 0x100000000 + 0x3CD7E57E;
const FILTER_RGB_FP = 0x95 * 0x100000000 + 0x1C2C5DC8;
const FILTER_AUDIO_FP = 0xD8 * 0x100000000 + 0xBC85E701;

interface Prog29 { fingerprint: number; usageCount: number; oldFilterLength: number; }
interface Filter29 { prog: Prog29; regs: Uint32Array; blockStart: number; blockLength: number; }

function rarvmNumber(br: BitIn): number {
  switch (br.bits(2)) {
    case 0: return br.bits(4);
    case 1: {
      const v = br.bits(8);
      if (v >= 16) return v;
      return ((0xFFFFFF00 | (v << 4) | br.bits(4)) >>> 0);
    }
    case 2: return br.bits(16);
    default: return br.bitsLong(32);
  }
}

class Unpack29 {
  win = new Uint8Array(WINSIZE29);
  pos = 0;              // total unpacked stream position (monotonic across solid files)
  offset = 0;           // flushed (written-out) position
  lengthTable = new Uint8Array(TABLESIZE29);
  main!: Huff; off!: Huff; lowoff!: Huff; len!: Huff;
  haveTables = false;
  oldOffset = new Int32Array(4);
  lastOffset = 0;
  lastLength = 0;
  lastLowOffset = 0;
  lowOffsetRepeats = 0;
  isPpmd = false;
  ppmd: Ppmd7 | null = null;
  ppmdValid = false;
  ppmdEscape = 2;
  ppmdEod = false;
  startNewTable = true;
  fileDone = false;     // hit the 256-newfile symbol
  progs: Prog29[] = [];
  lastFilterNum = 0;
  stack: Filter29[] = [];
  vmMem: Uint8Array | null = null;
  br!: BitIn;
  sink!: FileSink;
  fileEnd = 0;
  fileStart = 0;
  solidEnd = 0;   // stream position where the previous file's slice ended

  resetHard() {
    this.pos = 0; this.offset = 0; this.solidEnd = 0;
    this.lengthTable.fill(0);
    this.haveTables = false;
    this.oldOffset.fill(0);
    this.lastOffset = this.lastLength = 0;
    this.lastLowOffset = 0; this.lowOffsetRepeats = 0;
    this.isPpmd = false; this.ppmdValid = false; this.ppmdEod = false;
    this.startNewTable = true;
    this.progs = []; this.stack = [];
    this.lastFilterNum = 0;
  }

  private emitWindow(from: number, to: number) {
    const cutTo = Math.min(to, this.fileEnd);
    let f = Math.max(from, 0);
    while (f < cutTo) {
      const wo = f & WINMASK29;
      const run = Math.min(cutTo - f, WINSIZE29 - wo);
      this.sink.emit(this.win.slice(wo, wo + run));
      f += run;
    }
  }

  private literal(b: number) {
    this.win[this.pos & WINMASK29] = b;
    this.pos++;
  }

  private copyMatch(dist: number, len: number) {
    let p = this.pos;
    const w = this.win;
    for (let i = 0; i < len; i++) {
      w[p & WINMASK29] = w[(p - dist) & WINMASK29];
      p++;
    }
    this.pos = p;
  }

  private parseCodes() {
    const br = this.br;
    br.alignByte();
    if (br.bits(1)) {
      // PPMd block
      rarStats.ppmBlocks++;
      this.isPpmd = true;
      const flags = br.bits(7);
      let memMB = 0;
      if (flags & 0x20) memMB = br.bits(8) + 1;
      if (flags & 0x40) this.ppmdEscape = br.bits(8);
      else this.ppmdEscape = 2;
      if (flags & 0x20) {
        let maxOrder = (flags & 0x1F) + 1;
        if (maxOrder > 16) maxOrder = 16 + (maxOrder - 16) * 3;
        if (maxOrder === 1) throw new RarError("bad PPMd max order");
        this.ppmd = new Ppmd7(memMB << 20);
        this.ppmd.init(maxOrder, () => this.br.readByte());
        this.ppmdValid = true;
      } else {
        if (!this.ppmdValid || !this.ppmd) throw new RarError("PPMd continuation without model");
        this.ppmd.initRange(() => this.br.readByte());
      }
      if (flags & 0x40) this.ppmd.initEsc = this.ppmdEscape;
      return;
    }
    rarStats.lzBlocks++;
    this.isPpmd = false;
    this.lastLowOffset = 0;
    this.lowOffsetRepeats = 0;
    if (!br.bits(1)) this.lengthTable.fill(0);
    const bitLengths = new Uint8Array(BC29);
    for (let i = 0; i < BC29;) {
      const l = br.bits(4);
      if (l === 0xF) {
        const zc = br.bits(4);
        if (zc) {
          for (let j = 0; j < zc + 2 && i < BC29; j++) bitLengths[i++] = 0;
          continue;
        }
      }
      bitLengths[i++] = l;
    }
    const pre = new Huff(bitLengths, BC29);
    const tbl = this.lengthTable;
    for (let i = 0; i < TABLESIZE29;) {
      const val = pre.decode(br);
      if (val < 16) {
        tbl[i] = (tbl[i] + val) & 0xF;
        i++;
      } else if (val < 18) {
        if (i === 0) throw new RarError("bad table");
        const n = val === 16 ? br.bits(3) + 3 : br.bits(7) + 11;
        for (let j = 0; j < n && i < TABLESIZE29; j++) { tbl[i] = tbl[i - 1]; i++; }
      } else {
        const n = val === 18 ? br.bits(3) + 3 : br.bits(7) + 11;
        for (let j = 0; j < n && i < TABLESIZE29; j++) tbl[i++] = 0;
      }
    }
    this.main = new Huff(tbl.subarray(0, NC29), NC29);
    this.off = new Huff(tbl.subarray(NC29, NC29 + DC29), DC29);
    this.lowoff = new Huff(tbl.subarray(NC29 + DC29, NC29 + DC29 + LDC29), LDC29);
    this.len = new Huff(tbl.subarray(NC29 + DC29 + LDC29, TABLESIZE29), RC29);
    this.haveTables = true;
    this.startNewTable = false;
  }

  /** Decode LZ symbols until the stream position reaches `end` or the file/block ends. */
  private expand(end: number) {
    const br = this.br;
    for (;;) {
      if (this.pos >= end) return;
      if (this.isPpmd || this.fileDone) return;
      const symbol = this.main.decode(br);
      if (symbol < 256) { this.literal(symbol); continue; }
      if (symbol === 256) {
        const newFile = !br.bits(1);
        if (newFile) {
          this.startNewTable = !!br.bits(1);
          this.fileDone = true;
          return;
        }
        this.parseCodes();
        if (this.isPpmd) return;
        continue;
      }
      if (symbol === 257) {
        this.readFilter();
        continue;
      }
      let offs = 0, len = 0;
      if (symbol === 258) {
        if (this.lastLength === 0) continue;
        offs = this.lastOffset;
        len = this.lastLength;
      } else if (symbol <= 262) {
        const idx = symbol - 259;
        offs = this.oldOffset[idx];
        const lensym = this.len.decode(br);
        len = LEN_BASES[lensym] + 2 + (LEN_BITS[lensym] ? br.bits(LEN_BITS[lensym]) : 0);
        for (let i = idx; i > 0; i--) this.oldOffset[i] = this.oldOffset[i - 1];
        this.oldOffset[0] = offs;
      } else if (symbol <= 270) {
        const idx = symbol - 263;
        offs = SHORT_BASES[idx] + 1 + br.bits(SHORT_BITS[idx]);
        len = 2;
        for (let i = 3; i > 0; i--) this.oldOffset[i] = this.oldOffset[i - 1];
        this.oldOffset[0] = offs;
      } else {
        const li = symbol - 271;
        len = LEN_BASES[li] + 3 + (LEN_BITS[li] ? br.bits(LEN_BITS[li]) : 0);
        const os = this.off.decode(br);
        offs = OFF_BASES[os] + 1;
        const ob = OFF_BITS[os];
        if (ob > 0) {
          if (os > 9) {
            if (ob > 4) offs += br.bitsLong(ob - 4) << 4;
            if (this.lowOffsetRepeats > 0) {
              this.lowOffsetRepeats--;
              offs += this.lastLowOffset;
            } else {
              const lo = this.lowoff.decode(br);
              if (lo === 16) {
                this.lowOffsetRepeats = 15;
                offs += this.lastLowOffset;
              } else {
                offs += lo;
                this.lastLowOffset = lo;
              }
            }
          } else {
            offs += br.bits(ob);
          }
        }
        if (offs >= 0x40000) len++;
        if (offs >= 0x2000) len++;
        for (let i = 3; i > 0; i--) this.oldOffset[i] = this.oldOffset[i - 1];
        this.oldOffset[0] = offs;
      }
      this.lastOffset = offs;
      this.lastLength = len;
      this.copyMatch(offs, len);
    }
  }

  private readFilter() {
    const br = this.br;
    const flags = br.bits(8);
    let length = (flags & 7) + 1;
    if (length === 7) length = br.bits(8) + 7;
    else if (length === 8) length = br.bits(16);
    const code = new Uint8Array(length);
    for (let i = 0; i < length; i++) code[i] = br.bits(8);
    this.parseFilter(code, flags);
  }

  private readFilterPpm(ppmd: Ppmd7) {
    const next = () => {
      const c = ppmd.decodeSymbol();
      if (c < 0) throw new RarError("bad PPMd stream in filter");
      return c;
    };
    const flags = next();
    let length = (flags & 7) + 1;
    if (length === 7) length = next() + 7;
    else if (length === 8) { length = next() << 8; length |= next(); }
    const code = new Uint8Array(length);
    for (let i = 0; i < length; i++) code[i] = next();
    this.parseFilter(code, flags);
  }

  private parseFilter(code: Uint8Array, flags: number) {
    const br = new BitIn(code, 0);
    let num: number;
    if (flags & 0x80) {
      num = rarvmNumber(br);
      if (num === 0) {
        this.stack = [];
        this.progs = [];
      } else num--;
      if (num > this.progs.length) throw new RarError("bad filter number");
      this.lastFilterNum = num;
    } else num = this.lastFilterNum;
    let prog: Prog29 | undefined = this.progs[num];
    if (prog) prog.usageCount++;

    let blockStart = rarvmNumber(br);
    if (flags & 0x40) blockStart += 258;
    blockStart += this.pos;
    let blockLength: number;
    if (flags & 0x20) blockLength = rarvmNumber(br);
    else blockLength = prog ? prog.oldFilterLength : 0;
    if (blockLength > VM_MEMSIZE) throw new RarError("bad filter length");

    const regs = new Uint32Array(8);
    regs[3] = 0x3C000; // VM global memory address (unused by the standard filters)
    regs[4] = blockLength;
    regs[5] = prog ? prog.usageCount : 0;
    regs[7] = VM_MEMSIZE;
    if (flags & 0x10) {
      const mask = br.bits(7);
      for (let i = 0; i < 7; i++) if (mask & (1 << i)) regs[i] = rarvmNumber(br);
    }
    if (!prog) {
      if (num !== this.progs.length) throw new RarError("bad filter program number");
      const len = rarvmNumber(br);
      if (len === 0 || len > 0x10000) throw new RarError("bad filter program");
      const bytecode = new Uint8Array(len);
      for (let i = 0; i < len; i++) bytecode[i] = br.bits(8);
      let x = 0;
      for (let i = 1; i < len; i++) x ^= bytecode[i];
      if (x !== bytecode[0]) throw new RarError("bad filter checksum");
      const fp = len * 0x100000000 + crc32(0, bytecode);
      prog = { fingerprint: fp, usageCount: 0, oldFilterLength: 0 };
      this.progs.push(prog);
    }
    prog.oldFilterLength = blockLength;
    if (flags & 0x08) {
      const glen = rarvmNumber(br);
      if (glen > 0x2000) throw new RarError("bad filter global data");
      for (let i = 0; i < glen; i++) br.bits(8);
    }
    if (br.overrun) throw new RarError("truncated filter");
    rarStats.filters++;
    this.stack.push({ prog, regs, blockStart, blockLength });
  }

  private executeFilter(f: Filter29, vm: Uint8Array, pos: number): { addr: number; len: number } {
    const fp = f.prog.fingerprint;
    const length = f.regs[4];
    if (fp === FILTER_DELTA_FP) {
      const channels = f.regs[0];
      if (length > VM_MEMSIZE / 2 || channels === 0 || channels > 128) throw new RarError("bad delta filter");
      let src = 0;
      for (let i = 0; i < channels; i++) {
        let last = 0;
        for (let idx = i; idx < length; idx += channels) {
          last = (last - vm[src++]) & 0xFF;
          vm[length + idx] = last;
        }
      }
      return { addr: length, len: length };
    }
    if (fp === FILTER_E8_FP || fp === FILTER_E8E9_FP) {
      const e9 = fp === FILTER_E8E9_FP;
      const fileSize = 0x1000000;
      if (length <= 4 || length > VM_MEMSIZE) throw new RarError("bad e8 filter");
      for (let i = 0; i <= length - 5; i++) {
        const b = vm[i];
        if (b === 0xE8 || (e9 && b === 0xE9)) {
          const curpos = (pos + i + 1) >>> 0;
          const address = (vm[i + 1] | (vm[i + 2] << 8) | (vm[i + 3] << 16) | (vm[i + 4] << 24)) | 0;
          if (address < 0) {
            if (curpos >= ((~address >>> 0) + 1) >>> 0) {
              const val = (address + fileSize) >>> 0;
              vm[i + 1] = val & 0xFF; vm[i + 2] = (val >>> 8) & 0xFF; vm[i + 3] = (val >>> 16) & 0xFF; vm[i + 4] = (val >>> 24) & 0xFF;
            }
          } else if (address < fileSize) {
            const val = (address - curpos) >>> 0;
            vm[i + 1] = val & 0xFF; vm[i + 2] = (val >>> 8) & 0xFF; vm[i + 3] = (val >>> 16) & 0xFF; vm[i + 4] = (val >>> 24) & 0xFF;
          }
          i += 4;
        }
      }
      return { addr: 0, len: length };
    }
    if (fp === FILTER_RGB_FP) {
      const stride = f.regs[0], byteOffset = f.regs[1];
      if (length > VM_MEMSIZE / 2 || stride > length || length < 3 || byteOffset > 2) throw new RarError("bad rgb filter");
      let src = 0;
      const dst = length;
      for (let i = 0; i < 3; i++) {
        let byte = 0;
        let prev = dst + i - stride;
        for (let j = i; j < length; j += 3) {
          if (prev >= dst) {
            const d1 = Math.abs(vm[prev + 3] - vm[prev]);
            const d2 = Math.abs(byte - vm[prev]);
            const d3 = Math.abs(vm[prev + 3] - vm[prev] + byte - vm[prev]);
            if (d1 > d2 || d1 > d3) byte = d2 <= d3 ? vm[prev + 3] : vm[prev];
          }
          byte = (byte - vm[src++]) & 0xFF;
          vm[dst + j] = byte;
          prev += 3;
        }
      }
      for (let i = byteOffset; i < length - 2; i += 3) {
        vm[dst + i] = (vm[dst + i] + vm[dst + i + 1]) & 0xFF;
        vm[dst + i + 2] = (vm[dst + i + 2] + vm[dst + i + 1]) & 0xFF;
      }
      return { addr: length, len: length };
    }
    if (fp === FILTER_AUDIO_FP) {
      const channels = f.regs[0];
      if (length > VM_MEMSIZE / 2 || channels === 0 || channels > 128) throw new RarError("bad audio filter");
      let src = 0;
      const dst = length;
      for (let i = 0; i < channels; i++) {
        const D = new Int32Array(3);
        const K = new Int32Array(3);
        const err = new Int32Array(7);
        let lastByte = 0, lastDelta = 0, count = 0;
        for (let j = i; j < length; j += channels) {
          const delta = (vm[src++] << 24) >> 24;
          D[2] = D[1];
          D[1] = lastDelta - D[0];
          D[0] = lastDelta;
          const pred = ((8 * lastByte + K[0] * D[0] + K[1] * D[1] + K[2] * D[2]) >> 3) & 0xFF;
          const byte = (pred - delta) & 0xFF;
          const pe = delta * 8;
          err[0] += Math.abs(pe);
          err[1] += Math.abs(pe - D[0]); err[2] += Math.abs(pe + D[0]);
          err[3] += Math.abs(pe - D[1]); err[4] += Math.abs(pe + D[1]);
          err[5] += Math.abs(pe - D[2]); err[6] += Math.abs(pe + D[2]);
          lastDelta = ((byte - lastByte) << 24) >> 24;
          vm[dst + j] = byte;
          lastByte = byte;
          if (!(count++ & 0x1F)) {
            let idx = 0;
            for (let k = 1; k < 7; k++) if (err[k] < err[idx]) idx = k;
            err.fill(0);
            switch (idx) {
              case 1: if (K[0] >= -16) K[0]--; break;
              case 2: if (K[0] < 16) K[0]++; break;
              case 3: if (K[1] >= -16) K[1]--; break;
              case 4: if (K[1] < 16) K[1]++; break;
              case 5: if (K[2] >= -16) K[2]--; break;
              case 6: if (K[2] < 16) K[2]++; break;
            }
          }
        }
      }
      return { addr: length, len: length };
    }
    throw new RarError("unsupported RAR VM filter");
  }

  /** Apply the head filter chain (all stacked filters at the same block) and emit its output. */
  private applyHeadFilter() {
    const filter = this.stack[0];
    const start = filter.blockStart;
    const end = start + filter.blockLength;
    if (!this.vmMem) this.vmMem = new Uint8Array(VM_MEMSIZE + 4);
    const vm = this.vmMem;
    for (let i = 0; i < filter.blockLength; i++) vm[i] = this.win[(start + i) & WINMASK29];
    let r = this.executeFilter(filter, vm, this.offset - this.fileStart);
    this.stack.shift();
    while (this.stack.length && this.stack[0].blockStart === start && this.stack[0].blockLength === r.len) {
      vm.copyWithin(0, r.addr, r.addr + r.len);
      const f2 = this.stack.shift()!;
      r = this.executeFilter(f2, vm, this.offset - this.fileStart);
    }
    const outLen = Math.max(0, Math.min(r.len, this.fileEnd - this.offset));
    if (outLen > 0) this.sink.emit(vm.slice(r.addr, r.addr + outLen));
    this.offset = end;
  }

  /** Flush output through the filter queue up to min(pos, fileEnd). */
  private flush() {
    const upTo = Math.min(this.pos, this.fileEnd);
    for (;;) {
      if (this.stack.length) {
        const f = this.stack[0];
        if (f.blockStart < this.offset) throw new RarError("filter behind output");
        if (f.blockStart < upTo || (f.blockStart === this.offset && this.pos >= f.blockStart + f.blockLength)) {
          if (f.blockStart > this.offset) {
            this.emitWindow(this.offset, f.blockStart);
            this.offset = f.blockStart;
          }
          if (this.pos >= f.blockStart + f.blockLength) {
            this.applyHeadFilter();
            continue;
          }
          return; // waiting for the filter block to fill
        }
      }
      if (upTo > this.offset) {
        this.emitWindow(this.offset, upTo);
        this.offset = upTo;
      }
      return;
    }
  }

  /** Unpack one file's data. `packed` = this file's packed bytes (volume parts concatenated). */
  unpackFile(packed: Uint8Array, unpSize: number, solid: boolean, sink: FileSink) {
    if (!solid) this.resetHard();
    if (solid && this.br) {
      // A solid stream continues bit-exactly across file boundaries: keep the
      // unconsumed tail of the previous file's packed data in front.
      const rem = this.br.buf.subarray(this.br.pos);
      const merged = new Uint8Array(rem.length + packed.length);
      merged.set(rem, 0);
      merged.set(packed, rem.length);
      const bit = this.br.bit;
      this.br = new BitIn(merged, 0);
      this.br.bit = bit;
    } else {
      this.br = new BitIn(packed, 0);
    }
    this.sink = sink;
    this.ppmdEod = false;
    this.fileDone = false;
    if (this.ppmd) this.ppmd.byteIn = () => this.br.readByte();
    // pending filters never span solid files (programs do)
    this.stack = [];
    // A solid stream is one continuous output; each file is a size-slice of it.
    // The decoder may already have produced bytes past the previous file's end
    // (a match can cross the boundary) — those belong to this file.
    const fileStart = this.solidEnd;
    this.fileStart = fileStart;
    this.fileEnd = fileStart + unpSize;
    this.solidEnd = this.fileEnd;
    this.offset = fileStart;
    if (this.startNewTable) this.parseCodes();
    if (!this.haveTables && !this.isPpmd) throw new RarError("no tables");

    let stall = 0;
    while (this.offset < this.fileEnd) {
      if (this.ppmdEod || this.fileDone) break;
      if (this.br.overrun) throw new RarError("truncated packed data");
      const beforePos = this.pos, beforeOff = this.offset;
      if (this.isPpmd) {
        this.decodePpmdRun();
      } else {
        // don't let unflushed data exceed the window
        const end = Math.min(this.offset + WINSIZE29 - MAXMATCH29, this.fileEnd);
        this.expand(end);
      }
      this.flush();
      if (this.pos === beforePos && this.offset === beforeOff) {
        if (++stall > 4) throw new RarError("no progress in packed stream");
      } else stall = 0;
    }
    this.flush();
    if (this.offset < this.fileEnd) throw new RarError("packed stream ended early");
    // Consume this file's end-of-stream marker (the 256-newfile symbol, or the
    // PPMd escape+2 pair) if the stream has one exactly here. A symbol that
    // produces data instead is fine — it belongs to the next slice and stays
    // in the window.
    if (!this.fileDone && !this.ppmdEod && this.pos <= this.fileEnd && !this.br.overrun) {
      try {
        if (this.isPpmd) this.ppmdStep();
        else this.expand(this.pos + 1);
      } catch { /* stream ends without a marker: fine for the last file */ }
    }
  }

  /** Decode one PPMd symbol group. Returns whether it produced data or hit a control code. */
  private ppmdStep(): "data" | "tables" | "eod" {
    const ppmd = this.ppmd!;
    const c = ppmd.decodeSymbol();
    if (c < 0) throw new RarError("bad PPMd data");
    if (c !== this.ppmdEscape) {
      this.literal(c);
      return "data";
    }
    const code = ppmd.decodeSymbol();
    if (code < 0) throw new RarError("bad PPMd data");
    if (code === 3) { this.readFilterPpm(ppmd); return "data"; }
    if (code === 4) {
      let dist = 0;
      for (let i = 0; i < 3; i++) {
        const d = ppmd.decodeSymbol();
        if (d < 0) throw new RarError("bad PPMd data");
        dist = (dist << 8) + d;
      }
      const l = ppmd.decodeSymbol();
      if (l < 0) throw new RarError("bad PPMd data");
      this.copyMatch(dist + 2, l + 32);
      return "data";
    }
    if (code === 5) {
      const l = ppmd.decodeSymbol();
      if (l < 0) throw new RarError("bad PPMd data");
      this.copyMatch(1, l + 4);
      return "data";
    }
    this.literal(c);
    return "data";
  }

  private decodePpmdRun() {
    const budget = 0x20000;
    const start = this.pos;
    while (this.pos - start < budget) {
      if (this.ppmdStep() !== "data") return;
      if (this.pos >= this.fileEnd) return;
    }
  }
}

/* ================= unpack v5.0 (RAR5) ================= */

const NC50 = 306, DC50 = 64, LDC50 = 16, RC50 = 44, BC50 = 20;
const TABLESIZE50 = NC50 + DC50 + LDC50 + RC50;
const MAX_WIN50 = 64 << 20;

interface Filter50 { blockStart: number; blockLength: number; type: number; channels: number; }

class Unpack50 {
  win: Uint8Array | null = null;
  winSize = 0;
  winMask = 0;
  wrPtr = 0;      // global unpacked stream position
  lastWr = 0;     // flushed position
  distCache = new Int32Array([-1, -1, -1, -1]);
  lastLen = 0;
  bd!: Huff; ld!: Huff; dd!: Huff; ldd!: Huff; rd!: Huff;
  haveTables = false;
  filters: Filter50[] = [];
  sink!: FileSink;
  fileEnd = 0;
  fileStart = 0;

  ensureWindow(dictBits: number) {
    const size = 0x20000 << dictBits;
    if (size > MAX_WIN50) throw new RarError("RAR5 dictionary too large (>64 MB)");
    if (!this.win || this.winSize < size) {
      const old = this.win;
      const w = new Uint8Array(size);
      if (old) {
        for (let i = Math.max(0, this.wrPtr - this.winSize); i < this.wrPtr; i++) {
          w[i & (size - 1)] = old[i & this.winMask];
        }
      }
      this.win = w;
      this.winSize = size;
      this.winMask = size - 1;
    }
  }

  resetHard() {
    this.wrPtr = 0; this.lastWr = 0;
    this.distCache.set([-1, -1, -1, -1]);
    this.lastLen = 0;
    this.filters = [];
    this.haveTables = false;
  }

  private emitWindow(from: number, to: number) {
    const w = this.win!;
    const cutTo = Math.min(to, this.fileEnd);
    let f = from;
    while (f < cutTo) {
      const wo = f & this.winMask;
      const run = Math.min(cutTo - f, this.winSize - wo);
      this.sink.emit(w.slice(wo, wo + run));
      f += run;
    }
  }

  private parseTables(br: BitIn) {
    const bitLengths = new Uint8Array(BC50);
    for (let i = 0; i < BC50;) {
      const v = br.bits(4);
      if (v === 15) {
        const z = br.bits(4);
        if (z === 0) bitLengths[i++] = 15;
        else { for (let k = 0; k < z + 2 && i < BC50; k++) bitLengths[i++] = 0; }
      } else bitLengths[i++] = v;
    }
    this.bd = new Huff(bitLengths, BC50);
    const table = new Uint8Array(TABLESIZE50);
    for (let i = 0; i < TABLESIZE50;) {
      const num = this.bd.decode(br);
      if (num < 16) { table[i++] = num; }
      else if (num < 18) {
        const n = num === 16 ? br.bits(3) + 3 : br.bits(7) + 11;
        if (i === 0) throw new RarError("bad RAR5 table");
        for (let k = 0; k < n && i < TABLESIZE50; k++) { table[i] = table[i - 1]; i++; }
      } else {
        const n = num === 18 ? br.bits(3) + 3 : br.bits(7) + 11;
        for (let k = 0; k < n && i < TABLESIZE50; k++) table[i++] = 0;
      }
    }
    this.ld = new Huff(table.subarray(0, NC50), NC50);
    this.dd = new Huff(table.subarray(NC50, NC50 + DC50), DC50);
    this.ldd = new Huff(table.subarray(NC50 + DC50, NC50 + DC50 + LDC50), LDC50);
    this.rd = new Huff(table.subarray(NC50 + DC50 + LDC50, TABLESIZE50), RC50);
    this.haveTables = true;
  }

  private literal(b: number) {
    this.win![this.wrPtr & this.winMask] = b;
    this.wrPtr++;
  }

  private copyMatch(len: number, dist: number) {
    const w = this.win!, mask = this.winMask;
    let p = this.wrPtr;
    for (let i = 0; i < len; i++) {
      w[p & mask] = w[(p - dist) & mask];
      p++;
    }
    this.wrPtr = p;
  }

  private codeLength(br: BitIn, code: number): number {
    let length = 2;
    if (code < 8) length += code;
    else {
      const lbits = (code >> 2) - 1;
      length += (4 | (code & 3)) << lbits;
      if (lbits > 0) length += br.bitsLong(lbits);
    }
    return length;
  }

  private readFilterValue(br: BitIn): number {
    const byteCount = br.bits(2) + 1;
    let v = 0;
    for (let i = 0; i < byteCount; i++) v += br.bits(8) << (i * 8);
    return v >>> 0;
  }

  private parseFilter(br: BitIn) {
    const blockStart = this.readFilterValue(br);
    const blockLength = this.readFilterValue(br);
    const type = br.bits(3);
    if (blockLength < 4 || blockLength > 0x400000 || blockLength > this.winSize >> 1) {
      throw new RarError("invalid RAR5 filter");
    }
    if (type > 3) throw new RarError("unsupported RAR5 filter type");
    const f: Filter50 = { blockStart: this.wrPtr + blockStart, blockLength, type, channels: 0 };
    if (type === 0) f.channels = br.bits(5) + 1; // DELTA
    this.filters.push(f);
  }

  private runFilter(f: Filter50): Uint8Array {
    const w = this.win!, mask = this.winMask;
    const out = new Uint8Array(f.blockLength);
    if (f.type === 0) { // DELTA
      let src = 0;
      for (let i = 0; i < f.channels; i++) {
        let prev = 0;
        for (let d = i; d < f.blockLength; d += f.channels) {
          prev = (prev - w[(f.blockStart + src) & mask]) & 0xFF;
          out[d] = prev;
          src++;
        }
      }
      return out;
    }
    if (f.type === 1 || f.type === 2) { // E8 / E8E9
      const ext = f.type === 2;
      const fileSize = 0x1000000;
      for (let i = 0; i < f.blockLength; i++) out[i] = w[(f.blockStart + i) & mask];
      let i = 0;
      while (i < f.blockLength - 4) {
        const b = w[(f.blockStart + i++) & mask];
        if (b === 0xE8 || (ext && b === 0xE9)) {
          const off = ((i + f.blockStart - this.fileStart) % fileSize) >>> 0;
          const p = f.blockStart + i;
          const addr = (w[p & mask] | (w[(p + 1) & mask] << 8) | (w[(p + 2) & mask] << 16) | (w[(p + 3) & mask] << 24)) >>> 0;
          if (addr & 0x80000000) {
            if ((((addr + off) >>> 0) & 0x80000000) === 0) {
              const v = (addr + fileSize) >>> 0;
              out[i] = v & 0xFF; out[i + 1] = (v >>> 8) & 0xFF; out[i + 2] = (v >>> 16) & 0xFF; out[i + 3] = (v >>> 24) & 0xFF;
            }
          } else {
            if ((((addr - fileSize) >>> 0) & 0x80000000) !== 0) {
              const v = (addr - off) >>> 0;
              out[i] = v & 0xFF; out[i + 1] = (v >>> 8) & 0xFF; out[i + 2] = (v >>> 16) & 0xFF; out[i + 3] = (v >>> 24) & 0xFF;
            }
          }
          i += 4;
        }
      }
      return out;
    }
    // ARM
    for (let i = 0; i < f.blockLength; i++) out[i] = w[(f.blockStart + i) & mask];
    for (let i = 0; i + 3 < f.blockLength; i += 4) {
      if (w[(f.blockStart + i + 3) & mask] === 0xEB) {
        const p = f.blockStart + i;
        let off = (w[p & mask] | (w[(p + 1) & mask] << 8) | (w[(p + 2) & mask] << 16)) & 0xFFFFFF;
        off = (off - Math.floor((i + f.blockStart - this.fileStart) / 4)) & 0xFFFFFF;
        out[i] = off & 0xFF; out[i + 1] = (off >>> 8) & 0xFF; out[i + 2] = (off >>> 16) & 0xFF; out[i + 3] = 0xEB;
      }
    }
    return out;
  }

  /** Flush unpacked data through the filter queue up to the current write position. */
  private drain() {
    for (;;) {
      if (this.filters.length) {
        const f = this.filters[0];
        if (f.blockStart < this.lastWr) throw new RarError("RAR5 filter behind output");
        if (f.blockStart > this.lastWr) {
          const rawTo = Math.min(this.wrPtr, f.blockStart);
          if (rawTo > this.lastWr) {
            this.emitWindow(this.lastWr, rawTo);
            this.lastWr = rawTo;
          }
        }
        if (this.lastWr === f.blockStart && this.wrPtr >= f.blockStart + f.blockLength) {
          const out = this.runFilter(f);
          this.filters.shift();
          const keep = Math.max(0, Math.min(out.length, this.fileEnd - this.lastWr));
          if (keep > 0) this.sink.emit(keep === out.length ? out : out.slice(0, keep));
          this.lastWr = f.blockStart + f.blockLength;
          continue;
        }
        return;
      }
      if (this.wrPtr > this.lastWr) {
        this.emitWindow(this.lastWr, this.wrPtr);
        this.lastWr = this.wrPtr;
      }
      return;
    }
  }

  unpackFile(packed: Uint8Array, unpSize: number, solid: boolean, dictBits: number, sink: FileSink) {
    if (!solid) this.resetHard();
    this.ensureWindow(dictBits);
    this.sink = sink;
    const fileStart = this.wrPtr;
    this.fileStart = fileStart;
    this.fileEnd = fileStart + unpSize;
    this.lastWr = this.wrPtr;
    let pos = 0;

    for (;;) {
      if (this.lastWr >= this.fileEnd) break;
      if (pos + 2 > packed.length) throw new RarError("truncated RAR5 data");
      const flags = packed[pos];
      const cksum = packed[pos + 1];
      const byteCount = (flags >> 3) & 7;
      if (byteCount > 2) throw new RarError("bad RAR5 block header");
      let blockSize = 0;
      for (let i = 0; i <= byteCount; i++) blockSize |= packed[pos + 2 + i] << (i * 8);
      let calc = 0x5A ^ flags;
      for (let i = 0; i <= byteCount; i++) calc ^= packed[pos + 2 + i];
      if ((calc & 0xFF) !== cksum) throw new RarError("RAR5 block checksum error");
      const bitSize = (flags & 7) + 1;
      const lastBlock = !!(flags & 0x40);
      const tablePresent = !!(flags & 0x80);
      pos += 2 + byteCount + 1;
      const block = packed.subarray(pos, pos + blockSize);
      const br = new BitIn(block, 0);
      if (tablePresent) this.parseTables(br);
      if (!this.haveTables) throw new RarError("RAR5 block without tables");

      for (;;) {
        if (br.pos > blockSize - 1 || (br.pos === blockSize - 1 && br.bit >= bitSize)) break;
        if (br.overrun) throw new RarError("truncated RAR5 block");
        const num = this.ld.decode(br);
        if (num < 256) {
          this.literal(num);
        } else if (num >= 262) {
          const len0 = this.codeLength(br, num - 262);
          const slot = this.dd.decode(br);
          let dist = 1;
          if (slot < 4) { dist += slot; }
          else {
            const dbits = (slot >> 1) - 1;
            dist += (2 | (slot & 1)) * Math.pow(2, dbits);
            if (dbits >= 4) {
              if (dbits > 4) dist += br.bitsLong(dbits - 4) * 16;
              dist += this.ldd.decode(br);
            } else if (dbits > 0) {
              dist += br.bits(dbits);
            }
          }
          let len = len0;
          if (dist > 0x100) { len++; if (dist > 0x2000) { len++; if (dist > 0x40000) len++; } }
          this.distCache[3] = this.distCache[2]; this.distCache[2] = this.distCache[1]; this.distCache[1] = this.distCache[0];
          this.distCache[0] = dist;
          this.lastLen = len;
          this.copyMatch(len, dist);
        } else if (num === 256) {
          this.parseFilter(br);
        } else if (num === 257) {
          if (this.lastLen !== 0) this.copyMatch(this.lastLen, this.distCache[0]);
        } else { // 258..261
          const idx = num - 258;
          const dist = this.distCache[idx];
          for (let i = idx; i > 0; i--) this.distCache[i] = this.distCache[i - 1];
          this.distCache[0] = dist;
          const slot = this.rd.decode(br);
          const len = this.codeLength(br, slot);
          this.lastLen = len;
          this.copyMatch(len, dist);
        }
        if (this.wrPtr - this.lastWr > this.winSize - 0x1000) this.drain();
      }
      pos += blockSize;
      this.drain();
      if (lastBlock && (this.lastWr >= this.fileEnd || pos >= packed.length)) break;
    }
    this.drain();
    if (this.lastWr < this.fileEnd) throw new RarError("RAR5 stream ended early");
    if (this.wrPtr < this.fileEnd) this.wrPtr = this.fileEnd;
  }
}

/* ================= volume ordering + extraction ================= */

function volumeSortKey(name: string): { base: string; num: number } {
  const n = name.toLowerCase();
  let m = n.match(/^(.*?)\.part(\d+)\.rar$/);
  if (m) return { base: m[1], num: parseInt(m[2], 10) };
  m = n.match(/^(.*?)\.r(\d\d)$/);
  if (m) return { base: m[1], num: parseInt(m[2], 10) + 1 };
  m = n.match(/^(.*?)\.(rar|exe)$/);
  if (m) return { base: m[1], num: 0 };
  return { base: n, num: 0 };
}

export function isRarName(name: string): boolean {
  return /\.rar$/i.test(name) || /\.r\d\d$/i.test(name);
}

/** True when the picked set of names forms a single RAR volume set. */
export function isRarVolumeSet(names: string[]): boolean {
  if (!names.length) return false;
  if (!names.every((n) => isRarName(n))) return false;
  const bases = new Set(names.map((n) => volumeSortKey(n.split("/").pop() ?? n).base));
  return bases.size === 1;
}

export async function extractRar(
  volumes: { name: string; bytes: Uint8Array }[],
  onFile: (entry: RarEntry, data: Uint8Array) => void | Promise<void>,
  onProgress?: (text: string) => void,
  opts?: { ignoreCrc?: boolean },
): Promise<RarResult> {
  if (!volumes.length) throw new RarError("no data");
  const sorted = volumes.slice().sort((a, b) => {
    const ka = volumeSortKey(a.name.split("/").pop() ?? a.name);
    const kb = volumeSortKey(b.name.split("/").pop() ?? b.name);
    return ka.num - kb.num;
  });
  const sig0 = findRarSignature(sorted[0].bytes);
  if (!sig0) throw new RarError("not a RAR archive");
  const v5 = sig0.v5;

  const vols: ParsedVolume[] = [];
  for (let i = 0; i < sorted.length; i++) {
    const sig = i === 0 ? sig0 : findRarSignature(sorted[i].bytes);
    if (!sig) throw new RarError(`${sorted[i].name}: not a RAR volume`);
    if (sig.v5 !== v5) throw new RarError("mixed RAR versions in volume set");
    vols.push(v5 ? parseRar5Volume(i, sorted[i].bytes, sig.offset) : parseRar4Volume(i, sorted[i].bytes, sig.offset));
  }
  if (vols[vols.length - 1].hasNextVolume) {
    throw new RarError(`missing volume: the set continues after ${sorted[sorted.length - 1].name}`);
  }

  // merge split files across volumes
  const files: ParsedFile[] = [];
  for (let vi = 0; vi < vols.length; vi++) {
    for (const f of vols[vi].files) {
      if (f.splitBefore) {
        const prev = files.length ? files[files.length - 1] : null;
        if (prev && prev.splitAfter && prev.path === f.path) {
          prev.segs.push(...f.segs);
          prev.splitAfter = f.splitAfter;
          if (f.hasCrc) { prev.crc = f.crc; prev.hasCrc = true; }
          continue;
        }
        throw new RarError(`missing earlier volume for ${f.path} — import the whole set`);
      }
      files.push({ ...f, segs: f.segs.slice() });
    }
  }
  for (const f of files) {
    if (f.splitAfter) throw new RarError(`missing later volume for ${f.path} — import the whole set`);
  }

  const warnings: string[] = [];
  let count = 0;
  const up29 = new Unpack29();
  const up50 = new Unpack50();
  let solidBroken = false;

  for (const f of files) {
    if (f.isDir) {
      await onFile({ path: f.path, isDir: true, size: 0 }, new Uint8Array(0));
      continue;
    }
    if (f.encrypted) { warnings.push(`${f.path}: skipped (password-protected)`); continue; }
    if (f.unpSize < 0) { warnings.push(`${f.path}: skipped (unknown size)`); continue; }
    if (f.solid && solidBroken) { warnings.push(`${f.path}: skipped (earlier solid data failed)`); continue; }
    let packedLen = 0;
    for (const s of f.segs) packedLen += s.len;
    const packed = new Uint8Array(packedLen);
    {
      let o = 0;
      for (const s of f.segs) {
        packed.set(sorted[s.vol].bytes.subarray(s.off, s.off + s.len), o);
        o += s.len;
      }
    }
    onProgress?.(`Extracting ${f.path}`);
    const sink = new FileSink();
    try {
      const stored = v5 ? f.method === 0 : f.method === 0x30;
      if (stored) {
        sink.emit(packed.subarray(0, f.unpSize).slice());
      } else if (v5) {
        if (f.unpVer !== 50) throw new RarError(`unsupported RAR5 algorithm version ${f.unpVer - 50}`);
        up50.unpackFile(packed, f.unpSize, f.solid, f.dictBits, sink);
      } else {
        if (f.unpVer > 36) throw new RarError(`unsupported compression version ${f.unpVer}`);
        if (f.unpVer < 26 && f.method !== 0x30) {
          throw new RarError(`RAR ${f.unpVer < 20 ? "1.5" : "2.0"} compression not supported`);
        }
        if (f.solid) throw new RarError("solid RAR archive — not supported yet");
        up29.unpackFile(packed, f.unpSize, f.solid, sink);
      }
      const data = sink.finish();
      if (!opts?.ignoreCrc) {
        if (data.length !== f.unpSize) throw new RarError(`size mismatch (${data.length} != ${f.unpSize})`);
        if (f.hasCrc && sink.crc !== f.crc) throw new RarError("CRC error");
      }
      await onFile({ path: f.path, isDir: false, size: data.length }, data);
      count++;
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      warnings.push(`${f.path}: ${msg}`);
      if (f.solid) solidBroken = true;
      if (e instanceof RarError || e instanceof RangeError) continue;
      throw e;
    }
  }
  return { files: count, warnings };
}

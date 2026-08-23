// FAT12/16 driver for EXISTING volumes (the live C: image): directory walk, file create/write,
// subdirectory create, free-cluster allocation. Sector access goes through a small interface so
// it works on the worker's SparseImage and on plain Uint8Array images in tests.
import { fatDateTime, shortName, unpackName } from "./fat.ts";

export interface SectorIO {
  readSectors(lba: number, count: number, dst: Uint8Array): boolean;
  writeSectors(lba: number, count: number, src: Uint8Array): boolean;
}

export class ArraySectorIO implements SectorIO {
  constructor(public bytes: Uint8Array) {}
  readSectors(lba: number, count: number, dst: Uint8Array) { dst.set(this.bytes.subarray(lba * 512, (lba + count) * 512)); return true; }
  writeSectors(lba: number, count: number, src: Uint8Array) { this.bytes.set(src.subarray(0, count * 512), lba * 512); return true; }
}

export interface DirEntry { name: string; attr: number; cluster: number; size: number; index: number; dirSector: number; dirOffset: number; }

const SECTOR = 512;

export class FatFs {
  base = 0;
  spc = 0; reserved = 0; fats = 0; rootEntries = 0; spf = 0; fat16 = true;
  totalSectors = 0; clusters = 0;
  private fat!: Uint8Array;
  private fatDirty = false;
  private nextHint = 2;

  constructor(public io: SectorIO) {}

  /** Mount the first partition of an MBR image, or a bare volume at sector 0. */
  mount(): this {
    const s0 = new Uint8Array(SECTOR);
    this.io.readSectors(0, 1, s0);
    const r16 = (b: Uint8Array, o: number) => b[o] | (b[o + 1] << 8);
    const r32 = (b: Uint8Array, o: number) => (r16(b, o) | (r16(b, o + 2) << 16)) >>> 0;
    let bs = s0;
    if (!(s0[0] === 0xEB || s0[0] === 0xE9) || r16(s0, 0x0B) !== 512) {
      // MBR: first partition entry
      this.base = r32(s0, 0x1BE + 8);
      bs = new Uint8Array(SECTOR);
      this.io.readSectors(this.base, 1, bs);
    }
    this.spc = bs[0x0D]; this.reserved = r16(bs, 0x0E); this.fats = bs[0x10]; this.rootEntries = r16(bs, 0x11);
    this.totalSectors = r16(bs, 0x13) || r32(bs, 0x20);
    this.spf = r16(bs, 0x16);
    const dataSectors = this.totalSectors - this.reserved - this.fats * this.spf - this.rootSectors;
    this.clusters = Math.floor(dataSectors / this.spc);
    this.fat16 = this.clusters >= 4085;
    this.fat = new Uint8Array(this.spf * SECTOR);
    this.io.readSectors(this.base + this.reserved, this.spf, this.fat);
    return this;
  }

  get rootSectors() { return Math.ceil(this.rootEntries * 32 / SECTOR); }
  get rootStart() { return this.base + this.reserved + this.fats * this.spf; }
  get dataStart() { return this.rootStart + this.rootSectors; }
  get eoc() { return this.fat16 ? 0xFFFF : 0xFFF; }
  clusterSector(c: number) { return this.dataStart + (c - 2) * this.spc; }

  getFat(c: number): number {
    if (this.fat16) return this.fat[c * 2] | (this.fat[c * 2 + 1] << 8);
    const i = Math.floor(c * 1.5);
    const v = this.fat[i] | (this.fat[i + 1] << 8);
    return (c & 1) ? v >> 4 : v & 0xFFF;
  }
  setFat(c: number, v: number) {
    if (this.fat16) { this.fat[c * 2] = v & 0xFF; this.fat[c * 2 + 1] = (v >> 8) & 0xFF; }
    else {
      const i = Math.floor(c * 1.5);
      if (c & 1) { this.fat[i] = (this.fat[i] & 0x0F) | ((v << 4) & 0xF0); this.fat[i + 1] = (v >> 4) & 0xFF; }
      else { this.fat[i] = v & 0xFF; this.fat[i + 1] = (this.fat[i + 1] & 0xF0) | ((v >> 8) & 0x0F); }
    }
    this.fatDirty = true;
  }
  isEoc(v: number) { return this.fat16 ? v >= 0xFFF8 : v >= 0xFF8; }

  freeClusterCount(): number {
    let n = 0;
    for (let c = 2; c < this.clusters + 2; c++) if (this.getFat(c) === 0) n++;
    return n;
  }

  private allocCluster(): number {
    for (let i = 0; i < this.clusters; i++) {
      const c = 2 + ((this.nextHint - 2 + i) % this.clusters);
      if (this.getFat(c) === 0) { this.nextHint = c + 1; this.setFat(c, this.eoc); return c; }
    }
    throw new Error("disk full");
  }

  flush() {
    if (!this.fatDirty) return;
    for (let f = 0; f < this.fats; f++) this.io.writeSectors(this.base + this.reserved + f * this.spf, this.spf, this.fat);
    this.fatDirty = false;
  }

  /* ---------------- directories ---------------- */
  /** Sectors that make up a directory (root or cluster chain). */
  private dirSectors(cluster: number): number[] {
    if (cluster === 0) return Array.from({ length: this.rootSectors }, (_, i) => this.rootStart + i);
    const out: number[] = [];
    let c = cluster, guard = 0;
    while (c >= 2 && !this.isEoc(c) && guard++ < 65536) {
      for (let i = 0; i < this.spc; i++) out.push(this.clusterSector(c) + i);
      c = this.getFat(c);
    }
    return out;
  }

  list(cluster: number): DirEntry[] {
    const out: DirEntry[] = [];
    const buf = new Uint8Array(SECTOR);
    let index = 0;
    for (const s of this.dirSectors(cluster)) {
      this.io.readSectors(s, 1, buf);
      for (let o = 0; o < SECTOR; o += 32, index++) {
        const first = buf[o];
        if (first === 0) return out;
        if (first === 0xE5) continue;
        const attr = buf[o + 11];
        if ((attr & 0x0F) === 0x0F) continue; // long-name entries (none expected on 4.01 volumes)
        out.push({ name: unpackName(buf.subarray(o, o + 11)), attr, cluster: buf[o + 26] | (buf[o + 27] << 8), size: (buf[o + 28] | (buf[o + 29] << 8) | (buf[o + 30] << 16) | (buf[o + 31] << 24)) >>> 0, index, dirSector: s, dirOffset: o });
      }
    }
    return out;
  }

  find(cluster: number, name: string): DirEntry | undefined {
    const u = name.toUpperCase();
    return this.list(cluster).find((e) => e.name === u && !(e.attr & 0x08));
  }

  /** Resolve "GAMES\\FOO" to a directory cluster (0 = root); undefined if missing. */
  resolveDir(path: string): number | undefined {
    let c = 0;
    for (const part of path.split(/[\\/]/).filter(Boolean)) {
      const e = this.find(c, part);
      if (!e || !(e.attr & 0x10)) return undefined;
      c = e.cluster;
    }
    return c;
  }

  /** Find a free 32-byte slot in a directory, growing it by a cluster if needed. Returns [sector, offset]. */
  private freeSlot(cluster: number): [number, number] {
    const buf = new Uint8Array(SECTOR);
    const sectors = this.dirSectors(cluster);
    for (const s of sectors) {
      this.io.readSectors(s, 1, buf);
      for (let o = 0; o < SECTOR; o += 32) if (buf[o] === 0 || buf[o] === 0xE5) return [s, o];
    }
    if (cluster === 0) throw new Error("root directory full");
    // grow
    let last = cluster;
    while (!this.isEoc(this.getFat(last))) last = this.getFat(last);
    const nc = this.allocCluster();
    this.setFat(last, nc);
    const zero = new Uint8Array(SECTOR * this.spc);
    this.io.writeSectors(this.clusterSector(nc), this.spc, zero);
    return [this.clusterSector(nc), 0];
  }

  private writeEntry(sector: number, offset: number, name83: string, attr: number, cluster: number, size: number, when: Date) {
    const buf = new Uint8Array(SECTOR);
    this.io.readSectors(sector, 1, buf);
    const raw = buf.subarray(offset, offset + 32);
    raw.fill(0);
    const packed = new Uint8Array(11).fill(0x20);
    if (name83 === "." || name83 === "..") for (let i = 0; i < name83.length; i++) packed[i] = 0x2E;
    else {
      const [b, e = ""] = name83.split(".");
      for (let i = 0; i < 8 && i < b.length; i++) packed[i] = b.charCodeAt(i);
      for (let i = 0; i < 3 && i < e.length; i++) packed[8 + i] = e.charCodeAt(i);
    }
    raw.set(packed, 0);
    raw[11] = attr;
    const { date, time } = fatDateTime(when);
    raw[22] = time & 0xFF; raw[23] = time >> 8; raw[24] = date & 0xFF; raw[25] = date >> 8;
    raw[16] = date & 0xFF; raw[17] = date >> 8;
    raw[26] = cluster & 0xFF; raw[27] = (cluster >> 8) & 0xFF;
    raw[28] = size & 0xFF; raw[29] = (size >> 8) & 0xFF; raw[30] = (size >> 16) & 0xFF; raw[31] = (size >>> 24) & 0xFF;
    this.io.writeSectors(sector, 1, buf);
  }

  private takenNames(cluster: number): Set<string> { return new Set(this.list(cluster).map((e) => e.name)); }

  /** Create a subdirectory (returns its cluster; existing directory is returned as-is). */
  mkdir(parentCluster: number, name: string, when = new Date()): { cluster: number; shortName: string } {
    const existing = this.list(parentCluster).find((e) => e.name === name.toUpperCase() && (e.attr & 0x10));
    if (existing) return { cluster: existing.cluster, shortName: existing.name };
    const sn = shortName(name, this.takenNames(parentCluster));
    const c = this.allocCluster();
    const zero = new Uint8Array(SECTOR * this.spc);
    this.io.writeSectors(this.clusterSector(c), this.spc, zero);
    const [s, o] = this.freeSlot(parentCluster);
    this.writeEntry(s, o, sn, 0x10, c, 0, when);
    const first = this.clusterSector(c);
    this.writeEntry(first, 0, ".", 0x10, c, 0, when);
    this.writeEntry(first, 32, "..", 0x10, parentCluster, 0, when);
    return { cluster: c, shortName: sn };
  }

  /** Create or replace a file. Returns the 8.3 name used. */
  writeFile(dirCluster: number, name: string, data: Uint8Array, when = new Date()): string {
    const existing = this.list(dirCluster).find((e) => e.name === name.toUpperCase() && !(e.attr & 0x10));
    if (existing) this.freeChain(existing.cluster);
    const sn = existing ? existing.name : shortName(name, this.takenNames(dirCluster));
    const clusterBytes = this.spc * SECTOR;
    const n = Math.ceil(data.length / clusterBytes);
    if (n > this.freeClusterCount()) throw new Error("disk full");
    let first = 0, prev = 0;
    const padded = new Uint8Array(clusterBytes);
    for (let i = 0; i < n; i++) {
      const c = this.allocCluster();
      if (prev) this.setFat(prev, c); else first = c;
      prev = c;
      padded.fill(0);
      padded.set(data.subarray(i * clusterBytes, Math.min((i + 1) * clusterBytes, data.length)));
      this.io.writeSectors(this.clusterSector(c), this.spc, padded);
    }
    if (existing) this.writeEntry(existing.dirSector, existing.dirOffset, sn, 0x20, first, data.length, when);
    else { const [s, o] = this.freeSlot(dirCluster); this.writeEntry(s, o, sn, 0x20, first, data.length, when); }
    return sn;
  }

  readFile(e: DirEntry): Uint8Array {
    const out = new Uint8Array(e.size);
    const clusterBytes = this.spc * SECTOR;
    const buf = new Uint8Array(clusterBytes);
    let c = e.cluster, pos = 0, guard = 0;
    while (c >= 2 && !this.isEoc(c) && pos < e.size && guard++ < 65536) {
      this.io.readSectors(this.clusterSector(c), this.spc, buf);
      const n = Math.min(clusterBytes, e.size - pos);
      out.set(buf.subarray(0, n), pos);
      pos += n;
      c = this.getFat(c);
    }
    return out;
  }

  private freeChain(first: number) {
    let c = first, guard = 0;
    while (c >= 2 && !this.isEoc(c) && guard++ < 65536) { const n = this.getFat(c); this.setFat(c, 0); c = n; }
  }

  /** Ensure a directory path exists (e.g. "GAMES\\MYTHING"); returns the cluster and the 8.3 path. */
  ensurePath(path: string, when = new Date()): { cluster: number; dosPath: string } {
    let c = 0;
    const parts: string[] = [];
    for (const part of path.split(/[\\/]/).filter(Boolean)) {
      const r = this.mkdir(c, part, when);
      c = r.cluster;
      parts.push(r.shortName);
    }
    return { cluster: c, dosPath: "C:\\" + parts.join("\\") };
  }
}

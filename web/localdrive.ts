// Local game drive: a host folder exposed to DOS as the second hard disk (BIOS 81h → D:).
// The volume is synthetic: MBR, boot record, FATs, root and directory clusters are generated
// into a sparse metadata image; file DATA clusters map straight onto the real files and are
// fetched lazily in 64 KB blocks (read-ahead + LRU cache). A read that is not resident yet
// returns "pending" (2) — the BIOS rewinds INT 13h and halts until the block arrives.
// DOS writes land in a per-folder sector overlay (the real folder is never modified), which
// persists so saves survive; the overlay is keyed to a folder signature and dropped when the
// folder's contents changed externally.
// Works over the File System Access API in the browser and over Deno files in the test rigs.
import { fatDateTime, MBR_CODE, planVolume, shortName, type VolumeGeometry } from "./fat.ts";
import { partEntry } from "./hdd.ts";
import { SparseImage } from "./store.ts";

const SECTOR = 512;
const BLOCK = 65536;              // lazy-read granularity (file-offset aligned)
const READ_AHEAD = 8;             // blocks fetched beyond the one that missed
const CACHE_BLOCKS = 256;         // 16 MB LRU
const SPT = 63, HEADS = 255;
const CYLS = 261;                 // ~2047 MB volume — the FAT16 / DOS 4.01 ceiling

/** One file of the mounted folder, path relative with forward slashes. */
export interface LocalEntry {
  path: string;
  size: number;
  mtime: number;
  read(off: number, len: number): Promise<Uint8Array>;
}

export interface LocalDriveInfo {
  files: number;
  dropped: number;
  bytes: number;
  signature: string;
  totalSectors: number;
}

function fnv(s: string): string {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 0x01000193); }
  return (h >>> 0).toString(16).padStart(8, "0");
}

interface DirNode {
  path: string;                    // "" for root
  subdirs: string[];               // child dir paths
  files: number[];                 // entry indices
  cluster: number;                 // first cluster (0 for root)
  clusters: number;
  buf?: Uint8Array;
  taken: Set<string>;
  shortOf: Map<string, string>;    // child path → 8.3 name
}

interface Interval { s0: number; s1: number; idx: number; } // [s0, s1) absolute LBA of file data

export class LocalFatDrive {
  meta!: SparseImage;              // MBR + BPB + FATs + root + directory clusters
  geo!: VolumeGeometry;
  info!: LocalDriveInfo;
  private entries: LocalEntry[] = [];
  private intervals: Interval[] = [];
  private dataStart = 0;
  private volStart = SPT;
  private overlay = new Map<number, Uint8Array>();
  private overlayDirty = new Set<number>();
  private cache = new Map<string, Uint8Array>();
  private pendingBlocks = new Set<string>();
  private errorBlocks = new Set<string>();
  private log: (s: string) => void;

  constructor(log?: (s: string) => void) { this.log = log ?? (() => {}); }

  /** Deterministic layout: same folder contents → same volume → the overlay stays valid. */
  build(entries: LocalEntry[]): this {
    const totalSectors = CYLS * HEADS * SPT;
    const volSectors = totalSectors - this.volStart;
    const g = this.geo = planVolume(volSectors, SPT, HEADS, this.volStart);
    const rootSectors = Math.ceil(g.rootEntries * 32 / SECTOR);
    const fatStart = this.volStart + g.reservedSectors;
    const rootStart = fatStart + g.fats * g.sectorsPerFat;
    this.dataStart = rootStart + rootSectors;
    const cs = g.sectorsPerCluster * SECTOR;
    const clusterSector = (c: number) => this.dataStart + (c - 2) * g.sectorsPerCluster;

    const sorted = [...entries].sort((a, b) => a.path.toUpperCase() < b.path.toUpperCase() ? -1 : 1);
    this.entries = sorted;
    const sig = fnv(sorted.map((e) => `${e.path}|${e.size}|${e.mtime}`).join("\n"));

    // directory tree
    const dirs = new Map<string, DirNode>();
    const node = (path: string): DirNode => {
      let d = dirs.get(path);
      if (d) return d;
      d = { path, subdirs: [], files: [], cluster: 0, clusters: 0, taken: new Set(), shortOf: new Map() };
      dirs.set(path, d);
      if (path !== "") {
        const parent = node(path.includes("/") ? path.slice(0, path.lastIndexOf("/")) : "");
        parent.subdirs.push(path);
      }
      return d;
    };
    node("");
    sorted.forEach((e, i) => {
      const dir = e.path.includes("/") ? e.path.slice(0, e.path.lastIndexOf("/")) : "";
      node(dir).files.push(i);
    });

    // allocation: directories first (sorted), then file data (sorted); everything contiguous
    let nextFree = 2;
    const totalClusters = g.clusters;
    const dirList = [...dirs.values()].sort((a, b) => a.path.toUpperCase() < b.path.toUpperCase() ? -1 : 1);
    for (const d of dirList) {
      if (d.path === "") continue;
      const slots = d.subdirs.length + d.files.length + 2; // + "." ".."
      d.clusters = Math.max(1, Math.ceil((slots * 32) / cs));
      d.cluster = nextFree;
      nextFree += d.clusters;
    }
    let dropped = 0, bytes = 0;
    for (const e of sorted) (e as unknown as { _c: number })._c = -1;
    this.intervals = [];
    sorted.forEach((e, i) => {
      const n = Math.max(1, Math.ceil(e.size / cs));
      if (nextFree + n > totalClusters + 2) { dropped++; return; }
      (sorted[i] as unknown as { _c: number })._c = nextFree;
      if (e.size > 0) this.intervals.push({ s0: clusterSector(nextFree), s1: clusterSector(nextFree) + Math.ceil(e.size / SECTOR), idx: i });
      nextFree += n;
      bytes += e.size;
    });
    this.intervals.sort((a, b) => a.s0 - b.s0);

    // root capacity: the label + top-level children
    const root = dirs.get("")!;
    const rootSlots = g.rootEntries - 1;
    let rootDropped = 0;
    if (root.subdirs.length + root.files.length > rootSlots) {
      rootDropped = root.subdirs.length + root.files.length - rootSlots;
    }

    // FAT (both copies identical)
    const fat = new Uint8Array(g.sectorsPerFat * SECTOR);
    const setFat = (c: number, v: number) => { fat[c * 2] = v & 0xFF; fat[c * 2 + 1] = (v >> 8) & 0xFF; };
    setFat(0, 0xFF00 | g.media);
    setFat(1, 0xFFFF);
    const chain = (first: number, n: number) => { for (let i = 0; i < n; i++) setFat(first + i, i === n - 1 ? 0xFFFF : first + i + 1); };
    for (const d of dirList) if (d.path !== "") chain(d.cluster, d.clusters);
    sorted.forEach((e) => {
      const c = (e as unknown as { _c: number })._c;
      if (c >= 0) chain(c, Math.max(1, Math.ceil(e.size / cs)));
    });

    // directory buffers
    const rootBuf = new Uint8Array(rootSectors * SECTOR);
    const writeEntry = (buf: Uint8Array, slot: number, name83: string, attr: number, cluster: number, size: number, mtime: number) => {
      const o = slot * 32;
      const raw = buf.subarray(o, o + 32);
      if (attr & 0x08) {
        raw.fill(0x20, 0, 11);
        for (let i = 0; i < 11 && i < name83.length; i++) raw[i] = name83.charCodeAt(i);
      } else {
        raw.fill(0x20, 0, 11);
        if (name83 === "." || name83 === "..") { for (let i = 0; i < name83.length; i++) raw[i] = 0x2E; }
        else {
          const [base, ext = ""] = name83.split(".");
          for (let i = 0; i < 8 && i < base.length; i++) raw[i] = base.charCodeAt(i);
          for (let i = 0; i < 3 && i < ext.length; i++) raw[8 + i] = ext.charCodeAt(i);
        }
      }
      raw[11] = attr;
      const { date, time } = fatDateTime(new Date(mtime));
      raw[22] = time & 0xFF; raw[23] = time >> 8;
      raw[24] = date & 0xFF; raw[25] = date >> 8;
      raw[26] = cluster & 0xFF; raw[27] = (cluster >> 8) & 0xFF;
      raw[28] = size & 0xFF; raw[29] = (size >> 8) & 0xFF; raw[30] = (size >> 16) & 0xFF; raw[31] = (size >>> 24) & 0xFF;
    };
    const leaf = (p: string) => p.includes("/") ? p.slice(p.lastIndexOf("/") + 1) : p;
    const now = Date.now();
    writeEntry(rootBuf, 0, "LOCAL DRIVE", 0x08, 0, 0, now);
    const fillDir = (d: DirNode, buf: Uint8Array, firstSlot: number) => {
      let slot = firstSlot;
      const cap = Math.floor(buf.length / 32);
      for (const sub of d.subdirs) {
        if (slot >= cap) break;
        const sn = shortName(leaf(sub), d.taken);
        d.shortOf.set(sub, sn);
        writeEntry(buf, slot++, sn, 0x10, dirs.get(sub)!.cluster, 0, now);
      }
      for (const fi of d.files) {
        if (slot >= cap) break;
        const e = sorted[fi];
        const c = (e as unknown as { _c: number })._c;
        if (c < 0) continue;
        const sn = shortName(leaf(e.path), d.taken);
        d.shortOf.set(e.path, sn);
        // plain archive attr (not read-only): games write their own config/save files, and
        // those writes land in the overlay without touching the real folder
        writeEntry(buf, slot++, sn, 0x20, c, e.size, e.mtime);
      }
    };
    fillDir(root, rootBuf, 1);
    for (const d of dirList) {
      if (d.path === "") continue;
      d.buf = new Uint8Array(d.clusters * cs);
      const parent = dirs.get(d.path.includes("/") ? d.path.slice(0, d.path.lastIndexOf("/")) : "")!;
      writeEntry(d.buf, 0, ".", 0x10, d.cluster, 0, now);
      writeEntry(d.buf, 1, "..", 0x10, parent.path === "" ? 0 : parent.cluster, 0, now);
      fillDir(d, d.buf, 2);
    }

    // compose the metadata image
    const img = this.meta = new SparseImage(totalSectors);
    const mbr = new Uint8Array(SECTOR);
    mbr.set(MBR_CODE, 0);
    partEntry(mbr, 0x1BE, false, 0x06, this.volStart, this.volStart, volSectors, HEADS, SPT);
    mbr[0x1FE] = 0x55; mbr[0x1FF] = 0xAA;
    img.write(0, 1, mbr);
    const bs = new Uint8Array(SECTOR);
    const w16 = (o: number, v: number) => { bs[o] = v & 0xFF; bs[o + 1] = (v >> 8) & 0xFF; };
    const w32 = (o: number, v: number) => { w16(o, v & 0xFFFF); w16(o + 2, (v >>> 16) & 0xFFFF); };
    bs.set(new TextEncoder().encode("MSDOS4.0"), 3);
    w16(0x0B, SECTOR); bs[0x0D] = g.sectorsPerCluster; w16(0x0E, g.reservedSectors); bs[0x10] = g.fats;
    w16(0x11, g.rootEntries); w16(0x13, 0); bs[0x15] = g.media; w16(0x16, g.sectorsPerFat);
    w16(0x18, SPT); w16(0x1A, HEADS); w32(0x1C, this.volStart); w32(0x20, volSectors);
    bs[0x24] = 0x80; bs[0x26] = 0x29; w32(0x27, parseInt(sig, 16) >>> 0);
    bs.set(new TextEncoder().encode("LOCAL DRIVE"), 0x2B);
    bs.set(new TextEncoder().encode("FAT16   "), 0x36);
    bs[0x1FE] = 0x55; bs[0x1FF] = 0xAA;
    img.write(this.volStart, 1, bs);
    for (let f = 0; f < g.fats; f++) {
      img.write(fatStart + f * g.sectorsPerFat, g.sectorsPerFat, fat);
    }
    img.write(rootStart, rootSectors, rootBuf);
    for (const d of dirList) {
      if (d.path === "") continue;
      img.write(clusterSector(d.cluster), d.clusters * g.sectorsPerCluster, d.buf!);
    }

    this.info = { files: sorted.length - dropped, dropped: dropped + rootDropped, bytes, signature: sig, totalSectors };
    if (dropped) this.log(`local drive: ${dropped} file(s) did not fit in 2 GB and were left out`);
    if (rootDropped) this.log(`local drive: ${rootDropped} top-level item(s) beyond ${rootSlots} were left out`);
    return this;
  }

  private findInterval(s: number): Interval | null {
    let lo = 0, hi = this.intervals.length - 1;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1, iv = this.intervals[mid];
      if (s < iv.s0) hi = mid - 1;
      else if (s >= iv.s1) lo = mid + 1;
      else return iv;
    }
    return null;
  }

  private fetch(idx: number, blockIdx: number) {
    const key = `${idx}:${blockIdx}`;
    if (this.cache.has(key) || this.pendingBlocks.has(key) || this.errorBlocks.has(key)) return;
    const e = this.entries[idx];
    const off = blockIdx * BLOCK;
    if (off >= e.size) return;
    this.pendingBlocks.add(key);
    e.read(off, Math.min(BLOCK, e.size - off)).then((data) => {
      this.pendingBlocks.delete(key);
      this.cache.set(key, data);
      while (this.cache.size > CACHE_BLOCKS) {
        const oldest = this.cache.keys().next().value as string;
        this.cache.delete(oldest);
      }
    }, (err) => {
      this.pendingBlocks.delete(key);
      this.errorBlocks.add(key);
      this.log(`local drive read failed (${e.path}): ${err}`);
    });
  }

  /** 0 = done, 1 = I/O error, 2 = pending (retry after the host has loaded the block). */
  read(lba: number, count: number, dst: Uint8Array): number {
    if (lba + count > this.meta.sectors) return 1;
    let pending = false, error = false;
    for (let i = 0; i < count; i++) {
      const s = lba + i;
      const out = dst.subarray(i * SECTOR, (i + 1) * SECTOR);
      const ov = this.overlay.get(s);
      if (ov) { out.set(ov); continue; }
      const iv = this.findInterval(s);
      if (!iv) { this.meta.read(s, 1, out); continue; }
      const e = this.entries[iv.idx];
      const off = (s - iv.s0) * SECTOR;
      const blockIdx = Math.floor(off / BLOCK);
      const key = `${iv.idx}:${blockIdx}`;
      const block = this.cache.get(key);
      if (block) {
        // LRU touch
        this.cache.delete(key); this.cache.set(key, block);
        const bo = off % BLOCK;
        const n = Math.max(0, Math.min(SECTOR, block.length - bo));
        out.fill(0);
        if (n > 0) out.set(block.subarray(bo, bo + n));
        // read ahead within the file
        const lastBlock = Math.floor((e.size - 1) / BLOCK);
        for (let a = 1; a <= READ_AHEAD && blockIdx + a <= lastBlock; a++) this.fetch(iv.idx, blockIdx + a);
        continue;
      }
      if (this.errorBlocks.has(key)) { error = true; continue; }
      this.fetch(iv.idx, blockIdx);
      const lastBlock = Math.floor((e.size - 1) / BLOCK);
      for (let a = 1; a <= READ_AHEAD && blockIdx + a <= lastBlock; a++) this.fetch(iv.idx, blockIdx + a);
      pending = true;
    }
    return error ? 1 : pending ? 2 : 0;
  }

  write(lba: number, count: number, src: Uint8Array): boolean {
    if (lba + count > this.meta.sectors) return false;
    for (let i = 0; i < count; i++) {
      const s = lba + i;
      let ov = this.overlay.get(s);
      if (!ov) { ov = new Uint8Array(SECTOR); this.overlay.set(s, ov); }
      ov.set(src.subarray(i * SECTOR, (i + 1) * SECTOR));
      this.overlayDirty.add(s);
    }
    return true;
  }

  /** Dirty overlay sectors for persistence (sector-LBA keyed). */
  takeDirtyOverlay(): Map<number, Uint8Array> {
    const out = new Map<number, Uint8Array>();
    for (const s of this.overlayDirty) out.set(s, this.overlay.get(s)!);
    this.overlayDirty.clear();
    return out;
  }

  loadOverlay(m: Map<number, Uint8Array>) {
    for (const [s, b] of m) this.overlay.set(s, b.slice(0, SECTOR));
  }
}

/** Walk a FileSystemDirectoryHandle (FSA picker or OPFS) into LocalEntry[]. */
export async function entriesFromDirectory(dir: FileSystemDirectoryHandle, onProgress?: (n: number) => void): Promise<LocalEntry[]> {
  const out: LocalEntry[] = [];
  const walk = async (d: FileSystemDirectoryHandle, prefix: string) => {
    const names: { name: string; handle: FileSystemHandle }[] = [];
    const iter = (d as unknown as { entries(): AsyncIterable<[string, FileSystemHandle]> }).entries();
    for await (const [name, handle] of iter) {
      if (name.startsWith(".")) continue;
      names.push({ name, handle });
    }
    names.sort((a, b) => a.name.toUpperCase() < b.name.toUpperCase() ? -1 : 1);
    for (const { name, handle } of names) {
      const path = prefix ? `${prefix}/${name}` : name;
      if (handle.kind === "directory") await walk(handle as FileSystemDirectoryHandle, path);
      else {
        const fh = handle as FileSystemFileHandle;
        const f = await fh.getFile();
        out.push({
          path, size: f.size, mtime: f.lastModified,
          read: async (off, len) => new Uint8Array(await (await fh.getFile()).slice(off, off + len).arrayBuffer()),
        });
        if (out.length % 64 === 0) onProgress?.(out.length);
      }
    }
  };
  await walk(dir, "");
  return out;
}

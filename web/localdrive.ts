// Local game drive: a host folder exposed to DOS as the second hard disk (BIOS 81h).
// The disk is synthetic: an 8 GB (CHS ceiling) drive carved into 2 GB FAT16 volumes exactly like
// the built-in C: image (formatDisk does the MBR/EBR/BPB plumbing). Top-level items of the folder
// are placed WHOLE onto volumes, first-fit in alphabetical order — a game is never split, so it
// either runs or is reported. Top-level archive files (.zip/.iso/...) are skipped: DOS cannot run
// them and they would eat the letter space. Volume 1 carries a generated README.TXT that maps
// every item to its volume and lists what did not fit.
// Metadata (boot records, FATs, directories) lives in a sparse image; file DATA clusters map onto
// the real files and are fetched lazily in 64 KB blocks (read-ahead + LRU). A read that is not
// resident yet returns "pending" (2) — the BIOS rewinds INT 13h and halts until it arrives.
// DOS writes land in a persistent per-folder sector overlay; the real folder is never modified.
// Works over the File System Access API in the browser and over Deno files in the test rigs.
import { fatDateTime, planVolume, shortName, type VolumeGeometry } from "./fat.ts";
import { formatDisk, MAX_VOLUME_MB, planDisk } from "./hdd.ts";
import { SparseImage } from "./store.ts";
import type { SectorIO } from "./fatfs.ts";

const SECTOR = 512;
const BLOCK = 65536;              // lazy-read granularity (file-offset aligned)
const READ_AHEAD = 8;             // blocks fetched beyond the one that missed
const CACHE_BLOCKS = 256;         // 16 MB LRU
const ROOT_ENTRIES = 512;
const ARCHIVE_RE = /\.(zip|iso|7z|rar|arj|lha|lzh|cue|bin|img|ima|chd|mds|mdf|nrg|ccd|sub|gog|dmg)$/i;

/** One file of the mounted folder, path relative with forward slashes. */
export interface LocalEntry {
  path: string;
  size: number;
  mtime: number;
  read(off: number, len: number): Promise<Uint8Array>;
}

export interface LocalDriveOptions {
  diskMB?: number;      // synthetic disk size (default 8 GB, the CHS ceiling)
  volumeMB?: number;    // per-volume cap (default 2047, the FAT16 ceiling)
}

export interface LocalDriveInfo {
  files: number;                    // files placed
  bytes: number;                    // bytes placed
  signature: string;
  totalSectors: number;
  volumes: { items: string[] }[];   // top-level item names per volume
  skippedArchives: number;
  tooBig: string[];                 // items larger than one volume
  notFit: string[];                 // items that ran out of space
}

function fnv(s: string): string {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 0x01000193); }
  return (h >>> 0).toString(16).padStart(8, "0");
}

interface DirNode {
  path: string;                    // "" = the volume root
  subdirs: string[];
  files: number[];                 // entry indices
  cluster: number;
  clusters: number;
  buf?: Uint8Array;
  taken: Set<string>;
}

interface Interval { s0: number; s1: number; idx: number; } // [s0, s1) absolute LBA of file data

interface Volume {
  startLba: number;
  sectors: number;
  geo: VolumeGeometry;
  nextFree: number;                // next free cluster
  rootUsed: number;
  itemNames: string[];             // display names of top-level items placed here
  entryIdx: number[];              // entries placed on this volume
}

export class LocalFatDrive {
  meta!: SparseImage;
  info!: LocalDriveInfo;
  private entries: LocalEntry[] = [];
  private intervals: Interval[] = [];
  private overlay = new Map<number, Uint8Array>();
  private overlayDirty = new Set<number>();
  private cache = new Map<string, Uint8Array>();
  private pendingBlocks = new Set<string>();
  private errorBlocks = new Set<string>();
  private log: (s: string) => void;

  constructor(log?: (s: string) => void) { this.log = log ?? (() => {}); }

  /** Deterministic layout: same folder contents → same volumes → the overlay stays valid. */
  build(all: LocalEntry[], opts?: LocalDriveOptions): this {
    const diskMB = opts?.diskMB ?? 8192;
    const volMB = opts?.volumeMB ?? MAX_VOLUME_MB;
    const sorted = [...all].sort((a, b) => a.path.toUpperCase() < b.path.toUpperCase() ? -1 : 1);
    const sig = fnv("layout2|" + sorted.map((e) => `${e.path}|${e.size}|${e.mtime}`).join("\n"));

    // top-level archives are unusable in DOS — skip them before anything is placed
    let skippedArchives = 0;
    const usable: LocalEntry[] = [];
    for (const e of sorted) {
      if (!e.path.includes("/") && ARCHIVE_RE.test(e.path)) { skippedArchives++; continue; }
      usable.push(e);
    }
    this.entries = usable;

    // group into top-level items, in sorted order
    const items = new Map<string, { name: string; idx: number[]; bytes: number }>();
    usable.forEach((e, i) => {
      const top = e.path.includes("/") ? e.path.slice(0, e.path.indexOf("/")) : e.path;
      let it = items.get(top);
      if (!it) { it = { name: top, idx: [], bytes: 0 }; items.set(top, it); }
      it.idx.push(i);
      it.bytes += e.size;
    });

    // disk skeleton: MBR + EBR chain + per-volume BPB/FATs/roots, all into the sparse image
    const plan = planDisk(diskMB, volMB);
    const img = this.meta = new SparseImage(plan.totalSectors);
    const io: SectorIO = {
      readSectors: (l, c, d) => img.read(l, c, d),
      writeSectors: (l, c, s) => img.write(l, c, s),
    };
    formatDisk(io, diskMB, LocalFatDrive.zeroBoot, "LOCAL DRIVE", volMB);

    const vols: Volume[] = plan.volumes.map((v) => ({
      startLba: v.startLba, sectors: v.sectors,
      geo: planVolume(v.sectors, plan.spt, plan.heads, v.startLba),
      nextFree: 2, rootUsed: 1 /* label */, itemNames: [], entryIdx: [],
    }));
    // reserve README.TXT on volume 1: one root slot + 64 KB of clusters at the front
    const v0cs = vols[0].geo.sectorsPerCluster * SECTOR;
    const readmeClusters = Math.max(1, Math.ceil(65536 / v0cs));
    vols[0].rootUsed++;
    vols[0].nextFree += readmeClusters;

    // exact cluster need of one whole item on a volume with cluster size cs
    const needOf = (it: { idx: number[] }, geo: VolumeGeometry): number => {
      const cs = geo.sectorsPerCluster * SECTOR;
      const dirs = new Map<string, { files: number; subdirs: Set<string> }>();
      const dnode = (p: string): { files: number; subdirs: Set<string> } => {
        let d = dirs.get(p);
        if (!d) {
          d = { files: 0, subdirs: new Set() };
          dirs.set(p, d);
          if (p.includes("/")) dnode(p.slice(0, p.lastIndexOf("/"))).subdirs.add(p);
        }
        return d;
      };
      let n = 0;
      for (const i of it.idx) {
        const e = this.entries[i];
        n += Math.max(1, Math.ceil(e.size / cs));
        if (e.path.includes("/")) dnode(e.path.slice(0, e.path.lastIndexOf("/"))).files++;
      }
      for (const d of dirs.values()) n += Math.max(1, Math.ceil(((d.files + d.subdirs.size + 2) * 32) / cs));
      return n;
    };

    // whole-item first-fit, alphabetical
    const tooBig: string[] = [], notFit: string[] = [];
    for (const it of items.values()) {
      let placed = false, fitsSomewhereEmpty = false;
      for (const vol of vols) {
        const capacity = vol.geo.clusters + 2 - vol.nextFree;
        const need = needOf(it, vol.geo);
        if (need <= vol.geo.clusters - (vol === vols[0] ? readmeClusters : 0)) fitsSomewhereEmpty = true;
        if (need <= capacity && vol.rootUsed < Math.min(ROOT_ENTRIES, vol.geo.rootEntries)) {
          vol.nextFree += need;
          vol.rootUsed++;
          vol.itemNames.push(it.name);
          vol.entryIdx.push(...it.idx);
          placed = true;
          break;
        }
      }
      if (!placed) (fitsSomewhereEmpty ? notFit : tooBig).push(it.name);
    }

    // README content (original names; volume numbers)
    const lines: string[] = [
      "DOS MOBILE - LOCAL GAMES FOLDER", "===============================", "",
      "Your folder is mounted read-only across these volumes.",
      "Volume 1 is this drive (usually D:). Later volumes take the",
      "letters after the built-in C: drive's own - with the standard",
      "8 GB C: that means volume 2=H:, 3=I:, 4=J:.", "",
    ];
    vols.forEach((v, i) => {
      if (!v.itemNames.length) return;
      lines.push(`VOLUME ${i + 1}:`);
      for (const n of v.itemNames) lines.push(`  ${n}`);
      lines.push("");
    });
    if (tooBig.length) {
      lines.push("TOO BIG FOR ONE 2 GB VOLUME (FAT16 limit - cannot be mounted):");
      for (const n of tooBig) lines.push(`  ${n}`);
      lines.push("");
    }
    if (notFit.length) {
      lines.push("DID NOT FIT (the disk is full - remove or move something to play these):");
      for (const n of notFit) lines.push(`  ${n}`);
      lines.push("");
    }
    if (skippedArchives) {
      lines.push(`SKIPPED: ${skippedArchives} archive/image file(s) (.zip/.iso/...) -`);
      lines.push("DOS cannot run them; extract them into folders to play.", "");
    }
    let readme = lines.join("\r\n");
    if (readme.length > readmeClusters * v0cs) readme = readme.slice(0, readmeClusters * v0cs - 5) + "\r\n...";
    const readmeBytes = new TextEncoder().encode(readme);

    // build every volume's FATs, root and directory clusters
    this.intervals = [];
    vols.forEach((vol, vi) => this.buildVolume(vol, vi === 0 ? { cluster: 2, clusters: readmeClusters, bytes: readmeBytes } : null));
    this.intervals.sort((a, b) => a.s0 - b.s0);

    const placedFiles = vols.reduce((n, v) => n + v.entryIdx.length, 0);
    const placedBytes = vols.reduce((n, v) => n + v.entryIdx.reduce((m, i) => m + this.entries[i].size, 0), 0);
    this.info = {
      files: placedFiles, bytes: placedBytes, signature: sig, totalSectors: plan.totalSectors,
      volumes: vols.map((v) => ({ items: v.itemNames })),
      skippedArchives, tooBig, notFit,
    };
    const volsDesc = vols.filter((v) => v.itemNames.length).map((v, i) => `vol${i + 1}=${v.itemNames.length}`).join(" ");
    this.log(`local drive: ${placedFiles} files in ${items.size - tooBig.length - notFit.length} items (${volsDesc})`
      + (skippedArchives ? `; ${skippedArchives} archives skipped` : "")
      + (tooBig.length ? `; ${tooBig.length} over 2 GB` : "")
      + (notFit.length ? `; ${notFit.length} did not fit` : ""));
    return this;
  }

  /** Fill one volume: FAT chains, root + subdirectory clusters, data intervals. */
  private buildVolume(vol: Volume, readmeFile: { cluster: number; clusters: number; bytes: Uint8Array } | null) {
    const g = vol.geo;
    const cs = g.sectorsPerCluster * SECTOR;
    const rootSectors = Math.ceil(g.rootEntries * 32 / SECTOR);
    const fatStart = vol.startLba + g.reservedSectors;
    const rootStart = fatStart + g.fats * g.sectorsPerFat;
    const dataStart = rootStart + rootSectors;
    const clusterSector = (c: number) => dataStart + (c - 2) * g.sectorsPerCluster;
    const now = Date.now();

    // volume-local directory tree; "" is the volume root, children are this volume's items
    const dirs = new Map<string, DirNode>();
    const node = (path: string): DirNode => {
      let d = dirs.get(path);
      if (d) return d;
      d = { path, subdirs: [], files: [], cluster: 0, clusters: 0, taken: new Set() };
      dirs.set(path, d);
      if (path !== "") {
        const parent = node(path.includes("/") ? path.slice(0, path.lastIndexOf("/")) : "");
        parent.subdirs.push(path);
      }
      return d;
    };
    node("");
    for (const i of vol.entryIdx) {
      const e = this.entries[i];
      const dir = e.path.includes("/") ? e.path.slice(0, e.path.lastIndexOf("/")) : "";
      node(dir).files.push(i);
    }

    // allocation: README first (already reserved), then directories, then file data
    let nextFree = 2 + (readmeFile ? readmeFile.clusters : 0);
    const dirList = [...dirs.values()].sort((a, b) => a.path.toUpperCase() < b.path.toUpperCase() ? -1 : 1);
    for (const d of dirList) {
      if (d.path === "") continue;
      const slots = d.subdirs.length + d.files.length + 2;
      d.clusters = Math.max(1, Math.ceil((slots * 32) / cs));
      d.cluster = nextFree;
      nextFree += d.clusters;
    }
    const firstClusterOf = new Map<number, number>();
    for (const i of [...vol.entryIdx].sort((a, b) => this.entries[a].path.toUpperCase() < this.entries[b].path.toUpperCase() ? -1 : 1)) {
      const e = this.entries[i];
      const n = Math.max(1, Math.ceil(e.size / cs));
      firstClusterOf.set(i, nextFree);
      if (e.size > 0) {
        this.intervals.push({ s0: clusterSector(nextFree), s1: clusterSector(nextFree) + Math.ceil(e.size / SECTOR), idx: i });
      }
      nextFree += n;
    }

    // FAT (identical copies)
    const fat = new Uint8Array(g.sectorsPerFat * SECTOR);
    const setFat = (c: number, v: number) => { fat[c * 2] = v & 0xFF; fat[c * 2 + 1] = (v >> 8) & 0xFF; };
    setFat(0, 0xFF00 | g.media);
    setFat(1, 0xFFFF);
    const chain = (first: number, n: number) => { for (let i = 0; i < n; i++) setFat(first + i, i === n - 1 ? 0xFFFF : first + i + 1); };
    if (readmeFile) chain(readmeFile.cluster, readmeFile.clusters);
    for (const d of dirList) if (d.path !== "") chain(d.cluster, d.clusters);
    for (const i of vol.entryIdx) chain(firstClusterOf.get(i)!, Math.max(1, Math.ceil(this.entries[i].size / cs)));

    // directory entries
    const rootBuf = new Uint8Array(rootSectors * SECTOR);
    const writeEntry = (buf: Uint8Array, slot: number, name83: string, attr: number, cluster: number, size: number, mtime: number) => {
      const raw = buf.subarray(slot * 32, slot * 32 + 32);
      raw.fill(0);
      raw.fill(0x20, 0, 11);
      if (attr & 0x08) { for (let i = 0; i < 11 && i < name83.length; i++) raw[i] = name83.charCodeAt(i); }
      else if (name83 === "." || name83 === "..") { for (let i = 0; i < name83.length; i++) raw[i] = 0x2E; }
      else {
        const [base, ext = ""] = name83.split(".");
        for (let i = 0; i < 8 && i < base.length; i++) raw[i] = base.charCodeAt(i);
        for (let i = 0; i < 3 && i < ext.length; i++) raw[8 + i] = ext.charCodeAt(i);
      }
      raw[11] = attr;
      const { date, time } = fatDateTime(new Date(mtime));
      raw[22] = time & 0xFF; raw[23] = time >> 8;
      raw[24] = date & 0xFF; raw[25] = date >> 8;
      raw[26] = cluster & 0xFF; raw[27] = (cluster >> 8) & 0xFF;
      raw[28] = size & 0xFF; raw[29] = (size >> 8) & 0xFF; raw[30] = (size >> 16) & 0xFF; raw[31] = (size >>> 24) & 0xFF;
    };
    const leaf = (p: string) => p.includes("/") ? p.slice(p.lastIndexOf("/") + 1) : p;
    const fillDir = (d: DirNode, buf: Uint8Array, firstSlot: number) => {
      let slot = firstSlot;
      const cap = Math.floor(buf.length / 32);
      for (const sub of d.subdirs) {
        if (slot >= cap) break;
        writeEntry(buf, slot++, shortName(leaf(sub), d.taken), 0x10, dirs.get(sub)!.cluster, 0, now);
      }
      for (const fi of d.files) {
        if (slot >= cap) break;
        const e = this.entries[fi];
        writeEntry(buf, slot++, shortName(leaf(e.path), d.taken), 0x20, firstClusterOf.get(fi)!, e.size, e.mtime);
      }
    };
    const root = dirs.get("")!;
    writeEntry(rootBuf, 0, "LOCAL DRIVE", 0x08, 0, 0, now);
    let rootFirst = 1;
    if (readmeFile) { writeEntry(rootBuf, 1, "README.TXT", 0x20, readmeFile.cluster, readmeFile.bytes.length, now); rootFirst = 2; }
    fillDir(root, rootBuf, rootFirst);
    for (const d of dirList) {
      if (d.path === "") continue;
      d.buf = new Uint8Array(d.clusters * cs);
      const parent = dirs.get(d.path.includes("/") ? d.path.slice(0, d.path.lastIndexOf("/")) : "")!;
      writeEntry(d.buf, 0, ".", 0x10, d.cluster, 0, now);
      writeEntry(d.buf, 1, "..", 0x10, parent.path === "" ? 0 : parent.cluster, 0, now);
      fillDir(d, d.buf, 2);
    }

    // write it all into the metadata image
    for (let f = 0; f < g.fats; f++) this.meta.write(fatStart + f * g.sectorsPerFat, g.sectorsPerFat, fat);
    this.meta.write(rootStart, rootSectors, rootBuf);
    if (readmeFile) {
      const buf = new Uint8Array(readmeFile.clusters * cs);
      buf.set(readmeFile.bytes.subarray(0, buf.length));
      this.meta.write(clusterSector(readmeFile.cluster), readmeFile.clusters * g.sectorsPerCluster, buf);
    }
    for (const d of dirList) {
      if (d.path === "") continue;
      this.meta.write(clusterSector(d.cluster), d.clusters * g.sectorsPerCluster, d.buf!);
    }
  }

  private static zeroBoot = new Uint8Array(SECTOR);

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
      const lastBlock = Math.floor((e.size - 1) / BLOCK);
      if (block) {
        this.cache.delete(key); this.cache.set(key, block); // LRU touch
        const bo = off % BLOCK;
        const n = Math.max(0, Math.min(SECTOR, block.length - bo));
        out.fill(0);
        if (n > 0) out.set(block.subarray(bo, bo + n));
        for (let a = 1; a <= READ_AHEAD && blockIdx + a <= lastBlock; a++) this.fetch(iv.idx, blockIdx + a);
        continue;
      }
      if (this.errorBlocks.has(key)) { error = true; continue; }
      this.fetch(iv.idx, blockIdx);
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

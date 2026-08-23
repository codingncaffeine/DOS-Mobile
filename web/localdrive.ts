// Local game drive: a host folder exposed to DOS across MULTIPLE synthetic hard disks.
// MS-DOS 4.01's real limits (verified in the source we build): primaries exist only on the first
// two BIOS disks, but DoMini walks the extended partition chain of EVERY disk, with room for 23
// logical drives - so disk 81h carries a primary (D:) plus logicals, and disks 82h+ carry only
// extended partitions. With LASTDRIVE=Z that fills the alphabet: up to ~40 GB of mounted games.
// Top-level items are placed WHOLE, first-fit alphabetically - a game is never split, so it
// either runs or is reported. Top-level archive files (.zip/.iso/...) are skipped: DOS cannot
// run them. Volume 1 carries a generated README.TXT mapping every item to its drive letter.
// Metadata (boot records, FATs, directories) lives in sparse images; file DATA clusters map onto
// the real files and are fetched lazily in 64 KB blocks (read-ahead + LRU). A read that is not
// resident yet returns "pending" (2) - the BIOS rewinds INT 13h and halts until it arrives.
// DOS writes land in persistent per-folder sector overlays; the real folder is never modified.
// Works over the File System Access API in the browser and over Deno files in the test rigs.
import { fatDateTime, planVolume, shortName, type VolumeGeometry } from "./fat.ts";
import { formatDisk, formatDiskExt, MAX_VOLUME_MB, planDisk, planDiskExt } from "./hdd.ts";
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
  diskMB?: number;        // per-disk size (default 8 GB, the CHS ceiling)
  volumeMB?: number;      // per-volume cap (default 2047, the FAT16 ceiling)
  maxDisks?: number;      // synthetic disks (default 5 → with the 8 GB C: exactly fills C..Z)
  imagePrimaries?: number; // built-in image primary count (for drive-letter prediction)
  imageLogicals?: number;  // built-in image logical count (for drive-letter prediction)
}

export interface LocalDriveInfo {
  files: number;
  bytes: number;
  signature: string;
  disks: number;                              // synthetic disks in use
  volumes: { disk: number; letter: string; items: string[] }[];
  skippedArchives: number;
  tooBig: string[];
  notFit: string[];
}

function fnv(s: string): string {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 0x01000193); }
  return (h >>> 0).toString(16).padStart(8, "0");
}

interface DirNode {
  path: string;
  subdirs: string[];
  files: number[];
  cluster: number;
  clusters: number;
  buf?: Uint8Array;
  taken: Set<string>;
}

interface Interval { s0: number; s1: number; idx: number; } // [s0, s1) disk-relative LBA

interface Volume {
  disk: number;
  startLba: number;
  sectors: number;
  logical: boolean;
  geo: VolumeGeometry;
  nextFree: number;
  rootUsed: number;
  letter: string;
  itemNames: string[];
  entryIdx: number[];
}

interface DiskState {
  meta: SparseImage;
  intervals: Interval[];
  overlay: Map<number, Uint8Array>;
  overlayDirty: Set<number>;
}

export class LocalFatDrive {
  info!: LocalDriveInfo;
  private disksArr: DiskState[] = [];
  private entries: LocalEntry[] = [];
  private cache = new Map<string, Uint8Array>();
  private pendingBlocks = new Set<string>();
  private errorBlocks = new Set<string>();
  private log: (s: string) => void;

  constructor(log?: (s: string) => void) { this.log = log ?? (() => {}); }

  diskSectors(d: number): number { return this.disksArr[d]?.meta.sectors ?? 0; }

  /** Deterministic layout: same folder contents → same volumes → the overlays stay valid. */
  build(all: LocalEntry[], opts?: LocalDriveOptions): this {
    const diskMB = opts?.diskMB ?? 8192;
    const volMB = opts?.volumeMB ?? MAX_VOLUME_MB;
    const maxDisks = opts?.maxDisks ?? 4;
    const imagePrimaries = opts?.imagePrimaries ?? 1;
    const imageLogicals = opts?.imageLogicals ?? 3;
    const sorted = [...all].sort((a, b) => a.path.toUpperCase() < b.path.toUpperCase() ? -1 : 1);
    const sig = fnv("layout3|" + sorted.map((e) => `${e.path}|${e.size}|${e.mtime}`).join("\n"));

    // top-level archives are unusable in DOS - skip them before anything is placed
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

    // plan every disk's volumes up front (all sparse - only used disks are kept)
    const zeroBoot = new Uint8Array(SECTOR);
    const plans = Array.from({ length: maxDisks }, (_, d) => d === 0 ? planDisk(diskMB, volMB) : planDiskExt(diskMB, volMB));
    const vols: Volume[] = [];
    for (let d = 0; d < maxDisks; d++) {
      for (const v of plans[d].volumes) {
        vols.push({
          disk: d, startLba: v.startLba, sectors: v.sectors, logical: v.logical,
          geo: planVolume(v.sectors, plans[d].spt, plans[d].heads, v.startLba),
          nextFree: 2, rootUsed: 1, letter: "",
          itemNames: [], entryIdx: [],
        });
      }
    }
    // DOS 4.01 letters (from its source): primaries C and D (first two disks only), then it
    // reserves ONE letter per remaining physical disk (DRVMAX = floppies + disk count, even
    // though those disks get no primary), and logical drives follow - image logicals first,
    // then every local disk's in order. Assign with `disks` physical local disks attached.
    const assignLetters = (localDisks: number) => {
      const hnum = imagePrimaries + localDisks;              // physical hard disks total
      let code = 67 + hnum + imageLogicals;                  // first LOCAL logical letter
      for (const v of vols) {
        if (!v.logical) v.letter = String.fromCharCode(67 + imagePrimaries); // our disk-0 primary
        else if (v.disk < localDisks) v.letter = code > 90 ? "-" : String.fromCharCode(code++);
        else v.letter = "-";
      }
    };
    assignLetters(maxDisks); // worst case: most ghost letters; the final pass can only move down
    const usableVols = vols.filter((v) => v.letter !== "-");

    // reserve README.TXT on the first volume
    const v0 = usableVols[0];
    const v0cs = v0.geo.sectorsPerCluster * SECTOR;
    const readmeClusters = Math.max(1, Math.ceil(65536 / v0cs));
    v0.rootUsed++;
    v0.nextFree += readmeClusters;

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
      let placed = false, fitsAnEmpty = false;
      for (const vol of usableVols) {
        const capacity = vol.geo.clusters + 2 - vol.nextFree;
        const need = needOf(it, vol.geo);
        if (need <= vol.geo.clusters - (vol === v0 ? readmeClusters : 0)) fitsAnEmpty = true;
        if (need <= capacity && vol.rootUsed < Math.min(ROOT_ENTRIES, vol.geo.rootEntries)) {
          vol.nextFree += need;
          vol.rootUsed++;
          vol.itemNames.push(it.name);
          vol.entryIdx.push(...it.idx);
          placed = true;
          break;
        }
      }
      if (!placed) (fitsAnEmpty ? notFit : tooBig).push(it.name);
    }

    // final letters with the disk count actually in use (fewer ghost letters than worst case)
    let usedDisks = 1;
    for (const v of usableVols) if (v.itemNames.length) usedDisks = Math.max(usedDisks, v.disk + 1);
    assignLetters(usedDisks);

    // README content
    const lines: string[] = [
      "DOS MOBILE - LOCAL GAMES FOLDER", "===============================", "",
      "Your folder is mounted read-only across the drive letters below",
      "(letters assume the standard built-in C: drive; DOS reserves a",
      "letter for each extra disk, so some letters in between are unused).", "",
    ];
    for (const v of usableVols) {
      if (!v.itemNames.length) continue;
      lines.push(`DRIVE ${v.letter}:`);
      for (const n of v.itemNames) lines.push(`  ${n}`);
      lines.push("");
    }
    if (tooBig.length) {
      lines.push("TOO BIG FOR ONE 2 GB VOLUME (FAT16 limit - cannot be mounted):");
      for (const n of tooBig) lines.push(`  ${n}`);
      lines.push("");
    }
    if (notFit.length) {
      lines.push("DID NOT FIT (all volumes full - remove or move something to play these):");
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

    // instantiate the disks that received items (a contiguous prefix by construction)
    this.disksArr = [];
    for (let d = 0; d < usedDisks; d++) {
      const meta = new SparseImage(plans[d].totalSectors);
      const io: SectorIO = {
        readSectors: (l, c, dst) => meta.read(l, c, dst),
        writeSectors: (l, c, src) => meta.write(l, c, src),
      };
      if (d === 0) formatDisk(io, diskMB, zeroBoot, "LOCAL DRIVE", volMB);
      else formatDiskExt(io, diskMB, zeroBoot, "LOCAL DRV", volMB);
      this.disksArr.push({ meta, intervals: [], overlay: new Map(), overlayDirty: new Set() });
    }

    // build every used volume's FATs, root and directory clusters
    for (const vol of usableVols) {
      if (vol.disk >= usedDisks) break;
      this.buildVolume(vol, vol === v0 ? { cluster: 2, clusters: readmeClusters, bytes: readmeBytes } : null);
    }
    for (const ds of this.disksArr) ds.intervals.sort((a, b) => a.s0 - b.s0);

    const placedFiles = usableVols.reduce((n, v) => n + v.entryIdx.length, 0);
    const placedBytes = usableVols.reduce((n, v) => n + v.entryIdx.reduce((m, i) => m + this.entries[i].size, 0), 0);
    this.info = {
      files: placedFiles, bytes: placedBytes, signature: sig, disks: usedDisks,
      volumes: usableVols.filter((v) => v.itemNames.length).map((v) => ({ disk: v.disk, letter: v.letter, items: v.itemNames })),
      skippedArchives, tooBig, notFit,
    };
    this.log(`local drive: ${placedFiles} files, ${(placedBytes / 1048576).toFixed(0)} MB on ${this.info.volumes.length} volume(s) / ${usedDisks} disk(s)`
      + (skippedArchives ? `; ${skippedArchives} archives skipped` : "")
      + (tooBig.length ? `; ${tooBig.length} over 2 GB` : "")
      + (notFit.length ? `; ${notFit.length} did not fit` : ""));
    return this;
  }

  /** Fill one volume: FAT chains, root + subdirectory clusters, data intervals. */
  private buildVolume(vol: Volume, readmeFile: { cluster: number; clusters: number; bytes: Uint8Array } | null) {
    const ds = this.disksArr[vol.disk];
    const g = vol.geo;
    const cs = g.sectorsPerCluster * SECTOR;
    const rootSectors = Math.ceil(g.rootEntries * 32 / SECTOR);
    const fatStart = vol.startLba + g.reservedSectors;
    const rootStart = fatStart + g.fats * g.sectorsPerFat;
    const dataStart = rootStart + rootSectors;
    const clusterSector = (c: number) => dataStart + (c - 2) * g.sectorsPerCluster;
    const now = Date.now();

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
        ds.intervals.push({ s0: clusterSector(nextFree), s1: clusterSector(nextFree) + Math.ceil(e.size / SECTOR), idx: i });
      }
      nextFree += n;
    }

    const fat = new Uint8Array(g.sectorsPerFat * SECTOR);
    const setFat = (c: number, v: number) => { fat[c * 2] = v & 0xFF; fat[c * 2 + 1] = (v >> 8) & 0xFF; };
    setFat(0, 0xFF00 | g.media);
    setFat(1, 0xFFFF);
    const chain = (first: number, n: number) => { for (let i = 0; i < n; i++) setFat(first + i, i === n - 1 ? 0xFFFF : first + i + 1); };
    if (readmeFile) chain(readmeFile.cluster, readmeFile.clusters);
    for (const d of dirList) if (d.path !== "") chain(d.cluster, d.clusters);
    for (const i of vol.entryIdx) chain(firstClusterOf.get(i)!, Math.max(1, Math.ceil(this.entries[i].size / cs)));

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

    for (let f = 0; f < g.fats; f++) ds.meta.write(fatStart + f * g.sectorsPerFat, g.sectorsPerFat, fat);
    ds.meta.write(rootStart, rootSectors, rootBuf);
    if (readmeFile) {
      const buf = new Uint8Array(readmeFile.clusters * cs);
      buf.set(readmeFile.bytes.subarray(0, buf.length));
      ds.meta.write(clusterSector(readmeFile.cluster), readmeFile.clusters * g.sectorsPerCluster, buf);
    }
    for (const d of dirList) {
      if (d.path === "") continue;
      ds.meta.write(clusterSector(d.cluster), d.clusters * g.sectorsPerCluster, d.buf!);
    }
  }

  private findInterval(ds: DiskState, s: number): Interval | null {
    let lo = 0, hi = ds.intervals.length - 1;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1, iv = ds.intervals[mid];
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
  read(disk: number, lba: number, count: number, dst: Uint8Array): number {
    const ds = this.disksArr[disk];
    if (!ds || lba + count > ds.meta.sectors) return 1;
    let pending = false, error = false;
    for (let i = 0; i < count; i++) {
      const s = lba + i;
      const out = dst.subarray(i * SECTOR, (i + 1) * SECTOR);
      const ov = ds.overlay.get(s);
      if (ov) { out.set(ov); continue; }
      const iv = this.findInterval(ds, s);
      if (!iv) { ds.meta.read(s, 1, out); continue; }
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

  write(disk: number, lba: number, count: number, src: Uint8Array): boolean {
    const ds = this.disksArr[disk];
    if (!ds || lba + count > ds.meta.sectors) return false;
    for (let i = 0; i < count; i++) {
      const s = lba + i;
      let ov = ds.overlay.get(s);
      if (!ov) { ov = new Uint8Array(SECTOR); ds.overlay.set(s, ov); }
      ov.set(src.subarray(i * SECTOR, (i + 1) * SECTOR));
      ds.overlayDirty.add(s);
    }
    return true;
  }

  /** Dirty overlay sectors of one disk for persistence (sector-LBA keyed). */
  takeDirtyOverlay(disk: number): Map<number, Uint8Array> {
    const ds = this.disksArr[disk];
    const out = new Map<number, Uint8Array>();
    if (!ds) return out;
    for (const s of ds.overlayDirty) out.set(s, ds.overlay.get(s)!);
    ds.overlayDirty.clear();
    return out;
  }

  loadOverlay(disk: number, m: Map<number, Uint8Array>) {
    const ds = this.disksArr[disk];
    if (!ds) return;
    for (const [s, b] of m) ds.overlay.set(s, b.slice(0, SECTOR));
  }
}

/** Walk a FileSystemDirectoryHandle (FSA picker or OPFS) into LocalEntry[]. */
export async function entriesFromDirectory(dir: FileSystemDirectoryHandle, onProgress?: (n: number) => void): Promise<LocalEntry[]> {
  const out: LocalEntry[] = [];
  let unreadable = 0;
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
      try {
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
      } catch {
        unreadable++; // one locked/cloud-only file must not kill the whole mount
      }
    }
  };
  await walk(dir, "");
  if (unreadable) console.log(`[localdrive] skipped ${unreadable} unreadable file(s)/folder(s)`);
  return out;
}

// FAT12/FAT16 image builder + reader (pure TypeScript; used by the browser shell and the Deno tools).
// Builds bootable hard-disk images (MBR + volume boot record from MS-DOS 4.01) and floppy images.

export interface FatEntry {
  name: string;      // 8.3, upper-case, e.g. "IO.SYS" or "GAMES"
  attr: number;      // 0x10 dir, 0x20 archive, 0x01 ro, 0x02 hidden, 0x04 system
  cluster: number;
  size: number;
  date: number;      // FAT date
  time: number;      // FAT time
}

const SECTOR = 512;

export function fatDateTime(d = new Date()): { date: number; time: number } {
  const y = Math.max(1980, d.getFullYear());
  return {
    date: ((y - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate(),
    time: (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1),
  };
}

/** Produce a valid 8.3 short name (upper-case, invalid chars replaced, ~N tails on collision). */
export function shortName(longName: string, taken: Set<string>): string {
  let name = longName.toUpperCase().replace(/[^A-Z0-9!#$%&'()\-@^_`{}~.]/g, "_");
  const dot = name.lastIndexOf(".");
  let base = dot > 0 ? name.slice(0, dot) : name;
  let ext = dot > 0 ? name.slice(dot + 1) : "";
  base = base.replace(/\./g, "_").replace(/ /g, "");
  ext = ext.replace(/\./g, "").slice(0, 3);
  if (!base) base = "_";
  const mk = (b: string) => ext ? `${b}.${ext}` : b;
  if (base.length <= 8 && !taken.has(mk(base))) { taken.add(mk(base)); return mk(base); }
  for (let n = 1; n < 1000000; n++) {
    const tail = "~" + n;
    const b = base.slice(0, 8 - tail.length) + tail;
    if (!taken.has(mk(b))) { taken.add(mk(b)); return mk(b); }
  }
  throw new Error("no short name available");
}

function packName(name83: string): Uint8Array {
  const out = new Uint8Array(11).fill(0x20);
  if (name83 === "." || name83 === "..") { for (let i = 0; i < name83.length; i++) out[i] = 0x2E; return out; }
  const [base, ext = ""] = name83.split(".");
  for (let i = 0; i < 8 && i < base.length; i++) out[i] = base.charCodeAt(i);
  for (let i = 0; i < 3 && i < ext.length; i++) out[8 + i] = ext.charCodeAt(i);
  if (out[0] === 0xE5) out[0] = 0x05;
  return out;
}

export function unpackName(raw: Uint8Array): string {
  const base = String.fromCharCode(...raw.subarray(0, 8)).replace(/ +$/, "");
  const ext = String.fromCharCode(...raw.subarray(8, 11)).replace(/ +$/, "");
  return ext ? `${base}.${ext}` : base;
}

export interface VolumeGeometry {
  totalSectors: number;     // sectors in the volume (partition)
  bytesPerSector: number;
  sectorsPerCluster: number;
  reservedSectors: number;
  fats: number;
  rootEntries: number;
  sectorsPerFat: number;
  media: number;
  sectorsPerTrack: number;
  heads: number;
  hiddenSectors: number;
  fat16: boolean;
  clusters: number;
}

export function planVolume(totalSectors: number, spt: number, heads: number, hidden: number, forceFloppy?: { spc: number; root: number; media: number }): VolumeGeometry {
  let spc: number, root: number, media: number;
  if (forceFloppy) { spc = forceFloppy.spc; root = forceFloppy.root; media = forceFloppy.media; }
  else {
    const mb = totalSectors / 2048;
    spc = mb <= 16 ? 1 : mb <= 128 ? 4 : mb <= 256 ? 8 : mb <= 512 ? 16 : mb <= 1024 ? 32 : 64;
    root = 512; media = 0xF8;
  }
  // iterate: clusters depend on FAT size which depends on clusters
  const rootSectors = Math.ceil(root * 32 / SECTOR);
  let fat16 = true, spf = 1, clusters = 0;
  for (let i = 0; i < 8; i++) {
    const dataSectors = totalSectors - 1 - 2 * spf - rootSectors;
    clusters = Math.floor(dataSectors / spc);
    fat16 = clusters >= 4085;
    const bytes = fat16 ? (clusters + 2) * 2 : Math.ceil((clusters + 2) * 1.5);
    const nspf = Math.ceil(bytes / SECTOR);
    if (nspf === spf) break;
    spf = nspf;
  }
  if (fat16 && clusters > 65524) throw new Error("volume too large for FAT16");
  return { totalSectors, bytesPerSector: SECTOR, sectorsPerCluster: spc, reservedSectors: 1, fats: 2, rootEntries: root, sectorsPerFat: spf, media, sectorsPerTrack: spt, heads, hiddenSectors: hidden, fat16, clusters };
}

/** In-memory FAT volume writer. */
export class FatVolume {
  img: Uint8Array;
  geo: VolumeGeometry;
  private nextFree = 2;
  private rootUsed = 0;
  private dirs = new Map<string, { head: number; tail: number; used: number; taken: Set<string> }>();
  private rootTaken = new Set<string>();

  constructor(geo: VolumeGeometry, public base: number, img: Uint8Array) {
    this.geo = geo;
    this.img = img;
  }

  get fatStart() { return this.geo.reservedSectors; }
  get rootStart() { return this.fatStart + this.geo.fats * this.geo.sectorsPerFat; }
  get rootSectors() { return Math.ceil(this.geo.rootEntries * 32 / SECTOR); }
  get dataStart() { return this.rootStart + this.rootSectors; }
  clusterSector(c: number) { return this.dataStart + (c - 2) * this.geo.sectorsPerCluster; }
  private off(sector: number) { return (this.base + sector) * SECTOR; }

  setFat(cluster: number, value: number) {
    for (let f = 0; f < this.geo.fats; f++) {
      const fatOff = this.off(this.fatStart + f * this.geo.sectorsPerFat);
      if (this.geo.fat16) {
        this.img[fatOff + cluster * 2] = value & 0xFF;
        this.img[fatOff + cluster * 2 + 1] = (value >> 8) & 0xFF;
      } else {
        const i = Math.floor(cluster * 1.5);
        if (cluster & 1) {
          this.img[fatOff + i] = (this.img[fatOff + i] & 0x0F) | ((value << 4) & 0xF0);
          this.img[fatOff + i + 1] = (value >> 4) & 0xFF;
        } else {
          this.img[fatOff + i] = value & 0xFF;
          this.img[fatOff + i + 1] = (this.img[fatOff + i + 1] & 0xF0) | ((value >> 8) & 0x0F);
        }
      }
    }
  }

  getFat(cluster: number): number {
    const fatOff = this.off(this.fatStart);
    if (this.geo.fat16) return this.img[fatOff + cluster * 2] | (this.img[fatOff + cluster * 2 + 1] << 8);
    const i = Math.floor(cluster * 1.5);
    const v = this.img[fatOff + i] | (this.img[fatOff + i + 1] << 8);
    return (cluster & 1) ? v >> 4 : v & 0xFFF;
  }

  get eoc() { return this.geo.fat16 ? 0xFFFF : 0xFFF; }
  get freeClusters() { return this.geo.clusters + 2 - this.nextFree; }

  /** Allocate a chain of n clusters (contiguous) and return the first cluster. */
  private allocChain(n: number): number {
    if (n === 0) return 0;
    if (this.nextFree + n > this.geo.clusters + 2) throw new Error("disk full");
    const first = this.nextFree;
    for (let i = 0; i < n; i++) this.setFat(first + i, i === n - 1 ? this.eoc : first + i + 1);
    this.nextFree += n;
    return first;
  }

  private writeClusters(first: number, data: Uint8Array) {
    const cs = this.geo.sectorsPerCluster * SECTOR;
    let c = first, pos = 0;
    while (pos < data.length) {
      const o = this.off(this.clusterSector(c));
      const n = Math.min(cs, data.length - pos);
      this.img.set(data.subarray(pos, pos + n), o);
      pos += n;
      c = this.getFat(c);
    }
  }

  private writeEntry(dirPath: string, e: FatEntry) {
    let entryOff: number;
    if (dirPath === "") {
      if (this.rootUsed >= this.geo.rootEntries) throw new Error("root directory full");
      entryOff = this.off(this.rootStart) + this.rootUsed * 32;
      this.rootUsed++;
    } else {
      const d = this.dirs.get(dirPath);
      if (!d) throw new Error("no such directory " + dirPath);
      const perCluster = this.geo.sectorsPerCluster * SECTOR / 32;
      if (d.used >= perCluster) { /* grow the directory by one cluster */
        const c = this.allocChain(1);
        this.setFat(d.tail, c);
        d.tail = c;
        d.used = 0;
      }
      entryOff = this.off(this.clusterSector(d.tail)) + d.used * 32;
      d.used++;
    }
    const raw = this.img.subarray(entryOff, entryOff + 32);
    if (e.attr & 0x08) { /* volume label: 11 raw characters */
      const lab = new Uint8Array(11).fill(0x20);
      for (let i = 0; i < 11 && i < e.name.length; i++) lab[i] = e.name.charCodeAt(i);
      raw.set(lab, 0);
    } else raw.set(packName(e.name), 0);
    raw[11] = e.attr;
    raw[22] = e.time & 0xFF; raw[23] = e.time >> 8;
    raw[24] = e.date & 0xFF; raw[25] = e.date >> 8;
    raw[16] = e.date & 0xFF; raw[17] = e.date >> 8; /* creation date (4.01 ignores) */
    raw[26] = e.cluster & 0xFF; raw[27] = (e.cluster >> 8) & 0xFF;
    raw[28] = e.size & 0xFF; raw[29] = (e.size >> 8) & 0xFF; raw[30] = (e.size >> 16) & 0xFF; raw[31] = (e.size >>> 24) & 0xFF;
  }

  private takenFor(dirPath: string): Set<string> {
    return dirPath === "" ? this.rootTaken : this.dirs.get(dirPath)!.taken;
  }

  /** Create a directory (parent must exist). Returns its cluster. */
  mkdir(path: string, when = new Date()): number {
    const parts = path.toUpperCase().split(/[\\/]/).filter(Boolean);
    const name = parts.pop()!;
    const parent = parts.join("\\");
    if (parent && !this.dirs.has(parent)) this.mkdir(parent, when);
    const full = parent ? `${parent}\\${name}` : name;
    if (this.dirs.has(full)) return this.dirs.get(full)!.head;
    const c = this.allocChain(1);
    const { date, time } = fatDateTime(when);
    const sn = shortName(name, this.takenFor(parent));
    this.writeEntry(parent, { name: sn, attr: 0x10, cluster: c, size: 0, date, time });
    this.dirs.set(full, { head: c, tail: c, used: 0, taken: new Set() });
    const parentHead = parent ? this.dirs.get(parent)!.head : 0;
    this.writeEntry(full, { name: ".", attr: 0x10, cluster: c, size: 0, date, time });
    this.writeEntry(full, { name: "..", attr: 0x10, cluster: parentHead, size: 0, date, time });
    return c;
  }

  /** Add a file; directories are created as needed. */
  addFile(path: string, data: Uint8Array, attr = 0x20, when = new Date()) {
    const parts = path.split(/[\\/]/).filter(Boolean);
    const fname = parts.pop()!;
    const dir = parts.join("\\").toUpperCase();
    if (dir && !this.dirs.has(dir)) this.mkdir(dir, when);
    const cs = this.geo.sectorsPerCluster * SECTOR;
    const n = Math.ceil(data.length / cs);
    const first = this.allocChain(n);
    if (n) this.writeClusters(first, data);
    const { date, time } = fatDateTime(when);
    const sn = shortName(fname, this.takenFor(dir));
    this.writeEntry(dir, { name: sn, attr, cluster: first, size: data.length, date, time });
  }

  /** Write the boot sector (MS-DOS 4.01 MSBOOT.BIN with BPB filled in) and initialise the FATs. */
  format(bootCode: Uint8Array, label = "DOS MOBILE", serial = 0x12345678) {
    const bs = this.img.subarray(this.off(0), this.off(0) + SECTOR);
    bs.set(bootCode.subarray(0, SECTOR));
    const g = this.geo;
    const w16 = (o: number, v: number) => { bs[o] = v & 0xFF; bs[o + 1] = (v >> 8) & 0xFF; };
    const w32 = (o: number, v: number) => { w16(o, v & 0xFFFF); w16(o + 2, (v >>> 16) & 0xFFFF); };
    bs.set(new TextEncoder().encode("MSDOS4.0"), 3);
    w16(0x0B, g.bytesPerSector);
    bs[0x0D] = g.sectorsPerCluster;
    w16(0x0E, g.reservedSectors);
    bs[0x10] = g.fats;
    w16(0x11, g.rootEntries);
    w16(0x13, g.totalSectors < 65536 ? g.totalSectors : 0);
    bs[0x15] = g.media;
    w16(0x16, g.sectorsPerFat);
    w16(0x18, g.sectorsPerTrack);
    w16(0x1A, g.heads);
    w32(0x1C, g.hiddenSectors);
    w32(0x20, g.totalSectors < 65536 ? 0 : g.totalSectors);
    bs[0x24] = g.media === 0xF8 ? 0x80 : 0x00;
    bs[0x25] = 0;
    bs[0x26] = 0x29;
    w32(0x27, serial);
    const lab = new Uint8Array(11).fill(0x20); lab.set(new TextEncoder().encode(label.slice(0, 11)));
    bs.set(lab, 0x2B);
    bs.set(new TextEncoder().encode(g.fat16 ? "FAT16   " : "FAT12   "), 0x36);
    bs[0x1FE] = 0x55; bs[0x1FF] = 0xAA;
    // FATs: media descriptor entry + EOC
    for (let f = 0; f < g.fats; f++) {
      const o = this.off(this.fatStart + f * g.sectorsPerFat);
      this.img.fill(0, o, o + g.sectorsPerFat * SECTOR);
    }
    this.setFat(0, g.fat16 ? 0xFF00 | g.media : 0xF00 | g.media);
    this.setFat(1, this.eoc);
    // root directory clear
    this.img.fill(0, this.off(this.rootStart), this.off(this.rootStart) + this.rootSectors * SECTOR);
    // volume label entry
    const { date, time } = fatDateTime();
    this.writeEntry("", { name: label.toUpperCase().replace(/\./g, ""), attr: 0x08, cluster: 0, size: 0, date, time });
    this.rootUsed = 0; // the label is written later: keep IO.SYS/MSDOS.SYS as entries 0 and 1
    this.img.fill(0, this.off(this.rootStart), this.off(this.rootStart) + 32);
  }

  /** Volume label goes after the system files. */
  writeLabel(label: string) {
    const { date, time } = fatDateTime();
    this.writeEntry("", { name: label.toUpperCase().slice(0, 11), attr: 0x08, cluster: 0, size: 0, date, time });
  }

}

/** MBR boot code: relocates to 0000:0600, loads the active partition's boot sector, jumps to it. */
export const MBR_CODE = new Uint8Array([
  0xFA, 0x33, 0xC0, 0x8E, 0xD0, 0xBC, 0x00, 0x7C, 0x8E, 0xD8, 0x8E, 0xC0, 0xFB, 0xFC, 0xBE, 0x00,
  0x7C, 0xBF, 0x00, 0x06, 0xB9, 0x00, 0x01, 0xF3, 0xA5, 0xEA, 0x1E, 0x06, 0x00, 0x00, 0xBE, 0xBE,
  0x07, 0xB9, 0x04, 0x00, 0xF6, 0x04, 0x80, 0x75, 0x07, 0x83, 0xC6, 0x10, 0xE2, 0xF6, 0xCD, 0x18,
  0x8A, 0x74, 0x01, 0x8B, 0x4C, 0x02, 0xBB, 0x00, 0x7C, 0xB8, 0x01, 0x02, 0xCD, 0x13, 0x72, 0x0B,
  0x81, 0x3E, 0xFE, 0x7D, 0x55, 0xAA, 0x75, 0x03, 0xEA, 0x00, 0x7C, 0x00, 0x00, 0xBE, 0x5E, 0x06,
  0xAC, 0x3C, 0x00, 0x74, 0x08, 0xB4, 0x0E, 0xB7, 0x00, 0xCD, 0x10, 0xEB, 0xF3, 0xF4, 0xEB, 0xFD,
  ...new TextEncoder().encode("Boot error\r\n"), 0x00,
]);

export interface HddImage { image: Uint8Array; volume: FatVolume; }

/** Create a hard-disk image: MBR + one FAT partition starting at LBA 63. */
export function createHddImage(sizeMB: number, bootCode: Uint8Array, label = "DOS MOBILE"): HddImage {
  const heads = sizeMB > 504 ? 255 : 16, spt = 63;
  const cylSectors = heads * spt;
  const totalSectors = Math.floor(sizeMB * 2048 / cylSectors) * cylSectors;
  const cyls = totalSectors / cylSectors;
  const img = new Uint8Array(totalSectors * SECTOR);
  const partStart = spt;
  const partSectors = totalSectors - partStart;
  const geo = planVolume(partSectors, spt, heads, partStart);
  // MBR
  img.set(MBR_CODE, 0);
  const pe = 0x1BE;
  const chs = (lba: number) => {
    const c = Math.floor(lba / cylSectors), h = Math.floor((lba % cylSectors) / spt), s = (lba % spt) + 1;
    const cc = Math.min(c, 1023);
    return [h, (s & 0x3F) | ((cc >> 2) & 0xC0), cc & 0xFF];
  };
  img[pe] = 0x80;
  img.set(chs(partStart), pe + 1);
  img[pe + 4] = geo.fat16 ? (partSectors < 65536 ? 0x04 : 0x06) : 0x01;
  img.set(chs(partStart + partSectors - 1), pe + 5);
  const w32 = (o: number, v: number) => { img[o] = v & 0xFF; img[o + 1] = (v >> 8) & 0xFF; img[o + 2] = (v >> 16) & 0xFF; img[o + 3] = (v >>> 24) & 0xFF; };
  w32(pe + 8, partStart);
  w32(pe + 12, partSectors);
  img[0x1FE] = 0x55; img[0x1FF] = 0xAA;
  void cyls;
  const volume = new FatVolume(geo, partStart, img);
  volume.format(bootCode, label);
  return { image: img, volume };
}

/** Create a floppy image (sizeKB = 360, 720, 1200, 1440, 2880). */
export function createFloppyImage(sizeKB: number, bootCode?: Uint8Array, label = "DOS MOBILE"): HddImage {
  const table: Record<number, { spt: number; heads: number; spc: number; root: number; media: number }> = {
    360: { spt: 9, heads: 2, spc: 2, root: 112, media: 0xFD },
    720: { spt: 9, heads: 2, spc: 2, root: 112, media: 0xF9 },
    1200: { spt: 15, heads: 2, spc: 1, root: 224, media: 0xF9 },
    1440: { spt: 18, heads: 2, spc: 1, root: 224, media: 0xF0 },
    2880: { spt: 36, heads: 2, spc: 2, root: 240, media: 0xF0 },
  };
  const t = table[sizeKB];
  if (!t) throw new Error("unsupported floppy size");
  const totalSectors = sizeKB * 2;
  const img = new Uint8Array(totalSectors * SECTOR);
  const geo = planVolume(totalSectors, t.spt, t.heads, 0, { spc: t.spc, root: t.root, media: t.media });
  const volume = new FatVolume(geo, 0, img);
  const code = bootCode ?? new Uint8Array(SECTOR);
  volume.format(code, label);
  if (!bootCode) { img[0] = 0xEB; img[1] = 0x3C; img[2] = 0x90; }
  return { image: img, volume };
}

/** Read-only directory listing (for export / tests). */
export function listRoot(img: Uint8Array, base = 0): FatEntry[] {
  const r16 = (o: number) => img[o] | (img[o + 1] << 8);
  const bs = base * SECTOR;
  const reserved = r16(bs + 0x0E), fats = img[bs + 0x10], rootEntries = r16(bs + 0x11), spf = r16(bs + 0x16);
  const rootStart = (base + reserved + fats * spf) * SECTOR;
  const out: FatEntry[] = [];
  for (let i = 0; i < rootEntries; i++) {
    const o = rootStart + i * 32;
    if (img[o] === 0) break;
    if (img[o] === 0xE5) continue;
    out.push({ name: unpackName(img.subarray(o, o + 11)), attr: img[o + 11], cluster: r16(o + 26), size: r16(o + 28) | (r16(o + 30) << 16), date: r16(o + 24), time: r16(o + 22) });
  }
  return out;
}

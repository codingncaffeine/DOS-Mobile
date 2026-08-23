// Hard-disk image creation on a sector store: MBR with a primary partition (C:) plus an extended
// partition holding further 2 GB logical drives, each formatted FAT16 for MS-DOS 4.01.
// Only the sectors that matter (MBR, boot records, FATs, root directories) are written, so a
// multi-gigabyte disk in the sparse IndexedDB store costs a few hundred KB until it is used.
import { MBR_CODE, planVolume, type VolumeGeometry } from "./fat.ts";
import type { SectorIO } from "./fatfs.ts";

const SECTOR = 512;
export const MAX_VOLUME_MB = 2047; // FAT16 ceiling for MS-DOS 4.01 (32 KB clusters)

export interface DiskPlan {
  heads: number; spt: number; cyls: number; totalSectors: number;
  volumes: { startLba: number; sectors: number; logical: boolean }[];
}

/** Split a disk of sizeMB into volumes: C: up to 2 GB, the rest as logical drives in an extended partition. */
export function planDisk(sizeMB: number, maxVolumeMB = MAX_VOLUME_MB): DiskPlan {
  const heads = sizeMB > 504 ? 255 : 16, spt = 63;
  const cylSectors = heads * spt;
  let cyls = Math.floor(sizeMB * 2048 / cylSectors);
  if (cyls > 1024) cyls = 1024;
  if (cyls < 2) cyls = 2;
  const totalSectors = cyls * cylSectors;
  const volumes: DiskPlan["volumes"] = [];
  const maxVol = maxVolumeMB * 2048;
  // primary: starts at track 1, ends on a cylinder boundary
  let start = spt;
  let remaining = totalSectors - start;
  const primary = Math.min(remaining, Math.floor(maxVol / cylSectors) * cylSectors - spt);
  volumes.push({ startLba: start, sectors: primary, logical: false });
  start += primary;
  remaining -= primary;
  // logical drives inside the extended partition: each EBR takes one track, each volume is cylinder-aligned
  while (remaining >= cylSectors * 2) {
    const ebr = start; // EBR at the start of the cylinder, volume starts one track later
    const volStart = ebr + spt;
    const size = Math.min(remaining - spt, Math.floor(maxVol / cylSectors) * cylSectors - spt);
    if (size < cylSectors) break;
    volumes.push({ startLba: volStart, sectors: size, logical: true });
    start = volStart + size;
    remaining = totalSectors - start;
  }
  return { heads, spt, cyls, totalSectors, volumes };
}

function chs(lba: number, heads: number, spt: number): [number, number, number] {
  const cylSectors = heads * spt;
  const c = Math.min(Math.floor(lba / cylSectors), 1023);
  const h = Math.floor((lba % cylSectors) / spt), s = (lba % spt) + 1;
  return [h, (s & 0x3F) | ((c >> 2) & 0xC0), c & 0xFF];
}

/* CHS fields are absolute disk positions; the LBA field is relative to the table's own base
 * (0 for the MBR, the extended partition start for EBR links, the EBR itself for its volume). */
export function partEntry(buf: Uint8Array, off: number, active: boolean, type: number, absStart: number, relStart: number, sectors: number, heads: number, spt: number) {
  buf[off] = active ? 0x80 : 0x00;
  buf.set(chs(absStart, heads, spt), off + 1);
  buf[off + 4] = type;
  buf.set(chs(absStart + sectors - 1, heads, spt), off + 5);
  const w32 = (o: number, v: number) => { buf[o] = v & 0xFF; buf[o + 1] = (v >> 8) & 0xFF; buf[o + 2] = (v >> 16) & 0xFF; buf[o + 3] = (v >>> 24) & 0xFF; };
  w32(off + 8, relStart);
  w32(off + 12, sectors);
}

function volumeType(geo: VolumeGeometry): number {
  return geo.fat16 ? (geo.totalSectors < 65536 ? 0x04 : 0x06) : 0x01;
}

/** Write the boot sector + FATs + root directory of one FAT volume. */
export function formatVolume(io: SectorIO, startLba: number, sectors: number, spt: number, heads: number, bootCode: Uint8Array, label: string, serial: number): VolumeGeometry {
  const g = planVolume(sectors, spt, heads, startLba);
  const bs = new Uint8Array(SECTOR);
  bs.set(bootCode.subarray(0, SECTOR));
  const w16 = (o: number, v: number) => { bs[o] = v & 0xFF; bs[o + 1] = (v >> 8) & 0xFF; };
  const w32 = (o: number, v: number) => { w16(o, v & 0xFFFF); w16(o + 2, (v >>> 16) & 0xFFFF); };
  bs.set(new TextEncoder().encode("MSDOS4.0"), 3);
  w16(0x0B, 512); bs[0x0D] = g.sectorsPerCluster; w16(0x0E, g.reservedSectors); bs[0x10] = g.fats; w16(0x11, g.rootEntries);
  w16(0x13, g.totalSectors < 65536 ? g.totalSectors : 0); bs[0x15] = g.media; w16(0x16, g.sectorsPerFat);
  w16(0x18, spt); w16(0x1A, heads); w32(0x1C, startLba); w32(0x20, g.totalSectors < 65536 ? 0 : g.totalSectors);
  bs[0x24] = 0x80; bs[0x25] = 0; bs[0x26] = 0x29; w32(0x27, serial);
  const lab = new Uint8Array(11).fill(0x20); lab.set(new TextEncoder().encode(label.slice(0, 11))); bs.set(lab, 0x2B);
  bs.set(new TextEncoder().encode(g.fat16 ? "FAT16   " : "FAT12   "), 0x36);
  bs[0x1FE] = 0x55; bs[0x1FF] = 0xAA;
  io.writeSectors(startLba, 1, bs);
  // FATs: zero, then the media/EOC entries
  const zero = new Uint8Array(SECTOR * 64);
  const fatStart = startLba + g.reservedSectors;
  for (let f = 0; f < g.fats; f++) {
    for (let s = 0; s < g.sectorsPerFat; s += 64) io.writeSectors(fatStart + f * g.sectorsPerFat + s, Math.min(64, g.sectorsPerFat - s), zero);
    const first = new Uint8Array(SECTOR);
    if (g.fat16) { first[0] = g.media; first[1] = 0xFF; first[2] = 0xFF; first[3] = 0xFF; }
    else { first[0] = g.media; first[1] = 0xFF; first[2] = 0xFF; }
    io.writeSectors(fatStart + f * g.sectorsPerFat, 1, first);
  }
  const rootStart = fatStart + g.fats * g.sectorsPerFat;
  const rootSectors = Math.ceil(g.rootEntries * 32 / SECTOR);
  for (let s = 0; s < rootSectors; s += 64) io.writeSectors(rootStart + s, Math.min(64, rootSectors - s), zero);
  return g;
}

/** Create the partition table(s) and format every volume. Returns the plan. */
export function formatDisk(io: SectorIO, sizeMB: number, bootCode: Uint8Array, label = "DOS MOBILE", maxVolumeMB = MAX_VOLUME_MB): DiskPlan {
  const plan = planDisk(sizeMB, maxVolumeMB);
  const { heads, spt } = plan;
  const mbr = new Uint8Array(SECTOR);
  mbr.set(MBR_CODE, 0);
  const primary = plan.volumes[0];
  const pg = formatVolume(io, primary.startLba, primary.sectors, spt, heads, bootCode, label, 0x12345678);
  partEntry(mbr, 0x1BE, true, volumeType(pg), primary.startLba, primary.startLba, primary.sectors, heads, spt);
  const logicals = plan.volumes.slice(1);
  if (logicals.length) {
    const extStart = logicals[0].startLba - spt;
    const extEnd = logicals[logicals.length - 1].startLba + logicals[logicals.length - 1].sectors;
    partEntry(mbr, 0x1CE, false, 0x05, extStart, extStart, extEnd - extStart, heads, spt);
    // EBR chain: each EBR holds its own volume (relative to the EBR) and a link to the next EBR (relative to the extended start)
    for (let i = 0; i < logicals.length; i++) {
      const v = logicals[i];
      const ebrLba = v.startLba - spt;
      const ebr = new Uint8Array(SECTOR);
      const g = formatVolume(io, v.startLba, v.sectors, spt, heads, bootCode, `DRIVE ${String.fromCharCode(68 + i)}`, 0x20000000 + i);
      partEntry(ebr, 0x1BE, false, volumeType(g), v.startLba, spt, v.sectors, heads, spt);
      if (i + 1 < logicals.length) {
        const next = logicals[i + 1];
        const nextEbr = next.startLba - spt;
        partEntry(ebr, 0x1CE, false, 0x05, nextEbr, nextEbr - extStart, next.sectors + spt, heads, spt);
      }
      ebr[0x1FE] = 0x55; ebr[0x1FF] = 0xAA;
      io.writeSectors(ebrLba, 1, ebr);
    }
  }
  mbr[0x1FE] = 0x55; mbr[0x1FF] = 0xAA;
  io.writeSectors(0, 1, mbr);
  return plan;
}

/** Plan a disk whose volumes are ALL logical drives in one extended partition (no primary).
 * MS-DOS 4.01 sets up primaries only for the first two BIOS disks, but walks the extended
 * chain of EVERY disk - so third and later disks carry their volumes this way. */
export function planDiskExt(sizeMB: number, maxVolumeMB = MAX_VOLUME_MB): DiskPlan {
  const heads = sizeMB > 504 ? 255 : 16, spt = 63;
  const cylSectors = heads * spt;
  let cyls = Math.floor(sizeMB * 2048 / cylSectors);
  if (cyls > 1024) cyls = 1024;
  if (cyls < 3) cyls = 3;
  const totalSectors = cyls * cylSectors;
  const volumes: DiskPlan["volumes"] = [];
  const maxVol = maxVolumeMB * 2048;
  let start = cylSectors; // cylinder 0 holds only the MBR; the extended partition starts here
  let remaining = totalSectors - start;
  while (remaining >= cylSectors * 2) {
    const ebr = start;
    const volStart = ebr + spt;
    const size = Math.min(remaining - spt, Math.floor(maxVol / cylSectors) * cylSectors - spt);
    if (size < cylSectors) break;
    volumes.push({ startLba: volStart, sectors: size, logical: true });
    start = volStart + size;
    remaining = totalSectors - start;
  }
  return { heads, spt, cyls, totalSectors, volumes };
}

/** Partition + format an extended-only disk (see planDiskExt). */
export function formatDiskExt(io: SectorIO, sizeMB: number, bootCode: Uint8Array, label = "DOS MOBILE", maxVolumeMB = MAX_VOLUME_MB): DiskPlan {
  const plan = planDiskExt(sizeMB, maxVolumeMB);
  const { heads, spt } = plan;
  const logicals = plan.volumes;
  if (!logicals.length) throw new Error("disk too small for a logical volume");
  const extStart = logicals[0].startLba - spt;
  const extEnd = logicals[logicals.length - 1].startLba + logicals[logicals.length - 1].sectors;
  const mbr = new Uint8Array(SECTOR);
  mbr.set(MBR_CODE, 0);
  partEntry(mbr, 0x1BE, false, 0x05, extStart, extStart, extEnd - extStart, heads, spt);
  mbr[0x1FE] = 0x55; mbr[0x1FF] = 0xAA;
  io.writeSectors(0, 1, mbr);
  for (let i = 0; i < logicals.length; i++) {
    const v = logicals[i];
    const ebrLba = v.startLba - spt;
    const ebr = new Uint8Array(SECTOR);
    const g = formatVolume(io, v.startLba, v.sectors, spt, heads, bootCode, `${label.slice(0, 9)} ${i}`, 0x30000000 + i);
    partEntry(ebr, 0x1BE, false, volumeType(g), v.startLba, spt, v.sectors, heads, spt);
    if (i + 1 < logicals.length) {
      const next = logicals[i + 1];
      const nextEbr = next.startLba - spt;
      partEntry(ebr, 0x1CE, false, 0x05, nextEbr, nextEbr - extStart, next.sectors + spt, heads, spt);
    }
    ebr[0x1FE] = 0x55; ebr[0x1FF] = 0xAA;
    io.writeSectors(ebrLba, 1, ebr);
  }
  return plan;
}

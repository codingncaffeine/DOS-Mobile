// Builds the bootable C: drive from the MS-DOS 4.01 file set (dos/ directory) on any sector store.
import { formatDisk, type DiskPlan } from "./hdd.ts";
import { FatFs, type SectorIO } from "./fatfs.ts";
import { buildMouseSys } from "./mousesys.ts";

export const SYSTEM_FILES = ["IO.SYS", "MSDOS.SYS"] as const;
export const ROOT_FILES = ["COMMAND.COM"] as const;
export const DEFAULT_HDD_MB = 8192; /* the CHS ceiling (1024x255x63): C:..F: at the 2 GB FAT16 limit of MS-DOS 4.01 */

export const DEFAULT_CONFIG_SYS = [
  "FILES=30",
  "BUFFERS=20",
  "LASTDRIVE=Z",
  "BREAK=ON",
  "DEVICE=C:\\DOS\\MOUSE.SYS",
  "SHELL=C:\\COMMAND.COM C:\\ /P /E:512",
  "INSTALL=C:\\DOS\\SHARE.EXE",
  "",
].join("\r\n");

export const DEFAULT_AUTOEXEC_BAT = [
  "@ECHO OFF",
  "PATH C:\\DOS;C:\\",
  "PROMPT $P$G",
  "SET BLASTER=A220 I5 D1 H5 P330 T6",
  "SET TEMP=C:\\TEMP",
  "ECHO.",
  "ECHO DOS Mobile - drop your programs into C:\\GAMES",
  "ECHO.",
  "",
].join("\r\n");

const enc = (s: string) => new TextEncoder().encode(s);

export interface SystemDiskOptions { configSys?: string; autoexec?: string; }

/** Format the store as a disk of sizeMB and install MS-DOS on C:. files: upper-case name -> bytes. */
export function buildSystemDisk(io: SectorIO, sizeMB: number, files: Map<string, Uint8Array>, extras?: SystemDiskOptions): DiskPlan {
  const boot = files.get("MSBOOT.BIN");
  if (!boot) throw new Error("MSBOOT.BIN missing");
  for (const f of [...SYSTEM_FILES, ...ROOT_FILES]) if (!files.has(f)) throw new Error(`${f} missing`);
  const plan = formatDisk(io, sizeMB, boot, "DOS MOBILE");
  const fs = new FatFs(io).mount();
  const when = new Date(1988, 9, 6, 12, 0, 0); /* the 4.01 release date, for a tidy DIR */
  /* the boot sector requires IO.SYS and MSDOS.SYS to be the first two root entries */
  fs.writeFile(0, "IO.SYS", files.get("IO.SYS")!, when, 0x27);
  fs.writeFile(0, "MSDOS.SYS", files.get("MSDOS.SYS")!, when, 0x27);
  fs.writeLabel("DOS MOBILE");
  fs.writeFile(0, "COMMAND.COM", files.get("COMMAND.COM")!, when);
  fs.writeFile(0, "CONFIG.SYS", enc(extras?.configSys ?? DEFAULT_CONFIG_SYS));
  fs.writeFile(0, "AUTOEXEC.BAT", enc(extras?.autoexec ?? DEFAULT_AUTOEXEC_BAT));
  const dos = fs.mkdir(0, "DOS", when).cluster;
  fs.writeFile(dos, "MOUSE.SYS", buildMouseSys(), when);
  const skip = new Set<string>([...SYSTEM_FILES, ...ROOT_FILES, "MSBOOT.BIN", "LICENSE-MSDOS.TXT", "MANIFEST.JSON"]);
  const names = [...files.keys()].filter((n) => !skip.has(n.toUpperCase())).sort();
  for (const n of names) fs.writeFile(dos, n, files.get(n)!, when);
  if (files.has("LICENSE-MSDOS.TXT")) fs.writeFile(dos, "LICENSE.TXT", files.get("LICENSE-MSDOS.TXT")!, when);
  fs.mkdir(0, "GAMES");
  fs.mkdir(0, "TEMP");
  fs.flush();
  return plan;
}

// Builds the bootable C: image from the MS-DOS 4.01 file set (dos/ directory).
import { createHddImage, type HddImage } from "./fat.ts";

export const SYSTEM_FILES = ["IO.SYS", "MSDOS.SYS"] as const;
export const ROOT_FILES = ["COMMAND.COM"] as const;

export const DEFAULT_CONFIG_SYS = [
  "FILES=30",
  "BUFFERS=20",
  "LASTDRIVE=E",
  "BREAK=ON",
  "SHELL=C:\\COMMAND.COM C:\\ /P /E:512",
  "INSTALL=C:\\DOS\\SHARE.EXE",
  "",
].join("\r\n");

export const DEFAULT_AUTOEXEC_BAT = [
  "@ECHO OFF",
  "PATH C:\\DOS;C:\\",
  "PROMPT $P$G",
  "SET TEMP=C:\\TEMP",
  "ECHO.",
  "ECHO DOS Mobile - drop your programs into C:\\GAMES",
  "ECHO.",
  "",
].join("\r\n");

const enc = (s: string) => new TextEncoder().encode(s);

/** files: name (upper-case, e.g. "IO.SYS") -> bytes. Must include IO.SYS, MSDOS.SYS, COMMAND.COM, MSBOOT.BIN. */
export function buildSystemDisk(sizeMB: number, files: Map<string, Uint8Array>, extras?: { configSys?: string; autoexec?: string }): HddImage {
  const boot = files.get("MSBOOT.BIN");
  if (!boot) throw new Error("MSBOOT.BIN missing");
  for (const f of [...SYSTEM_FILES, ...ROOT_FILES]) if (!files.has(f)) throw new Error(`${f} missing`);
  const disk = createHddImage(sizeMB, boot, "DOS MOBILE");
  const v = disk.volume;
  const when = new Date(1988, 9, 6, 12, 0, 0); /* the 4.01 release date, for a tidy DIR */
  v.addFile("IO.SYS", files.get("IO.SYS")!, 0x27, when);
  v.addFile("MSDOS.SYS", files.get("MSDOS.SYS")!, 0x27, when);
  v.writeLabel("DOS MOBILE");
  v.addFile("COMMAND.COM", files.get("COMMAND.COM")!, 0x20, when);
  v.addFile("CONFIG.SYS", enc(extras?.configSys ?? DEFAULT_CONFIG_SYS), 0x20);
  v.addFile("AUTOEXEC.BAT", enc(extras?.autoexec ?? DEFAULT_AUTOEXEC_BAT), 0x20);
  v.mkdir("DOS", when);
  const skip = new Set<string>([...SYSTEM_FILES, ...ROOT_FILES, "MSBOOT.BIN", "LICENSE-MSDOS.TXT", "MANIFEST.JSON"]);
  const names = [...files.keys()].filter((n) => !skip.has(n.toUpperCase())).sort();
  for (const n of names) v.addFile(`DOS\\${n}`, files.get(n)!, 0x20, when);
  if (files.has("LICENSE-MSDOS.TXT")) v.addFile("DOS\\LICENSE.TXT", files.get("LICENSE-MSDOS.TXT")!, 0x20, when);
  v.mkdir("GAMES");
  v.mkdir("TEMP");
  return disk;
}

// Dogfood oracle: rebuild MS-DOS 4.01 INSIDE the emulator with its own toolchain and compare the
// output byte-for-byte with the reference build in dos/. Exercises MASM, MSC, LINK, NMAKE — large
// real-mode programs — under our CPU, BIOS, DOS and disk path.
//   deno run -A tools/dogfood.ts [--minutes 20] [--gen 5] [--mhz 66] [--dir CMD\\MORE]
// Requires tools/_msdos-build/src (deno run -A tools/msdos-build.ts --stage).
import { fromFileUrl, join } from "jsr:@std/path@1";
import { Core, GEN } from "../web/core.ts";
import { buildSystemDisk } from "../web/sysdisk.ts";
import { planDisk } from "../web/hdd.ts";
import { ArraySectorIO, FatFs } from "../web/fatfs.ts";

const root = fromFileUrl(new URL("..", import.meta.url));
const a = Deno.args;
const opt = (n: string, d: string) => { const i = a.indexOf("--" + n); return i >= 0 ? a[i + 1] : d; };
const minutes = Number(opt("minutes", "20"));
const gen = Number(opt("gen", String(GEN.G486)));
const mhz = Number(opt("mhz", "66"));
const subdir = opt("dir", ""); // build only one directory (e.g. CMD\MORE) for a quick run

const srcDir = join(root, "tools", "_msdos-build", "src");
const files = new Map<string, Uint8Array>();
for await (const e of Deno.readDir(join(root, "dos"))) if (e.isFile) files.set(e.name.toUpperCase(), await Deno.readFile(join(root, "dos", e.name)));
const autoexec = [
  "@ECHO OFF", "PATH C:\\DOS;C:\\", "PROMPT $P$G",
  "SET CL=", "SET LINK=", "SET MASM=", "SET COUNTRY=usa-ms", "SET BAKROOT=C:",
  "SET LIB=C:\\SRC\\TOOLS\\BLD\\LIB", "SET INIT=C:\\SRC\\TOOLS", "SET INCLUDE=C:\\SRC\\TOOLS\\BLD\\INC",
  "SET PATH=C:\\SRC\\TOOLS;C:\\DOS;C:\\",
  subdir ? `CD C:\\SRC\\${subdir}` : "CD C:\\SRC",
  "NMAKE /I > C:\\BUILD.LOG",
  "ECHO NMAKE-DONE >> C:\\BUILD.LOG",
  ...(subdir ? [] : ["CD C:\\SRC", "CALL CPY.BAT C:\\BIN >> C:\\BUILD.LOG"]),
  "ECHO DONE > C:\\DONE.TXT",
  "",
].join("\r\n");
const image = new Uint8Array(planDisk(96).totalSectors * 512);
buildSystemDisk(new ArraySectorIO(image), 96, files, { autoexec });
console.log("copying the source tree into the image …");
const fs = new FatFs(new ArraySectorIO(image)).mount();
let n = 0;
async function copyDir(host: string, cluster: number) {
  for await (const e of Deno.readDir(host)) {
    if (e.isDirectory) { const d = fs.mkdir(cluster, e.name); await copyDir(join(host, e.name), d.cluster); }
    else { fs.writeFile(cluster, e.name, await Deno.readFile(join(host, e.name))); n++; }
  }
}
const src = fs.mkdir(0, "SRC");
await copyDir(srcDir, src.cluster);
fs.mkdir(0, "BIN");
fs.flush();
console.log(`${n} files copied; free clusters ${fs.freeClusterCount()}`);

class MemDisks {
  constructor(public img: Uint8Array) {}
  read(_s: number, lba: number, count: number, dst: Uint8Array) { dst.set(this.img.subarray(lba * 512, (lba + count) * 512)); return true; }
  write(_s: number, lba: number, count: number, srcb: Uint8Array) { this.img.set(srcb, lba * 512); return true; }
}
const core = new Core(new MemDisks(image), (s) => console.log("[core] " + s));
await core.load(await Deno.readFile(join(root, "dist", "dosmobile.wasm")));
core.ex.core_init(gen, Math.round(mhz * 1000), 8192, 0, 1, 4, 0);
core.ex.core_disk_attach(2, image.length / 512, 0);

const t0 = performance.now();
let done = false;
for (let ms = 0; ms < minutes * 60_000; ms += 100) {
  const r = core.ex.core_run_us(100_000);
  if (r === 1) { console.log("FATAL"); break; }
  if (ms % 5000 === 0) {
    const f = new FatFs(new ArraySectorIO(image)).mount();
    if (f.find(0, "DONE.TXT")) { done = true; break; }
    if (ms % 30000 === 0) console.log(`  ${ms / 1000}s emulated, ${((performance.now() - t0) / 1000).toFixed(0)}s wall; screen: ${core.textScreen().filter((l) => l.trim()).slice(-1)[0] ?? ""}`);
  }
}
console.log(done ? "build finished" : "timed out", `after ${((performance.now() - t0) / 1000).toFixed(0)} s wall`);
console.log("--- screen ---");
for (const l of core.textScreen()) if (l.trim()) console.log("|" + l);

const fsOut = new FatFs(new ArraySectorIO(image)).mount();
const logEntry = fsOut.find(0, "BUILD.LOG");
if (logEntry) {
  const log = new TextDecoder().decode(fsOut.readFile(logEntry));
  await Deno.writeTextFile(join(root, ".cache", "dogfood-build.log"), log);
  const errs = log.split(/\r?\n/).filter((l) => /error|fatal/i.test(l) && !/0 Warning|0 Severe|error\.(asm|obj|c|h)/i.test(l));
  console.log(`build log: ${log.length} bytes, ${errs.length} error lines`);
  for (const e of errs.slice(0, 20)) console.log("  " + e);
}
// compare
const binDir = subdir ? fsOut.resolveDir("SRC\\" + subdir) : fsOut.resolveDir("BIN");
if (binDir !== undefined) {
  let same = 0, diff = 0, missing = 0;
  const entries = fsOut.list(binDir).filter((e) => !(e.attr & 0x10));
  for (const e of entries) {
    const ref = files.get(e.name);
    if (!ref) continue;
    const got = fsOut.readFile(e);
    if (got.length === ref.length && got.every((b, i) => b === ref[i])) same++;
    else { diff++; console.log(`  differs: ${e.name} (${got.length} vs ${ref.length})`); }
  }
  for (const name of ["IO.SYS", "MSDOS.SYS", "COMMAND.COM"]) if (!entries.some((e) => e.name === name)) missing++;
  console.log(`compare vs dos/: ${same} identical, ${diff} different, ${missing} of the core three missing`);
}

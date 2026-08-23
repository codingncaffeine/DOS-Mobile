// Headless runner: boots the core in Deno, optionally with a disk image, types text, dumps the screen.
// Usage: deno run -A tools/headless.ts [--hdd path] [--floppy path] [--ms N] [--type "text\n"] [--gen 5] [--mhz 66]
import { fromFileUrl, join } from "jsr:@std/path@1";
import { Core, GEN, textToScancodes } from "../web/core.ts";
import { SparseImage } from "../web/store.ts";
import { buildSystemDisk } from "../web/sysdisk.ts";
import { planDisk } from "../web/hdd.ts";
import type { SectorIO } from "../web/fatfs.ts";

const root = fromFileUrl(new URL("..", import.meta.url));
const a = Deno.args;
const opt = (name: string, def?: string) => { const i = a.indexOf("--" + name); return i >= 0 ? a[i + 1] : def; };

class FileDisks {
  images = new Map<number, Uint8Array>();
  sparse = new Map<number, SparseImage>();
  read(slot: number, lba: number, count: number, dst: Uint8Array) {
    const sp = this.sparse.get(slot); if (sp) return sp.read(lba, count, dst);
    const img = this.images.get(slot); if (!img) return false;
    const off = lba * 512;
    if (off + count * 512 > img.length) return false;
    dst.set(img.subarray(off, off + count * 512));
    return true;
  }
  write(slot: number, lba: number, count: number, src: Uint8Array) {
    const sp = this.sparse.get(slot); if (sp) return sp.write(lba, count, src);
    const img = this.images.get(slot); if (!img) return false;
    const off = lba * 512;
    if (off + count * 512 > img.length) return false;
    img.set(src, off);
    return true;
  }
}

const disks = new FileDisks();
const core = new Core(disks, (s) => console.log("[core] " + s));
await core.load(await Deno.readFile(join(root, "dist", "dosmobile.wasm")));
const gen = Number(opt("gen", String(GEN.G486)));
const mhz = Number(opt("mhz", "66"));
core.ex.core_init(gen, Math.round(mhz * 1000), Number(opt("ram", "8192")), 0, 1, 4, 0);
const now = new Date();
core.ex.core_set_time(now.getFullYear(), now.getMonth() + 1, now.getDate(), now.getHours(), now.getMinutes(), now.getSeconds());

if (opt("trace")) core.ex.core_set_trace(Number(opt("trace")));
const sparseMb = opt("sparse-mb");
if (sparseMb) { /* build a system disk of this size in a sparse in-memory store */
  const files = new Map<string, Uint8Array>();
  for await (const e of Deno.readDir(join(root, "dos"))) if (e.isFile) files.set(e.name.toUpperCase(), await Deno.readFile(join(root, "dos", e.name)));
  const plan = planDisk(Number(sparseMb));
  const img = new SparseImage(plan.totalSectors);
  const io: SectorIO = { readSectors: (l, c, d) => img.read(l, c, d), writeSectors: (l, c, s) => img.write(l, c, s) };
  buildSystemDisk(io, Number(sparseMb), files);
  disks.sparse.set(2, img);
  core.ex.core_disk_attach(2, plan.totalSectors, 0);
  console.log(`sparse disk: ${plan.totalSectors} sectors, ${plan.volumes.length} volumes, ${img.chunks.size} chunks used`);
}
const hdd = opt("hdd");
if (hdd) { const img = await Deno.readFile(hdd); disks.images.set(2, img); core.ex.core_disk_attach(2, img.length / 512, 0); }
const fd = opt("floppy");
if (fd) { const img = await Deno.readFile(fd); disks.images.set(0, img); core.ex.core_disk_attach(0, img.length / 512, 0); }

const totalMs = Number(opt("ms", "2000"));
const typeText = opt("type");
const typeAt = Number(opt("type-at", "1000"));
let typed = false;
const t0 = performance.now();
for (let ms = 0; ms < totalMs; ms += 10) {
  if (typeText && !typed && ms >= typeAt) {
    typed = true;
    const codes = textToScancodes(typeText.replace(/\\n/g, "\n"));
    for (const c of codes) core.ex.core_key(c);
  }
  const r = core.ex.core_run_us(10_000);
  if (r === 1) { console.log("FATAL at", ms, "ms"); break; }
}
const wall = performance.now() - t0;
console.log("--- screen ---");
for (const line of core.textScreen()) console.log("|" + line.padEnd(80) + "|");
console.log("--- regs ---", JSON.stringify(core.regs()));
const insns = Number(core.ex.core_insns());
console.log(`insns=${insns} emu=${Number(core.ex.core_emu_ns()) / 1e6} ms wall=${wall.toFixed(0)} ms  ~${(insns / wall / 1000).toFixed(1)} MIPS host`);
if (hdd && opt("save")) await Deno.writeFile(opt("save")!, disks.images.get(2)!);

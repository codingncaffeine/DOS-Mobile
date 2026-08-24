// Headless runner: boots the core in Deno, optionally with a disk image, types text, dumps the screen.
// Usage: deno run -A tools/headless.ts [--hdd path] [--floppy path] [--ms N] [--type "text\n"] [--gen 5] [--mhz 66]
import { fromFileUrl, join } from "jsr:@std/path@1";
import { Core, GEN, textToScancodes } from "../web/core.ts";
import { SparseImage } from "../web/store.ts";
import { buildSystemDisk } from "../web/sysdisk.ts";
import { planDisk } from "../web/hdd.ts";
import { FatFs, type SectorIO } from "../web/fatfs.ts";
import { LocalFatDrive } from "../web/localdrive.ts";
import { localEntriesFromDir } from "./localdeno.ts";
import { encodePng } from "./png.ts";

const root = fromFileUrl(new URL("..", import.meta.url));
const a = Deno.args;
const opt = (name: string, def?: string) => { const i = a.indexOf("--" + name); return i >= 0 ? a[i + 1] : def; };

class FileDisks {
  images = new Map<number, Uint8Array>();
  sparse = new Map<number, SparseImage>();
  local?: LocalFatDrive; // slots 3+ = the --local-dir games folder disks
  read(slot: number, lba: number, count: number, dst: Uint8Array): boolean | number {
    if (slot >= 3) return this.local ? this.local.read(slot - 3, lba, count, dst) : false;
    const sp = this.sparse.get(slot); if (sp) return sp.read(lba, count, dst);
    const img = this.images.get(slot); if (!img) return false;
    const off = lba * 512;
    if (off + count * 512 > img.length) return false;
    dst.set(img.subarray(off, off + count * 512));
    return true;
  }
  write(slot: number, lba: number, count: number, src: Uint8Array) {
    if (slot >= 3) return this.local ? this.local.write(slot - 3, lba, count, src) : false;
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
  const importDir = opt("import");
  if (importDir) {
    const fs = new FatFs(io).mount();
    const name = importDir.replace(/[\/]+$/, "").split(/[\/]/).pop()!;
    const t = fs.ensurePath("GAMES\\" + name);
    let n = 0;
    const copy = async (host: string, cluster: number) => {
      for await (const e of Deno.readDir(host)) {
        if (e.isDirectory) { const d = fs.mkdir(cluster, e.name); await copy(join(host, e.name), d.cluster); }
        else { fs.writeFile(cluster, e.name, await Deno.readFile(join(host, e.name))); n++; }
      }
    };
    await copy(importDir, t.cluster);
    fs.flush();
    console.log(`imported ${n} files to ${t.dosPath}`);
  }
  disks.sparse.set(2, img);
  core.ex.core_disk_attach(2, plan.totalSectors, 0);
  console.log(`sparse disk: ${plan.totalSectors} sectors, ${plan.volumes.length} volumes, ${img.chunks.size} chunks used`);
}
const hdd = opt("hdd");
if (hdd) { const img = await Deno.readFile(hdd); disks.images.set(2, img); core.ex.core_disk_attach(2, img.length / 512, 0); }
const fd = opt("floppy");
if (fd) { const img = await Deno.readFile(fd); disks.images.set(0, img); core.ex.core_disk_attach(0, img.length / 512, 0); }
const localDir = opt("local-dir");
if (localDir) { /* mount a host directory as hard disks 81h+ (D:, H:, ...), lazily read */
  const entries = await localEntriesFromDir(localDir);
  const drive = new LocalFatDrive((s) => console.log("[local] " + s)).build(entries);
  disks.local = drive;
  for (let d = 0; d < drive.info.disks; d++) core.ex.core_disk_attach(3 + d, drive.diskSectors(d), 0);
  console.log(`local dir: ${drive.info.files} files, ${(drive.info.bytes / 1048576).toFixed(1)} MB on ${drive.info.volumes.map((v) => v.letter + ":").join(" ")}, sig ${drive.info.signature}`);
}

const totalMs = Number(opt("ms", "2000"));
const typeText = opt("type");
const typeAt = Number(opt("type-at", "1000"));
let typed = false;
const script = (opt("script") ?? "").split(";").filter(Boolean).map((s) => { const i = s.indexOf(":"); return { at: Number(s.slice(0, i)), text: s.slice(i + 1).replace(/\\n/g, "\n"), done: false }; });
const pngPath = opt("png");
const pngAt = opt("png-at") ? opt("png-at")!.split(",").map(Number) : [];
let pngIndex = 0;
function dumpPng(path: string) {
  const w = core.ex.core_fb_width(), hgt = core.ex.core_fb_height();
  if (!w || !hgt) { console.log("png: no frame"); return; }
  const ptr = core.ex.core_fb_ptr();
  const rgba = core.u8.subarray(ptr, ptr + w * hgt * 4);
  Deno.writeFileSync(path, encodePng(w, hgt, rgba));
  console.log(`png: ${path} ${w}x${hgt}`);
}
const wavPath = opt("wav");
const wavChunks: Int16Array[] = [];
let wavPtr = 0;
const t0 = performance.now();
for (let ms = 0; ms < totalMs; ms += 10) {
  if (wavPath) {
    if (!wavPtr) wavPtr = core.ex.core_alloc(4096 * 4);
    for (;;) {
      const n = core.ex.core_audio_read(wavPtr, 4096);
      if (!n) break;
      wavChunks.push(new Int16Array(core.ex.memory.buffer.slice(wavPtr, wavPtr + n * 4)));
      if (n < 4096) break;
    }
  }
  if (typeText && !typed && ms >= typeAt) {
    typed = true;
    const codes = textToScancodes(typeText.replace(/\\n/g, "\n"));
    for (const c of codes) core.ex.core_key(c);
  }
  for (const s of script) if (!s.done && ms >= s.at) {
    s.done = true;
    if (/^M-?\d+,-?\d+,\d+$/.test(s.text.trim())) { const [dx, dy, btn] = s.text.slice(1).split(",").map(Number); core.ex.core_mouse(dx, dy, btn); }
    else if (/^K[0-9A-Fa-f]{2}(,[0-9A-Fa-f]{2})*$/.test(s.text.trim())) { /* raw scancodes, hex, e.g. KE0,50,E0,D0 (gray down press+release) */
      for (const h of s.text.slice(1).split(",")) core.ex.core_key(parseInt(h, 16));
    } else for (const c of textToScancodes(s.text)) core.ex.core_key(c);
  }
  if (pngIndex < pngAt.length && ms >= pngAt[pngIndex]) { dumpPng((pngPath ?? ".cache/shot.png").replace(/\.png$/, `-${pngAt[pngIndex]}.png`)); pngIndex++; }
  const r = core.ex.core_run_us(10_000);
  if (r === 1) { console.log("FATAL at", ms, "ms"); break; }
  // with an async disk attached, let its pending file reads resolve (the pending path
  // always halts the CPU, so yielding only when halted keeps full speed while computing)
  if (localDir && core.ex.core_halted()) await new Promise((res) => setTimeout(res, 0));
}
if (pngPath) dumpPng(pngPath);
const wall = performance.now() - t0;
console.log("--- screen ---");
for (const line of core.textScreen()) console.log("|" + line.padEnd(80) + "|");
console.log("--- regs ---", JSON.stringify(core.regs()));
const insns = Number(core.ex.core_insns());
console.log(`insns=${insns} emu=${Number(core.ex.core_emu_ns()) / 1e6} ms wall=${wall.toFixed(0)} ms  ~${(insns / wall / 1000).toFixed(1)} MIPS host`);
if (opt("save")) { const sp = disks.sparse.get(2); await Deno.writeFile(opt("save")!, sp ? sp.toBytes() : disks.images.get(2)!); console.log("saved", opt("save")); }
for (const spec of (opt("dump") ?? "").split(";").filter(Boolean)) { /* --dump hexaddr,len[;...] guest linear memory */
  const [a, n] = spec.split(",").map((x) => parseInt(x, 16));
  const base = core.ex.core_mem_ptr();
  const bytes = core.u8.subarray(base + a, base + a + n);
  let out = "";
  for (let i = 0; i < n; i++) {
    if (i % 16 === 0) out += (i ? "\n" : "") + (a + i).toString(16).padStart(6, "0") + ": ";
    out += bytes[i].toString(16).padStart(2, "0") + " ";
  }
  console.log(`dump ${spec}:\n` + out);
}
if (wavPath) {
  let total = 0;
  for (const c of wavChunks) total += c.length;
  const pcm = new Int16Array(total);
  let o = 0;
  for (const c of wavChunks) { pcm.set(c, o); o += c.length; }
  const hdr = new ArrayBuffer(44);
  const dv = new DataView(hdr);
  const w = (off: number, s: string) => { for (let i = 0; i < s.length; i++) dv.setUint8(off + i, s.charCodeAt(i)); };
  w(0, "RIFF"); dv.setUint32(4, 36 + pcm.length * 2, true); w(8, "WAVE");
  w(12, "fmt "); dv.setUint32(16, 16, true); dv.setUint16(20, 1, true); dv.setUint16(22, 2, true);
  dv.setUint32(24, 48000, true); dv.setUint32(28, 48000 * 4, true); dv.setUint16(32, 4, true); dv.setUint16(34, 16, true);
  w(36, "data"); dv.setUint32(40, pcm.length * 2, true);
  const outBytes = new Uint8Array(44 + pcm.length * 2);
  outBytes.set(new Uint8Array(hdr));
  outBytes.set(new Uint8Array(pcm.buffer), 44);
  await Deno.writeFile(wavPath, outBytes);
  let sum = 0, nz = 0, peak = 0;
  for (let i = 0; i < pcm.length; i++) { const v = pcm[i]; sum += v * v; if (v) nz++; if (Math.abs(v) > peak) peak = Math.abs(v); }
  console.log(`wav: ${wavPath} ${(pcm.length / 2 / 48000).toFixed(1)}s rms=${Math.sqrt(sum / Math.max(1, pcm.length)).toFixed(0)} peak=${peak} nonzero=${(nz * 100 / Math.max(1, pcm.length)).toFixed(0)}%`);
}

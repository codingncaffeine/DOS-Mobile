// Emulation worker: hosts the wasm core, paces emulated time against the wall clock,
// serves disk sectors from the IndexedDB-backed image, and paints frames.
/// <reference lib="webworker" />
import { Core } from "./core.ts";
import { ChunkStore, SparseImage } from "./store.ts";
import { buildSystemDisk } from "./sysdisk.ts";
import { planDisk } from "./hdd.ts";
import { FatFs, type SectorIO } from "./fatfs.ts";
import { readZip } from "./zip.ts";
import { textToScancodes } from "./core.ts";
import type { FromWorker, MachineSettings, ToWorker } from "./protocol.ts";

const post = (m: FromWorker, transfer?: Transferable[]) => (self as unknown as Worker).postMessage(m, transfer ?? []);

class Disks {
  images = new Map<number, SparseImage>();
  read(slot: number, lba: number, count: number, dst: Uint8Array) { return this.images.get(slot)?.read(lba, count, dst) ?? false; }
  write(slot: number, lba: number, count: number, src: Uint8Array) { return this.images.get(slot)?.write(lba, count, src) ?? false; }
}

const disks = new Disks();
const core = new Core(disks, (s) => post({ type: "log", text: s }));
let store: ChunkStore;
let settings: MachineSettings;
let canvas: OffscreenCanvas | undefined;
let ctx: OffscreenCanvasRenderingContext2D | null = null;
let running = false, paused = false;
let lastFrameId = -1;
let lastWall = 0;
let backlogUs = 0;
let cpuBusyMs = 0, windowStart = 0, windowInsns = 0n, windowEmuUs = 0;
let flushTimer: ReturnType<typeof setInterval> | undefined;
let debugText = false;
let autoType: { text: string; after: number } | null = null;
let audioPtr = 0;
const AUDIO_CHUNK = 2048;

class SparseIO implements SectorIO {
  constructor(public img: SparseImage) {}
  readSectors(lba: number, count: number, dst: Uint8Array) { return this.img.read(lba, count, dst); }
  writeSectors(lba: number, count: number, src: Uint8Array) { return this.img.write(lba, count, src); }
}

function baseName(name: string): string {
  const n = name.replace(/\\/g, "/").split("/").pop() ?? name;
  const dot = n.lastIndexOf(".");
  return dot > 0 ? n.slice(0, dot) : n;
}

/** Write files into C:\GAMES\<name>; the machine is rebooted afterwards so DOS sees them. */
async function importInto(name: string, files: { path: string; bytes: Uint8Array }[]) {
  const img = disks.images.get(2);
  if (!img) throw new Error("no drive C:");
  // strip a single common top-level directory
  let list = files.map((f) => ({ path: f.path.replace(/\\/g, "/").replace(/^\.?\//, ""), bytes: f.bytes })).filter((f) => f.path && !f.path.endsWith("/"));
  const tops = new Set(list.map((f) => f.path.split("/")[0]));
  if (tops.size === 1 && list.every((f) => f.path.includes("/"))) {
    const top = [...tops][0];
    name = top;
    list = list.map((f) => ({ path: f.path.slice(top.length + 1), bytes: f.bytes }));
  }
  const wasRunning = running;
  running = false;
  const fs = new FatFs(new SparseIO(img)).mount();
  const target = fs.ensurePath("GAMES\\" + baseName(name));
  let count = 0;
  const dirCache = new Map<string, number>();
  for (const f of list) {
    const parts = f.path.split("/").filter(Boolean);
    const fname = parts.pop()!;
    let c = target.cluster;
    let key = "";
    for (const d of parts) {
      key += d + "/";
      const cached = dirCache.get(key);
      if (cached !== undefined) { c = cached; continue; }
      c = fs.mkdir(c, d).cluster;
      dirCache.set(key, c);
    }
    fs.writeFile(c, fname, f.bytes);
    count++;
    if (count % 25 === 0) post({ type: "progress", text: `Copying ${count}/${list.length}` });
  }
  fs.flush();
  await flush();
  post({ type: "imported", dosPath: target.dosPath, count });
  core.ex.core_reset(0);
  autoType = { text: `CD ${target.dosPath}\nDIR /W\n`, after: Number(core.ex.core_emu_ns()) + 1_500_000_000 };
  running = true;
  lastWall = performance.now();
  void wasRunning;
  schedule();
}

function promptVisible(): boolean {
  const lines = core.textScreen();
  for (let i = lines.length - 1; i >= 0; i--) { if (lines[i].trim()) return /^[A-Z]:\\.*>$/.test(lines[i].trim()); }
  return false;
}
const HDD_ID = "hdd0";

async function fetchDosFiles(base: string): Promise<Map<string, Uint8Array>> {
  const manifest: { name: string; size: number }[] = await (await fetch(`${base}/manifest.json`)).json();
  const files = new Map<string, Uint8Array>();
  let done = 0;
  await Promise.all(manifest.map(async (m) => {
    const r = await fetch(`${base}/${m.name}`);
    files.set(m.name.toUpperCase(), new Uint8Array(await r.arrayBuffer()));
    done++;
    post({ type: "progress", text: `Loading MS-DOS ${done}/${manifest.length}` });
  }));
  return files;
}

async function ensureHdd(dosBase: string, sizeMB: number) {
  const meta = await store.getMeta(HDD_ID);
  if (meta && meta.sectors > 0) {
    post({ type: "progress", text: "Loading drive C:" });
    const img = new SparseImage(meta.sectors);
    img.chunks = await store.loadChunks(HDD_ID);
    disks.images.set(2, img);
    return;
  }
  post({ type: "progress", text: "Preparing drive C:" });
  const files = await fetchDosFiles(dosBase);
  const plan = planDisk(sizeMB);
  const img = new SparseImage(plan.totalSectors);
  buildSystemDisk(new SparseIO(img), sizeMB, files);
  disks.images.set(2, img);
  await store.putMeta({ id: HDD_ID, sectors: img.sectors, label: "DOS MOBILE", created: Date.now() });
  await store.putChunks(HDD_ID, img.takeDirty());
}

async function flush() {
  const img = disks.images.get(2);
  if (!img || img.dirty.size === 0) return;
  await store.putChunks(HDD_ID, img.takeDirty());
}

function attachDisks() {
  for (const [slot, img] of disks.images) core.ex.core_disk_attach(slot, img.sectors, 0);
}

function initCore() {
  core.ex.core_init(settings.gen, Math.round(settings.mhz * 1000), settings.ramKb, settings.fpu ? 1 : 0, 1, 4, 0);
  const now = new Date();
  core.ex.core_set_time(now.getFullYear(), now.getMonth() + 1, now.getDate(), now.getHours(), now.getMinutes(), now.getSeconds());
  attachDisks();
  lastFrameId = -1;
}

function paint() {
  const id = core.ex.core_frame_id();
  if (id === lastFrameId) return;
  lastFrameId = id;
  const w = core.ex.core_fb_width(), h = core.ex.core_fb_height();
  if (w === 0 || h === 0) return;
  const ptr = core.ex.core_fb_ptr();
  const view = new Uint8ClampedArray(core.ex.memory.buffer, ptr, w * h * 4);
  if (ctx && canvas) {
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
    ctx.putImageData(new ImageData(view, w, h), 0, 0);
  } else {
    const buf = view.slice().buffer;
    post({ type: "frame", w, h, buf }, [buf]);
  }
}

const channel = new MessageChannel();
channel.port1.onmessage = () => tick();
const schedule = () => channel.port2.postMessage(null);

function tick() {
  if (!running) return;
  if (paused) { lastWall = performance.now(); setTimeout(schedule, 50); return; }
  const now = performance.now();
  let dueUs = (now - lastWall) * 1000 + backlogUs;
  lastWall = now;
  // never try to catch up more than 60 ms at once: beyond that the machine simply runs slow
  if (dueUs > 60000) dueUs = 60000;
  const sliceUs = 4000;
  const t0 = performance.now();
  let ran = 0;
  while (dueUs - ran >= sliceUs) {
    const r = core.ex.core_run_us(sliceUs);
    ran += sliceUs;
    if (r === 1) { post({ type: "error", text: "The machine stopped (see log)." }); running = false; paint(); return; }
    if (r === 2) { post({ type: "log", text: "machine reset" }); }
    if (performance.now() - t0 > 12) break; // yield to messages
  }
  if (autoType && Number(core.ex.core_emu_ns()) > autoType.after && promptVisible()) {
    for (const c of textToScancodes(autoType.text)) core.ex.core_key(c);
    autoType = null;
  }
  backlogUs = dueUs - ran;
  if (backlogUs > 60000) backlogUs = 60000;
  const busy = performance.now() - t0;
  cpuBusyMs += busy;
  windowEmuUs += ran;
  paint();
  /* drain audio to the page (s16 stereo) */
  if (!audioPtr) audioPtr = core.ex.core_alloc(AUDIO_CHUNK * 4);
  for (;;) {
    const frames = core.ex.core_audio_read(audioPtr, AUDIO_CHUNK);
    if (!frames) break;
    const buf = core.ex.memory.buffer.slice(audioPtr, audioPtr + frames * 4);
    post({ type: "audio", buf, frames }, [buf]);
    if (frames < AUDIO_CHUNK) break;
  }
  if (now - windowStart >= 500) {
    const wall = now - windowStart;
    const insns = core.ex.core_insns();
    const dInsns = Number(insns - windowInsns);
    const load = cpuBusyMs / wall;
    const effective = settings.mhz * (windowEmuUs / 1000 / wall);
    post({ type: "status", mhz: settings.mhz, effectiveMhz: effective, load, halted: !!core.ex.core_halted(), fatal: !!core.ex.core_fatal(), mips: dInsns / wall / 1000 });
    if (debugText) post({ type: "text", lines: core.textScreen() });
    windowStart = now; cpuBusyMs = 0; windowInsns = insns; windowEmuUs = 0;
  }
  schedule();
}

self.onmessage = async (ev: MessageEvent<ToWorker>) => {
  const m = ev.data;
  try {
    switch (m.type) {
      case "init": {
        settings = m.settings;
        canvas = m.canvas;
        debugText = !!m.debug;
        ctx = canvas ? canvas.getContext("2d") : null;
        post({ type: "progress", text: "Starting the machine" });
        await core.load(m.wasm);
        store = await new ChunkStore().open();
        await ensureHdd(m.dosBase, settings.hddSizeMB);
        initCore();
        post({ type: "ready", fbW: core.ex.core_fb_width(), fbH: core.ex.core_fb_height() });
        running = true;
        lastWall = performance.now();
        windowStart = lastWall;
        windowInsns = core.ex.core_insns();
        flushTimer = setInterval(() => { flush().catch((e) => post({ type: "log", text: "flush failed: " + e })); }, 3000);
        schedule();
        break;
      }
      case "key":
        for (const c of m.codes) core.ex.core_key(c);
        break;
      case "mouse":
        core.ex.core_mouse(m.dx, m.dy, m.buttons);
        break;
      case "reset":
        if (!running) { running = true; core.ex.core_reset(m.warm ? 1 : 0); lastWall = performance.now(); schedule(); }
        else core.ex.core_reset(m.warm ? 1 : 0);
        break;
      case "setSpeed":
        settings.mhz = m.mhz;
        core.ex.core_set_khz(Math.round(m.mhz * 1000));
        break;
      case "pause":
        paused = m.paused;
        break;
      case "flush":
        await flush();
        break;
      case "attachFloppy": {
        const img = SparseImage.fromBytes(new Uint8Array(m.bytes));
        disks.images.set(0, img);
        core.ex.core_disk_attach(0, img.sectors, 0);
        post({ type: "log", text: `floppy attached: ${m.name} (${img.sectors * 512 / 1024} KB)` });
        break;
      }
      case "detachFloppy":
        disks.images.delete(0);
        core.ex.core_disk_detach(0);
        break;
      case "exportDisk": {
        await flush();
        const image = disks.images.get(2)!;
        if (image.sectors * 512 > 768 * 1048576) { post({ type: "error", text: "Drive C: is too large to export as one image; folder export is coming." }); break; }
        const bytes = image.toBytes();
        const buf = bytes.buffer as ArrayBuffer;
        post({ type: "disk", bytes: buf }, [buf]);
        break;
      }
      case "wipeDisk":
        // Stop the machine and its flushes first, and only acknowledge once the IDB delete
        // has committed — the page must not reload earlier or the abort keeps the old drive.
        running = false;
        clearInterval(flushTimer);
        disks.images.delete(2);
        await store.deleteDisk(HDD_ID);
        post({ type: "log", text: "drive C: wiped" });
        post({ type: "wiped" });
        break;
      case "importFiles":
        await importInto(m.name, m.files.map((f) => ({ path: f.path, bytes: new Uint8Array(f.bytes) })));
        break;
      case "importZip": {
        const entries = await readZip(new Uint8Array(m.bytes));
        const files: { path: string; bytes: Uint8Array }[] = [];
        for (const e of entries) if (!e.isDir) files.push({ path: e.path, bytes: await e.data() });
        await importInto(m.name, files);
        break;
      }
    }
  } catch (e) {
    post({ type: "error", text: String(e) });
  }
};

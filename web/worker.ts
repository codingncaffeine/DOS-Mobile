// Emulation worker: hosts the wasm core, paces emulated time against the wall clock,
// serves disk sectors from the IndexedDB-backed image, and paints frames.
/// <reference lib="webworker" />
import { Core } from "./core.ts";
import { ChunkStore, SparseImage } from "./store.ts";
import { buildSystemDisk } from "./sysdisk.ts";
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
  const disk = buildSystemDisk(sizeMB, files);
  const img = SparseImage.fromBytes(disk.image);
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
  backlogUs = dueUs - ran;
  if (backlogUs > 60000) backlogUs = 60000;
  const busy = performance.now() - t0;
  cpuBusyMs += busy;
  windowEmuUs += ran;
  paint();
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
        const bytes = disks.images.get(2)!.toBytes();
        const buf = bytes.buffer as ArrayBuffer;
        post({ type: "disk", bytes: buf }, [buf]);
        break;
      }
      case "wipeDisk":
        running = false;
        clearInterval(flushTimer);
        await store.deleteDisk(HDD_ID);
        post({ type: "log", text: "drive C: wiped; reload the page to rebuild it" });
        break;
      case "importFiles":
        post({ type: "log", text: "file import arrives in the next update" });
        break;
    }
  } catch (e) {
    post({ type: "error", text: String(e) });
  }
};

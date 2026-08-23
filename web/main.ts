// Page shell: starts the worker, routes input, paints frames (fallback), renders status.
import { SCAN, textToScancodes } from "./core.ts";
import { DEFAULT_SETTINGS, type FromWorker, type MachineSettings, type ToWorker } from "./protocol.ts";

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const canvas = $<HTMLCanvasElement>("screen");
const statusEl = $("status");
const overlay = $("overlay");
const overlayText = $("overlay-text");
const logEl = $<HTMLPreElement>("log");
const menu = $("menu");
const proxy = $<HTMLInputElement>("kbd-proxy");
const speed = $<HTMLInputElement>("speed");
const speedLabel = $("speed-label");

const settings: MachineSettings = { ...DEFAULT_SETTINGS };
try { const s = localStorage.getItem("dosmobile.settings"); if (s) Object.assign(settings, JSON.parse(s)); } catch { /* ignore */ }
// The drive size has no UI, so a persisted value is always a stale old default; it must not pin
// rebuilds below the shipped size (a wiped drive would come back small and imports hit disk full).
settings.hddSizeMB = DEFAULT_SETTINGS.hddSizeMB;
const saveSettings = () => { try { localStorage.setItem("dosmobile.settings", JSON.stringify(settings)); } catch { /* ignore */ } };

const worker = new Worker("worker.js", { type: "module" });
const send = (m: ToWorker, transfer?: Transferable[]) => worker.postMessage(m, transfer ?? []);

let fallbackCtx: CanvasRenderingContext2D | null = null;
function log(text: string) {
  logEl.textContent = (logEl.textContent + text + "\n").split("\n").slice(-200).join("\n");
  logEl.scrollTop = logEl.scrollHeight;
}

worker.onmessage = (ev: MessageEvent<FromWorker>) => {
  const m = ev.data;
  switch (m.type) {
    case "progress": overlayText.textContent = m.text; break;
    case "ready": overlay.hidden = true; break;
    case "frame": {
      if (!fallbackCtx) fallbackCtx = canvas.getContext("2d");
      if (canvas.width !== m.w || canvas.height !== m.h) { canvas.width = m.w; canvas.height = m.h; }
      fallbackCtx!.putImageData(new ImageData(new Uint8ClampedArray(m.buf), m.w, m.h), 0, 0);
      break;
    }
    case "status": {
      // pace audio playback to the machine's real speed: a struggling machine then sounds
      // continuous at slightly lower pitch (like real hardware) instead of stuttering
      if (audioNode) audioNode.port.postMessage({ ratio: m.ratio });
      const pct = Math.round(m.effectiveMhz / m.mhz * 100);
      statusEl.textContent = `${m.mhz} MHz · ${m.mips.toFixed(0)} MIPS · load ${Math.round(m.load * 100)}%` + (pct < 95 ? ` · running at ${pct}%` : "");
      statusEl.className = m.fatal ? "bad" : pct < 90 ? "warn" : "";
      break;
    }
    case "log": log(m.text); break;
    case "wiped": location.reload(); break;
    case "localFolder":
      localState = { state: m.state, name: m.name, handle: m.handle };
      updateLocalButton();
      if (m.state === "mounted") toast(`Games folder "${m.name}" mounted as ${m.letters ?? "D:"} — map in D:\\README.TXT`, 9000);
      else if (m.state === "needs-permission") {
        toast(`Your games folder "${m.name}" needs one click to reconnect: Menu → Reconnect games folder`, 15000);
        log(`games folder "${m.name}" is waiting for permission — press "Reconnect games folder" in the menu`);
      }
      break;
    case "audio":
      if (audioNode) audioNode.port.postMessage({ buf: m.buf }, [m.buf]);
      else { audioBacklog.push(m.buf); if (audioBacklog.length > 20) audioBacklog.shift(); }
      break;
    case "imported": toast(`Copied ${m.count} file(s) to ${m.dosPath} — rebooting`, 6000); log(`imported ${m.count} files to ${m.dosPath}`); break;
    case "text": {
      let el = document.getElementById("screen-text");
      if (!el) { el = document.createElement("pre"); el.id = "screen-text"; el.hidden = true; document.body.appendChild(el); }
      el.textContent = m.lines.join("\n");
      break;
    }
    case "error": overlay.hidden = false; overlayText.textContent = m.text; log("ERROR " + m.text); break;
    case "disk": {
      const blob = new Blob([m.bytes], { type: "application/octet-stream" });
      const a = document.createElement("a");
      a.href = URL.createObjectURL(blob); a.download = "dosmobile-c.img"; a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 10000);
      break;
    }
  }
};

async function start() {
  overlayText.textContent = "Loading the machine…";
  const wasm = await (await fetch("dosmobile.wasm")).arrayBuffer();
  const transfer: Transferable[] = [wasm];
  let off: OffscreenCanvas | undefined;
  if ("transferControlToOffscreen" in canvas) {
    try { off = canvas.transferControlToOffscreen(); transfer.push(off); } catch { off = undefined; }
  }
  send({ type: "init", wasm, settings, dosBase: "dos", canvas: off, debug: new URLSearchParams(location.search).has("debug") }, transfer);
}

/* ---------------- keyboard ---------------- */
const pressed = new Set<number>();
function scanFor(code: string): number | undefined { return SCAN[code]; }
function sendScan(sc: number, make: boolean) {
  const codes: number[] = [];
  if (sc > 0xFF) codes.push(0xE0, (sc & 0xFF) | (make ? 0 : 0x80));
  else codes.push(sc | (make ? 0 : 0x80));
  send({ type: "key", codes });
}
const passthrough = new Set(["F11"]);
window.addEventListener("keydown", (e) => {
  if (e.target === speed || (e.target as HTMLElement)?.tagName === "BUTTON") return;
  if (e.target === proxy && (e.key.length === 1 || e.key === "Unidentified" || e.keyCode === 229)) return; // IME/text path
  const sc = scanFor(e.code);
  if (sc === undefined) return;
  if (!passthrough.has(e.code)) e.preventDefault();
  if (e.repeat) { sendScan(sc, true); return; }
  pressed.add(sc);
  sendScan(sc, true);
});
window.addEventListener("keyup", (e) => {
  const sc = scanFor(e.code);
  if (sc === undefined) return;
  e.preventDefault();
  pressed.delete(sc);
  sendScan(sc, false);
});
window.addEventListener("blur", () => { for (const sc of pressed) sendScan(sc, false); pressed.clear(); });

// mobile text input via the proxy field
proxy.addEventListener("beforeinput", (e: Event) => {
  const ie = e as InputEvent;
  ie.preventDefault();
  switch (ie.inputType) {
    case "insertText": case "insertCompositionText": if (ie.data) send({ type: "key", codes: textToScancodes(ie.data) }); break;
    case "insertLineBreak": case "insertParagraph": send({ type: "key", codes: textToScancodes("\n") }); break;
    case "deleteContentBackward": send({ type: "key", codes: [0x0E, 0x8E] }); break;
  }
});
proxy.addEventListener("input", () => { proxy.value = ""; });
$("btn-cad").onclick = () => { send({ type: "cad" }); };

/* ---------------- menu ---------------- */
$("btn-menu").onclick = () => { menu.hidden = !menu.hidden; };
speed.value = String(settings.mhz);
speedLabel.textContent = `${settings.mhz} MHz`;
speed.oninput = () => {
  settings.mhz = Number(speed.value);
  speedLabel.textContent = `${settings.mhz} MHz`;
  send({ type: "setSpeed", mhz: settings.mhz });
  saveSettings();
};
$("btn-reset").onclick = () => send({ type: "reset", warm: false });
$("btn-fullscreen").onclick = () => { document.documentElement.requestFullscreen?.().catch(() => {}); };
/* Local games folder: mounted read-lazily as drive D:; DOS writes stay in a local overlay. */
const btnLocal = $("btn-local");
let localState: { state: "mounted" | "needs-permission" | "none"; name?: string; handle?: FileSystemDirectoryHandle } = { state: "none" };
if (!("showDirectoryPicker" in self)) btnLocal.hidden = true;
function updateLocalButton() {
  if (localState.state === "mounted") btnLocal.textContent = `Disconnect games folder (${localState.name ?? ""})`;
  else if (localState.state === "needs-permission") btnLocal.textContent = `Reconnect games folder (${localState.name ?? ""})`;
  else btnLocal.textContent = "Connect games folder…";
}
btnLocal.onclick = async () => {
  try {
    if (localState.state === "mounted") { send({ type: "disconnectLocalFolder" }); return; }
    if (localState.state === "needs-permission" && localState.handle) {
      const h = localState.handle as unknown as { requestPermission?(o: { mode: string }): Promise<string> };
      const p = h.requestPermission ? await h.requestPermission.call(localState.handle, { mode: "read" }) : "denied";
      if (p !== "granted") return;
      send({ type: "connectLocalFolder", handle: localState.handle, name: localState.name ?? "games" });
      return;
    }
    const picker = (self as unknown as { showDirectoryPicker(o?: unknown): Promise<FileSystemDirectoryHandle> }).showDirectoryPicker;
    const dir = await picker({ id: "dm-games", mode: "read" });
    send({ type: "connectLocalFolder", handle: dir, name: dir.name });
  } catch { /* cancelled */ }
};
$("btn-export").onclick = () => send({ type: "exportDisk" });
$("btn-wipe").onclick = () => {
  if (!confirm("Erase drive C: and rebuild it with a fresh MS-DOS? Everything on it is lost.")) return;
  overlay.hidden = false;
  overlayText.textContent = "Wiping drive C:…";
  send({ type: "wipeDisk" });
  // The reload happens on the worker's "wiped" ack (the IDB delete has committed by then);
  // the timer is only a fallback in case the worker died.
  setTimeout(() => location.reload(), 30000);
};
const fileFloppy = $<HTMLInputElement>("file-floppy");
$("btn-floppy").onclick = () => fileFloppy.click();
fileFloppy.onchange = async () => { const f = fileFloppy.files?.[0]; if (f) await insertFloppy(f); fileFloppy.value = ""; };
async function insertFloppy(f: File) {
  const bytes = await f.arrayBuffer();
  send({ type: "attachFloppy", bytes, name: f.name }, [bytes]);
}
/* ---------------- toast ---------------- */
const toastEl = document.createElement("div");
toastEl.id = "toast";
document.body.appendChild(toastEl);
let toastTimer: ReturnType<typeof setTimeout> | undefined;
function toast(text: string, ms = 4000) {
  toastEl.textContent = text;
  toastEl.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toastEl.classList.remove("show"), ms);
}

/* ---------------- import (ZIP / files / folders) ---------------- */
const FLOPPY_EXT = /\.(img|ima|dsk|vfd)$/i;
interface Picked { path: string; file: File }

async function readEntry(entry: FileSystemEntry, prefix: string, out: Picked[]): Promise<void> {
  if (entry.isFile) {
    const file = await new Promise<File>((res, rej) => (entry as FileSystemFileEntry).file(res, rej));
    out.push({ path: prefix + entry.name, file });
  } else if (entry.isDirectory) {
    const reader = (entry as FileSystemDirectoryEntry).createReader();
    for (;;) {
      const batch = await new Promise<FileSystemEntry[]>((res, rej) => reader.readEntries(res, rej));
      if (!batch.length) break;
      for (const e of batch) await readEntry(e, prefix + entry.name + "/", out);
    }
  }
}

async function importPicked(items: Picked[], nameHint?: string) {
  if (!items.length) return;
  if (items.length === 1 && FLOPPY_EXT.test(items[0].file.name)) { await insertFloppy(items[0].file); return; }
  if (items.length === 1 && /\.zip$/i.test(items[0].file.name)) {
    const bytes = await items[0].file.arrayBuffer();
    toast(`Copying ${items[0].file.name} to C:\\GAMES…`);
    send({ type: "importZip", name: items[0].file.name, bytes }, [bytes]);
    return;
  }
  const files: { path: string; bytes: ArrayBuffer }[] = [];
  for (const it of items) files.push({ path: it.path, bytes: await it.file.arrayBuffer() });
  const name = nameHint ?? (items[0].path.includes("/") ? items[0].path.split("/")[0] : items[0].file.name);
  toast(`Copying ${files.length} file(s) to C:\\GAMES…`);
  send({ type: "importFiles", name, files }, files.map((f) => f.bytes));
}

const fileAdd = $<HTMLInputElement>("file-add");
const fileAddFolder = $<HTMLInputElement>("file-add-folder");
$("btn-add").onclick = () => fileAdd.click();
$("btn-add-folder").onclick = () => fileAddFolder.click();
if (!("webkitdirectory" in fileAddFolder)) $("btn-add-folder").hidden = true;
fileAdd.onchange = async () => {
  const list = [...(fileAdd.files ?? [])].map((f) => ({ path: f.name, file: f }));
  fileAdd.value = "";
  await importPicked(list);
};
fileAddFolder.onchange = async () => {
  const list = [...(fileAddFolder.files ?? [])].map((f) => ({ path: (f as File & { webkitRelativePath?: string }).webkitRelativePath || f.name, file: f }));
  fileAddFolder.value = "";
  await importPicked(list);
};

const wrap = $("screen-wrap");
wrap.addEventListener("dragover", (e) => { e.preventDefault(); wrap.classList.add("drop"); });
wrap.addEventListener("dragleave", () => wrap.classList.remove("drop"));
wrap.addEventListener("drop", async (e) => {
  e.preventDefault(); wrap.classList.remove("drop");
  const dt = e.dataTransfer;
  if (!dt) return;
  const picked: Picked[] = [];
  const entries = [...dt.items].map((i) => (i as DataTransferItem & { webkitGetAsEntry?: () => FileSystemEntry | null }).webkitGetAsEntry?.()).filter(Boolean) as FileSystemEntry[];
  if (entries.length) for (const en of entries) await readEntry(en, "", picked);
  else for (const f of dt.files) picked.push({ path: f.name, file: f });
  await importPicked(picked);
});

/* ---------------- on-screen keyboard ---------------- */
const osk = $("oskbd");
const sticky = new Map<number, HTMLButtonElement>();
let repeatTimer: ReturnType<typeof setTimeout> | undefined, repeatInterval: ReturnType<typeof setInterval> | undefined;
function releaseSticky() {
  for (const [sc, btn] of sticky) { sendScan(sc, false); btn.classList.remove("held"); }
  sticky.clear();
}
for (const btn of osk.querySelectorAll<HTMLButtonElement>("button[data-code]")) {
  const sc = SCAN[btn.dataset.code!];
  if (sc === undefined) continue;
  const isSticky = btn.hasAttribute("data-sticky");
  btn.addEventListener("pointerdown", (e) => {
    e.preventDefault();
    if (isSticky) {
      if (sticky.has(sc)) { sendScan(sc, false); sticky.delete(sc); btn.classList.remove("held"); }
      else { sendScan(sc, true); sticky.set(sc, btn); btn.classList.add("held"); }
      return;
    }
    sendScan(sc, true);
    repeatTimer = setTimeout(() => { repeatInterval = setInterval(() => sendScan(sc, true), 60); }, 400);
  });
  const up = (e: Event) => {
    e.preventDefault();
    if (isSticky) return;
    clearTimeout(repeatTimer); clearInterval(repeatInterval);
    sendScan(sc, false);
    releaseSticky();
  };
  btn.addEventListener("pointerup", up);
  btn.addEventListener("pointercancel", up);
  btn.addEventListener("pointerleave", (e) => { if ((e as PointerEvent).buttons) up(e); });
}
$("osk-text").onclick = () => proxy.focus();
$("btn-keyboard").onclick = () => { osk.hidden = !osk.hidden; if (!osk.hidden) proxy.focus(); };
proxy.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === "Backspace") { /* handled by the window handler via scancodes */ } });
/* ---------------- audio ---------------- */
let audioCtx: AudioContext | null = null;
let audioNode: AudioWorkletNode | null = null;
const audioBacklog: ArrayBuffer[] = [];
async function ensureAudio() {
  if (audioCtx) { if (audioCtx.state === "suspended") audioCtx.resume().catch(() => {}); return; }
  try {
    audioCtx = new AudioContext({ sampleRate: 48000 });
    await audioCtx.audioWorklet.addModule("audio-worklet.js");
    audioNode = new AudioWorkletNode(audioCtx, "dm-audio", { outputChannelCount: [2] });
    audioNode.connect(audioCtx.destination);
    audioNode.port.postMessage({ rate: 48000 });
    audioNode.port.onmessage = (e) => { // per-second jitter-buffer telemetry (menu log)
      const s = (e.data as { stats?: { underruns: number; bufferedMs: number; targetMs: number } }).stats;
      if (s && s.underruns > 0) log(`audio: ${s.underruns} underrun(s), buffered ${s.bufferedMs.toFixed(0)} ms, target ${s.targetMs.toFixed(0)} ms`);
    };
    for (const buf of audioBacklog.splice(0)) audioNode.port.postMessage({ buf }, [buf]);
  } catch (e) { log("audio unavailable: " + e); }
}
window.addEventListener("pointerdown", () => { ensureAudio(); }, { capture: true });
window.addEventListener("keydown", () => { ensureAudio(); }, { capture: true });

/* ---------------- mouse / touch pointer ---------------- */
let mouseButtons = 0;
function mouseScale(): number {
  const rect = canvas.getBoundingClientRect();
  return rect.width > 0 ? 640 / rect.width : 1;
}
canvas.addEventListener("pointerdown", (e) => {
  if (e.pointerType === "mouse") {
    if (document.pointerLockElement !== canvas) { canvas.requestPointerLock?.(); }
    mouseButtons |= e.button === 2 ? 2 : e.button === 1 ? 4 : 1;
    send({ type: "mouse", dx: 0, dy: 0, buttons: mouseButtons });
    e.preventDefault();
  } else {
    touchStart(e);
  }
});
canvas.addEventListener("pointerup", (e) => {
  if (e.pointerType === "mouse") {
    mouseButtons &= ~(e.button === 2 ? 2 : e.button === 1 ? 4 : 1);
    send({ type: "mouse", dx: 0, dy: 0, buttons: mouseButtons });
    e.preventDefault();
  } else {
    touchEnd(e);
  }
});
canvas.addEventListener("pointermove", (e) => {
  if (e.pointerType === "mouse") {
    if (document.pointerLockElement === canvas) {
      const s = mouseScale();
      send({ type: "mouse", dx: Math.round(e.movementX * s), dy: Math.round(e.movementY * s), buttons: mouseButtons });
    }
  } else {
    touchMove(e);
  }
});
canvas.addEventListener("contextmenu", (e) => e.preventDefault());
/* touch: drag = relative move, quick tap = left click, two-finger tap = right click */
const touches = new Map<number, { x: number; y: number; t: number; moved: boolean }>();
function touchStart(e: PointerEvent) {
  canvas.setPointerCapture(e.pointerId);
  touches.set(e.pointerId, { x: e.clientX, y: e.clientY, t: performance.now(), moved: false });
  e.preventDefault();
}
function touchMove(e: PointerEvent) {
  const t = touches.get(e.pointerId);
  if (!t) return;
  const dx = e.clientX - t.x, dy = e.clientY - t.y;
  if (Math.abs(dx) + Math.abs(dy) > 1) {
    t.moved = true;
    const s = mouseScale() * 1.5;
    send({ type: "mouse", dx: Math.round(dx * s), dy: Math.round(dy * s), buttons: mouseButtons });
    t.x = e.clientX; t.y = e.clientY;
  }
  e.preventDefault();
}
function touchEnd(e: PointerEvent) {
  const t = touches.get(e.pointerId);
  touches.delete(e.pointerId);
  if (!t) return;
  const quick = performance.now() - t.t < 300 && !t.moved;
  if (quick) {
    const btn = touches.size >= 1 ? 2 : 1; /* a second finger still down = right click */
    send({ type: "mouse", dx: 0, dy: 0, buttons: mouseButtons | btn });
    setTimeout(() => send({ type: "mouse", dx: 0, dy: 0, buttons: mouseButtons }), 60);
  }
  e.preventDefault();
}

document.addEventListener("visibilitychange", () => { if (document.hidden) send({ type: "flush" }); });
window.addEventListener("pagehide", () => send({ type: "flush" }));

if (new URLSearchParams(location.search).has("debug")) (window as unknown as { dm: unknown }).dm = { send, importPicked, toast };
start().catch((e) => { overlayText.textContent = "Failed to start: " + e; });

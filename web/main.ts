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
      const pct = Math.round(m.effectiveMhz / m.mhz * 100);
      statusEl.textContent = `${m.mhz} MHz · ${m.mips.toFixed(0)} MIPS · load ${Math.round(m.load * 100)}%` + (pct < 95 ? ` · running at ${pct}%` : "");
      statusEl.className = m.fatal ? "bad" : pct < 90 ? "warn" : "";
      break;
    }
    case "log": log(m.text); break;
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
$("btn-keyboard").onclick = () => { proxy.focus(); };
$("btn-cad").onclick = () => { send({ type: "key", codes: [0x1D, 0x38, 0xE0, 0x53, 0xE0, 0xD3, 0xB8, 0x9D] }); };

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
$("btn-export").onclick = () => send({ type: "exportDisk" });
$("btn-wipe").onclick = () => { if (confirm("Erase drive C: and rebuild it with a fresh MS-DOS? Everything on it is lost.")) { send({ type: "wipeDisk" }); setTimeout(() => location.reload(), 500); } };
const fileFloppy = $<HTMLInputElement>("file-floppy");
$("btn-floppy").onclick = () => fileFloppy.click();
fileFloppy.onchange = async () => { const f = fileFloppy.files?.[0]; if (f) await insertFloppy(f); fileFloppy.value = ""; };
async function insertFloppy(f: File) {
  const bytes = await f.arrayBuffer();
  send({ type: "attachFloppy", bytes, name: f.name }, [bytes]);
}
const wrap = $("screen-wrap");
wrap.addEventListener("dragover", (e) => { e.preventDefault(); wrap.classList.add("drop"); });
wrap.addEventListener("dragleave", () => wrap.classList.remove("drop"));
wrap.addEventListener("drop", async (e) => {
  e.preventDefault(); wrap.classList.remove("drop");
  const f = e.dataTransfer?.files?.[0];
  if (f) await insertFloppy(f);
});
document.addEventListener("visibilitychange", () => { if (document.hidden) send({ type: "flush" }); });
window.addEventListener("pagehide", () => send({ type: "flush" }));

start().catch((e) => { overlayText.textContent = "Failed to start: " + e; });

// Headless browser smoke test through the DevTools protocol: loads the page, waits, reads the
// debug text screen and console errors. Usage: deno run -A tools/browsercheck.ts [url] [seconds]
import { fromFileUrl } from "jsr:@std/path@1";

const url = Deno.args[0] ?? "http://127.0.0.1:8088/?debug";
const waitSec = Number(Deno.args[1] ?? "10");
const typeText = Deno.args[2];
const candidates = [
  "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
  "C:/Program Files/Google/Chrome/Application/chrome.exe",
  "C:/Program Files (x86)/Google/Chrome/Application/chrome.exe",
];
let exe = "";
for (const c of candidates) { try { await Deno.stat(c); exe = c; break; } catch { /* next */ } }
if (!exe) { console.error("no Chromium browser found"); Deno.exit(2); }
const profile = fromFileUrl(new URL("../.cache/edge-profile", import.meta.url));
const port = 9333;
const proc = new Deno.Command(exe, {
  args: ["--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check", "--disable-extensions",
    `--user-data-dir=${profile}`, `--remote-debugging-port=${port}`, "about:blank"],
  stdout: "null", stderr: "null",
}).spawn();

async function targets(): Promise<{ webSocketDebuggerUrl: string; type: string }[]> {
  for (let i = 0; i < 50; i++) {
    try { return await (await fetch(`http://127.0.0.1:${port}/json`)).json(); } catch { await new Promise((r) => setTimeout(r, 200)); }
  }
  throw new Error("browser did not start");
}
const page = (await targets()).find((t) => t.type === "page")!;
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
let id = 0;
const pending = new Map<number, (v: unknown) => void>();
const errors: string[] = [];
ws.onmessage = (ev) => {
  const m = JSON.parse(ev.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)!(m.result); pending.delete(m.id); }
  if (m.method === "Runtime.exceptionThrown") errors.push(m.params.exceptionDetails.text + " " + (m.params.exceptionDetails.exception?.description ?? ""));
  if (m.method === "Runtime.consoleAPICalled" && (m.params.type === "error" || m.params.type === "warning")) errors.push(m.params.args.map((a: { value?: string; description?: string }) => a.value ?? a.description).join(" "));
  if (m.method === "Log.entryAdded" && m.params.entry.level === "error") errors.push(m.params.entry.text);
};
const call = (method: string, params: Record<string, unknown> = {}) => new Promise<unknown>((res) => { const i = ++id; pending.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (expr: string) => { const r = await call("Runtime.evaluate", { expression: expr, returnByValue: true }) as { result?: { value?: unknown } }; return r?.result?.value; };

await call("Runtime.enable");
await call("Log.enable");
await call("Page.enable");
await call("Page.navigate", { url });
const t0 = Date.now();
let typed = false;
while (Date.now() - t0 < waitSec * 1000) {
  await new Promise((r) => setTimeout(r, 500));
  if (typeText && !typed && Date.now() - t0 > waitSec * 500) {
    typed = true;
    for (const ch of typeText.replace(/\\n/g, "\n")) {
      if (ch === "\n") await call("Input.dispatchKeyEvent", { type: "keyDown", code: "Enter", key: "Enter" }), await call("Input.dispatchKeyEvent", { type: "keyUp", code: "Enter", key: "Enter" });
      else {
        const code = /[a-z]/i.test(ch) ? "Key" + ch.toUpperCase() : /[0-9]/.test(ch) ? "Digit" + ch : ch === " " ? "Space" : ch === "." ? "Period" : ch === "\\" ? "Backslash" : ch === ":" ? "Semicolon" : "";
        const shift = /[A-Z:]/.test(ch);
        if (shift) await call("Input.dispatchKeyEvent", { type: "keyDown", code: "ShiftLeft", key: "Shift" });
        await call("Input.dispatchKeyEvent", { type: "keyDown", code, key: ch });
        await call("Input.dispatchKeyEvent", { type: "keyUp", code, key: ch });
        if (shift) await call("Input.dispatchKeyEvent", { type: "keyUp", code: "ShiftLeft", key: "Shift" });
      }
    }
  }
}
const status = await evalJs(`document.getElementById('status')?.textContent`);
const overlay = await evalJs(`document.getElementById('overlay')?.hidden ? '' : document.getElementById('overlay-text')?.textContent`);
const text = await evalJs(`document.getElementById('screen-text')?.textContent ?? '(no screen text)'`);
const logText = await evalJs(`document.getElementById('log')?.textContent ?? ''`);
console.log("status :", status);
if (overlay) console.log("overlay:", overlay);
console.log("--- screen ---");
console.log(text);
if (logText) { console.log("--- log ---"); console.log(logText); }
if (errors.length) { console.log("--- errors ---"); for (const e of errors) console.log(e); }
ws.close();
try { proc.kill(); } catch { /* gone */ }
Deno.exit(errors.length ? 1 : 0);

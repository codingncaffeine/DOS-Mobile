// Build: compile the C core to wasm32 with clang, bundle the TypeScript shell, assemble dist/.
// Usage: deno run -A tools/build.ts [--release] [--core-only] [--web-only]
import { join, fromFileUrl } from "jsr:@std/path@1";

const root = fromFileUrl(new URL("..", import.meta.url));
const args = new Set(Deno.args);
const release = args.has("--release");

const CLANG = Deno.env.get("DM_CLANG") ?? "C:/Program Files/LLVM/bin/clang.exe";

async function run(cmd: string, cmdArgs: string[], cwd = root) {
  const p = new Deno.Command(cmd, { args: cmdArgs, cwd, stdout: "inherit", stderr: "inherit" });
  const { code } = await p.output();
  if (code !== 0) throw new Error(`${cmd} exited with ${code}`);
}

async function buildCore() {
  const coreDir = join(root, "core");
  const sources: string[] = [];
  for await (const e of Deno.readDir(coreDir)) if (e.isFile && e.name.endsWith(".c")) sources.push(join("core", e.name));
  sources.sort();
  await Deno.mkdir(join(root, "dist"), { recursive: true });
  const out = join(root, "dist", "dosmobile.wasm");
  const flags = [
    "--target=wasm32", release ? "-O3" : "-O2", "-std=c11", "-nostdlib", "-ffreestanding",
    "-fno-builtin-memcpy", "-fno-builtin-memset", "-fvisibility=hidden",
    "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function", "-Werror=implicit-function-declaration",
    "-mbulk-memory", "-msign-ext", "-mnontrapping-fptoint", "-mmutable-globals",
    "-Wl,--no-entry", "-Wl,--max-memory=805306368", "-Wl,-z,stack-size=1048576", "-Wl,--export=__heap_base",
    "-o", out, ...sources,
  ];
  console.log(`clang → ${out}`);
  await run(CLANG, flags);
  const st = await Deno.stat(out);
  console.log(`core: ${(st.size / 1024).toFixed(1)} KB`);
}

async function buildWeb() {
  await Deno.mkdir(join(root, "dist"), { recursive: true });
  const bundle = async (entry: string, outName: string) => {
    await run(Deno.execPath(), ["bundle", "--quiet", "--platform", "browser", ...(release ? ["--minify"] : ["--sourcemap=inline"]), "-o", join(root, "dist", outName), join(root, "web", entry)]);
    console.log(`bundle: ${outName}`);
  };
  await bundle("main.ts", "app.js");
  await bundle("worker.ts", "worker.js");
  await bundle("audio-worklet.ts", "audio-worklet.js");
  for (const f of ["index.html", "app.css", "manifest.webmanifest", "icon.svg", "sw.js", ".htaccess"]) {
    try { await Deno.copyFile(join(root, "web", f), join(root, "dist", f)); } catch { /* optional */ }
  }
  // Cache busting: index.html is served no-cache; everything it pulls in carries the build stamp,
  // so a plain reload picks up every deploy (no hard-refresh ritual, PWA windows included).
  const stamp = Date.now().toString(36);
  const stampFile = async (name: string, refs: string[]) => {
    const p = join(root, "dist", name);
    let t = await Deno.readTextFile(p);
    for (const r of refs) t = t.replaceAll(r, `${r}?v=${stamp}`);
    await Deno.writeTextFile(p, t);
  };
  await stampFile("index.html", ["app.css", "app.js"]);
  await stampFile("app.js", ["worker.js", "dosmobile.wasm", "audio-worklet.js"]);
  console.log(`stamp: ${stamp}`);
  // DOS files: the in-browser FAT builder fetches them by manifest
  const dosDir = join(root, "dos");
  const outDos = join(root, "dist", "dos");
  await Deno.mkdir(outDos, { recursive: true });
  const manifest: { name: string; size: number }[] = [];
  for await (const e of Deno.readDir(dosDir)) {
    if (!e.isFile) continue;
    const st = await Deno.stat(join(dosDir, e.name));
    await Deno.copyFile(join(dosDir, e.name), join(outDos, e.name));
    manifest.push({ name: e.name, size: st.size });
  }
  manifest.sort((a, b) => a.name.localeCompare(b.name));
  await Deno.writeTextFile(join(outDos, "manifest.json"), JSON.stringify(manifest));
  console.log(`dos: ${manifest.length} files`);
}

if (!args.has("--web-only")) await buildCore();
if (!args.has("--core-only")) await buildWeb();
console.log("done");

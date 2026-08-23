// Deploy dist/ to the static host over scp. Usage: deno run -A tools/deploy.ts [--no-build] [--dry]
// Host settings come from environment variables (or a local .cache/deploy.json, gitignored):
//   DM_DEPLOY_HOST  user@host      DM_DEPLOY_PORT  22      DM_DEPLOY_KEY  path to private key
//   DM_DEPLOY_PATH  remote directory (created if missing)   DM_DEPLOY_URL  public URL for verification
import { fromFileUrl, join } from "jsr:@std/path@1";

const root = fromFileUrl(new URL("..", import.meta.url));
const args = new Set(Deno.args);
let cfg: Record<string, string> = {};
try { cfg = JSON.parse(await Deno.readTextFile(join(root, ".cache", "deploy.json"))); } catch { /* env only */ }
const get = (k: string, d?: string) => Deno.env.get(k) ?? cfg[k] ?? d;
const host = get("DM_DEPLOY_HOST"), port = get("DM_DEPLOY_PORT", "22"), key = get("DM_DEPLOY_KEY"), path = get("DM_DEPLOY_PATH"), url = get("DM_DEPLOY_URL");
if (!host || !path) { console.error("DM_DEPLOY_HOST and DM_DEPLOY_PATH are required"); Deno.exit(2); }

async function run(cmd: string, a: string[]) {
  if (args.has("--dry")) { console.log("$", cmd, a.join(" ")); return; }
  const p = await new Deno.Command(cmd, { args: a, stdout: "inherit", stderr: "inherit" }).output();
  if (p.code !== 0) throw new Error(`${cmd} failed (${p.code})`);
}
const sshOpts = ["-p", port!, ...(key ? ["-i", key] : []), "-o", "StrictHostKeyChecking=accept-new"];
const scpOpts = ["-P", port!, ...(key ? ["-i", key] : []), "-o", "StrictHostKeyChecking=accept-new"];

if (!args.has("--no-build")) {
  const b = await new Deno.Command(Deno.execPath(), { args: ["run", "-A", join(root, "tools", "build.ts"), "--release"], stdout: "inherit", stderr: "inherit" }).output();
  if (b.code !== 0) throw new Error("build failed");
}
const dist = join(root, "dist");
await run("ssh", [...sshOpts, host!, `mkdir -p '${path}/dos' && chmod 755 '${path}' '${path}/dos'`]);
const top: string[] = [];
for await (const e of Deno.readDir(dist)) if (e.isFile) top.push(join(dist, e.name));
await run("scp", [...scpOpts, ...top, `${host}:${path}/`]);
const dos: string[] = [];
for await (const e of Deno.readDir(join(dist, "dos"))) if (e.isFile) dos.push(join(dist, "dos", e.name));
await run("scp", [...scpOpts, ...dos, `${host}:${path}/dos/`]);
await run("ssh", [...sshOpts, host!, `chmod -R u=rwX,go=rX '${path}'`]);
if (url && !args.has("--dry")) {
  const r = await fetch(url + (url.endsWith("/") ? "" : "/") + "dosmobile.wasm", { method: "HEAD" }).catch((e) => { console.log("verify skipped:", String(e).split("
")[0]); return null; });
  if (!r) { console.log("deployed to", url); Deno.exit(0); }
  console.log(`verify: ${r.status} ${r.headers.get("content-type")} ${r.headers.get("content-length")} bytes`);
  const local = (await Deno.stat(join(dist, "dosmobile.wasm"))).size;
  console.log(local === Number(r.headers.get("content-length")) ? "wasm size matches" : `size mismatch: local ${local}`);
}
console.log("deployed to", url ?? `${host}:${path}`);

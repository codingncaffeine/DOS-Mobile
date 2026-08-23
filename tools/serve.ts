// Static dev server for dist/ with the right MIME types. Usage: deno run -A tools/serve.ts [port]
import { fromFileUrl, join, extname } from "jsr:@std/path@1";

const root = fromFileUrl(new URL("../dist", import.meta.url));
const port = Number(Deno.args[0] ?? "8088");
const types: Record<string, string> = {
  ".html": "text/html; charset=utf-8", ".js": "application/javascript; charset=utf-8", ".css": "text/css; charset=utf-8",
  ".wasm": "application/wasm", ".json": "application/json", ".webmanifest": "application/manifest+json",
  ".png": "image/png", ".svg": "image/svg+xml", ".ico": "image/x-icon",
};

Deno.serve({ port, hostname: "127.0.0.1" }, async (req) => {
  const url = new URL(req.url);
  let path = decodeURIComponent(url.pathname);
  if (path.endsWith("/")) path += "index.html";
  const file = join(root, path);
  try {
    const data = await Deno.readFile(file);
    const type = types[extname(file).toLowerCase()] ?? "application/octet-stream";
    return new Response(data, { headers: { "content-type": type, "cache-control": "no-cache", "cross-origin-opener-policy": "same-origin", "cross-origin-embedder-policy": "require-corp" } });
  } catch {
    return new Response("not found: " + path, { status: 404 });
  }
});
console.log(`serving ${root} at http://127.0.0.1:${port}/`);

// Deno adapter for the local game drive: a real directory walked into LocalEntry[]
// (sorted, dotfiles skipped) with genuinely async seek-reads — used by tools/headless.ts
// --local-dir and by tests/local_test.ts to exercise the pending-read path end to end.
import { join } from "jsr:@std/path@1";
import type { LocalEntry } from "../web/localdrive.ts";

export async function localEntriesFromDir(dir: string): Promise<LocalEntry[]> {
  const out: LocalEntry[] = [];
  const walk = async (host: string, prefix: string) => {
    const items: { name: string; isDirectory: boolean }[] = [];
    for await (const e of Deno.readDir(host)) items.push({ name: e.name, isDirectory: e.isDirectory });
    items.sort((x, y) => x.name.toUpperCase() < y.name.toUpperCase() ? -1 : 1);
    for (const it of items) {
      if (it.name.startsWith(".")) continue;
      const p = join(host, it.name);
      const rel = prefix ? `${prefix}/${it.name}` : it.name;
      if (it.isDirectory) { await walk(p, rel); continue; }
      const st = await Deno.stat(p);
      out.push({
        path: rel,
        size: st.size,
        mtime: st.mtime?.getTime() ?? 0,
        read: async (off, len) => {
          const f = await Deno.open(p, { read: true });
          try {
            await f.seek(off, Deno.SeekMode.Start);
            const buf = new Uint8Array(len);
            let got = 0;
            while (got < len) {
              const n = await f.read(buf.subarray(got));
              if (n === null) break;
              got += n;
            }
            return buf.subarray(0, got);
          } finally {
            f.close();
          }
        },
      });
    }
  };
  await walk(dir, "");
  return out;
}

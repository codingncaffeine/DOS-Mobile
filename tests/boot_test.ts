// Boot smoke test: build a C: image from dos/, boot it headless, type DIR, check the output.
import { assert, assertStringIncludes } from "jsr:@std/assert@1";
import { fromFileUrl, join } from "jsr:@std/path@1";
import { Core, GEN, textToScancodes } from "../web/core.ts";
import { ArraySectorIO } from "../web/fatfs.ts";
import { buildSystemDisk } from "../web/sysdisk.ts";
import { planDisk } from "../web/hdd.ts";

const root = fromFileUrl(new URL("..", import.meta.url));

class MemDisks {
  images = new Map<number, Uint8Array>();
  read(slot: number, lba: number, count: number, dst: Uint8Array) {
    const img = this.images.get(slot); if (!img || (lba + count) * 512 > img.length) return false;
    dst.set(img.subarray(lba * 512, (lba + count) * 512)); return true;
  }
  write(slot: number, lba: number, count: number, src: Uint8Array) {
    const img = this.images.get(slot); if (!img || (lba + count) * 512 > img.length) return false;
    img.set(src, lba * 512); return true;
  }
}

async function bootMachine(gen: number, mhz: number) {
  const files = new Map<string, Uint8Array>();
  for await (const e of Deno.readDir(join(root, "dos"))) if (e.isFile) files.set(e.name.toUpperCase(), await Deno.readFile(join(root, "dos", e.name)));
  const image = new Uint8Array(planDisk(32).totalSectors * 512);
  buildSystemDisk(new ArraySectorIO(image), 32, files);
  const disks = new MemDisks();
  disks.images.set(2, image);
  const logs: string[] = [];
  const core = new Core(disks, (s) => logs.push(s));
  await core.load(await Deno.readFile(join(root, "dist", "dosmobile.wasm")));
  core.ex.core_init(gen, Math.round(mhz * 1000), 4096, 0, 1, 4, 0);
  core.ex.core_disk_attach(2, image.length / 512, 0);
  return { core, logs };
}

function runMs(core: Core, ms: number) {
  for (let t = 0; t < ms; t += 10) {
    const r = core.ex.core_run_us(10_000);
    if (r === 1) throw new Error("machine fatal at " + t + " ms");
  }
}

Deno.test("MS-DOS 4.01 boots to the prompt and runs DIR (486 @ 66 MHz)", async () => {
  const { core, logs } = await bootMachine(GEN.G486, 66);
  runMs(core, 2500);
  let screen = core.textScreen().join("\n");
  assertStringIncludes(screen, "C:\\>");
  for (const c of textToScancodes("DIR\n")) core.ex.core_key(c);
  runMs(core, 1500);
  screen = core.textScreen().join("\n");
  assertStringIncludes(screen, "COMMAND  COM");
  assertStringIncludes(screen, "bytes free");
  assert(!logs.some((l) => l.startsWith("CPU: #")), "no CPU faults: " + logs.join(" | "));
});

Deno.test("boots on an 8088-class machine too", async () => {
  const { core } = await bootMachine(GEN.G8088, 4.77);
  runMs(core, 12000);
  assertStringIncludes(core.textScreen().join("\n"), "C:\\>");
});

// Local game drive: a real host directory mounted as D: through the synthetic FAT view with
// genuinely async (pending) sector reads. Boots DOS, lists it, types a file, and round-trips
// a 200 KB binary through DOS COPY back onto C:, byte-compared against the source.
import { assert, assertEquals, assertStringIncludes } from "jsr:@std/assert@1";
import { fromFileUrl, join } from "jsr:@std/path@1";
import { Core, GEN, textToScancodes } from "../web/core.ts";
import { ArraySectorIO, FatFs } from "../web/fatfs.ts";
import { buildSystemDisk } from "../web/sysdisk.ts";
import { planDisk } from "../web/hdd.ts";
import { LocalFatDrive } from "../web/localdrive.ts";
import { localEntriesFromDir } from "../tools/localdeno.ts";

const root = fromFileUrl(new URL("..", import.meta.url));

class Disks {
  images = new Map<number, Uint8Array>();
  local?: LocalFatDrive;
  read(slot: number, lba: number, count: number, dst: Uint8Array): boolean | number {
    if (slot === 3) return this.local ? this.local.read(lba, count, dst) : false;
    const img = this.images.get(slot); if (!img || (lba + count) * 512 > img.length) return false;
    dst.set(img.subarray(lba * 512, (lba + count) * 512)); return true;
  }
  write(slot: number, lba: number, count: number, src: Uint8Array) {
    if (slot === 3) return this.local ? this.local.write(lba, count, src) : false;
    const img = this.images.get(slot); if (!img || (lba + count) * 512 > img.length) return false;
    img.set(src, lba * 512); return true;
  }
}

/** Run emulated time while letting the local drive's async file reads resolve. */
async function runMs(core: Core, ms: number) {
  for (let t = 0; t < ms; t += 10) {
    const r = core.ex.core_run_us(10_000);
    if (r === 1) throw new Error("machine fatal at " + t + " ms");
    await new Promise((res) => setTimeout(res, 0));
  }
}

function pattern(n: number): Uint8Array {
  const b = new Uint8Array(n);
  for (let i = 0; i < n; i++) b[i] = (i * 7 + (i >> 8)) & 0xFF;
  return b;
}

Deno.test("host directory mounts as D: with lazy reads and DOS copies from it intact", async () => {
  const dir = await Deno.makeTempDir({ prefix: "dmlocal" });
  const data = pattern(200_000); // spans multiple 64 KB blocks and clusters
  try {
    await Deno.mkdir(join(dir, "GAMEA", "LEVELS"), { recursive: true });
    await Deno.writeTextFile(join(dir, "GAMEA", "README.TXT"), "HELLO FROM THE LOCAL DRIVE\r\n");
    await Deno.writeFile(join(dir, "GAMEA", "LEVELS", "DATA.BIN"), data);
    await Deno.writeTextFile(join(dir, "TOP.TXT"), "TOP LEVEL\r\n");

    const files = new Map<string, Uint8Array>();
    for await (const e of Deno.readDir(join(root, "dos"))) if (e.isFile) files.set(e.name.toUpperCase(), await Deno.readFile(join(root, "dos", e.name)));
    const image = new Uint8Array(planDisk(32).totalSectors * 512);
    buildSystemDisk(new ArraySectorIO(image), 32, files);

    const disks = new Disks();
    disks.images.set(2, image);
    const logs: string[] = [];
    disks.local = new LocalFatDrive((s) => logs.push(s)).build(await localEntriesFromDir(dir));
    assertEquals(disks.local.info.files, 3);

    const core = new Core(disks, (s) => logs.push(s));
    await core.load(await Deno.readFile(join(root, "dist", "dosmobile.wasm")));
    core.ex.core_init(GEN.G486, 66_000, 4096, 0, 1, 4, 0);
    core.ex.core_disk_attach(2, image.length / 512, 0);
    core.ex.core_disk_attach(3, disks.local.info.totalSectors, 0);

    await runMs(core, 2500);
    assertStringIncludes(core.textScreen().join("\n"), "C:\\>");

    for (const c of textToScancodes("D:\nDIR\n")) core.ex.core_key(c);
    await runMs(core, 2000);
    let screen = core.textScreen().join("\n");
    assertStringIncludes(screen, "D:\\>");
    assertStringIncludes(screen, "GAMEA");
    assertStringIncludes(screen, "TOP      TXT");

    for (const c of textToScancodes("TYPE GAMEA\\README.TXT\n")) core.ex.core_key(c);
    await runMs(core, 1500);
    assertStringIncludes(core.textScreen().join("\n"), "HELLO FROM THE LOCAL DRIVE");

    for (const c of textToScancodes("COPY GAMEA\\LEVELS\\DATA.BIN C:\\X.BIN\n")) core.ex.core_key(c);
    await runMs(core, 6000);
    screen = core.textScreen().join("\n");
    assertStringIncludes(screen, "1 File(s) copied");

    const fs = new FatFs(new ArraySectorIO(image)).mount();
    const entry = fs.find(0, "X.BIN");
    assert(entry, "X.BIN exists on C: (" + logs.join(" | ") + ")");
    assertEquals(fs.readFile(entry!), data, "DOS copy of the lazily-read file is byte-identical");
  } finally {
    await Deno.remove(dir, { recursive: true });
  }
});

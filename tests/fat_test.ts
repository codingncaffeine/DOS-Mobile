// FAT driver tests: write files into an existing C: image, then let DOS itself verify them.
import { assert, assertEquals, assertStringIncludes } from "jsr:@std/assert@1";
import { fromFileUrl, join } from "jsr:@std/path@1";
import { Core, GEN, textToScancodes } from "../web/core.ts";
import { buildSystemDisk } from "../web/sysdisk.ts";
import { ArraySectorIO, FatFs } from "../web/fatfs.ts";
import { readZip } from "../web/zip.ts";

const root = fromFileUrl(new URL("..", import.meta.url));

async function dosFiles() {
  const files = new Map<string, Uint8Array>();
  for await (const e of Deno.readDir(join(root, "dos"))) if (e.isFile) files.set(e.name.toUpperCase(), await Deno.readFile(join(root, "dos", e.name)));
  return files;
}

class MemDisks {
  images = new Map<number, Uint8Array>();
  read(slot: number, lba: number, count: number, dst: Uint8Array) { const img = this.images.get(slot)!; dst.set(img.subarray(lba * 512, (lba + count) * 512)); return true; }
  write(slot: number, lba: number, count: number, src: Uint8Array) { const img = this.images.get(slot)!; img.set(src, lba * 512); return true; }
}

Deno.test("FatFs writes files and directories DOS can read", async () => {
  const disk = buildSystemDisk(32, await dosFiles());
  const fs = new FatFs(new ArraySectorIO(disk.image)).mount();
  assert(fs.fat16);
  const freeBefore = fs.freeClusterCount();
  const t = fs.ensurePath("GAMES\\My Long Program Name");
  assertEquals(t.dosPath, "C:\\GAMES\\MY_LON~1");
  const text = new TextEncoder().encode("hello from the host\r\n");
  const sn = fs.writeFile(t.cluster, "readme first.txt", text);
  assertEquals(sn, "README~1.TXT");
  const big = new Uint8Array(100_000); for (let i = 0; i < big.length; i++) big[i] = i & 0xFF;
  fs.writeFile(t.cluster, "DATA.BIN", big);
  const sub = fs.mkdir(t.cluster, "levels");
  fs.writeFile(sub.cluster, "L1.DAT", new Uint8Array([1, 2, 3]));
  // many entries force the directory to grow past one cluster
  for (let i = 0; i < 80; i++) fs.writeFile(sub.cluster, `FILE${i}.TXT`, new Uint8Array([i]));
  fs.flush();
  assert(fs.freeClusterCount() < freeBefore);
  // re-mount and read back
  const fs2 = new FatFs(new ArraySectorIO(disk.image)).mount();
  const dir = fs2.resolveDir("GAMES\\MY_LON~1")!;
  const entry = fs2.list(dir).find((e) => e.name === "DATA.BIN")!;
  assertEquals(entry.size, 100_000);
  assertEquals(fs2.readFile(entry), big);
  assertEquals(fs2.list(fs2.resolveDir("GAMES\\MY_LON~1\\LEVELS")!).filter((e) => e.name.startsWith("FILE")).length, 80);

  // DOS agrees: boot, TYPE the text file, CHKDSK reports no errors
  const disks = new MemDisks();
  disks.images.set(2, disk.image);
  const core = new Core(disks, () => {});
  await core.load(await Deno.readFile(join(root, "dist", "dosmobile.wasm")));
  core.ex.core_init(GEN.G486, 66000, 4096, 0, 1, 4, 0);
  core.ex.core_disk_attach(2, disk.image.length / 512, 0);
  const run = (ms: number) => { for (let t = 0; t < ms; t += 10) if (core.ex.core_run_us(10_000) === 1) throw new Error("fatal"); };
  run(2500);
  for (const c of textToScancodes("TYPE C:\\GAMES\\MY_LON~1\\README~1.TXT\nCHKDSK\n")) core.ex.core_key(c);
  run(3000);
  const screen = core.textScreen().join("\n");
  assertStringIncludes(screen, "hello from the host");
  assertStringIncludes(screen, "bytes free");
  assert(!screen.includes("Errors found"), screen);
});

Deno.test("ZIP reader handles stored entries", async () => {
  // build a tiny stored ZIP by hand: one file "A/B.TXT" = "hi"
  const name = new TextEncoder().encode("A/B.TXT"), data = new TextEncoder().encode("hi");
  const crc = 0; // not verified by the reader
  const local = new Uint8Array(30 + name.length + data.length);
  const dv = new DataView(local.buffer);
  dv.setUint32(0, 0x04034b50, true); dv.setUint16(8, 0, true); dv.setUint32(14, crc, true);
  dv.setUint32(18, data.length, true); dv.setUint32(22, data.length, true); dv.setUint16(26, name.length, true);
  local.set(name, 30); local.set(data, 30 + name.length);
  const cd = new Uint8Array(46 + name.length);
  const cv = new DataView(cd.buffer);
  cv.setUint32(0, 0x02014b50, true); cv.setUint16(10, 0, true); cv.setUint32(20, data.length, true); cv.setUint32(24, data.length, true);
  cv.setUint16(28, name.length, true); cv.setUint32(42, 0, true);
  cd.set(name, 46);
  const eocd = new Uint8Array(22);
  const ev = new DataView(eocd.buffer);
  ev.setUint32(0, 0x06054b50, true); ev.setUint16(8, 1, true); ev.setUint16(10, 1, true); ev.setUint32(12, cd.length, true); ev.setUint32(16, local.length, true);
  const zip = new Uint8Array([...local, ...cd, ...eocd]);
  const entries = await readZip(zip);
  assertEquals(entries.length, 1);
  assertEquals(entries[0].path, "A/B.TXT");
  assertEquals(new TextDecoder().decode(await entries[0].data()), "hi");
});

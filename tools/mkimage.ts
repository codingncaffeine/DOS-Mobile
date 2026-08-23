// Build a bootable C: image from dos/ for headless tests: deno run -A tools/mkimage.ts [sizeMB] [out]
import { fromFileUrl, join } from "jsr:@std/path@1";
import { buildSystemDisk } from "../web/sysdisk.ts";
import { listRoot } from "../web/fat.ts";

const root = fromFileUrl(new URL("..", import.meta.url));
const sizeMB = Number(Deno.args[0] ?? "64");
const out = Deno.args[1] ?? join(root, ".cache", "hdd.img");
const files = new Map<string, Uint8Array>();
for await (const e of Deno.readDir(join(root, "dos"))) if (e.isFile) files.set(e.name.toUpperCase(), await Deno.readFile(join(root, "dos", e.name)));
const disk = buildSystemDisk(sizeMB, files);
await Deno.mkdir(join(root, ".cache"), { recursive: true });
await Deno.writeFile(out, disk.image);
console.log(`wrote ${out}: ${disk.image.length / 1048576} MB, FAT${disk.volume.geo.fat16 ? 16 : 12}, ${disk.volume.geo.clusters} clusters of ${disk.volume.geo.sectorsPerCluster * 512} bytes`);
for (const e of listRoot(disk.image, disk.volume.base)) console.log(`  ${e.name.padEnd(12)} attr=${e.attr.toString(16).padStart(2, "0")} cluster=${e.cluster} size=${e.size}`);

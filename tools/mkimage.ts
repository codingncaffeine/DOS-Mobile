// Build a bootable disk image from dos/ for headless tests:
//   deno run -A tools/mkimage.ts [sizeMB] [out] [--emm386] [--config "line;line"] [--autoexec "line;line"]
import { fromFileUrl, join } from "jsr:@std/path@1";
import { buildSystemDisk, DEFAULT_AUTOEXEC_BAT, DEFAULT_CONFIG_SYS } from "../web/sysdisk.ts";
import { planDisk } from "../web/hdd.ts";
import { ArraySectorIO, FatFs } from "../web/fatfs.ts";

const root = fromFileUrl(new URL("..", import.meta.url));
const args = Deno.args.filter((a) => !a.startsWith("--"));
const opt = (n: string) => { const i = Deno.args.indexOf("--" + n); return i >= 0 ? Deno.args[i + 1] : undefined; };
const sizeMB = Number(args[0] ?? "64");
const out = args[1] ?? join(root, ".cache", "hdd.img");
const files = new Map<string, Uint8Array>();
for await (const e of Deno.readDir(join(root, "dos"))) if (e.isFile) files.set(e.name.toUpperCase(), await Deno.readFile(join(root, "dos", e.name)));
let configSys = DEFAULT_CONFIG_SYS;
if (Deno.args.includes("--emm386")) configSys = "DEVICE=C:\\DOS\\EMM386.SYS\r\n" + configSys;
if (opt("config")) configSys = opt("config")!.split(";").join("\r\n") + "\r\n";
const autoexec = opt("autoexec") ? opt("autoexec")!.split(";").join("\r\n") + "\r\n" : DEFAULT_AUTOEXEC_BAT;
const plan = planDisk(sizeMB);
const image = new Uint8Array(plan.totalSectors * 512);
buildSystemDisk(new ArraySectorIO(image), sizeMB, files, { configSys, autoexec });
await Deno.mkdir(join(root, ".cache"), { recursive: true });
await Deno.writeFile(out, image);
const fs = new FatFs(new ArraySectorIO(image)).mount();
console.log(`wrote ${out}: ${(image.length / 1048576).toFixed(1)} MB, ${plan.volumes.length} volume(s), C: FAT${fs.fat16 ? 16 : 12} ${fs.clusters} clusters of ${fs.spc * 512} bytes`);

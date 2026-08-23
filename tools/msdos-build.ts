// Rebuild MS-DOS 4.01 from Microsoft's MIT-licensed source tree using the toolchain that ships
// with it (MASM 5.1, Microsoft C 5.1, LINK, NMAKE), inside a throwaway DOS environment.
//
//   deno run -A tools/msdos-build.ts            # clone, stage, build, copy binaries into dos/
//   deno run -A tools/msdos-build.ts --stage    # clone + stage only (inspect tools/_msdos-build)
//
// The DOS environment is DOSBox Staging (downloaded to tools/_dosbox, never shipped). Both the
// source clone and the build tree are gitignored; only the resulting binaries live in dos/.
//
// Source-tree fixes applied before building (artifacts of the GitHub upload, not of the code):
//  1. Comment lines whose CP437 box-drawing characters became U+FFFD (3 bytes each) exceed MASM's
//     128-column limit: files that are fully valid UTF-8 are mapped back to single CP437 bytes.
//  2. Skeleton/answer files stored with LF line endings: the message compiler and EXE2BIN's
//     interactive prompt need CR LF.
//  3. Every sub-make is run with /I so one optional component cannot stop the tree.
import { fromFileUrl, join } from "jsr:@std/path@1";

const root = fromFileUrl(new URL("..", import.meta.url));
const srcClone = join(root, "tools", "_msdos-src");
const buildDir = join(root, "tools", "_msdos-build");
const dosboxDir = join(root, "tools", "_dosbox");
const stageOnly = Deno.args.includes("--stage");

const MSDOS_REPO = "https://github.com/microsoft/MS-DOS.git";
const DOSBOX_ZIP = "https://github.com/dosbox-staging/dosbox-staging/releases/download/v0.82.2/dosbox-staging-windows-x64-v0.82.2.zip";

async function run(cmd: string, args: string[], opts: { cwd?: string; env?: Record<string, string>; check?: boolean } = {}) {
  const p = await new Deno.Command(cmd, { args, cwd: opts.cwd, env: opts.env, stdout: "inherit", stderr: "inherit" }).output();
  if (opts.check !== false && p.code !== 0) throw new Error(`${cmd} ${args[0]} failed (${p.code})`);
}

async function exists(p: string) { try { await Deno.stat(p); return true; } catch { return false; } }

async function copyTree(src: string, dst: string) {
  await Deno.mkdir(dst, { recursive: true });
  for await (const e of Deno.readDir(src)) {
    const s = join(src, e.name), d = join(dst, e.name);
    if (e.isDirectory) await copyTree(s, d); else await Deno.copyFile(s, d);
  }
}

const TEXT_EXT = new Set([".ASM", ".INC", ".C", ".H", ".SKL", ".MSG", ".TXT", ".MAC", ".BAT", ".MAK", ".LNK", ".DEF", ".INF", ".CHG", ".MEU", ".LBR", ".INI", ".DOC", ".SIL", ".DAT", ""]);
const BOX: Record<number, number> = { 0x2500: 0xC4, 0x2502: 0xB3, 0x250C: 0xDA, 0x2510: 0xBF, 0x2514: 0xC0, 0x2518: 0xD9, 0x251C: 0xC3, 0x2524: 0xB4, 0x252C: 0xC2, 0x2534: 0xC1, 0x253C: 0xC5, 0x2550: 0xCD, 0x2551: 0xBA, 0x2554: 0xC9, 0x2557: 0xBB, 0x255A: 0xC8, 0x255D: 0xBC, 0x2588: 0xDB, 0x2591: 0xB0, 0x2592: 0xB1, 0x2593: 0xB2, 0xFFFD: 0xC4 };

function decodeUtf8Strict(b: Uint8Array): number[] | null {
  const cps: number[] = [];
  for (let i = 0; i < b.length; i++) {
    const c = b[i];
    if (c < 0x80) { cps.push(c); continue; }
    const len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
    if (!len || i + len > b.length) return null;
    let cp = len === 2 ? (c & 0x1F) : len === 3 ? (c & 0x0F) : (c & 0x07);
    for (let k = 1; k < len; k++) { if ((b[i + k] & 0xC0) !== 0x80) return null; cp = (cp << 6) | (b[i + k] & 0x3F); }
    cps.push(cp); i += len - 1;
  }
  return cps;
}

async function fixSources(dir: string) {
  let enc = 0, crlf = 0;
  const walk = async (d: string) => {
    for await (const e of Deno.readDir(d)) {
      const p = join(d, e.name);
      if (e.isDirectory) { await walk(p); continue; }
      const dot = e.name.lastIndexOf(".");
      const ext = dot >= 0 ? e.name.slice(dot).toUpperCase() : "";
      if (!TEXT_EXT.has(ext)) continue;
      let b = await Deno.readFile(p);
      let changed = false;
      if (b.some((x) => x >= 0x80)) {
        const cps = decodeUtf8Strict(b);
        if (cps) { // fully valid UTF-8: map back to CP437 bytes
          const out: number[] = [];
          for (const cp of cps) {
            if (cp < 0x80) out.push(cp);
            else if (BOX[cp] !== undefined) out.push(BOX[cp]);
            else out.push(...new TextEncoder().encode(String.fromCodePoint(cp)));
          }
          b = new Uint8Array(out); changed = true; enc++;
        }
      }
      if (!b.includes(13) && b.includes(10)) {
        const out: number[] = [];
        for (const c of b) { if (c === 10) out.push(13, 10); else out.push(c); }
        b = new Uint8Array(out); changed = true; crlf++;
      }
      if (changed) await Deno.writeFile(p, b);
    }
  };
  await walk(dir);
  console.log(`encoding fixes: ${enc} files, LF->CRLF: ${crlf} files`);
  // make every sub-make ignore errors so optional components cannot stop the tree
  const makefiles: string[] = [];
  const findMk = async (d: string) => { for await (const e of Deno.readDir(d)) { const p = join(d, e.name); if (e.isDirectory) await findMk(p); else if (e.name.toUpperCase() === "MAKEFILE") makefiles.push(p); } };
  await findMk(dir);
  let patched = 0;
  for (const mk of makefiles) {
    const t = await Deno.readTextFile(mk);
    const n = t.replace(/^(make[ \t]*=[ \t]*nmake)[ \t]*\r?$/m, "$1 /I\r");
    if (n !== t) { await Deno.writeTextFile(mk, n); patched++; }
  }
  console.log(`makefiles patched: ${patched}`);
}

const BUILD_BAT = [
  "@echo off", "set CL=", "set LINK=", "set MASM=", "set COUNTRY=usa-ms", "set BAKROOT=d:",
  "set LIB=d:\\src\\tools\\bld\\lib", "set INIT=d:\\src\\tools", "set INCLUDE=d:\\src\\tools\\bld\\inc", "set PATH=d:\\src\\tools",
  "d:", "cd d:\\src", "nmake /I /X d:\\build.err > d:\\build.log", "echo NMAKE-DONE >> d:\\build.log",
  "cd d:\\src", "call cpy.bat d:\\bin >> d:\\build.log", "copy boot\\msboot.bin d:\\bin >> d:\\build.log",
  "echo BUILD-FINISHED >> d:\\build.log", "",
].join("\r\n");

const DOSBOX_CONF = ["[sdl]", "fullscreen = false", "[dosbox]", "machine = svga_s3", "memsize = 16", "[cpu]", "core = normal", "cputype = auto", "cycles = max", "[mixer]", "nosound = true", "[autoexec]", ""].join("\n");

// ---- stage
if (!await exists(srcClone)) {
  console.log("cloning microsoft/MS-DOS …");
  await run("git", ["clone", "-q", "-c", "core.autocrlf=false", "--depth", "1", MSDOS_REPO, srcClone]);
}
await Deno.remove(buildDir, { recursive: true }).catch(() => {});
await copyTree(join(srcClone, "v4.0", "src"), join(buildDir, "src"));
await Deno.mkdir(join(buildDir, "bin"), { recursive: true });
await fixSources(join(buildDir, "src"));
await Deno.writeTextFile(join(buildDir, "src", "BUILD.BAT"), BUILD_BAT);
await Deno.writeTextFile(join(buildDir, "dosbox.conf"), DOSBOX_CONF);
if (stageOnly) { console.log("staged at", buildDir); Deno.exit(0); }

// ---- DOSBox Staging (build tool only)
let dosboxExe = "";
if (await exists(dosboxDir)) for await (const e of Deno.readDir(dosboxDir)) if (e.isDirectory && await exists(join(dosboxDir, e.name, "dosbox.exe"))) dosboxExe = join(dosboxDir, e.name, "dosbox.exe");
if (!dosboxExe) {
  console.log("downloading DOSBox Staging …");
  await Deno.mkdir(dosboxDir, { recursive: true });
  const zip = join(dosboxDir, "dosbox.zip");
  await Deno.writeFile(zip, new Uint8Array(await (await fetch(DOSBOX_ZIP)).arrayBuffer()));
  await run("tar", ["-xf", zip, "-C", dosboxDir]);
  for await (const e of Deno.readDir(dosboxDir)) if (e.isDirectory && await exists(join(dosboxDir, e.name, "dosbox.exe"))) dosboxExe = join(dosboxDir, e.name, "dosbox.exe");
  if (!dosboxExe) throw new Error("dosbox.exe not found after extraction");
}

// ---- build (the window is parked off-screen; SDL has no usable headless driver for this build)
console.log("building MS-DOS 4.01 … (about a minute)");
await run(dosboxExe, ["--noprimaryconf", "-conf", join(buildDir, "dosbox.conf"), "-c", `mount d "${buildDir}"`, "-c", "d:", "-c", "cd src", "-c", "call build.bat", "-c", "exit"],
  { env: { SDL_VIDEO_WINDOW_POS: "-5000,-5000", SDL_AUDIODRIVER: "dummy" }, check: false });
const log = await Deno.readTextFile(join(buildDir, "build.log")).catch(() => "");
if (!log.includes("BUILD-FINISHED")) throw new Error("build did not finish; see " + join(buildDir, "build.log"));

// ---- collect
const WANTED = ["IO.SYS", "MSDOS.SYS", "COMMAND.COM", "MSBOOT.BIN", "ANSI.SYS", "APPEND.EXE", "ASSIGN.COM", "ATTRIB.EXE", "BACKUP.COM", "CHKDSK.COM", "COMP.COM", "COUNTRY.SYS", "DEBUG.COM", "DISKCOMP.COM", "DISKCOPY.COM", "DISPLAY.SYS", "EDLIN.COM", "EGA.CPI", "EMM386.SYS", "EXE2BIN.EXE", "FASTOPEN.EXE", "FC.EXE", "FDISK.EXE", "FIND.EXE", "FORMAT.COM", "GRAFTABL.COM", "GRAPHICS.COM", "GRAPHICS.PRO", "JOIN.EXE", "KEYB.COM", "KEYBOARD.SYS", "LABEL.COM", "MEM.EXE", "MODE.COM", "MORE.COM", "NLSFUNC.EXE", "PRINT.COM", "RAMDRIVE.SYS", "RECOVER.COM", "REPLACE.EXE", "RESTORE.COM", "SHARE.EXE", "SMARTDRV.SYS", "SORT.EXE", "SUBST.EXE", "SYS.COM", "TREE.COM", "XCOPY.EXE"];
const outDir = join(root, "dos");
await Deno.mkdir(outDir, { recursive: true });
let missing = 0;
for (const f of WANTED) {
  const src = join(buildDir, "bin", f);
  if (!await exists(src)) { console.log("missing:", f); missing++; continue; }
  let bytes = await Deno.readFile(src);
  if (f === "MSBOOT.BIN") bytes = bytes.subarray(0x7C00, 0x7E00); /* EXE2BIN output is ORG 7C00h padded */
  await Deno.writeFile(join(outDir, f), bytes);
}
await Deno.copyFile(join(srcClone, "v4.0", "LICENSE"), join(outDir, "LICENSE-MSDOS.txt"));
console.log(`dos/: ${WANTED.length - missing} files written${missing ? `, ${missing} missing` : ""}`);

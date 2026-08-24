// RAR reader tests.
//
// The r5*.rar fixtures were created with WinRAR 7 from the deterministic
// payloads in fixtures/rargen.ts:
//   rar a -ma5 -m5 -md128k -ep1 r5.rar  text.txt pattern.bin prog.exe sub
//   rar a -ma5 -m5 -md128k -s -ep1 r5s.rar  text.txt pattern.bin prog.exe
//   rar a -ma5 -m5 -md128k -v3k -ep1 r5v.rar  text.txt prog.exe
// The RAR4 containers are synthesized in-test (stored entries only — v2.9
// decompression is exercised against real archives by the local differential
// rig, which needs WinRAR installed).
import { assert, assertEquals, assertRejects } from "jsr:@std/assert@1";
import { fromFileUrl, join } from "jsr:@std/path@1";
import { crc32, extractRar, findRarSignature, isRarVolumeSet, RarError } from "../web/rar.ts";
import { FIXTURE_FILES } from "./fixtures/rargen.ts";

const root = fromFileUrl(new URL("..", import.meta.url));

async function fixture(name: string): Promise<{ name: string; bytes: Uint8Array }> {
  return { name, bytes: await Deno.readFile(join(root, "tests", "fixtures", name)) };
}

function expectPayloads(): Map<string, Uint8Array> {
  const m = new Map<string, Uint8Array>();
  for (const f of FIXTURE_FILES) m.set(f.path, f.data());
  return m;
}

async function extractAll(vols: { name: string; bytes: Uint8Array }[]) {
  const files = new Map<string, Uint8Array>();
  const dirs: string[] = [];
  const result = await extractRar(vols, (e, data) => {
    if (e.isDir) dirs.push(e.path);
    else files.set(e.path.replace(/\\/g, "/"), data);
  });
  return { files, dirs, result };
}

function assertMatches(files: Map<string, Uint8Array>, expected: Map<string, Uint8Array>, only?: string[]) {
  for (const [path, want] of expected) {
    if (only && !only.includes(path)) continue;
    const got = files.get(path);
    assert(got, `missing ${path} (have: ${[...files.keys()].join(", ")})`);
    assertEquals(got.length, want.length, `${path} size`);
    assertEquals(crc32(0, got), crc32(0, want), `${path} content`);
  }
}

Deno.test("RAR5: compressed archive with subdirectory", async () => {
  const { files, result } = await extractAll([await fixture("r5.rar")]);
  assertEquals(result.warnings, []);
  assertEquals(files.size, 4);
  assertMatches(files, expectPayloads());
});

Deno.test("RAR5: solid archive", async () => {
  const { files, result } = await extractAll([await fixture("r5s.rar")]);
  assertEquals(result.warnings, []);
  assertMatches(files, expectPayloads(), ["text.txt", "pattern.bin", "prog.exe"]);
});

Deno.test("RAR5: multi-volume set with files split across volumes", async () => {
  const vols = [await fixture("r5v.part02.rar"), await fixture("r5v.part01.rar"), await fixture("r5v.part03.rar")];
  const { files, result } = await extractAll(vols); // deliberately unsorted
  assertEquals(result.warnings, []);
  assertMatches(files, expectPayloads(), ["text.txt", "prog.exe"]);
});

Deno.test("RAR5: missing volume is reported", async () => {
  await assertRejects(
    async () => { await extractAll([await fixture("r5v.part01.rar")]); },
    RarError,
    "missing",
  );
});

Deno.test("volume set detection", () => {
  assert(isRarVolumeSet(["game.rar"]));
  assert(isRarVolumeSet(["game.part1.rar", "game.part2.rar"]));
  assert(isRarVolumeSet(["game.rar", "game.r00", "game.r01"]));
  assert(!isRarVolumeSet(["a.rar", "b.rar"]));
  assert(!isRarVolumeSet(["game.rar", "readme.txt"]));
  assert(!isRarVolumeSet([]));
});

/* ---------- synthesized RAR4 containers (stored entries) ---------- */

function w16(a: number[], v: number) { a.push(v & 0xFF, (v >> 8) & 0xFF); }
function w32(a: number[], v: number) { a.push(v & 0xFF, (v >>> 8) & 0xFF, (v >>> 16) & 0xFF, (v >>> 24) & 0xFF); }

function rar4Block(type: number, flags: number, body: number[], data?: Uint8Array): Uint8Array {
  const size = 7 + body.length;
  const head = [type, flags & 0xFF, (flags >> 8) & 0xFF, size & 0xFF, (size >> 8) & 0xFF, ...body];
  const crc = crc32(0, new Uint8Array(head)) & 0xFFFF;
  const out = new Uint8Array(2 + head.length + (data?.length ?? 0));
  out[0] = crc & 0xFF; out[1] = (crc >> 8) & 0xFF;
  out.set(head, 2);
  if (data) out.set(data, 2 + head.length);
  return out;
}

function rar4File(name: string, data: Uint8Array, flags: number, fileCrc: number, packed: Uint8Array): Uint8Array {
  const body: number[] = [];
  w32(body, packed.length);      // pack size
  w32(body, data.length);        // unp size
  body.push(0);                  // host os
  w32(body, fileCrc);
  w32(body, 0x2B7A3F51);         // dos time
  body.push(20);                 // unp ver
  body.push(0x30);               // method: stored
  w16(body, name.length);
  w32(body, 0x20);               // attributes
  for (const c of name) body.push(c.charCodeAt(0) & 0xFF);
  return rar4Block(0x74, flags | 0x8000, body, packed);
}

function cat(...parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}

const RAR4_MARK = new Uint8Array([0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00]);

Deno.test("RAR4: synthesized stored archive with directory entry", async () => {
  const payload = new TextEncoder().encode("Hello from a stored RAR4 entry!\r\n".repeat(20));
  const main = rar4Block(0x73, 0x0000, [0, 0, 0, 0, 0, 0]);
  const dir = rar4Block(0x74, 0x00E0 | 0x8000, (() => {
    const b: number[] = [];
    w32(b, 0); w32(b, 0); b.push(0); w32(b, 0); w32(b, 0x2B7A3F51); b.push(20); b.push(0x30);
    w16(b, 5); w32(b, 0x10);
    for (const c of "GAMES") b.push(c.charCodeAt(0));
    return b;
  })());
  const file = rar4File("GAMES\\HELLO.TXT", payload, 0, crc32(0, payload), payload);
  const end = rar4Block(0x7B, 0x0000, []);
  const archive = cat(RAR4_MARK, main, dir, file, end);
  assertEquals(findRarSignature(archive)?.v5, false);
  const { files, dirs } = await extractAll([{ name: "test.rar", bytes: archive }]);
  assertEquals(dirs, ["GAMES"]);
  assertEquals(files.get("GAMES/HELLO.TXT")?.length, payload.length);
  assertEquals(crc32(0, files.get("GAMES/HELLO.TXT")!), crc32(0, payload));
});

Deno.test("RAR4: synthesized two-volume stored split file", async () => {
  const payload = new Uint8Array(5000);
  for (let i = 0; i < payload.length; i++) payload[i] = (i * 7) & 0xFF;
  const half = 2600;
  const p1 = payload.subarray(0, half), p2 = payload.subarray(half);
  const fullCrc = crc32(0, payload);
  const mainVol = rar4Block(0x73, 0x0001, [0, 0, 0, 0, 0, 0]);
  const vol1 = cat(
    RAR4_MARK, mainVol,
    rar4File("SPLIT.BIN", payload, 0x0002 /* split after */, crc32(0, p1), p1),
    rar4Block(0x7B, 0x0001, []),
  );
  const vol2 = cat(
    RAR4_MARK, mainVol,
    rar4File("SPLIT.BIN", payload, 0x0001 /* split before */, fullCrc, p2),
    rar4Block(0x7B, 0x0000, []),
  );
  const { files } = await extractAll([
    { name: "test.r00", bytes: vol2 },
    { name: "test.rar", bytes: vol1 },
  ]);
  assertEquals(files.get("SPLIT.BIN")?.length, payload.length);
  assertEquals(crc32(0, files.get("SPLIT.BIN")!), fullCrc);
});

Deno.test("RAR4: encrypted entry is skipped with a warning", async () => {
  const payload = new TextEncoder().encode("secret");
  const main = rar4Block(0x73, 0x0000, [0, 0, 0, 0, 0, 0]);
  const file = rar4File("SECRET.TXT", payload, 0x0004, crc32(0, payload), payload);
  const end = rar4Block(0x7B, 0x0000, []);
  const { files, result } = await extractAll([{ name: "t.rar", bytes: cat(RAR4_MARK, main, file, end) }]);
  assertEquals(files.size, 0);
  assertEquals(result.warnings.length, 1);
  assert(result.warnings[0].includes("password"));
});

/* ---------- local-only differential smoke vs UnRAR (skipped elsewhere) ---------- */

const CORPUS = "D:/Emulation/DOS Games/dos collection/DOS A-Z/#-A";
const UNRAR = "C:/Program Files/WinRAR/UnRAR.exe";

async function exists(p: string) {
  try { await Deno.stat(p); return true; } catch { return false; }
}

Deno.test({
  name: "RAR4 v2.9 differential smoke vs UnRAR (local corpus)",
  ignore: !(await exists(CORPUS)) || !(await exists(UNRAR)),
  fn: async () => {
    let tested = 0;
    for await (const e of Deno.readDir(CORPUS)) {
      if (!e.name.toLowerCase().endsWith(".rar")) continue;
      if (tested >= 2) break;
      const archive = join(CORPUS, e.name);
      const { files } = await extractAll([{ name: e.name, bytes: await Deno.readFile(archive) }]);
      // reference
      const refDir = await Deno.makeTempDir({ prefix: "rartest_" });
      const r = await new Deno.Command(UNRAR, {
        args: ["x", "-y", "-inul", "--", archive, refDir.replaceAll("/", "\\") + "\\"],
        stdout: "null", stderr: "null",
      }).output();
      assertEquals(r.code, 0);
      let checked = 0;
      const walk = async (dir: string, prefix: string) => {
        for await (const f of Deno.readDir(dir)) {
          const full = join(dir, f.name);
          if (f.isDirectory) { await walk(full, prefix + f.name + "/"); continue; }
          const want = await Deno.readFile(full);
          const got = files.get(prefix + f.name);
          assert(got, `missing ${prefix + f.name}`);
          assertEquals(got.length, want.length, `${prefix + f.name} size`);
          assertEquals(crc32(0, got), crc32(0, want), `${prefix + f.name} content`);
          checked++;
        }
      };
      await walk(refDir, "");
      await Deno.remove(refDir, { recursive: true });
      assert(checked > 0);
      tested++;
    }
    assert(tested > 0, "no corpus archives found");
  },
});

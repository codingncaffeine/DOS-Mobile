// Deterministic payloads for the committed RAR fixtures (tests/fixtures/r5*.rar).
// The fixtures were created from these exact bytes with `Rar.exe` (see the
// commands in tests/rar_test.ts); the test regenerates the payloads and
// compares extraction output against them byte-for-byte.

export function textPayload(): Uint8Array {
  let s = "";
  for (let i = 0; i < 1400; i++) {
    s += `Line ${i}: the quick brown fox jumps over the lazy dog ${(i * 37) % 1000} times.\r\n`;
  }
  return new TextEncoder().encode(s);
}

export function patternPayload(): Uint8Array {
  const b = new Uint8Array(24576);
  for (let i = 0; i < b.length; i++) b[i] = i % 251;
  return b;
}

export function exePayload(): Uint8Array {
  // x86-flavoured bytes with plenty of E8 rel32 call sites so the archiver's
  // executable filter has something to chew on.
  const b = new Uint8Array(32768);
  let x = 0x1234;
  for (let i = 0; i < b.length; i++) {
    x = (x * 25173 + 13849) & 0xFFFF;
    b[i] = x & 0xFF;
  }
  for (let i = 0; i + 5 < b.length; i += 64) {
    b[i] = 0xE8;
    const rel = (i * 3) & 0xFFFF;
    b[i + 1] = rel & 0xFF; b[i + 2] = (rel >> 8) & 0xFF; b[i + 3] = 0; b[i + 4] = 0;
  }
  return b;
}

export const FIXTURE_FILES: { path: string; data: () => Uint8Array }[] = [
  { path: "text.txt", data: textPayload },
  { path: "pattern.bin", data: patternPayload },
  { path: "prog.exe", data: exePayload },
  { path: "sub/nested.txt", data: () => new TextEncoder().encode("nested file content\r\n") },
];

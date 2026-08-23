// Minimal PNG encoder (RGBA input, stored deflate blocks) for headless screenshots.
function crc32(buf: Uint8Array): number {
  let c = ~0;
  for (const b of buf) { c ^= b; for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xEDB88320 & -(c & 1)); }
  return ~c >>> 0;
}
function adler32(buf: Uint8Array): number {
  let a = 1, b = 0;
  for (const x of buf) { a = (a + x) % 65521; b = (b + a) % 65521; }
  return ((b << 16) | a) >>> 0;
}
export function encodePng(w: number, h: number, rgba: Uint8Array): Uint8Array {
  const raw = new Uint8Array((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w * 3 + 1)] = 0;
    for (let x = 0; x < w; x++) { const s = (y * w + x) * 4, d = y * (w * 3 + 1) + 1 + x * 3; raw[d] = rgba[s]; raw[d + 1] = rgba[s + 1]; raw[d + 2] = rgba[s + 2]; }
  }
  const parts: number[] = [0x78, 0x01];
  for (let i = 0; i < raw.length; i += 65535) {
    const n = Math.min(65535, raw.length - i);
    parts.push(i + n >= raw.length ? 1 : 0, n & 255, n >> 8, ~n & 255, (~n >> 8) & 255);
    for (let k = 0; k < n; k++) parts.push(raw[i + k]);
  }
  const ad = adler32(raw);
  parts.push(ad >>> 24, (ad >>> 16) & 255, (ad >>> 8) & 255, ad & 255);
  const chunk = (type: string, data: Uint8Array) => {
    const t = new TextEncoder().encode(type);
    const body = new Uint8Array(t.length + data.length); body.set(t); body.set(data, t.length);
    const crc = crc32(body);
    return [data.length >>> 24, (data.length >>> 16) & 255, (data.length >>> 8) & 255, data.length & 255, ...body, crc >>> 24, (crc >>> 16) & 255, (crc >>> 8) & 255, crc & 255];
  };
  const ihdr = new Uint8Array([w >>> 24, (w >>> 16) & 255, (w >>> 8) & 255, w & 255, h >>> 24, (h >>> 16) & 255, (h >>> 8) & 255, h & 255, 8, 2, 0, 0, 0]);
  return new Uint8Array([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, ...chunk("IHDR", ihdr), ...chunk("IDAT", new Uint8Array(parts)), ...chunk("IEND", new Uint8Array(0))]);
}

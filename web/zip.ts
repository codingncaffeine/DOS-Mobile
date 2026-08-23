// Minimal ZIP reader (stored + deflate) for importing archives into the virtual disk.

export interface ZipEntry { path: string; isDir: boolean; data: () => Promise<Uint8Array>; }

export async function readZip(bytes: Uint8Array): Promise<ZipEntry[]> {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  // locate the end-of-central-directory record
  let eocd = -1;
  for (let i = bytes.length - 22; i >= Math.max(0, bytes.length - 65558); i--) {
    if (dv.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error("not a ZIP file");
  const count = dv.getUint16(eocd + 10, true);
  let off = dv.getUint32(eocd + 16, true);
  const entries: ZipEntry[] = [];
  const td = new TextDecoder("utf-8");
  for (let i = 0; i < count; i++) {
    if (dv.getUint32(off, true) !== 0x02014b50) break;
    const method = dv.getUint16(off + 10, true);
    const csize = dv.getUint32(off + 20, true);
    const usize = dv.getUint32(off + 24, true);
    const nlen = dv.getUint16(off + 28, true), xlen = dv.getUint16(off + 30, true), clen = dv.getUint16(off + 32, true);
    const lho = dv.getUint32(off + 42, true);
    const flags = dv.getUint16(off + 8, true);
    const rawName = bytes.subarray(off + 46, off + 46 + nlen);
    const name = (flags & 0x800) ? td.decode(rawName) : cp437ToString(rawName);
    off += 46 + nlen + xlen + clen;
    const isDir = name.endsWith("/");
    entries.push({
      path: name.replace(/\\/g, "/"),
      isDir,
      data: async () => {
        if (isDir) return new Uint8Array(0);
        const ln = dv.getUint16(lho + 26, true), lx = dv.getUint16(lho + 28, true);
        const start = lho + 30 + ln + lx;
        const comp = bytes.subarray(start, start + csize);
        if (method === 0) return comp.slice();
        if (method === 8) return inflateRaw(comp, usize);
        throw new Error(`unsupported compression method ${method} for ${name}`);
      },
    });
  }
  return entries;
}

async function inflateRaw(data: Uint8Array, expected: number): Promise<Uint8Array> {
  const ds = new DecompressionStream("deflate-raw");
  const writer = ds.writable.getWriter();
  writer.write(data.slice());
  writer.close();
  const out = new Uint8Array(expected);
  const reader = ds.readable.getReader();
  let pos = 0;
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    if (pos + value.length > out.length) throw new Error("inflate overflow");
    out.set(value, pos);
    pos += value.length;
  }
  return pos === expected ? out : out.subarray(0, pos);
}

function cp437ToString(b: Uint8Array): string {
  let s = "";
  for (const c of b) s += c < 0x80 ? String.fromCharCode(c) : "_";
  return s;
}

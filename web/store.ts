// IndexedDB-backed sparse chunk store for disk images (works in workers and the main thread).

const DB_NAME = "dosmobile";
const DB_VERSION = 1;
export const CHUNK = 65536;

function openDb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, DB_VERSION);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains("chunks")) db.createObjectStore("chunks");
      if (!db.objectStoreNames.contains("meta")) db.createObjectStore("meta");
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

export interface DiskMeta { id: string; sectors: number; label?: string; created: number; }

export class ChunkStore {
  private db!: IDBDatabase;
  async open() { this.db = await openDb(); return this; }

  getMeta(id: string): Promise<DiskMeta | undefined> {
    return new Promise((res, rej) => {
      const r = this.db.transaction("meta").objectStore("meta").get(id);
      r.onsuccess = () => res(r.result as DiskMeta | undefined);
      r.onerror = () => rej(r.error);
    });
  }
  putMeta(m: DiskMeta): Promise<void> {
    return new Promise((res, rej) => {
      const t = this.db.transaction("meta", "readwrite");
      t.objectStore("meta").put(m, m.id);
      t.oncomplete = () => res();
      t.onerror = () => rej(t.error);
    });
  }
  /** Load every stored chunk of a disk: Map<chunkIndex, bytes>. */
  loadChunks(id: string): Promise<Map<number, Uint8Array>> {
    return new Promise((res, rej) => {
      const out = new Map<number, Uint8Array>();
      const range = IDBKeyRange.bound(`${id}/`, `${id}/￿`);
      const req = this.db.transaction("chunks").objectStore("chunks").openCursor(range);
      req.onsuccess = () => {
        const cur = req.result;
        if (!cur) { res(out); return; }
        const idx = Number((cur.key as string).slice(id.length + 1));
        out.set(idx, new Uint8Array(cur.value as ArrayBuffer));
        cur.continue();
      };
      req.onerror = () => rej(req.error);
    });
  }
  putChunks(id: string, chunks: Map<number, Uint8Array>): Promise<void> {
    return new Promise((res, rej) => {
      const t = this.db.transaction("chunks", "readwrite");
      const st = t.objectStore("chunks");
      for (const [i, c] of chunks) st.put(c.slice().buffer, `${id}/${i.toString().padStart(8, "0")}`);
      t.oncomplete = () => res();
      t.onerror = () => rej(t.error);
    });
  }
  deleteDisk(id: string): Promise<void> {
    return new Promise((res, rej) => {
      const t = this.db.transaction(["chunks", "meta"], "readwrite");
      t.objectStore("chunks").delete(IDBKeyRange.bound(`${id}/`, `${id}/￿`));
      t.objectStore("meta").delete(id);
      t.oncomplete = () => res();
      t.onerror = () => rej(t.error);
    });
  }
}

/** A disk image held as sparse 64 KB chunks with dirty tracking. */
export class SparseImage {
  chunks = new Map<number, Uint8Array>();
  dirty = new Set<number>();
  constructor(public sectors: number) {}

  static fromBytes(bytes: Uint8Array): SparseImage {
    const img = new SparseImage(bytes.length / 512);
    for (let i = 0; i * CHUNK < bytes.length; i++) {
      const part = bytes.subarray(i * CHUNK, Math.min((i + 1) * CHUNK, bytes.length));
      if (part.some((b) => b !== 0)) {
        const c = new Uint8Array(CHUNK); c.set(part);
        img.chunks.set(i, c);
        img.dirty.add(i);
      }
    }
    return img;
  }

  read(lba: number, count: number, dst: Uint8Array): boolean {
    if (lba + count > this.sectors) return false;
    let pos = lba * 512, left = count * 512, o = 0;
    while (left > 0) {
      const ci = Math.floor(pos / CHUNK), co = pos % CHUNK;
      const n = Math.min(left, CHUNK - co);
      const c = this.chunks.get(ci);
      if (c) dst.set(c.subarray(co, co + n), o); else dst.fill(0, o, o + n);
      pos += n; left -= n; o += n;
    }
    return true;
  }
  write(lba: number, count: number, src: Uint8Array): boolean {
    if (lba + count > this.sectors) return false;
    let pos = lba * 512, left = count * 512, o = 0;
    while (left > 0) {
      const ci = Math.floor(pos / CHUNK), co = pos % CHUNK;
      const n = Math.min(left, CHUNK - co);
      let c = this.chunks.get(ci);
      if (!c) { c = new Uint8Array(CHUNK); this.chunks.set(ci, c); }
      c.set(src.subarray(o, o + n), co);
      this.dirty.add(ci);
      pos += n; left -= n; o += n;
    }
    return true;
  }
  takeDirty(): Map<number, Uint8Array> {
    const out = new Map<number, Uint8Array>();
    for (const i of this.dirty) out.set(i, this.chunks.get(i)!);
    this.dirty.clear();
    return out;
  }
  toBytes(): Uint8Array {
    const out = new Uint8Array(this.sectors * 512);
    for (const [i, c] of this.chunks) out.set(c.subarray(0, Math.min(CHUNK, out.length - i * CHUNK)), i * CHUNK);
    return out;
  }
}

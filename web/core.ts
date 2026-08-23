// Thin typed wrapper around the wasm core. Works in the worker and in Deno (tests/tools).

export interface DiskProvider {
  read(slot: number, lba: number, count: number, dst: Uint8Array): boolean;
  write(slot: number, lba: number, count: number, src: Uint8Array): boolean;
}

export interface CoreExports {
  memory: WebAssembly.Memory;
  core_version(): number;
  core_init(gen: number, khz: number, ramKb: number, fpu: number, floppies: number, ft0: number, ft1: number): number;
  core_reset(warm: number): void;
  core_set_khz(khz: number): void;
  core_get_khz(): number;
  core_run_us(us: number): number;
  core_key(scancode: number): void;
  core_key_space(): number;
  core_disk_attach(slot: number, sectors: number, readonly: number): number;
  core_disk_detach(slot: number): void;
  core_fb_ptr(): number;
  core_fb_width(): number;
  core_fb_height(): number;
  core_frame_id(): number;
  core_set_time(y: number, mo: number, d: number, h: number, mi: number, s: number): void;
  core_halted(): number;
  core_fatal(): number;
  core_insns(): bigint;
  core_emu_ns(): bigint;
  core_reg(i: number): number;
  core_mem_ptr(): number;
  core_mem_size(): number;
  core_text_plane(p: number): number;
  core_text_cols(): number;
  core_text_rows(): number;
  core_text_is_text(): number;
  core_alloc(size: number): number;
}

export const GEN = { G8088: 0, G8086: 1, G186: 2, G286: 3, G386: 4, G486: 5, P5: 6, P6: 7 } as const;

export class Core {
  ex!: CoreExports;
  private logLine = "";
  constructor(public disks: DiskProvider, public onLog: (s: string) => void = (s) => console.log("[core] " + s)) {}

  get u8(): Uint8Array { return new Uint8Array(this.ex.memory.buffer); }

  async load(wasmBytes: BufferSource | WebAssembly.Module): Promise<void> {
    const imports = {
      env: {
        host_log: (ptr: number, len: number) => {
          const s = new TextDecoder().decode(this.u8.subarray(ptr, ptr + len));
          if (len === 1 && s !== "\n") { this.logLine += s; return; }
          if (this.logLine) { this.onLog(this.logLine); this.logLine = ""; }
          this.onLog(s);
        },
        host_disk_read: (slot: number, lba: number, count: number, dst: number) => {
          const ok = this.disks.read(slot, lba >>> 0, count, this.u8.subarray(dst, dst + count * 512));
          return ok ? 0 : 1;
        },
        host_disk_write: (slot: number, lba: number, count: number, src: number) => {
          const ok = this.disks.write(slot, lba >>> 0, count, this.u8.subarray(src, src + count * 512));
          return ok ? 0 : 1;
        },
      },
    };
    const mod = wasmBytes instanceof WebAssembly.Module ? wasmBytes : await WebAssembly.compile(wasmBytes);
    const inst = await WebAssembly.instantiate(mod, imports);
    this.ex = inst.exports as unknown as CoreExports;
  }

  /** 80x25 text snapshot (characters only), for tests and the headless harness. */
  textScreen(): string[] {
    const cols = this.ex.core_text_cols(), rows = this.ex.core_text_rows();
    const p0 = this.ex.core_text_plane(0);
    const mem = this.u8;
    const lines: string[] = [];
    for (let r = 0; r < rows; r++) {
      let line = "";
      for (let c = 0; c < cols; c++) {
        const ch = mem[p0 + ((r * cols + c) * 2)];
        line += ch >= 32 && ch < 127 ? String.fromCharCode(ch) : ch === 0 ? " " : ".";
      }
      lines.push(line.replace(/\s+$/, ""));
    }
    return lines;
  }

  regs(): Record<string, string> {
    const names = ["EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI", "EIP", "EFL", "ES", "CS", "SS", "DS"];
    const out: Record<string, string> = {};
    names.forEach((n, i) => out[n] = this.ex.core_reg(i).toString(16).padStart(i >= 10 ? 4 : 8, "0"));
    return out;
  }
}

/** Scancode helpers (set 1). */
export const SCAN: Record<string, number> = {
  Escape: 0x01, Digit1: 0x02, Digit2: 0x03, Digit3: 0x04, Digit4: 0x05, Digit5: 0x06, Digit6: 0x07, Digit7: 0x08,
  Digit8: 0x09, Digit9: 0x0A, Digit0: 0x0B, Minus: 0x0C, Equal: 0x0D, Backspace: 0x0E, Tab: 0x0F,
  KeyQ: 0x10, KeyW: 0x11, KeyE: 0x12, KeyR: 0x13, KeyT: 0x14, KeyY: 0x15, KeyU: 0x16, KeyI: 0x17, KeyO: 0x18,
  KeyP: 0x19, BracketLeft: 0x1A, BracketRight: 0x1B, Enter: 0x1C, ControlLeft: 0x1D, KeyA: 0x1E, KeyS: 0x1F,
  KeyD: 0x20, KeyF: 0x21, KeyG: 0x22, KeyH: 0x23, KeyJ: 0x24, KeyK: 0x25, KeyL: 0x26, Semicolon: 0x27,
  Quote: 0x28, Backquote: 0x29, ShiftLeft: 0x2A, Backslash: 0x2B, KeyZ: 0x2C, KeyX: 0x2D, KeyC: 0x2E, KeyV: 0x2F,
  KeyB: 0x30, KeyN: 0x31, KeyM: 0x32, Comma: 0x33, Period: 0x34, Slash: 0x35, ShiftRight: 0x36,
  NumpadMultiply: 0x37, AltLeft: 0x38, Space: 0x39, CapsLock: 0x3A, F1: 0x3B, F2: 0x3C, F3: 0x3D, F4: 0x3E,
  F5: 0x3F, F6: 0x40, F7: 0x41, F8: 0x42, F9: 0x43, F10: 0x44, NumLock: 0x45, ScrollLock: 0x46, Numpad7: 0x47,
  Numpad8: 0x48, Numpad9: 0x49, NumpadSubtract: 0x4A, Numpad4: 0x4B, Numpad5: 0x4C, Numpad6: 0x4D,
  NumpadAdd: 0x4E, Numpad1: 0x4F, Numpad2: 0x50, Numpad3: 0x51, Numpad0: 0x52, NumpadDecimal: 0x53,
  IntlBackslash: 0x56, F11: 0x57, F12: 0x58,
  // extended (E0-prefixed)
  NumpadEnter: 0xE01C, ControlRight: 0xE01D, NumpadDivide: 0xE035, AltRight: 0xE038, Home: 0xE047,
  ArrowUp: 0xE048, PageUp: 0xE049, ArrowLeft: 0xE04B, ArrowRight: 0xE04D, End: 0xE04F, ArrowDown: 0xE050,
  PageDown: 0xE051, Insert: 0xE052, Delete: 0xE053, MetaLeft: 0xE05B, MetaRight: 0xE05C, ContextMenu: 0xE05D,
};

/** ASCII text → scancode press/release sequence (US layout). */
export function textToScancodes(text: string): number[] {
  const out: number[] = [];
  const shifted = '~!@#$%^&*()_+{}|:"<>?';
  const unshifted = "`1234567890-=[]\\;',./";
  const press = (code: number, shift: boolean) => {
    if (shift) out.push(0x2A);
    if (code > 0xFF) { out.push(0xE0, code & 0xFF, 0xE0, (code & 0xFF) | 0x80); }
    else out.push(code, code | 0x80);
    if (shift) out.push(0xAA);
  };
  for (const ch of text) {
    if (ch === "\n" || ch === "\r") { press(0x1C, false); continue; }
    if (ch === " ") { press(0x39, false); continue; }
    if (ch === "\t") { press(0x0F, false); continue; }
    if (ch === "\b") { press(0x0E, false); continue; }
    if (ch === "\x1b") { press(0x01, false); continue; }
    const lower = ch.toLowerCase();
    if (lower >= "a" && lower <= "z") { press(SCAN["Key" + lower.toUpperCase()], ch !== lower); continue; }
    if (ch >= "0" && ch <= "9") { press(SCAN["Digit" + ch], false); continue; }
    const si = shifted.indexOf(ch);
    if (si >= 0) { press(SCAN[codeForPunct(unshifted[si])], true); continue; }
    const ui = unshifted.indexOf(ch);
    if (ui >= 0) { press(SCAN[codeForPunct(ch)], false); continue; }
  }
  return out;
}
function codeForPunct(ch: string): string {
  const m: Record<string, string> = {
    "`": "Backquote", "1": "Digit1", "2": "Digit2", "3": "Digit3", "4": "Digit4", "5": "Digit5", "6": "Digit6",
    "7": "Digit7", "8": "Digit8", "9": "Digit9", "0": "Digit0", "-": "Minus", "=": "Equal", "[": "BracketLeft",
    "]": "BracketRight", "\\": "Backslash", ";": "Semicolon", "'": "Quote", ",": "Comma", ".": "Period", "/": "Slash",
  };
  return m[ch];
}

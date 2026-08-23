// Messages between the page and the worker.

export interface MachineSettings {
  gen: number;       // GEN_*
  mhz: number;
  ramKb: number;
  fpu: boolean;
  hddSizeMB: number;
}

export const DEFAULT_SETTINGS: MachineSettings = { gen: 5, mhz: 66, ramKb: 8192, fpu: false, hddSizeMB: 8192 };

export type ToWorker =
  | { type: "init"; wasm: ArrayBuffer; settings: MachineSettings; dosBase: string; canvas?: OffscreenCanvas; debug?: boolean }
  | { type: "key"; codes: number[] }
  | { type: "mouse"; dx: number; dy: number; buttons: number }
  | { type: "reset"; warm: boolean }
  | { type: "setSpeed"; mhz: number }
  | { type: "pause"; paused: boolean }
  | { type: "flush" }
  | { type: "attachFloppy"; bytes: ArrayBuffer; name: string }
  | { type: "detachFloppy" }
  | { type: "importFiles"; name: string; files: { path: string; bytes: ArrayBuffer }[] }
  | { type: "importZip"; name: string; bytes: ArrayBuffer }
  | { type: "exportDisk" }
  | { type: "wipeDisk" };

export type FromWorker =
  | { type: "ready"; fbW: number; fbH: number }
  | { type: "frame"; w: number; h: number; buf: ArrayBuffer }
  | { type: "status"; mhz: number; effectiveMhz: number; load: number; halted: boolean; fatal: boolean; mips: number }
  | { type: "log"; text: string }
  | { type: "text"; lines: string[] }
  | { type: "progress"; text: string }
  | { type: "imported"; dosPath: string; count: number }
  | { type: "disk"; bytes: ArrayBuffer }
  | { type: "audio"; buf: ArrayBuffer; frames: number }
  | { type: "wiped" }
  | { type: "error"; text: string };

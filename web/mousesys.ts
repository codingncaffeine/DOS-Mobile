// MOUSE.SYS — a minimal DOS device driver (hand-assembled) whose only job is to point INT 33h
// at the ROM mouse services after DOS has claimed the 20h-3Fh vector range during boot.
// The INT 33h ROM stub lives at F000:(0x100 + 0x33*16).

const STUB_SEG = 0xF000;
const STUB_OFF = 0x100 + 0x33 * 16;

export function buildMouseSys(): Uint8Array {
  const b: number[] = [];
  const w16 = (v: number) => { b.push(v & 0xFF, (v >> 8) & 0xFF); };
  // device header
  b.push(0xFF, 0xFF, 0xFF, 0xFF);        // next: none
  w16(0x8000);                            // attributes: character device
  const strategyPatch = b.length; w16(0); // strategy offset (patched)
  const interruptPatch = b.length; w16(0);// interrupt offset (patched)
  for (const c of "MOUSE$  ") b.push(c.charCodeAt(0));
  const reqPtr = b.length; w16(0); w16(0);// request header pointer storage
  // strategy: save ES:BX
  const strategy = b.length;
  b.push(0x2E, 0x89, 0x1E); w16(reqPtr);  // mov [cs:reqPtr], bx
  b.push(0x2E, 0x8C, 0x06); w16(reqPtr + 2); // mov [cs:reqPtr+2], es
  b.push(0xCB);                           // retf
  // interrupt: on INIT set the vector and the resident size
  const interrupt = b.length;
  b.push(0x1E, 0x53, 0x50);               // push ds / push bx / push ax
  b.push(0x2E, 0xC5, 0x1E); w16(reqPtr);  // lds bx, [cs:reqPtr]
  b.push(0x8A, 0x47, 0x02);               // mov al, [bx+2] (command)
  b.push(0x08, 0xC0);                     // or al, al
  const jnzPatch = b.length; b.push(0x75, 0x00); // jnz done
  b.push(0x31, 0xC0, 0x8E, 0xD8);         // xor ax,ax / mov ds,ax
  b.push(0xC7, 0x06); w16(0x33 * 4); w16(STUB_OFF);     // mov word [0xCC], STUB_OFF
  b.push(0xC7, 0x06); w16(0x33 * 4 + 2); w16(STUB_SEG); // mov word [0xCE], F000
  b.push(0x2E, 0xC5, 0x1E); w16(reqPtr);  // lds bx, [cs:reqPtr]
  const endPatch = b.length + 3;
  b.push(0xC7, 0x47, 0x0E); w16(0);       // mov word [bx+0Eh], end (patched)
  b.push(0x8C, 0x4F, 0x10);               // mov [bx+10h], cs
  const done = b.length;
  b.push(0x2E, 0xC5, 0x1E); w16(reqPtr);  // lds bx, [cs:reqPtr]
  b.push(0xC7, 0x47, 0x03); w16(0x0100);  // mov word [bx+3], 0100h (status: done)
  b.push(0x58, 0x5B, 0x1F);               // pop ax / pop bx / pop ds
  b.push(0xCB);                           // retf
  const end = b.length;
  // patches
  b[strategyPatch] = strategy & 0xFF; b[strategyPatch + 1] = strategy >> 8;
  b[interruptPatch] = interrupt & 0xFF; b[interruptPatch + 1] = interrupt >> 8;
  b[jnzPatch + 1] = done - (jnzPatch + 2);
  b[endPatch] = end & 0xFF; b[endPatch + 1] = end >> 8;
  return new Uint8Array(b);
}

# DOS Mobile

A PC that runs MS-DOS in your browser — on phones, tablets and desktops, with no install and no
native code.

DOS Mobile is a from-scratch IBM PC emulator written in C, compiled to WebAssembly, wrapped in a
small TypeScript shell. It boots **MS-DOS 4.01 built from Microsoft's MIT-licensed source** and
runs the programs you bring to it from a persistent virtual hard disk kept in the browser.

## Status

Early. What works today:

- x86 CPU (8086 through 486 real-mode instruction set, 386 operand forms), selectable generation
  and clock speed
- PIT, dual PIC, 8042 keyboard controller, CMOS/RTC, VGA (text modes, CGA/EGA/VGA graphics modes
  at the register level)
- BIOS with the usual services (video, disk, keyboard, timer, memory)
- MS-DOS 4.01 boots to the prompt from a FAT16 drive C: that is created in the browser on first
  run and persisted in IndexedDB; floppy images can be inserted into A:
- Headless harness and browser smoke test

Planned: protected mode and paging (DOS extenders), Sound Blaster/OPL/PC speaker audio, mouse and
joystick, VESA modes up to 1280×1024, CD-ROM images, file import by drag-and-drop, a touch UI with
an on-screen keyboard, and machine presets from a 4.77 MHz XT up to Pentium-class speeds.

## Build

Requirements: [Deno](https://deno.com) 2.x and a clang with the `wasm32` target (LLVM 16+).

```
deno task build          # core (clang → dist/dosmobile.wasm) + shell (dist/)
deno task serve          # http://127.0.0.1:8088/
deno task test
```

Useful tools:

```
deno run -A tools/headless.ts --hdd .cache/hdd.img --ms 4000 --type "DIR\n"   # run the machine in Deno
deno run -A tools/mkimage.ts 64                                               # build a C: image from dos/
deno run -A tools/browsercheck.ts http://127.0.0.1:8088/?debug 12             # headless browser boot check
deno run -A tools/msdos-build.ts                                              # rebuild MS-DOS from source
```

## MS-DOS

The `dos/` directory holds MS-DOS 4.01 binaries built from
[microsoft/MS-DOS](https://github.com/microsoft/MS-DOS) (`v4.0/src`) with the assembler, C
compiler, linker and NMAKE that ship in that tree. `tools/msdos-build.ts` reproduces the build.
The source and binaries are © Microsoft Corporation, MIT License — see `dos/LICENSE-MSDOS.txt`.

## Layout

```
core/    C emulator core (CPU, chipset, VGA, BIOS, disk)
web/     TypeScript shell: worker (core + disk persistence), page, FAT image builder
dos/     MS-DOS 4.01 binaries + license
tools/   Deno scripts: build, serve, headless runner, image builder, DOS build, browser check
tests/   Deno tests
```

## License

MIT — see `LICENSE`. MS-DOS components: MIT, © Microsoft Corporation.

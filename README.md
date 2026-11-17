# Nescyan

Nescyan is a small, experimental NES emulator written in C++17 with SDL2. It loads iNES ROM files using mapper 0 (NROM), maps CHR ROM or CHR RAM, executes a practical subset of the 6502 instruction set, draws the NES background and sprites, and maps the first controller to the keyboard.

> This is an early emulator, not a cycle-accurate NES. It currently supports mapper 0 only; audio, mapper banking, accurate PPU scrolling/timing, and some 6502 opcodes are not implemented. ROM compatibility will vary.

## Build

Install a C++17 compiler, CMake, and SDL2 development libraries, then:

```sh
cmake -S . -B build
cmake --build build
./build/nescyan path/to/game.nes
```

On Windows, run `build\\Debug\\nescyan.exe path\\to\\game.nes` (or the corresponding configuration directory). SDL2 must be available to CMake.

## Controls

| NES button | Keyboard |
| --- | --- |
| A / B | Z / X |
| Select / Start | Right Shift / Return |
| Up / Down / Left / Right | Arrow keys |
| Quit | Escape or close the window |

Use legally obtained, unmodified `.nes` ROM files. The emulator does not include games or BIOS files.

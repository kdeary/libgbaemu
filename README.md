# libgbaemu - A Headless GBA Emulator Core

This repository contains a standalone, headless Game Boy Advance emulator core packaged as a portable C library. It is intended to be embedded inside custom frontends (desktop, firmware, microcontroller targets) without pulling in platform I/O stacks or filesystem calls.

The implementation is derived from the excellent [Hades emulator](https://github.com/hades-emu/Hades) and keeps its high-accuracy execution model while adapting the memory layout to run within tight RAM budgets and demand-paged environments.

## Purpose
- Emulate the ARM7TDMI CPU, PPU, APU, DMA, timers, and cartridge peripherals of the GBA.
- Expose a clean C API (`include/gba/`) for frontends to feed ROM/BIOS buffers, drive input, and consume framebuffer/audio samples.
- Stay storage-agnostic: the host provides ROM/backup storage buffers, and the core never touches the filesystem directly.

## Usage
1. **Build the static library**
   ```sh
   make
   ```
   This produces `build/libgba.a`, which you can link into your platform-specific frontend or firmware.

2. **Integrate with a host**
   - Call `gba_create()` to allocate the core, then `gba_run()` on a worker thread.
   - Fill a `struct launch_config` with pointers to your BIOS/ROM data and runtime settings, then send a `MESSAGE_RESET` (see `include/gba/event.h`) so the core picks up the new game.
   - Drive inputs by pushing `MESSAGE_KEY` events, and consume video/audio by registering a scanline callback and using the shared APU ring buffer.

3. **Platform notes**
   - The core assumes the ROM buffer remains valid for the lifetime of the instance; on paged systems you can point it at memory-mapped views or demand-loaded chunks.
   - Backup storage changes are surfaced through `shared_data.backup_storage`; persist them using your own storage backend when `dirty` becomes `true`.

Refer to:
- `ports/sdl/` for a minimal desktop frontend that demonstrates message passing, rendering, and input plumbing.
- `ports/headless/` for a CLI-only frontend that runs the core and prints live frame/FPS counters using line-replaced console output.

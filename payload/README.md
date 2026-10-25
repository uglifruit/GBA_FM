# GBA payload

The GBA-side program uploaded into a cartridge-less Game Boy Advance via BIOS Multiboot.
It is a **2-operator FM synth voice with its own editor**, dependency-free (no libgba, no libc) so it
builds with a bare `arm-none-eabi` toolchain targeting the GBA's ARM7TDMI (armv4t).

## What it does

- Paints the screen **green in assembly**, in `crt0.s`, before the C runtime exists. That is the
  boot proof: white means the image never ran, green means it ran and the C startup died, and the
  play screen means a normal boot. Three states that were previously one identical white screen.
- Acts as a **normal-mode 32-bit serial slave** (external clock from the RP2040), re-armed by
  the **serial interrupt** so the console is not deaf while it redraws.
- Owns the instrument: pitch tracking, the modulation matrix, the buttons, drum mode, and
  **four fixed-point 2-operator FM voice slots** (at most three sounding) rendered in IWRAM and
  played in stereo through **Direct Sound A and B** (Timer 0 clocks both FIFOs, DMA1/DMA2 refill
  them from two 256-byte rings, and DMA2's interrupt renders each 16-sample block as the DMAs free
  it). See `audio.h` and `fm.c`.
- Draws a performance readout and an eight-page editor (MEM FM ENV MIX MAP BTN SET CAL).
- Carries 26 factory sounds (`presets.c`).
- Holds the patch, which the Workshop stores for it in flash — sixteen slots, transferred a byte
  per link word.
- Boots into a pluck on Voice 1 and a bass on Voice 2 (a kick and a snare in the drum slots),
  playable from the module's switch with nothing patched. See `synth_default_patch()`.

## Files

| File            | Purpose |
|-----------------|---------|
| `main.c`        | Init and the main loop |
| `link.c/.h`     | Serial slave, serial IRQ half, protocol decode |
| `irq.c/.h`      | The one BIOS IRQ handler: audio DMA first, serial second |
| `audio.c/.h`    | Direct Sound A setup, the ring, the DMA interrupt (IWRAM) |
| `fm.c/.h`       | FM renderer: voice slots, envelopes, pitch envelope, noise, stereo mix (IWRAM) |
| `presets.c/.h`  | The factory sounds |
| `synth.c/.h`    | Control layer: the matrix, buttons, drum mode, pitch, the patch |
| `ui.c/.h`       | Performance screen + eight-page editor |
| `gba.h`         | Register macros and the IWRAM section attributes |
| `gfx.c/.h`      | Mode-3 drawing and text |
| `diag.c/.h`     | Link-characterisation screens (used by `linkrate`/`bandwidth`) |
| `fm_tables.h`   | **Generated** by `gen_fm_tables.py`: sine, note increments, ratios, envelopes |
| `font5x7.h`     | 5×7 ASCII font |
| `crt0.s`        | GBA ROM header + multiboot entry (`_start` sits at offset `0xC0`) |
| `multiboot.ld`  | Links the image for EWRAM (`0x02000000`), with `.iwram` loaded inside it |
| `build.sh`      | Compile every `.c` → link → `.mb` → logo/checksum → `../gba_payload.h` |
| `bin2h.py`      | Converts `payload.mb` to the C byte array |

## Two constraints that will bite you

**No runtime division.** We link `-nostdlib`, so libgcc is absent: dividing by a *variable*
is an undefined `__aeabi_uidiv` / `__aeabi_idivmod` at link time, not merely slow code. That
is a deliberate tripwire and it has already caught a `% rows` in the editor. Divides by
compile-time constants are fine — GCC turns those into multiply-and-shift. `synth.c` has a
shift-subtract `udiv32` for the one place that genuinely needs it.

**The IRQ stack is small.** The BIOS calls the handler in IRQ mode on the IRQ stack at
`0x03007FA0`. `crt0.s` therefore puts the user stack at `0x03007C00` rather than the usual
`0x03007F00`, giving the handler 928 bytes instead of 160. It carries the whole audio render as well
as the serial harvest, so nothing the DMA2 interrupt calls may have large locals (the mix buffers
are static for this reason). Overrunning it is silent: it corrupts the main loop's stack.

**IWRAM code must be copied before it runs.** Multiboot only loads EWRAM. Anything marked
`IWRAM_CODE` / `IWRAM_RODATA` / `IWRAM_BSS` (see `gba.h`) is linked at `0x03000000` and copied or
zeroed by `crt0.s` before `main()`. A helper called from IWRAM code must itself be in IWRAM or be
`always_inline`, or the interrupt quietly calls out to slow EWRAM.

**Don't build any of this at `-Os`.** At `-Os` GCC calls `__aeabi_idiv` even for constant
divisors, so the `-nostdlib` tripwire fails the link. (Tried on `ui.c` to cut upload time: it saved
only ~4 KB anyway.)

**`0xC0` is a BRANCH, not code.** GBATEK: the BIOS overwrites bytes `0xC4` (boot mode) and `0xC5`
(slave ID) of the loaded image. Put instructions there and it corrupts them after loading, every
time — see the long note at the top of `crt0.s` for how that cost a white screen.

**No runtime division, and no implicit `memcpy` either.** Both are link errors here. A struct
assignment compiles to `memcpy`; copy byte by byte.

## Building

```sh
cd payload
./build.sh
```

This overwrites `../gba_payload.h` with the compiled payload, then rebuild the RP2040
firmware to bake it in.

### Toolchain

`build.sh` needs two things:

1. **A compiler** — either devkitARM, or any `arm-none-eabi-gcc` on `PATH`. The Raspberry
   Pi Pico SDK already ships one (`~/.pico-sdk/toolchain/*/bin/arm-none-eabi-gcc`) which
   compiles the payload fine when invoked with `-mcpu=arm7tdmi -marm -mthumb-interwork`.

2. **A gbafix** — inserts the 156-byte Nintendo logo and fixes the header complement. **The
   GBA BIOS compares the logo against its own copy and locks up if it's wrong**, so this
   step is mandatory for real hardware. `build.sh` uses real `gbafix` (devkitARM, via
   `DEVKITARM`/`DEVKITPRO` or `PATH`) if present, otherwise the **bundled, dependency-free
   [`gbafix.py`](gbafix.py)** — a faithful reimplementation of the logo + complement fix, so
   no devkitPro install is needed.

The committed [`../gba_payload.h`](../gba_payload.h) is already a real, bootable image
(valid logo, passing header complement). Re-run `build.sh` only after editing the payload.

## Serial protocol

Defined once in [`../gba_proto.h`](../gba_proto.h) and included by both this payload (C) and
the RP2040 firmware (C++), so the two sides cannot drift. See the comments there; the host
side is `../gba_link.cpp`.

32-bit words, MSB-first, mode 3, RP2040 is the clock master.

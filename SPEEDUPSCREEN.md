# Speeding up the GBA editor screen — handoff

**For:** whoever applies this to a sibling GBA payload, first of all **card 108, GBA PSG Voice**
(`uglifruit/89_GBA`, local `C:\Users\andyu\Documents\WorkshopGBA`). Written from GBA FM Voice
(`uglifruit/GBA_FM`), where it was done and measured on 2026-10-25/26. Working notes, like
CLAUDE.md: **not for a release.**

## The short version

The editor was slow because **the main loop was starved**, not because pixels were slow to fill.
Everything in a multiboot image runs from EWRAM: a 16-bit bus with two wait states, so each 32-bit
ARM instruction costs about **6 cycles** to fetch, against **1** from IWRAM. Two things paid that
price thousands of times a second:

1. **The drawing primitives**: `rect()` wrote one pixel per loop iteration, `glyph()` called
   `rect()` for every lit pixel, and `line()` called `rect(x, y, 1, 1)` per point.
2. **The 1 kHz control tick** (`synth_tick` / `synth_update`). `gfx_idle()` runs owed ticks after
   every character drawn, so every character paid for a tick.

The fix was to move both into IWRAM, make the fills word-wide, and stop the tick doing work for
voices that aren't playing. **Measure before and after:** a TICK LOAD row on the CAL page.

| GBA FM Voice | Before | After |
|---|---|---|
| Editor | painted visibly, row by row | "FAR better" (Andy) |
| TICK LOAD (wall time in the tick ÷ its 1 ms period) | most of each ms (~13K of 16.8K cycles, by instruction count) | ~40 % average, peak under 85 % |

**What 108 has (checked 2026-10-26):**
- **Nothing in IWRAM.** The linker script has no `.iwram` section, and `crt0.s` has no copy loop.
- **Pixel-at-a-time primitives:** `rect` per pixel, `glyph` → `rect`, `line` → `rect`.
- **A 7 KB `synth_update`** (the tick inlined) in EWRAM.
- **No audio interrupt** (the PSG makes its own sound), so the tick and the screen share the whole
  CPU.

Expect the primitives to be 108's bigger win, and the tick a real one too. Measure to find out.

## What NOT to do (considered and rejected)

Generic GBA advice suggests Mode 4 with page flipping, DMA3 copies from a WRAM canvas, or tile-mode
(Mode 0) text. None of it fits:

- **They fix fill rate,** and fill rate was never the bottleneck once the primitives were word-wide
  and in IWRAM. A one-row value change costs about 1 ms.
- **Page flipping fights the editor's design.** The UI redraws only what changed, with per-region
  caches (CLAUDE.md, "Payload UI rules"). With two pages, every change has to reach both pages.
- **Tile text needs an 8-pixel character grid.** The 5×7 font advances 6 pixels, so every page
  layout would change.
- **The undocumented EWRAM wait-state register** (`0x04000800`, 1 wait state instead of 2) would
  speed up all EWRAM code. Leave it alone: its behaviour across models, the GBA Micro especially,
  couldn't be confirmed.
- **Don't build the UI as Thumb or `-Os` to save fetch cycles.** Both make GCC call `__aeabi_idiv`
  even for constant divisors, which is a link error under `-nostdlib`. Don't "fix" that by linking
  libgcc: that tripwire is what keeps runtime division out.

## Step 0 — add the meter first

Without a number, "it feels faster" is all you have. Timer 3 is free in 108 (only Timer 0 is used,
as the control clock) and in FM (where `audio.c` starts it for DSP LOAD). Run it free at the CPU
clock:

```c
#define REG_TM3CNT_L (*(volatile uint16_t *)(0x04000000 + 0x010C))
#define REG_TM3CNT_H (*(volatile uint16_t *)(0x04000000 + 0x010E))
// at init:  REG_TM3CNT_L = 0; REG_TM3CNT_H = 0x0080;   // enable, prescaler 1 (16.78 MHz)
```

Time each tick inside the catch-up loop of `synth_update()`. The period is 16 control-timer ticks
of 1024 cycles = 16384 cycles, so percent ×16 is `cycles × 1600 / 16384 = cycles × 25 >> 8`. That's
a constant shift, with no division:

```c
volatile uint16_t g_tickAvg = 0, g_tickPeak = 0;      // percent x 16
static uint32_t   s_tickAcc = 0;
...
    while ((uint16_t)(now - last) >= 16 && budget--) {
        last = (uint16_t)(last + 16);
        uint16_t t0 = REG_TM3CNT_L;
        synth_tick();
        uint32_t pct = ((uint32_t)(uint16_t)(REG_TM3CNT_L - t0) * 25u) >> 8;
        s_tickAcc += pct - (s_tickAcc >> 6);           // one-pole, kept x64 so it can move
        g_tickAvg = (uint16_t)(s_tickAcc >> 6);
        if (pct > g_tickPeak) g_tickPeak = (uint16_t)pct;
    }
```

**How to read it:**
- **Display:** add a CAL row showing `AVG% PK PEAK%` (FM's `pct_at()` in `ui.c` formats percent ×16
  as `7.5%`).
- **It's wall time.** Any interrupt that lands inside the tick is counted, and that's deliberate:
  it's exactly the time the screen doesn't get.
- **Wrap-around:** the 16-bit timer wraps every 3.9 ms, which is fine for anything shorter.
- **Build a "before" copy.** Make one UF2 with only the meter, so Andy can compare on hardware.

## Step 1 — IWRAM plumbing (108 has none)

Multiboot loads only into EWRAM, so IWRAM code must be **linked** at `0x03000000`, **loaded**
inside the image, and **copied** by `crt0.s` before anything calls it. Copy these from FM.

**`multiboot.ld`**: add after `.data`, before `.bss`:

```ld
    .iwram : {
        __iwram_start = .;
        *(.iwram .iwram.*)
        *(.iwram_rodata .iwram_rodata.*)
        . = ALIGN(4);
        __iwram_end = .;
    } > IWRAM AT > EWRAM
    __iwram_lma = LOADADDR(.iwram);

    .iwram_bss (NOLOAD) : {
        __iwram_bss_start = .;
        *(.iwram_bss .iwram_bss.*)
        . = ALIGN(4);
        __iwram_bss_end = .;
    } > IWRAM
```

**`crt0.s`**: right after `ldr sp, ...` and before the `.bss` zeroing. Nothing in IWRAM may be
called before this point.

```asm
    ldr r0, =__iwram_lma
    ldr r1, =__iwram_start
    ldr r2, =__iwram_end
3:  cmp r1, r2
    ldrlo r3, [r0], #4
    strlo r3, [r1], #4
    blo 3b

    ldr r0, =__iwram_bss_start
    ldr r1, =__iwram_bss_end
    mov r2, #0
4:  cmp r0, r1
    strlo r2, [r0], #4
    blo 4b
```

Mind the existing local labels: 108 already uses `2:` for the `.bss` loop.

**A small `gba.h`** (FM's verbatim):

```c
#define IWRAM_CODE   __attribute__((section(".iwram"), noinline))
#define IWRAM_RODATA __attribute__((section(".iwram_rodata")))
#define IWRAM_BSS    __attribute__((section(".iwram_bss")))
```

**Why it's written this way:**
- **Three sections,** because GCC refuses to put code, read-only data and writable data in one named
  section ("section type conflict").
- **`noinline` on `IWRAM_CODE`,** because an IWRAM function inlined into an EWRAM caller quietly runs
  from EWRAM.
- **No `long_call` needed.** A plain `BL` reaches IWRAM from EWRAM and back: they are 16 MB apart and
  `BL` spans ±32 MB.

**Stacks:** 108 keeps its user stack at `0x03007E00`, which leaves its serial-only IRQ 416 bytes
(FM moved its stack to `0x03007C00` because its audio interrupt is deep). IWRAM code grows up from
`0x03000000`; FM uses about 18 KB, so there's room.

## Step 2 — the drawing primitives

All four go into IWRAM. These are FM's `gfx.c` versions, which drop straight in, since 108's
`gfx.h`, font and colours are the same:

```c
// Two pixels per 32-bit store, eight stores per pass. Keep the pointers VOLATILE: a plain fill
// loop is a memset idiom, and a GCC-emitted call to memset is a -nostdlib link error.
IWRAM_CODE void rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w <= 0 || h <= 0) return;

    uint32_t c2 = (uint32_t)color | ((uint32_t)color << 16);
    volatile uint16_t *row = VRAM + y * SCREEN_W + x;
    for (int j = 0; j < h; j++, row += SCREEN_W) {
        volatile uint16_t *p = row;
        int n = w;
        if (((uint32_t)p & 2u) != 0) { *p++ = color; n--; }       // to a word boundary
        volatile uint32_t *q = (volatile uint32_t *)p;
        while (n >= 16) {
            q[0] = c2; q[1] = c2; q[2] = c2; q[3] = c2;
            q[4] = c2; q[5] = c2; q[6] = c2; q[7] = c2;
            q += 8;
            n -= 16;
        }
        while (n >= 2) { *q++ = c2; n -= 2; }
        if (n) *(volatile uint16_t *)q = color;
    }
}

IWRAM_CODE void line(int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx =  (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int sx =  (x0 < x1) ? 1 : -1;
    int sy =  (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if ((unsigned)x0 < SCREEN_W && (unsigned)y0 < SCREEN_H) VRAM[y0 * SCREEN_W + x0] = color;
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Scale 1 (nearly all text) writes VRAM directly; larger scales and clipped glyphs keep the old
// rect()-per-pixel path. Mask the column to FONT_H bits: a stray high bit in the font data would
// otherwise draw a pixel below the glyph.
IWRAM_CODE void glyph(int x, int y, char ch, uint16_t color, int scale)
{
    if ((unsigned char)ch < FONT_FIRST || (unsigned char)ch > FONT_LAST) ch = '?';
    const uint8_t *g = font5x7[(unsigned char)ch - FONT_FIRST];
    if (scale == 1 && x >= 0 && y >= 0 && y + FONT_H <= SCREEN_H) {
        volatile uint16_t *base = VRAM + y * SCREEN_W + x;
        for (int c = 0; c < FONT_W && x + c < SCREEN_W; c++) {
            uint32_t col = g[c] & ((1u << FONT_H) - 1u);
            volatile uint16_t *p = base + c;
            for (; col; col >>= 1, p += SCREEN_W)
                if (col & 1u) *p = color;
        }
        return;
    }
    for (int c = 0; c < FONT_W; c++) {
        uint8_t col = g[c];
        for (int r = 0; r < FONT_H; r++)
            if (col & (1u << r)) rect(x + c * scale, y + r * scale, scale, scale, color);
    }
}

IWRAM_CODE void text(int x, int y, const char *s, uint16_t color, int scale)
{
    for (int i = 0; s[i]; i++) {
        glyph(x + i * FONT_ADV * scale, y, s[i], color, scale);
        gfx_idle();              // stays in EWRAM; keeps the link and the tick serviced
    }
}
```

`font5x7` itself can stay in EWRAM (a few byte loads per glyph). Keep `rect_clear()` banding
large clears with `gfx_idle()` between bands (CLAUDE.md UI rule 7). Faster fills don't remove
that need.

## Step 3 — the control tick

- **Mark the tick and every function it calls every tick `IWRAM_CODE`.** In 108 that is
  `synth_tick` (GCC has inlined it into `synth_update`, 7 KB) and its pitch, quantiser, button-scan
  and PSG-write helpers.
- **Small helpers** (`clampi`, `stepped`, calibration getters): make them
  `static inline __attribute__((always_inline))`. A plain `static inline` may be emitted out of line
  in EWRAM, and the IWRAM tick then calls out to it.
- **Leave rare calls in EWRAM** (preset loads, `link_post_note`). Calling them is legal, just slow,
  and they're rare.
- **Hot data:** put what the tick reads and writes every millisecond in `IWRAM_BSS`. In FM that
  was `g_patch` (178 bytes) and the voice control blocks shared with the audio interrupt. They are
  bss, so crt0 zeroes them. Const tables can stay in EWRAM, or go `IWRAM_RODATA` if the meter says
  it's worth it.
- **Skip work for things that aren't playing.** FM stopped recomputing a voice slot once it is both
  inactive and silent. The rule:
  - Refresh a slot if it's active now, **or** was active on the last tick (so the switch-off tick
    still writes its gate low), **or** its envelope is still non-zero (so it releases rather than
    cuts).
  - The tick it becomes active again recomputes it, together with its note-on.
  - Keep per-voice glide state running regardless.
  - 108's equivalent is any per-channel work done for PSG channels the current mode doesn't use.

## Step 4 — verify

- **Build:** `payload/build.sh` and the firmware build come out clean. Undefined `memset`, `memcpy`
  or `__aeabi_*` means a construct the tripwire caught: fix the construct.
- **Placement:**
  - `arm-none-eabi-nm -n build/payload.elf | grep ^0300` lists the moved functions and data.
  - `__iwram_bss_end` is well below the user stack.
- **No hidden EWRAM calls:** list the call targets of the IWRAM functions. Every `bl 02xxxxxx` should
  be a call you meant to leave in EWRAM:
  ```sh
  arm-none-eabi-objdump -d -j .iwram build/payload.elf | grep -E "\sbl\s" | grep " 2[0-9a-f]{6} <"
  ```
- **Stack:** compile the moved files with `-fstack-usage`. The tick runs on the user stack, not the
  IRQ stack, but don't let large locals creep into anything an interrupt calls (FM's CLAUDE.md,
  "GBA-side gotchas" 5).
- **Hardware:** compare TICK LOAD before and after, and page changes, scrolling and grids before and
  after. Notes, glide, gates and patch save/load should be unchanged.

## Where FM's version lives

- `payload/gfx.c` (primitives) and `payload/gba.h` (macros).
- `payload/synth.c`:
  - `synth_tick` and its helpers;
  - the `live` mask in the voices section;
  - the meter in `synth_update`.
- `payload/ui.c`: the CAL rows DSP LOAD and TICK LOAD.
- `payload/multiboot.ld`, `payload/crt0.s`.
- CLAUDE.md, "Drawing speed", has the reasoning in brief.

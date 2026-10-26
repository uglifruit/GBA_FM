# GBA FM Voice — Claude working notes

GBA Multiboot link for the Music Thing Modular **Workshop Computer** (RP2040-based Eurorack
module). Boots a cartridge-less Game Boy Advance over the pulse jacks via BIOS Multiboot, then
runs a live SPI link so the GBA is a **2-operator FM synth voice played by the modular**, rendered
in fixed point and played through Direct Sound, with the sound editor on the GBA's own screen.

Read this before editing. It records the non-obvious facts that make this applet work — most
would take a full re-investigation to rediscover.

## What this is

- **A sibling of card 108, "GBA PSG Voice"** (repo `uglifruit/89_GBA`, local clone
  `C:\Users\andyu\Documents\WorkshopGBA`). This tree started as a copy of it on 2026-10-24.
  **Multiboot, the serial layer, the protocol and the diagnostics are 108's. The firmware and
  protocol differ from 108 in ONE addition** (phase 2, 2026-10-24): `main.cpp` counts Audio In 1/2
  edges at 48 kHz and `gba_link.cpp` sends them as `GBA_OP_AUDEDGE` (0x09), announced by
  `GBA_CAP_AUD_EDGES` in HELLO — see "Audio-input edges" below. 108's payload ignores unknown
  opcodes, so the addition is harmless to it. Otherwise a fix to any shared layer belongs in BOTH
  trees. 108's own CLAUDE.md is the history of how those layers were made to work; the parts of it
  that only concern the PSG are not repeated here.
- **What it is now (phase 2):** two FM voices (V1 on CV2/PU2, V2 on AU2/AU1), a drum mode that
  turns AU1/AU2 into two drum triggers, stereo (FIFO A left, FIFO B right), 26 presets, and an
  eight-page editor (MEM FM ENV MIX MAP BTN SET CAL) with 108's MAP matrix and BTN actions back.
- **Not yet tested on hardware** (as of 2026-10-24). The FM maths has been checked in a bit-exact
  Python model (sidebands against Bessel values, envelope times, overflow in every voice, the noise
  blend, the pitch envelope and the mix) — see TECHNICAL.md, "The audio engine". The model is not
  in the repo; it lived in a session scratchpad. The first hardware session should settle the open
  risk below.
- **NEVER COPY A `build/` DIRECTORY BETWEEN TREES.** The initial copy from 108 carried
  `diagnostics/build/` with a CMake cache and ninja files holding 108's ABSOLUTE paths, so building
  it here compiled 108's sources and wrote 108's diagnostics outputs. It was deleted and
  reconfigured in place on 2026-10-24; if a build dir ever looks inherited, delete it.
- **Repo: `uglifruit/GBA_FM`** (public, created 2026-10-24). No card number yet. `info.yaml` says
  `draft: true`, `Status: Beta`. Run the monorepo validator before any release — the `Status`
  value in particular is unvalidated.
- Keep it self-contained (vendored `ComputerCard.h`, no external path deps) so copy-in is clean.
- **THIS FILE DOES NOT GO INTO A RELEASE.** It is working notes addressed to an assistant.
- **The built firmware goes in as `UF2/gba_link.uf2`**, rebuilt and re-copied whenever the payload
  or the firmware changes — a stale UF2 there is worse than none, because it looks current.
- `ComputerCard.h` is Andy's improved copy of the HAL, not stock — see below.
- `README.md` stays at user-manual level; depth belongs in `TECHNICAL.md`. 108's user manual PDF
  and screenshots describe the PSG voice and were deliberately NOT copied. `docs/images/` holds
  only the three generic drawings shared with 108 (hero, link-port pinout, consoles); there are
  no FM screenshots yet.

## OPEN RISK: confirm both sound DMAs raise their IRQs after EVERY FIFO block

The audio design (below) restarts DMA1 and DMA2 from their own interrupts and renders from DMA2's,
which relies on each DMA raising its IRQ (9, 10) at the end of every four-word FIFO transfer (word
count reached, repeat mode), and on DMA2 always finishing its block after DMA1 (DMA1 has priority).
That is how GBATEK describes it and how mGBA models it, but it has not been seen on this console.
**Test it first**, with a held note (B on the PLAY screen), on headphones so both channels are
heard: a periodic click at 32768/256 = 128 Hz in either ear, or silence, means it is not behaving
as assumed. The fallback is Timer 1 in count-up (cascade)
mode from Timer 0, reload `0xFFF0`, IRQ every 16 samples — but that is a PLAYBACK clock, not a
fetch clock, so the restart's phase against the FIFO's read-ahead then has to be found by
experiment.

## Andy's ComputerCard.h fixes (use this copy, don't regress to stock)

The vendored HAL here is a **pure superset of upstream v0.3.0** (+18 lines, nothing removed),
carrying two fixes Andy developed across his projects. The canonical copy lives in
`91_Chorgan` / `95_OffAir` (byte-identical); this one matches them. `96_Cathode` / `60_Markov`
still carry **stock** v0.3.0 — do not copy their HAL over this one.

1. **Power-on click removal.** Pre-fills the SPI DAC buffers with 0V "silence" words before
   the first DMA transfer, so the first output is silence rather than uninitialised RAM.
   Applies to any applet using audio/CV out (this one does).
2. **ADC channel-alignment fix.** Stops the ADC (`ADC_CS_START_MANY`) and drains the FIFO
   before touching AINSEL, guaranteeing the DMA burst always restarts on channel 0. Without
   it, an in-progress conversion lands as `ADC_Buffer[n][0]` and shifts the whole burst by
   1–3 slots (knobs/CV read from the wrong channels). Applies to any applet reading ADC.

When starting a **new** Workshop applet, seed `ComputerCard.h` from `95_OffAir`/`91_Chorgan`
(or this repo), not from a stock-HAL project. These fixes are Andy's preferred baseline.

## Hardware facts you must not forget

These are the ones that silently break things if ignored:

1. **Sample callback is 48 kHz.** Applets subclass `ComputerCard` and override
   `ProcessSample()`, called once per audio sample (~20.8 µs budget). Not configurable. Use
   integer/fixed-point math; float is soft-emulated and slow. This is the base rate for
   everything (timers, etc.).
2. **All four pulse jacks are inverted in hardware.** The HAL hides this
   (`PulseOut1(true)` → GPIO low; `PulseIn1()` = `!gpio_get`). But the moment you drive the
   pins **directly** (PIO/bitbang), you own the inversion. Here it's absorbed by IO-pad
   overrides in `gba_multiboot.cpp` (`gpio_set_outover`/`gpio_set_inover`) — see the net-
   inversion derivation in `gba_spi.pio`. Getting this wrong = "nothing works".
3. **Pin map** (RP2040 GPIO): Pulse Out 1 = **8**, Pulse Out 2 = **9**, Pulse In 1 = **2**,
   Pulse In 2 = **3**. LEDs 10–15. Audio DAC on SPI0 (18/19/21) — **do not touch SPI0**.
4. **Pulse In 1 (GPIO 2) is a slow transistor gate input**, not a clean logic pin, and it
   **INVERTS** — `GBA_MISO_INOVER = GPIO_OVERRIDE_INVERT`. This was the single most expensive
   bug in the project: a scope check that appeared to prove otherwise had both probes on the
   same net, either side of a patch cable. The GBA settled it by replying `0x8DFD9DFD`, the
   exact bit-complement of the expected `0x72026202`.
   Its **bandwidth was never the problem** — that long-flagged risk is closed. It carries
   2000 words/s cleanly (~64 kbit/s). What limits the link is the **GBA slave's re-arm gap**,
   not this input.
5. **EVERY transfer needs an inter-word gap.** The GBA slave holds exactly ONE pending
   transfer and re-arms with a read-modify-write of `SIOCNT`; a word clocked before it has
   re-armed is not merely lost, it is corrupted (bit-slip). At 100 kHz SCK a 32-bit word takes
   320 µs and ~180 µs of gap is ample; zero gap fails completely. **This applies to every
   message, including out-of-band ones** — a rate-announce word sent without a gap silently
   destroyed the word after it and produced a fixed error count that looked like a hardware
   floor.
6. **The link cable CROSSES SO/SI.** Measured, not assumed — `cablecheck.uf2` listens on Pulse
   In 1 and Pulse In 2 at once and reports which wire actually carries the GBA's SO. Sources
   genuinely disagree about GBA cable wiring; do not reason about it, measure it.
7. **The HAL samples pulse inputs only at 48 kHz.** For the SPI link we read GPIO 2 via PIO,
   never `PulseIn1()`.

## The division of labour (the decision everything else follows from)

**The Workshop senses. The GBA is the instrument.** The Workshop reads its five inputs and
streams them down raw; the GBA owns pitch tracking, the modulation matrix, the envelope, the
sound and the whole UI. It sends back only what the rack needs: buttons, note, gate, status.

This is why the on-screen editor costs no protocol. Re-mapping Audio In 1 from detune to
vibrato is a change to a table in GBA RAM — and why giving the switch's UP position a meaning
cost one line in `synth.c` rather than an opcode: its position was already arriving every poll,
it just had nowhere to go. Had the mapping lived on the Workshop side, every
edit page would have needed its own downstream opcode. **Do not move musical decisions to the
RP2040** — it is not where they belong and it makes the protocol grow without limit.

## Architecture (two cores)

- **Core 0**: ComputerCard's 48 kHz loop (`main.cpp` `ProcessSample`). Runs inside
  `DMA_IRQ_0`. Never block it.
- **Core 1**: the **entire GBA link engine** (`gba_link_core1`) — runs multiboot, then the
  post-boot polling loop. Owns **PIO0 SM0** and reassigns GPIO 8/9/2 to PIO on entry.
  ComputerCard leaves both PIO blocks, SPI1, `DMA_IRQ_1`, and high DMA channels free.
- Cores communicate **only** through the lock-free `GbaShared gGba` struct (`gba_link.h`):
  each field single-writer, `volatile`, ≤32-bit so writes are atomic on RP2040. Core 1
  publishes `buttons`; core 0 writes `params[]`. This mirrors 96_cathode's core-split.
- Reference precedent for pulse-pin PIO + DMA on core 1 is **`releases/96_cathode`** in the
  monorepo (composite-video applet). It's the template for this pattern.

## Protocol v1 (`gba_proto.h` — ONE header, both sides)

C-compatible (payload is C, firmware is C++): plain `#define` and `static inline` only.

- **Downstream** tag in bits [31:29]: `0b100` STREAM (gate + edge + one 12-bit input pair +
  2 check bits), `0b000` CONTROL (`[28:24]` opcode, `[23:0]` arg), **`0b101` RESERVED**.
- **`0b101` is fenced off deliberately.** The three diagnostic magics (`0xA5A5`, `0xA6A6`,
  `0xBE7C`) all begin `101`. Giving STREAM `100` makes collision *structurally impossible*.
  Under a weaker tag a live stream word would impersonate the ramp magic about every 30 s at
  1 kHz and throw the instrument into link-test mode mid-performance.
- **Upstream: the 16-bit tag IS the kind.** `0x600D` keeps its exact old meaning (buttons), so
  every existing diagnostic still parses replies unchanged. `0x601E` status, `0x602A` note,
  `0x6033` param. Low bytes form a distance-3 code, so one flipped bit cannot turn one kind
  into another.
- **Gate edges are a COUNTER, not a sticky flag, on both sides.** A flag set by one context and
  cleared by another has two writers, and the losing interleaving is exactly the one that
  matters — an edge arriving between the read and the clear vanishes. Each side keeps its own
  tally and advances by one per edge, so a sub-millisecond trigger is never lost or merged.
- **CONTROL WORDS HAVE NO CHECK BITS — `decode_applet`'s CONTROL case dispatches on the opcode
  with zero validation, unlike STREAM's 2-bit parity.** An edge-only CONTROL message (sent once,
  on change, and never repeated) is one bad word away from a state that is wrong FOR EVER: noise
  from moving the module can flip a word's top three bits to `000` and its opcode bits to a real
  one by pure chance, and the GBA has no way to reject it. This bit SWITCH once: it was sent only
  when the switch actually moved, so a corrupted "SW down" looked exactly like a held switch and
  stayed a stuck trigger source until the next real switch movement corrected it — sometimes
  read as "the switch is broken," when it was a single mis-decoded word. `gba_link.cpp` now
  re-announces SWITCH every ~64 words (~64 ms) even when unchanged, the same self-healing pattern
  HELLO (every ~2 s, so a missed word doesn't leave the CAL page's calibration flag wrong for
  ever) and SLOTS (every ~0.5 s) already used for the identical reason. **Any new edge-only
  CONTROL opcode needs the same periodic re-announce, or it inherits this exact failure mode.**

## GBA-side gotchas that cost silence, not errors

1. **`SOUNDCNT_X` bit 7 (master enable) must be set BEFORE any other sound register write.**
   With the master off the sound registers are not writable at all, and clearing bit 7 resets
   them. `audio_init()` does this first. (Inherited from 108, where it cost silence.)
2. **Timer 0 is the SAMPLE CLOCK** for both FIFOs. Direct Sound can only be paced by Timer 0 or 1. 108 used
   Timer 0 as its free-running control clock; that clock moved to **Timer 2** (same `0x0083`, same
   bounded catch-up in `synth_update`). Timer 3 is the DSP load meter. Timer 1 is free — and is the
   fallback for the open risk above. Reusing Timer 0 for anything else silently retunes the voice.
3. **Anything that runs per sample must be in IWRAM, and IWRAM code must be COPIED there.**
   Multiboot only loads EWRAM. `multiboot.ld` links `.iwram` at `0x03000000` with its load address
   inside the image, and `crt0.s` copies it and zeroes `.iwram_bss` before `.bss`. Mark code
   `IWRAM_CODE`, const tables `IWRAM_RODATA`, zeroed state `IWRAM_BSS` (`gba.h`) — three sections,
   because GCC refuses to mix code, rodata and data in one named section. **A helper called from
   IWRAM code must be IWRAM_CODE itself or `always_inline`**: plain `static inline` can be left
   un-inlined in `.text`, and the interrupt then calls out to EWRAM. Check with
   `arm-none-eabi-nm -n build/payload.elf | grep ^0300`.
4. **No runtime division, and no implicit `memcpy`.** The payload links `-nostdlib`, so a divide by
   a *variable* is an undefined `__aeabi_uidiv` at LINK time — a deliberate tripwire. Divides by
   constants are fine; `synth.c` carries `udiv32` for the one place that needs it. 64-bit
   *multiplies* (`(uint64_t)a * b`, `(int64_t)a * b`) compile to `UMULL`/`SMULL` inline and are
   fine; 64-bit divides are not.
5. **The user stack sits at `0x03007C00`; the IRQ stack is everything between it and
   `0x03007FA0` (928 bytes) — and overrunning it is SILENT.** It writes over the top of the main
   loop's stack and the editor freezes, with nothing pointing at the audio. That is exactly what
   happened when 4-op mode came in (2026-10-25): with the mixer's buffers as locals the IRQ path
   needed ~576 bytes against 416 (108's `0x03007E00`), and switching VOICE MODE on SET left a blank
   screen. **No large locals in anything the DMA2 interrupt calls** — `fm_render_block`'s buffers
   and `voice4_block`'s arrays are static IWRAM. Measure after any change to the audio path:
   compile `fm.c` with `-fstack-usage` and add up BIOS 24 + `irq_dispatch` + `audio_on_dma2_irq` +
   `fm_render_block` + `voice4_block` + `render_4op` (352 bytes today). IWRAM code and data grow
   up from `0x03000000` (~17.9 KB today, with the control tick), well clear of the stacks.
6. **The serial IRQ does NOT replace the polled path.** `link_service()` still runs the same body
   with interrupts masked, so the two are mutually exclusive rather than racing. If the IRQ never
   fires the payload degrades to the polled behaviour. The CAL page shows which is live.
7. **Zero is silence on Direct Sound.** Samples are signed, so 0 is the DAC's centre and the zeroed
   rings are true silence. Every one of 108's "levels that could not reach zero" was a PSG DAC quirk
   (digital zero at a rail, coarse wave volume); none of them applies here. Don't port the
   workarounds.

## The audio engine (the decisions, and why)

- **32768 Hz**, Timer 0 reload `0xFE00` (`0x10000 - 16777216/rate`; `0xFFFF - ...` is one cycle
  short). It is the native PWM rate at `SOUNDBIAS = 0x0200`, and FM's sidebands want the Nyquist.
- **Two rings (left → FIFO A by DMA1, right → FIFO B by DMA2) of 16 blocks x 16 samples,
  refilled ONE DMA BLOCK AT A TIME FROM THE DMAs' OWN INTERRUPTS.** Each DMA's handler restarts it
  at its ring start if it just fetched the last block — in FIFO mode the source never wraps, it
  walks off the end. DMA1's handler does only that. **DMA2's handler then renders** 16 samples into
  the block both rings just gave up: DMA2 is the LOWER priority, so when its block is done DMA1's
  already was. Render on DMA1 instead and the right ring's block may still be unread. Restart
  first: it has the deadline (one block, ~488 us), the render does not.
  Driving the restart from the fetches makes it immune to the FIFO's unknowable read-ahead; a
  playback-timer restart is not. Do not "simplify" this into rendering whole halves from the main
  loop: a page clear stalls `gfx_idle()` for milliseconds and the audio would drop out on every
  page change, and rendering a whole 256-sample half inside the IRQ would hold the serial IRQ off
  for ~460 us of its ~680 us re-arm window.
- **One dispatcher, `irq.c`, owns the BIOS vector.** Order IS priority (no nesting): DMA1, DMA2,
  then serial. It acknowledges only the IF bits it read on entry. `link_init()` only enables its bit.
- **Four slots, at most three sounding** (`g_fm[4]`: V1, V2, DRUM A, DRUM B; `g_fmActive` says
  which may start notes). A slot switched off by a mode change keeps rendering with its gate forced
  low until its release ends — mode changes never click. A silent slot (carrier idle, amplitude 0)
  is skipped. Each slot renders to a 16-sample buffer, mixed through its Q8 constant-power pan
  gains into int32 accumulators, then SATURATED to int8 (never wrapped).
- **Control/audio split.** `synth_tick()` (~1 kHz, main loop) writes `g_fm[]` (fm.h): increments,
  index, level, pan gains, noise, pitch envelope, envelope rates, gate. The IRQ owns phases,
  feedback history, both ADSRs and the pitch envelope (2048 Hz, ramped across each block). Every
  field has one writer and is 32-bit. **Note-ons are a COUNTER** (`noteOns`), for the same reason
  gate edges are on the link: a flag with two writers loses the edge between the read and the clear.
- **An attack always completes**, gate or no gate — a 1 ms sequencer trigger must give a full note.
- **Fixed-point scales** (fm.c header comment is the authority): phase Q32, sine Q15 (256 entries),
  amplitudes Q16. `m * mA << 3` is a peak deviation of 8π at full scale; `idxAmt = tenths * 261`.
  Every product was arranged to fit 31 bits; the model asserts it.
- **The inner loop is hand-unrolled** (`FM_PAIR`), with the amplitude ramp stepped per word, and
  exists TWICE in `render_voice`: with and without the noise blend, so the common noise-free path
  keeps its register budget. The first, rolled version spilled to the stack every sample (~50
  cycles/sample); the unrolled clean loop is 73 instructions per four samples, the noise loop 115,
  plus ~8 per sample per voice for the mix — about 30 % CPU for three voices, by count. If you
  touch it, re-read the disassembly (`objdump -d -j .iwram`), and watch DSP LOAD on the CAL page.
- **Level is a 2.5 dB-per-step law (`LEVEL_GAIN`), not linear.** Linear took only 5.5 dB off from
  15 to 8, so a bright drum could not be pulled back into the mix (Andy, on OP HAT).
- **Noise is half scale, and nothing should push the modulator past Nyquist.** OP HAT first shipped
  at C7 with a 1:11 ratio: the modulator wanted 23 kHz, sat pinned at the 16 kHz clamp, and with
  feedback 7 and full-scale noise it was a harsh wall far louder than its RMS (-15 dBFS, quieter
  than the kick on paper). Noisy/HF presets need judging by ear, not RMS.
- **Feedback doubles per step and the averaged form makes 7 about 2π effective — noise.** The
  saw-like region is 5. SAW LEAD shipped at 7 first and measured as broadband noise in the model.
- **Envelope coefficients are SEARCHED, not computed.** `gen_fm_tables.py` binary-searches each
  Q16 decay/release coefficient against the renderer's exact truncating recursion; the continuous
  formula ran every stage ~8% short. Do not "fix" the truncation into rounding: with long times the
  rounded recursion sticks at the snap level and the release never ends.
- **The RATIO destination is read with hysteresis** (`stepped()` in synth.c), so a knob or CV
  parked on a boundary does not flicker the modulator between two ratios.

## The 4-op voice (VOICE MODE 4-OP, 2026-10-25)

- A fifth render slot (`FM_ACTIVE_4OP` in `g_fmActive`, `g_fm4`), taking V1's place: V1's gate,
  pitch, V1-ticked mappings and V1 BTN actions drive it; V2 is off; drums still work. The 2-op V1
  keeps being computed with its gate low, so mode changes release rather than cut.
- **The eight algorithms are HAND-WRITTEN TWICE**: as `ALG1..ALG8` loop bodies in `fm.c` (speed —
  one loop copy per algorithm, picked per block) and as `FM4_MODS` / `FM4_CARRIERS` tables (control
  layer's carrier normalisation, the ENV diagram, the model). They must agree. The scratchpad model
  (`fm4_model.py`) parses both and asserts they match; re-run it after touching either.
- Operator level is DX-style 0..99 at 0.75 dB/step (`OP_LEVEL`); modulator full scale = 8π, as in
  the 2-op voice. Ratio = coarse × (1 + fine/100), coarse 0 = 0.5.
- **Carriers are normalised by 1/sqrt(count)** in `voice4_out()`. 1/count was tried first: never
  clipped, but ORGAN and the alg-5 pianos came out 6–9 dB quieter than BASS. With 1/sqrt the 4-op
  presets peak 80–123 of 127 at level 14, none saturating.
- **The 4-op presets are converted real Yamaha voices** (2026-10-25): `gen_dx_presets.py` holds 32
  raw VMEM records from Bryan Sutula's TX81Z/DX11 Voice Archive (via MusE's DeicsOnze `ARCH_ALIN`,
  README: "public domain"; the archive's own header says "Copyright 1991 by Bryan Sutula. See README
  file for restrictions" — Andy was told) and writes `presets4.h`. VMEM layout: operator slots
  4, 2, 3, 1 (verified against OP SHOP's tx81z-core.js / Edisyn). The hand-designed set they
  replaced leaned on alg 5 at cautious levels and sounded like doubled 2-op voices.
- **Operator 4 feedback is from its OUTPUT** (YM-accurate, ymfm's opm.cpp as reference):
  `fbAmt = 2^(10+FB)` on the sum of op 4's last two outputs, so FB 7 = 4π at full level. The
  first 4-op engine fed back the raw sine, full strength whatever op 4's level, and real DX
  voices (nearly all FB 7 with a moderate op 4) came out as noise. The 2-op voices keep their
  raw-sine feedback.
- Conversion lessons: honour D2R tails up to ~15 s (a 4 s cut-off left plucked voices sustaining
  at full brightness); DET is a fixed Hz offset on the chip, so divide its cents by the ratio
  (plain cents put DX7 EP's 15x tine 23 Hz out); measure at CENTRE pan (181/256) — at full gain
  several multi-carrier voices appeared to clip, at centre none do. Harmonic-to-noise is a poor
  judge of detuned or inharmonic voices: it reads chorus as noise. Listen.
- `melodic_pitch()` takes note/octave/glide rather than a `Voice*` so the 4-op voice can use it;
  it records each voice's target in `g_target[]` and the tick sets `g_pitchQ8` ONCE from the
  active main voice. (A first draft called it twice for V1 to restore g_pitchQ8 — that would have
  run V1's glide at double speed.)
- Upload is now ~53 KB / ~8 s. The eight loop copies, the grid UI and the 32 DX presets are most
  of the growth.

## Drawing speed

- **THE EDITOR'S SPEED IS SET BY WHAT THE MAIN LOOP IS LEFT, NOT BY THE FILL RATE.** After the
  primitives moved to IWRAM (below) the editor still painted row by row (Andy, 2026-10-26). The
  cause was the 1 kHz control tick: `synth_tick` and its callees (~1900 instructions per tick with
  four 2-op slots, the 4-op voice and the matrix) ran from EWRAM at ~6 cycles per ARM instruction,
  ~13K of the 16.8K cycles in each millisecond, and with the audio on top the UI got a sliver.
  `gfx_idle()` runs owed ticks after every character, so every character paid for one. Fixed
  2026-10-26: the tick and everything it calls every tick are `IWRAM_CODE` (helpers
  `always_inline`), `g_fm`, `g_fm4`, their envelope readouts and `g_patch` are `IWRAM_BSS`, and an
  inactive slot is not recomputed once it is silent. **The CAL page's TICK LOAD row measures it**
  (wall time inside the tick against its ~1 ms period, so it includes audio interrupts landing in
  it). Watch it after adding anything to the tick, the way DSP LOAD is watched for the renderer.
  **Measured after the fix (Andy, 2026-10-26): about 40 % average, peak under 85 %** — "FAR
  better". The average is above the ~15 % the instruction count predicts because it is wall time
  (audio interrupts inside the tick count) and the tick's tables (`NOTE_INC`, `ENV_COEF`, ...)
  are still read from EWRAM.
- **`SPEEDUPSCREEN.md` is the handoff for doing the same in a sibling** (written for 108, which on
  2026-10-26 still had no IWRAM sections at all, pixel-at-a-time primitives and its tick in EWRAM).
  Keep it in step if this approach changes. Like this file, it is working notes, not release docs.
- The generic GBA advice (Mode 4 page flipping, DMA3 copies from a WRAM canvas, tile-mode text) was
  considered and rejected: it fixes fill rate, which was never the bottleneck; page flipping fights
  the per-region caches (every change would go to both pages); tile text needs an 8-pixel grid and
  the font advances 6. The undocumented EWRAM 1-wait-state register (`0x04000800`) was also left
  alone: its behaviour across GBA models (the Micro especially) could not be confirmed.
- **`rect`, `line`, `glyph` and `text` run from IWRAM** (`gfx.c`). From EWRAM, one pixel per iteration and
  a `rect()` call per lit glyph pixel, a full-screen clear was ~1M cycles (~60 ms, estimated) and
  the editor visibly crawled (Andy, 2026-10-25). Now `rect` fills two pixels per 32-bit store,
  unrolled by eight, and `glyph` (scale 1) and `line` write VRAM directly. Their callers
  (`rect_clear`, the UI) stay in EWRAM.
- Keep the VRAM pointers `volatile`: a plain fill loop is a memset idiom, and GCC emitting a call
  to `memset` is a -nostdlib link error.
- `ui.c` cannot go Thumb or `-Os` to save upload time or speed up: both make GCC call
  `__aeabi_idiv` even for constant divisors (Thumb-1 has no long multiply).

## The matrix, the buttons and drum mode

- **108's matrix model, FM destinations.** One mapping per source (CV1 CV2 AU1 AU2 PU2 MAIN X Y),
  a depth, and a two-bit voice mask (V1, V2 — V2 also reaches both drums in drum mode). PU2
  replaced 108's SW source; the switch has its own SET setting (`swTarget`). Knobs are CENTRED.
  Depth → units is in `voice_out()`: full depth sweeps each control's useful range.
- **GATE is a destination**, not a separate TRIG page: depth ignored, the source's level and edges
  play the masked voices. Edges are tracked for EVERY source every tick whatever it is mapped to,
  so remapping never fires a stale backlog.
- **PITCH uses the calibration of the source's circuit**: CV (cvScale/cvOffset) for everything but
  AU1/AU2, which use auScale/auOffset. Factory values are Andy's measured trims: CV OFFSET −27,
  AUD OFFSET −37 (2026-10-25). AUD OFFSET first shipped at 0, which put V1 93 cents above V2 at
  every voltage (Andy heard "about a semitone"); −27 (copying CV, "same ADC") was a guess that
  turned out wrong — the audio inputs' circuit has its own offset.
- **Drum mode**: AU1/AU2's matrix rows are SKIPPED (shown as DRUM A/B HIT, locked in the editor,
  kept for normal mode); their edges strike DRUM A / DRUM B. Anything else that would play V2 plays
  DRUM A. V2 stops taking notes and releases.
- **BTN actions are inactive in the editor**, as in 108. PRESET± wraps.

## Audio-input edges (the firmware addition)

- The audio pair reaches the GBA as a snapshot every OTHER stream word (~440 Hz), so a 1 ms drum
  trigger can vanish between two. The host Schmitt-triggers each audio input at 48 kHz (up through
  341 counts ≈ +1 V, down through 170) and counts rising edges, Pulse In 2's pattern.
- `GBA_OP_AUDEDGE`: 8-bit wrapping counts, levels, and its OWN 6-bit check (an XOR fold — every
  single-bit error and an all-zero argument fail it), because CONTROL words carry no check bits and
  a phantom edge is a phantom drum hit. The payload works in deltas, so a lost word delays edges.
- **Sent on change but AT MOST ONE WORD IN FOUR**, plus a re-announce every 32 words in slot 0x10
  (which collides with no other periodic word). Without the rate limit, audio patched into AU1/AU2
  would send this word on every poll and push the input stream off the link entirely.
- Without `GBA_CAP_AUD_EDGES` the payload falls back to a Schmitt trigger on the snapshots; the CAL
  page's AUD EDGES row says which is live.

## Patch storage

**THE HOST ONLY KNOWS "ERASED" OR "WRITTEN".** `patch_store.cpp`'s `slot_valid()` came over from
108 checking for 108's magic 'GB', so every FM patch ('FM') looked like an empty slot: MEM save was
written and acked, then showed FREE, and load said SLOT IS EMPTY (fixed 2026-10-25). Judging what a
patch is belongs to the GBA (`synth_patch_valid()`), never the firmware.

**The magic is `'FM'` (0x464D), not 108's `'GB'`.** The Workshop's sixteen slots are one flash
sector shared by whichever GBA program is running, so a 108 patch can sit in a slot this program
loads. The different magic makes it a clean `BAD DATA - NOT LOADED` instead of a misread.

**`PATCH_VERSION` (now 3) is checked, not assumed, and must be bumped whenever a stored field
changes its LAYOUT *or* its MEANING.** A layout change cannot be recovered from the bytes, so
`synth_patch_valid()` rejects it; a change of meaning at the same layout should be migrated in
place by `synth_patch_migrate()`, called after the byte copy and **before**
`synth_patch_applied()`. Prefer migrating: a bump that throws away saved patches is a last resort.
`sizeof(Patch)` is 178 of the 256-byte slot; `patch_fits_a_slot` in `synth.c` is the backstop.
v2 (four voices, matrix, buttons, mixer) is a layout change: v1 patches from the single-voice beta
are rejected, which was acceptable because v1 was never released. **v3 (the 4-op voice) only
APPENDED fields**, so v2 is migrated: a v2 slot arrives zero-padded past its end, and the migration
gives the zeroed 4-op voice the factory one (an all-zero 4-op voice is silent). KEEP APPENDING.

**Presets are SOUNDS, not voices**: `preset_load()` replaces ratio … envelopes (and the note, for
drum presets) and keeps level, pan, octave and portamento, so browsing never moves the mix. Each
preset was rendered through the model and checked (zero-crossing rate and −40 dB decay time) before
it went in.

Transfer rules, all inherited from 108 and all still load-bearing:

- **The WHOLE slot is sent, zero-padded past the end of the `Patch`.** The host cannot know how
  big a `Patch` is, so a complete transfer is "every byte of the slot seen" and nothing else.
- **`gba_proto.h` is the authority on where a byte sits in the word, and the two directions do
  not use the same place.** Upstream `GBA_UP_PATCH` is index `[15:8]`, value `[7:0]`; downstream
  `GBA_OP_PATCH` is index `[23:16]`, value `[15:8]`.
- A save is self-healing without a retransmit protocol: the GBA repeats the whole block until the
  host acks, and the host commits only a block whose seen-bitmap is complete.

## Page order is an index, not a list

`PAGE_*` in `ui.h` ARE the indices into `page_tab[]` in `ui.c`, and the status word sends the
page number upstream to the LEDs. Renumbering the defines without reordering that array gives a
tab bar that lies rather than a compile error. `PAGE_MEM` is deliberately 0: recalling a patch
is the one editor action that happens mid-performance, so START lands on it.

## Payload UI rules (learned the hard way, all of them)

1. **NEVER ERASE THEN DRAW.** Every flickering region had the same shape: clear a box to the
   background, then paint into it, once per frame. Two rules fix it everywhere — paint
   continuously-changing regions in ONE overwriting pass (background band, then value band), and
   compare rendered text against what is on screen and skip when unchanged. Static legends are
   drawn once, never per frame.
2. **A cache key must include everything that changes the drawing.** The MAP column cursor was
   folded into every row's comparison string, so moving it repainted all seven rows; the VOICE
   page's labels changed with the channel while only values were compared, leaving stale labels.
3. **A clear must not reach past what it owns.** The list clear covered the full ten-row height
   regardless of the page, erasing legends drawn below it. It now clears the taller of the old
   and new layouts and nothing more.
4. **`dec32()` right-aligns into a FIXED width and drops leading digits when it does not fit.**
   `dec32(buf, 10, 1)` yields `"0"` — that printed every envelope time above 5 ms as `0 MS`. Use
   `dec_at()` for anything whose digit count varies.
5. **Watch the buffer sizes.** The CAL line composes to 29 characters and had a 24-byte buffer;
   the stack smash made its redraw guard never hold, so it drew, cleared and redrew for ever.
6. **The `-nostdlib` tripwire is doing real work.** It has caught a `% rows` with a variable
   divisor, a drum-sweep divide, and an implicit `memcpy` from a struct assignment — each a LINK
   error rather than a silent bug. Do not "fix" it by linking libgcc.
7. **A large `rect()` blocks `gfx_idle()` for as long as the fill takes — several milliseconds
   for a full-screen clear — and that pauses the synth's control tick and the link poll for the
   same span, not just the redraw.** `gfx_idle()`'s own comment already says this ("the control
   tick used to advance only once per main-loop iteration, so the effective control rate was the
   FRAME rate"), but it was only ever wired into `text()` (per character) and `srect()` (once,
   after). Every one-time, page-or-mode-change body/screen clear — `play_static()`,
   `edit_static()`, the SELECT-navigation blank, and every page's `!xInit`/cache-invalidated list
   clear — was a plain, un-chunked `rect()` covering 100+ rows with **zero** `gfx_idle()` calls
   during the fill, so pressing START or changing pages could stall a running envelope or
   portamento, or drop a link poll, for the whole clear. `rect_clear()` in `gfx.c` is the fix:
   the same fill, banded into 8-row chunks with a `gfx_idle()` between each. **Use it, not
   `rect()`, for any clear that is not itself bounded to a handful of rows** — small per-frame
   boxes (tabs, tickboxes, meters) are fine with plain `rect()`, since a handful of rows is not
   worth the per-band overhead and they were never the source of this.

## The multiboot protocol

- `gba_multiboot.cpp` implements the single-cartridge / download-play upload: sync (`0x6202`
  → `0x7202`) → header (0xC0 bytes) → palette/handshake (`0x63D1`, `0x64hh`) → length/seed →
  encrypted main transfer → final CRC handshake.
- **The encryption/CRC magic constants are not secrets** — they're a fixed, publicly
  documented BIOS obfuscation. Transcribed from two agreeing open references:
  `akkera102/gba_01_multiboot` and the RP2040 port `copyrat90/gba-pico-gamepad`. If you touch
  the transfer loop, cross-check against both. Key numbers: LCG mult `0x6F646573`, key
  `0x43202F2F`, CRC poly `0xC37B`, CRC seed `0xC387`, offset transform `0xFE000000 - i`.
- Transport is **mode 3 SPI (CPOL=1, CPHA=1), MSB-first, 32-bit**, RP2040 = clock master.
  GBATEK, *SIO Normal Mode*: SC idles HIGH, both ends drive on the falling edge and sample on
  the rising edge. **Multiboot runs at 100 kHz** — measured repeatable; 200 kHz is marginal and
  leaves the console restarting its boot, which is worse than running slower. Post-boot the
  applet polls at **1 kHz**.
- PIO core follows the Raspberry Pi pico-examples SPI CPHA=1 program.

## The GBA payload

- **No splash screen on a normal boot.** The green screen `crt0.s` paints in assembly is the
  boot proof; a title card after it was on screen for about as long as it took to clock one word.
  A message is drawn ONLY if the host stays silent, which is the one case where it says something.
- `payload/` is a **self-contained GBA-side sub-project** (no libgba, no libc) built for
  ARM7TDMI (armv4t). Modules: `link` (serial slave), `irq` (the shared dispatcher), `audio`
  (stereo Direct Sound + two DMA rings), `fm` (voice slots, envelopes, mix), `presets` (factory
  sounds), `synth` (control layer: matrix, buttons, drum mode, pitch, patch), `ui` (play screen +
  eight editor pages), `gfx`, `diag`, `main`.
  `fm_tables.h` is generated by `gen_fm_tables.py` and committed.
  `build.sh` compiles every `.c` in the directory, so adding a module needs no script edit.
- **`diag.c` keeps the link-characterisation screens and must not be deleted.**
  `linkrate.uf2` and `bandwidth.uf2` link this same generated payload image; they are the
  instruments that produced every number in `POSTMORTEM.md` and how we would re-measure after
  a regression. While a diagnostic mode is active EVERY word routes to its decoder, because
  its corruption counter works by counting words matching neither magic.
- **Multiboot entry is at offset `0xC0`** (not the header branch at 0x00). `crt0.s` must keep
  `_start` at exactly `0x020000C0`, image linked for EWRAM (`multiboot.ld`).
- **The GBA BIOS validates the Nintendo logo (0x04..0x9F) and the header complement (0xBD)**
  and locks up if either is wrong. `payload/build.sh` fixes both: it uses the Pico SDK's own
  `arm-none-eabi-gcc` to compile and a **bundled dependency-free `gbafix.py`** (logo +
  complement) — **no devkitPro install needed**. Real `gbafix`/devkitARM is used if present.
- `gba_payload.h` is the **generated** byte array (via `bin2h.py`, padded to 16 bytes). It's
  committed as a real bootable image; regenerate with `payload/build.sh` after editing the
  payload, then rebuild the firmware.

## Build

RP2040 firmware (produces `build/gba_link.uf2`):
```sh
cmake -G Ninja -B build -S .
cmake --build build
```
On this machine the toolchain lives under `~/.pico-sdk` (SDK 2.2.0, toolchain 14_2_Rel1) via
the Pico VS Code extension. From a bare Git Bash shell this is what worked on 2026-10-24:
```sh
export PICO_SDK_PATH=~/.pico-sdk/sdk/2.2.0 PICO_TOOLCHAIN_PATH=~/.pico-sdk/toolchain/14_2_Rel1
~/.pico-sdk/cmake/v3.31.5/bin/cmake -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/.pico-sdk/ninja/v1.12.1/ninja.exe -B build -S .
~/.pico-sdk/cmake/v3.31.5/bin/cmake --build build
``` SDK 2.2.0 has no `pio_set_input_sync_bypass_with_mask` helper —
poke `pio->input_sync_bypass` directly (see `gba_multiboot.cpp`).

GBA payload → `gba_payload.h` (then rebuild the firmware, and re-copy the UF2 into `UF2/`):
```sh
cd payload && ./build.sh
python gen_fm_tables.py   # only after changing the rate, ratios or envelope ladder
```

## Conventions

- Match `96_cathode`'s CMake link set and `set_sys_clock_khz(144000, true)` (clean multiple
  of 48 MHz → tidy PIO clkdivs).
- **`info.yaml`'s schema is set by what the program listing at computer.musicthing.co.uk parses,
  and it is NOT obvious — do not just eyeball a live release before changing it, RUN THE
  VALIDATOR.** The monorepo carries one: `tools/sitegen/src/validate/cli.js` in
  `TomWhitwell/Workshop_Computer` (`node tools/sitegen/src/validate/cli.js
  releases/108_GBA/info.yaml` from the monorepo root). A visual diff against another release's
  `info.yaml` proved actively misleading here — the `controls` shape had moved on since this file
  was last checked against `104_barbers_pole`/`96_cathode` (2026-09-10: `panel.inputs`/`outputs`
  now enforce an exact id enum — `AudioIn1/2`, `CVIn1/2`, `PulseIn1/2`, `AudioOut1/2`, `CVOut1/2`,
  `PulseOut1/2`, no shorthand like `Audio1` or `Pulse2`; knobs are `controls.knobs` entries
  (`main`/`x`/`y`, each `{name, description}`), not fake panel inputs; `controls.switch` is an
  object keyed by `up`/`middle`/`down`/`tap`, not a list; `controls.leds` is a list of *groups*,
  each `{display, items: [...]}`, not a flat list of LEDs) — and the validator caught it, a stale
  reference release would not have. Also run `git diff --name-status -z upstream/main...HEAD |
  node tools/sitegen/src/validate/prRulesCli.js out.json` before opening the PR — a second,
  separate check the site runs over the whole submission diff. Key order as written: `draft`,
  `Name` (no card number — the site prepends that from the folder), `short-description`
  (**not** `Description`), `Language`, `Creator`, `Version`, `Status`, `License`, `repository`
  (lowercase, and it points at the UPSTREAM repo, not the release folder), `Editor`,
  `date-created`, `date-updated`, then the blocks `contact`, `uf2`, `tags`, `summary`
  (**not** `manual`), `panel`, `controls`.
- **Measured link numbers** (`diagnostics/POSTMORTEM.md` has the full record): multiboot
  100 kHz, sustained 2000 words/s clean, ~64 kbit/s, round trip ≤0.5 ms, 32 KB payload uploads
  in ~5 s. SCK above 100 kHz has never been *fairly* tested — the readings that suggested a
  ceiling were taken with zero inter-word gap, so they measured the re-arm race instead.
- **When a measurement contradicts something already known to work, doubt the measurement.**
  Several days went into faults that turned out to be in the diagnostics rather than the
  hardware: a scope probe on the wrong net, a loopback with a structural blind spot, a
  diagnostic that blocked the 48 kHz callback, and an instrument that corrupted the very
  words it was counting. `POSTMORTEM.md` records each one and how it was caught.

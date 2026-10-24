# GBA FM Voice — technical guide

> This is the full reference: cable-building detail, fault codes, the FM engine and its audio
> path, and how the link works. For the quick start, see [`README.md`](README.md).

Turns the Music Thing Workshop Computer into a **Game Boy Advance link-cable master**, and the
GBA into a **two-voice FM synth with a drum mode, played by the modular**. The module boots a
cartridge-less GBA over the pulse jacks using BIOS **Multiboot**, then streams its inputs down a
live SPI link. The GBA synthesises the sound in software, in fixed-point integer maths, and plays
it in stereo through its **Direct Sound** DACs out of its own headphone jack, with the editor on
its own screen.

This program is a sibling of **GBA PSG Voice** (card 108, repo `89_GBA`). The multiboot loader,
the serial layer and the wire protocol are 108's. The RP2040 firmware is 108's plus one addition:
it counts trigger edges on the two audio inputs and sends them in one new CONTROL word (see
[How it works](#how-it-works)). Everything in Part 1 applies to both.

No cartridge, no flashcart, no modification to the console. The program is uploaded from the
module every time you switch on, in about six seconds.

**The division of labour is the design.** The Workshop senses; the GBA is the instrument. This
side reads its inputs and sends them raw; the GBA owns pitch tracking, the FM voices, the
envelopes, the modulation matrix and the UI. That is why replacing 108's whole sound engine needed
almost nothing from the firmware: the one addition is sensing, not a musical decision.

---

# Part 1 — Getting connected

## What you need

| | |
|---|---|
| **A Game Boy Advance** | An original **AGB-001**, an **Analogue Pocket** or a **GBA SP**.  Note: **The GBA SP does not have a headphone socket** — it needs Nintendo's SP headphone adapter, which occupies the charging port. |
| **No cartridge** | Multiboot needs the slot **empty**. The console must sit on the Nintendo logo screen. |
| **A link cable** | See below.  |
| **2 × 1 kΩ resistors** | Any tolerance. |

## Making the link cable

This is the only part of the build that can go wrong quietly, so it is worth doing carefully.

### Use a Game Boy Color link cable

**Get a Game Boy Color / Game Boy Pocket link cable rather than a third-party GBA one.** The
connector has been the same small six-pin part since the Game Boy Pocket, so a GBC cable plugs
straight into a GBA — and, crucially, **GBC cables are normally 6-core**: every pin in the
connector has a wire behind it.

Third-party GBA cables are wildly inconsistent: many
carry only the three or four conductors that particular cable's intended use needed, and which can vary. 

With a six core GBC you know every signal is there before you start. You still have to *find* which
is which but you are looking for something that exists.

> The original DMG-era cable, with the big chunky connector, will not fit. You want the small connector introduced with the Game Boy Pocket.

### The socket, and how it is numbered

Six pins in two rows of three. The shell is a hexagon with an asymmetric **lump**; pins 1 and 2
sit at the lump end. **The numbering mirrors between plug and socket** — the single easiest
thing here to get wrong.

```
      PLUG (contacts toward you)          SOCKET (looking into the GBA)
    ┌──────────────────────────┐      ┌──────────────────────────┐
    │    1    3    5           │      │           5    3    1    │
    │        ╱▔▔▔╲             │      │            ╱▔▔▔╲         │
    │    2    4    6           │      │           6    4    2    │
    └──────────────────────────┘      └──────────────────────────┘
         lump at the top
```


### Wiring

All directions are **from the GBA's point of view**.

| GBA pin | Signal | Workshop jack | RP2040 GPIO | Direction | Series R |
|---------|--------|---------------|-------------|-----------|----------|
| **5** | SC (clock) | Pulse Out 1 | GPIO 8 | RP2040 → GBA | **1 kΩ** |
| **3** | SI (MOSI)  | Pulse Out 2 | GPIO 9 | RP2040 → GBA | **1 kΩ** |
| **2** | SO (MISO)  | Pulse In 1  | GPIO 2 | GBA → RP2040 | **none** |
| **6** | GND        | ground      | —      | common | — |
| 1 | VCC +3.3 V | **nothing** | — | *GBA output* | — |
| 4 | SD | **nothing** | — | unused | — |


### The two resistors

**1 kΩ in series on SC and SI.** The Workshop's pulse outputs swing to about 6 V; the GBA's
inputs are 3.3 V logic. The resistor, together with the GBA's own clamp diode, limits the
current into the console. Skipping these could damage your GBA.

**No resistor on SO.** The GBA drives SO at
3.3 V into us, which the input handles as it stands.

### Terminating it

Three options, in descending order of tidiness:

1. **A breakout PCB.** E.g. 
   [this OSH Park shared project](https://oshpark.com/shared_projects/srSgm3Yj), with the two
   1 kΩ resistors soldered onto the board. The cable solders to one side and the patch leads
   to the other, so the resistors are permanently where you cannot forget them.
2. **A female link socket** on stripped board, cable into it, resistors inline.
3. **Bare wires**, resistors soldered inline and heatshrunk. Works; label everything!


### Making a cable, a word of warning

**If you make a dedicated cable by cutting a link cable, BEWARE.**

**Peer-to-peer link cables generally swap SO and SI between their two ends**, so the pin that
carries SO at the end you kept depends on which end you kept — and you cannot tell the two ends
apart by eye.

TO HELP YOU: **build and flash `cablecheck`, then run MODE 0.** It listens on Pulse In 1 and
Pulse In 2 at the same time and reports which wire actually carries the GBA's SO.

```sh
cmake -G Ninja -B diagnostics/build -S diagnostics
cmake --build diagnostics/build --target cablecheck   # -> diagnostics/build/cablecheck.uf2
```

Continuity tables and notes on specific cables are in
[`diagnostics/CABLES.md`](diagnostics/CABLES.md).

## First power-up

Power the **Computer first, then the GBA**. The console only syncs if the master is already clocking when it boots.

1. Flash `gba_link.uf2` to the Workshop Computer.
2. Remove any cartridge from the GBA. Connect the cable.
3. Power the Workshop. **LED 0 blinks** — it is looking for a console.
4. Switch the GBA on. If it shows the Nintendo logo it has connected.
5. **LEDs 1–5 sweep as a progress bar** for about six seconds.
6. The GBA lands on the performance screen. **LED 0 goes solid.** Push the Workshop Computer's momentary switch down
   and you should hear a note.

## If it does not work

Try rebooting Workshop Computer and GBA.

While the link is **not** up, LED 0 blinks and LEDs 1–3 report the last multiboot result as a
three-bit code:

| LEDs lit | Meaning | Look at |
|---|---|---|
| **1** | **NoGBA** — nothing answered at all | Power order. Cartridge removed? Is the GBA on the logo screen? Ground. Then the crossover. |
| **2** | **BadHandshake** — it answered, then the exchange went wrong | Marginal wiring, a missing series resistor, or a poor ground. |
| **1 + 2** | **TransferError** — a data word's echo did not match | Noise or a bad joint on SI/SC. Check for shorts. |
| **3** | **CrcMismatch** — it all arrived, but corrupted | As above; usually a marginal connection rather than a wrong one. |
| **1 + 3** | **BadPayload** | A build problem, not a wiring one. Rebuild the payload. |

For a step-by-step bring-up that isolates one thing at a time, work down
[`BENCH.md`](BENCH.md). Each step tells you *where* a failure is rather than just that there is
one.

---

# Part 2 — Playing it

Well done for getting this far!

## Audio

Patch the GBA's **headphone jack** into Eurorack if you wish, but NOTE the headphone ground on
the GBA SP adapter may short the signal with a shared ground. If so, use a ground-isolating
stereo audio cable.

Expect it to be **quiet**: around 1 Vpp against Eurorack's ~10 Vpp, so it wants an amplified input into Eurorack world.


## The default patch

**Voice 1** is a PLUCK (1:1, an index envelope that falls over ~650 ms), slightly left. **Voice 2**
is a BASS (1:1 with feedback), slightly right. In drum mode, **Drum A** is a KICK and **Drum B** a
SNARE. All four sit at level 14 (−2.5 dB), which leaves headroom for two voices peaking together. Push the Z
switch down to hear Voice 1, or hold it UP for a drone and sweep the Main knob to hear the index
open and close.

## The panel

Every input is a **source** in the modulation matrix (the MAP page). The factory map:

| Source | Destination | Amount | Voices |
|---|---|---|---|
| **CV In 1** | INDEX | 63 (±6.3 radians across the input's range) | V1 |
| **CV In 2** | PITCH | 32 (unity 1V/oct, CV calibration) | V1 |
| **Audio In 1** | GATE | – | V2 |
| **Audio In 2** | PITCH | 32 (unity 1V/oct, AUD calibration) | V2 |
| **Pulse In 2** | GATE | – | V1 |
| **Main knob** | INDEX | 63 | V1 + V2 |
| **X knob** | RATIO | 63 (±8 ratio steps) | V1 |
| **Y knob** | DEC+REL | 32 (±16 envelope-ladder steps) | V1 + V2 |

- **Knobs are centred**, as in 108: noon adds nothing, and the patch plays as stored.
- **GATE** ignores the amount: the source plays notes. Pulse In 2 and the audio inputs count edges
  on the Workshop at 48 kHz, so sub-millisecond triggers are never lost. CV In 1, CV In 2 and the
  knobs use a Schmitt trigger at about +1 V up and +0.5 V down.
- **PITCH** goes through the calibration for the source's circuit: CV (CV In 1/2) or AUD
  (Audio In 1/2), each trimmed on the CAL page.
- **Pulse In 2** as a value is full when high and nothing when low, so it can open an index as
  easily as play a note.
- **Stepped destinations** (RATIO) read with hysteresis, so a knob parked on a boundary does not
  flicker between two ratios.
- **The Z switch** has its own setting (SET → SWITCH PLAYS: OFF, V1, V2, BOTH): DOWN is a momentary
  trigger and UP holds the gate open.

| Output | Does |
|---|---|
| **CV Out 2** | Voice 1's note, quantised, as calibrated 1V/oct |
| **CV Out 1** | 5 V while Voice 1 is sounding (gate held, or still releasing) |
| **Audio Out 1 / 2** | GBA A / B button held |

**An attack always completes.** A gate that falls during the attack is acted on when the attack
reaches the top. A one-millisecond trigger therefore plays a full attack-decay-release note rather
than a click. Drum presets set sustain 0 and decay equal to release, so they sound the same
however long the trigger is.

## Drum mode

SET → DRUM MODE (or a BTN action). Voice 2 stops taking notes and releases whatever it was
playing; **Audio In 1** strikes **Drum A** and **Audio In 2** strikes **Drum B**, each at its own
DRUM NOTE. The AU1/AU2 rows of the MAP page show DRUM A HIT / DRUM B HIT and are locked; their
mappings are kept for when drum mode is turned off. Anything else that would play Voice 2 (a GATE
mapping with the V2 tick, the switch set to V2, TRIG V2) plays Drum A instead, and mappings with
the V2 tick modulate both drums. One CV mapped to PITCH on V2 tunes the whole kit.

---

# Part 3 — The editor

**START** toggles between the PLAY screen and the editor; entering the editor always lands on MEM.
**SELECT + Left/Right** changes page, the **D-pad** moves the cursor, **A + Up/Down** changes a
value and **A + Left/Right** changes it in steps of eight (an octave for notes, 64 for the CAL
scales). Inside the editor no button carries its performance meaning.

## MEM — sixteen patch slots

108's page, unchanged. **A + Up** saves to the highlighted slot, **A + Down** loads. The slots live
on the Workshop and are shared with GBA PSG Voice: an FM patch carries the magic `'FM'` where 108's
carries `'GB'`, so loading one program's patch into the other reports `BAD DATA - NOT LOADED` and
leaves the current sound alone. Patches from the single-voice beta (version 1) are rejected the
same way.

## FM — a voice

The first row picks the voice: VOICE 1, VOICE 2, DRUM A or DRUM B. A voice that is not sounding in
the current mode is marked (OFF), and can still be edited.

| Row | Range | |
|---|---|---|
| PRESET | 26 sounds | Choosing one LOADS it: browsing is auditioning. It replaces the sound and keeps level, pan, octave and portamento |
| RATIO C:M | 1:0.5 … 1:16 | Modulator frequency as a multiple of the carrier's: 0.5, 1, 1.5, 2, 2.5, 3, 3.5, 4, 5, 6, 7, 8, 9, 11, 13, 16 |
| MOD DETUNE | ±99 cents | Detunes the modulator only. A few cents gives slow beating; more gives inharmonic shimmer |
| INDEX | 0.0 … 9.9 | Peak phase deviation in radians, at the top of the index envelope |
| FEEDBACK | 0 … 7 | Modulator self-feedback, doubling per step. Around 5 is saw-like; 7 is noise |
| NOISE | 0 … 15 | Noise blended into the carrier; 15 is all noise, at half scale (full-scale white noise sounds far louder than a sine of the same peak) |
| PITCH ENV | ±48 semitones | Pitch offset at the start of each note, decaying to zero |
| P.ENV TIME | 2 ms … 8 s | How long the sweep takes to settle |
| 0V NOTE / DRUM NOTE | | The note 0 V plays (melodic), or the note a drum is struck at |
| OCTAVE | ±3 | |
| PORTAMENTO | 0 … 15 | One-pole glide; 0 is off. Melodic voices only |

**Presets.** Tuned: PLUCK, E.PIANO, BELL, TUBULAR, MARIMBA, VIBES, CLAV, GLASS, BASS, SUB BASS,
BRASS, ORGAN, FLUTE, STRINGS, SAW LEAD, PAD. Untuned: KICK, SNARE, CL HAT, OP HAT, TOM, RIM,
COWBELL, METAL, ZAP, GONG. Drum presets also set the voice's note.

## ENV — two envelopes per voice

Rows: VOICE, ENVELOPE (**MOD INDEX** or **CARRIER**), then attack, decay, sustain, release. Attack
is linear; decay and release are exponential, falling in equal decibels per unit time as an
acoustic sound does. The times run from instant to 8 s on a 32-step logarithmic ladder; a stage's
time is the time it takes to cover all but the last −60 dB, where it snaps to its target. The live
level is drawn beside the shape.

## MIX — level and pan

Four faders: V1, V2, DR A, DR B. **Left/Right** picks one; **A + Up/Down** sets its level (0–15: 0 is off, then 2.5 dB per step, so 15 → 11 is −10 dB),
**A + Left/Right** its pan (L7 … C … R7, constant power). The fader shows the set level with the
live output inside it. The two voices not sounding in the current mode are drawn dim.

## MAP — the modulation matrix

One row per source: destination, amount (−64 … +63) and two tickboxes, V1 and V2 (V2 also reaches
both drums in drum mode). **Left/Right** moves between columns; **A + Up/Down** changes the
destination or amount; **A** alone toggles a tick. Destinations: PITCH, GATE, INDEX, RATIO, LEVEL,
FEEDBACK, DETUNE, NOISE, ATTACK, DECAY, RELEASE, DEC+REL, GLIDE, PAN, P.ENV. At full amount each
sweeps the useful range of its control: ±6.3 radians of index, ±8 ratio steps, ±16 levels,
±32 envelope steps, ±7 pan, ±31 semitones of pitch envelope.

## BTN — the GBA's buttons

What each of A, B, L, R, Up, Down, Left and Right does on the PLAY screen: TRIG V1/V2, HOLD V1/V2
(latching drone), OCT ± V1/V2, RATIO ± V1, INDEX ± V1, FB ± V1, PRESET ± V1/V2 (wrapping through the
list), TRIG DRUM A/B, DRUM MODE. The factory layout is A trigger, B hold, Up/Down octave,
Left/Right ratio and L/R feedback, all on Voice 1.

## SET — modes and scale

DRUM MODE, SWITCH PLAYS (OFF / V1 / V2 / BOTH), SCALE, KEY, TUNING (±50 cents). The quantiser
applies to both melodic voices; drums are not quantised. **FREE** (the default) does not quantise.

## CAL — trimming 1V/oct

Two calibrations, because the CV and audio inputs are different circuits: **CV SCALE / CV OFFSET**
(CV In 1/2) and **AUD SCALE / AUD OFFSET** (Audio In 1/2). SCALE is counts per semitone, Q8;
**A + Left/Right** moves it in steps of 64. The two **TUNER** rows show what CV In 2 and Audio In 2
mean through the current calibration — note, cents and the raw count — from Voice 1's and
Voice 2's 0 V note. Two readings a known interval apart give SCALE directly as
256 × (high − low) / semitones. Then:

- **AUD EDGES** — 48 KHZ when the Workshop is counting audio-input edges itself, SNAPSHOT if the
  firmware predates that, plus a count of any edge words that failed their check.
- **LINK** — IRQ when the serial interrupt is doing the work, POLLED if it has fallen back.
- **DSP LOAD** — the renderer's time against its budget, smoothed, then the worst single block
  since boot. See [The audio engine](#the-audio-engine).

---
# Part 4 — Reference

## The audio engine

### From PSG registers to Direct Sound

108 made its sound by writing the four legacy PSG channels' registers about a thousand times a
second; the hardware generated the waveforms. FM cannot be done that way: the waveform itself has
to be computed, sample by sample, and streamed to a DAC. On the GBA that DAC is **Direct Sound**:
two 8-bit FIFOs (A and B), each emptied one sample at a time by a timer overflow and refilled by
DMA. This engine uses both: **FIFO A is the left channel, FIFO B the right**.

| | 108 (PSG) | FM Voice |
|---|---|---|
| Sound source | PSG channels 1–4, register writes | Direct Sound A + B: Timer 0, DMA1 + DMA2, both FIFOs |
| Who computes the waveform | the PSG hardware | `fm_render_block()`, in IWRAM |
| Timer 0 | free-running control clock | **the sample clock** |
| Control clock (~1 kHz tick) | Timer 0 | **Timer 2**, same /1024 setting |
| BIOS IRQ vector | owned by `link.c` | shared dispatcher, `irq.c` |
| Envelopes | control tick, 1 kHz | audio interrupt, 2048 Hz, ramped across each block |

### The signal chain

- **Rate: 32768 Hz.** Timer 0 reload is `0x10000 − 16777216/32768 = 0xFE00`: one overflow per
  sample, 512 CPU cycles apart. (`0xFFFF − cycles` is one cycle short of the rate it names.) It is
  also the GBA's native PWM rate at the BIOS-default `SOUNDBIAS` (9-bit at 32.768 kHz), and its
  16 kHz Nyquist leaves room for the sidebands FM throws upward.
- **`SOUNDCNT_X` bit 7 first**, before any other sound register, exactly as in 108. Then
  `SOUNDBIAS = 0x0200`, `SOUNDCNT_L = 0` (every PSG channel off) and `SOUNDCNT_H = 0x9A0C`:
  A and B at 100 %, A to the left, B to the right, both on Timer 0, both FIFOs reset.
- **DMA1** copies the left ring to `FIFO_A` (`0x040000A0`), **DMA2** the right ring to `FIFO_B`
  (`0x040000A4`), both with control `0xF640`: enable, IRQ, start on FIFO request, 32-bit, repeat,
  destination fixed. Each time a FIFO drains to half full its DMA moves one block of four words,
  sixteen samples.

### The rings: refilled one DMA block at a time

Each ring is 16 blocks of 16 samples, about 7.8 ms. Both FIFOs drain on the same timer, so both
DMAs request on the same tick; DMA1 has priority and goes first, DMA2 straight after. Both raise an
interrupt after every block:

1. **DMA1's interrupt only restarts DMA1** at the ring's start, if that was its last block. In FIFO
   mode a DMA's source never wraps on its own; left running it would walk off the end of the ring.
   The next request is a whole block (~488 µs) away, and because the restart is triggered by the
   DMA's own fetches it cannot land on the wrong side of one.
2. **DMA2's interrupt restarts DMA2 the same way, then renders** sixteen new samples into the block
   *both* rings have just finished with. Rendering here rather than on DMA1 is what makes that
   true: DMA2 is the lower priority, so by the time its block is done, DMA1's already was.

Rendering from the interrupt, a block at a time, means the audio cannot be starved by the UI (a
full-screen clear stalls the main loop for milliseconds), latency is fixed at about 7.8 ms, and
the serial interrupt waits at most one render behind it.

### Interrupt priority

The GBA has one user IRQ vector, so `irq.c` installs one dispatcher and each subsystem enables only
its own bit. Interrupts do not nest (the BIOS enters with IME = 0), so **the order inside the
dispatcher is the priority scheme**: DMA1, DMA2, then serial. The serial deadline (re-arming the
slave before the next 1 kHz poll, ~680 µs) has plenty of slack behind even a three-voice render.
Only the IF bits read on entry are acknowledged. 108's polled `link_service()` fallback is
untouched.

### Voices, slots and the mix

Four voice **slots** — V1, V2, DRUM A, DRUM B — each a 2-operator FM voice with its own phases,
feedback history, two ADSRs and a pitch envelope. V1 always plays; normal mode adds V2, drum mode
adds the two drums, so at most three sound at once. A slot switched off by a mode change is not
cut: its gate is forced low and it releases naturally. A slot whose carrier envelope is idle and
whose amplitude has reached zero is skipped entirely, so silence costs nothing.

Each slot renders into a 16-sample buffer, which is added into the left and right accumulators
through its pan gains (constant power, Q8). A final pass shifts the sums to 8 bits and
**saturates**: a wrapped sum is a full-scale spike, a clipped one merely loud.

### The FM voice, in fixed point

The ARM7TDMI at 16.78 MHz has no FPU and no divider, and the payload links `-nostdlib`, so a float
or a variable divide is a link error. Everything is integer:

| Quantity | Format |
|---|---|
| phase | Q32. The full circle is 2³², so the accumulators wrap for free; the top 8 bits index the sine |
| sine | 256 entries, Q15 (±32767), in IWRAM |
| amplitudes | Q16, 0–65535: envelope × level, envelope × index |
| pitch | Q8 semitones → Q32 phase increment via `NOTE_INC[129]`, interpolated |
| pitch envelope | Q16 level × ±48 semitones → Q16 frequency multiplier via `PITCH_MUL[98]` |
| level | `LEVEL_GAIN[16]`: 0 = off, then 2.5 dB per step (linear left most of the fader doing almost nothing) |
| mix | voice output (±32767) × pan gain (≤ 256), summed, `>> 16`, saturated to int8 |

Per sample:

```
m   = SINE[(mPhase + (m[n-1] + m[n-2]) * fbAmt) >> 24]      modulator, DX7-style averaged feedback
c   = SINE[(cPhase + ((m * mAmp) << 3)) >> 24]              carrier, phase-modulated
c  += (noise/2 - c) * noiseWeight >> 4                      only in the noise variant
out = (c * cAmp) >> 16                                      voice output, into the mix
```

Every product fits in 32 bits: `m × mAmp` ≤ 32767 × 65535 < 2³¹, and `<< 3` scales full deviation
to four whole cycles (8π radians, an index of 25). The noise is a shared xorshift32, blended in a
second copy of the unrolled loop so the noise-free path keeps its register budget. The pitch
envelope scales both phase increments by the same factor (via `UMULL`), so the ratio holds through
a kick's sweep.

The envelopes advance once per block (2048 Hz). Attack is a linear step; decay and release
multiply the distance to the target by a Q16 coefficient. Each block's amplitude change is spread
as a linear ramp, stepped every four samples. Every constant that would need `sin()`, `exp()` or a
divide is baked by `payload/gen_fm_tables.py` into `fm_tables.h`. The release coefficients are
searched against the renderer's exact integer recursion, truncation included.

**Checked in a bit-exact model.** A Python port of the integer render, run on the generated tables:

- sideband levels of a static index of 3.5 are within 0.12 LSB of the Bessel values |Jₖ(3.5)| × 128;
- release times are within 3 % of `ENV_MS` from 14 ms to 8 s;
- no intermediate in any voice, the noise blend, the pitch envelope or the mix overflows 32 bits;
- every preset renders without saturating on its own.

A drum-mode groove at the factory levels saturates 0.07 % of samples where kick and pluck peak
together. Trim on the MIX page if that matters.

### Memory and CPU

- **IWRAM:** the two DMA handlers, the dispatcher, the renderer, the sine and pitch tables (~4 KB),
  plus both rings and the voice state (~0.8 KB). It sits at the bottom of IWRAM; the stacks stay
  where 108 put them at the top (user `0x03007E00`, IRQ `0x03007FA0`). `crt0.s` copies `.iwram` in
  from its load address in the image and zeroes `.iwram_bss`, before `.bss`.
- **CPU, estimated from the disassembly:** the clean inner loop is 73 instructions per four
  samples, the noise loop 115, and the stereo mix about 8 per sample per voice. That is roughly
  48 cycles per sample for a voice, 65 with noise, so three voices sounding come to about 30 % of
  the CPU. The CAL page's DSP LOAD measures the real figure (excluding the BIOS dispatch, a few
  dozen cycles per block).
- **Upload:** 38 KB, about 5.8 s (108: 40 KB, 6.1 s).

## How it works

- **Multiboot upload.** The RP2040 runs the documented single-cartridge handshake (sync → header
  → palette/handshake → encrypted payload → CRC) and streams the program into the GBA's EWRAM.
  See `gba_multiboot.cpp`. **`0xC0` must hold a BRANCH, not code** — the BIOS overwrites bytes
  `0xC4` and `0xC5` of the loaded image.
- **Live link.** After boot the RP2040 polls at **1 kHz**, sending the inputs and receiving
  buttons, note and status framed by a 16-bit tag. See `gba_proto.h` and `gba_link.cpp`. The
  inter-word gap is what matters, not the clock: the slave holds one pending transfer and re-arms
  with a read-modify-write, so a poll landing early corrupts the word rather than merely wasting
  it. 2000 words/s is measured clean, so 1 kHz runs at half the proven rate.
- **Two cores.** ComputerCard's 48 kHz audio/CV loop runs on **core 0**; the entire link engine
  runs on **core 1** and talks to core 0 only through the lock-free `GbaShared` struct.
- **Serial IRQ.** The GBA re-arms its slave from the serial interrupt, so it is not deaf while it
  redraws — with the polled path kept as a live fallback. The CAL page reports which is running.
- **Audio In 1/2 edges (the one firmware addition).** The audio pair reaches the GBA only as a
  snapshot every other stream word (~440 Hz), so a 1 ms drum trigger can fall between two of them.
  `main.cpp` therefore runs a Schmitt trigger on each audio input at 48 kHz (up through ~+1 V,
  down through ~+0.5 V) and counts rising edges, as it already did for Pulse In 2. `gba_link.cpp`
  sends the counts and levels in **`GBA_OP_AUDEDGE` (0x09)** as soon as they change — at most one
  word in four, so audio patched into those jacks cannot crowd the input stream off the link —
  and re-announces them every 32 words, so a lost word delays edges rather than losing them. The
  word carries its own 6-bit check, since CONTROL words have none and a phantom edge is a phantom
  drum hit. HELLO advertises it with `GBA_CAP_AUD_EDGES`; without that bit the payload falls back
  to thresholding the snapshots.
- **PIO SPI with baked-in inversion.** The pulse jacks are hardware-inverted and GPIO 8/9/2
  aren't hardware-SPI pins, so the transfer is a PIO state machine (`gba_spi.pio`, mode 3,
  MSB-first, 32-bit). **Pulse In 1 inverts** — established by the GBA's own reply, not by
  inference.

> **Bring-up was not straightforward.** If you are going to touch the transport, read
> [`diagnostics/POSTMORTEM.md`](diagnostics/POSTMORTEM.md) first. Several days went into faults
> that turned out to be in the diagnostics rather than the hardware.

## Patch storage

Sixteen slots in the last flash sector of the RP2040 — 256 bytes each, one sector, because the
sector is the erase unit. A transfer is one byte per link word, each carrying its own index, so a
dropped word leaves a hole the receiver can see rather than silently shifting everything after
it. A save repeats the whole block until the host acknowledges; the host only ever commits a
complete one.

Patches carry a version. A patch saved by an older firmware whose layout has since changed is
**rejected** with `BAD DATA - NOT LOADED` rather than loaded as garbage.

## Building

RP2040 firmware (SDK 2.2.0, toolchain 14_2_Rel1):

```sh
cmake -G Ninja -B build -S .
cmake --build build          # -> build/gba_link.uf2
```

GBA payload → `gba_payload.h`:

```sh
cd payload && ./build.sh     # then rebuild the firmware
```

`fm_tables.h` is generated and committed; after changing the sample rate, the ratio list or the
envelope ladder, regenerate it with `python payload/gen_fm_tables.py` (run from `payload/`).

`build.sh` needs no devkitPro: it uses the Pico SDK's own `arm-none-eabi-gcc` and a bundled
dependency-free `gbafix.py` to insert the Nintendo logo and fix the header complement, both of
which the GBA BIOS validates. See [`payload/README.md`](payload/README.md).

## Files

| File | Role |
|------|------|
| `main.cpp` | ComputerCard subclass (core 0): reads the inputs, drives CV/gate out, LEDs |
| `gba_proto.h` | **The wire protocol, shared verbatim by both sides** |
| `gba_link.h` / `.cpp` | Core-1 link engine + `GbaShared` cross-core state |
| `gba_multiboot.h` / `.cpp` | Multiboot uploader + progress |
| `gba_spi.pio` / `gba_spi.h` | PIO SPI master transport (GPIO 8/9/2) |
| `patch_store.h` / `.cpp` | Sixteen patch slots in the RP2040's flash |
| `payload/link.c` | GBA serial slave, serial IRQ, protocol decode, patch transfer |
| `payload/irq.c` | The shared interrupt dispatcher: audio DMA first, serial second |
| `payload/audio.c` | Direct Sound A + B: Timer 0, DMA1/DMA2, both FIFOs, the rings and their interrupts |
| `payload/fm.c` | The FM renderer: four voice slots, envelopes, pitch envelope, noise, stereo mix (IWRAM) |
| `payload/presets.c` | The 26 factory sounds |
| `payload/fm_tables.h` | Generated by `gen_fm_tables.py`: sine, note increments, ratios, envelope tables |
| `payload/synth.c` | Control layer: the matrix, buttons, drum mode, pitch tracking, the patch |
| `payload/ui.c` | PLAY screen + eight-page editor |
| `payload/diag.c` | Link-characterisation screens (linkrate / bandwidth) |
| `ComputerCard.h` | Vendored HAL (Chris Johnson's library, with Andy's fixes) |

| Document | |
|---|---|
| [`README.md`](README.md) | Quick start and a one-screen tour of every editor page |
| [`BENCH.md`](BENCH.md) | Step-by-step bring-up. **Start here if it does not work** |
| [`diagnostics/`](diagnostics/) | **Ten link diagnostics and the notes behind them** — see its README |
| [`diagnostics/CABLES.md`](diagnostics/CABLES.md) | Cable continuity tables, per cable type |
| [`diagnostics/TESTPLAN.md`](diagnostics/TESTPLAN.md) | Bench procedure |
| [`diagnostics/POSTMORTEM.md`](diagnostics/POSTMORTEM.md) | What went wrong during bring-up, and how each fault was caught |

**The diagnostics ship with the release on purpose.** The link runs over jacks that were never
meant to carry SPI, through a cable whose internal wiring is not standardised, and the failures
are silent — a transposed pair, a missing conductor and a console that simply is not listening
all present as "nothing happens". Guessing between those is expensive; measuring is not. They
also reuse the applet's own transport, so a pass exercises the real code path rather than a
simulation of it.

## Credits & references

By **Andy Jenkinson**, 2026. Built on Chris Johnson's ComputerCard HAL for Tom Whitwell's
Music Thing Workshop Computer.

Multiboot protocol and constants transcribed from the public open-source references
[akkera102/gba_01_multiboot](https://github.com/akkera102/gba_01_multiboot) and the RP2040 port
in [copyrat90/gba-pico-gamepad](https://github.com/copyrat90/gba-pico-gamepad); the PIO SPI core
follows the Raspberry Pi
[pico-examples SPI](https://github.com/raspberrypi/pico-examples/tree/master/pio/spi) CPHA=1
program. Register details throughout from [GBATEK](https://problemkaputt.de/gbatek.htm).

MIT licensed.

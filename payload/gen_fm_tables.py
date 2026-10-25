#!/usr/bin/env python3
"""gen_fm_tables.py — emit payload/fm_tables.h: every constant table the FM voice needs.

ARM7TDMI has no divide instruction and no FPU, and the payload links -nostdlib (no libgcc), so a
runtime divide or a float is a LINK error rather than merely slow. Everything that would need one
is computed here instead and baked in:

  SINE256      one cycle of sine, 256 entries, Q15. Lives in IWRAM next to the renderer.
  NOTE_INC     the Q32 phase increment per sample for every MIDI note at AUDIO_RATE, plus one
               guard entry so note 127 can interpolate upward. Pitch is exponential, so it cannot
               be computed from a note number without exp(); sub-semitone pitch interpolates
               linearly between neighbours, which is under a cent off at its worst point.
  RATIO_Q8     the modulator:carrier frequency ratios the RATIO control steps through.
  ENV_MS       envelope stage times, log-spaced, for the UI and for the two tables below.
  ENV_ATK      linear attack step per envelope block (Q16 full scale / blocks).
  PITCH_MUL    Q16 frequency multiplier for -48..+49 semitones (one guard entry), for the
               pitch envelope: the renderer interpolates it linearly per block.
  LEVEL_GAIN   the mixer's level law: 2.5 dB per step, not linear.
  OP_LEVEL     the 4-op operator level law, DX-style: 0.75 dB per step, 0..99.
  PAN_L/PAN_R  constant-power pan gains, Q8, for pan -7..+7. Centre is 181/256 (-3 dB) per
               side, so two centred voices at full level sum to about 1.4x one - the mixer
               saturates rather than wraps, and the MIX page is where that headroom is set.
  ENV_COEF     Q16 per-block multiplier for the exponential decay and release: after the
               stage's time has elapsed the level has fallen to ENV_SNAP/65535 (about -60 dB),
               which is exactly where the envelope snaps to its target, so the time shown on
               screen is the time it actually takes. Searched against the renderer's exact
               integer recursion, truncation included - see blocks_to_snap().

Regenerate with:  python payload/gen_fm_tables.py
"""

import math

OUT = "fm_tables.h"

AUDIO_RATE = 32768
AUDIO_BLOCK = 16                         # one DMA FIFO block: four words, sixteen 8-bit samples
BLOCK_RATE = AUDIO_RATE / AUDIO_BLOCK    # envelopes advance once per block: 2048 Hz
ENV_SNAP = 64                            # see fm.c: the level at which a stage snaps to its target

RATIOS = [0.5, 1, 1.5, 2, 2.5, 3, 3.5, 4, 5, 6, 7, 8, 9, 11, 13, 16]

ENV_STEPS = 32
ENV_MIN_MS = 2.0
ENV_MAX_MS = 8000.0


def env_ms():
    # Index 0 is INSTANT; 1..31 are log-spaced from ENV_MIN_MS to ENV_MAX_MS, rounded to numbers
    # that read sensibly on screen.
    out = [0]
    n = ENV_STEPS - 1
    for i in range(n):
        ms = ENV_MIN_MS * (ENV_MAX_MS / ENV_MIN_MS) ** (i / (n - 1))
        if ms < 20:
            ms = round(ms)
        elif ms < 200:
            ms = round(ms / 5) * 5
        elif ms < 2000:
            ms = round(ms / 50) * 50
        else:
            ms = round(ms / 500) * 500
        # Rounding can collapse two neighbours at the short end into the same time, which would
        # be a dead step on the control. Keep the ladder strictly increasing.
        ms = max(int(ms), out[-1] + 1)
        out.append(ms)
    return out


def blocks_to_snap(coef):
    # Exactly what fm.c's release does: v = (v * coef) >> 16 until v < ENV_SNAP. The truncation
    # matters - near the bottom it removes up to a whole unit per block, which is as much as the
    # multiply itself does on a long release - so the coefficient is searched against this
    # integer recursion rather than computed from the continuous exponential, which ran every
    # stage about 8% short (a 400 ms release measured 367 ms).
    v, n = 65535, 0
    while v >= ENV_SNAP:
        v = (v * coef) >> 16
        n += 1
    return n


def coef_for(blocks):
    # The largest coefficient that still arrives within the stage's time. blocks_to_snap is
    # monotonic in coef, so a binary search finds it.
    lo, hi = 0, 65535
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if blocks_to_snap(mid) <= blocks:
            lo = mid
        else:
            hi = mid - 1
    return lo


def rows(vals, per, fmt):
    lines = []
    for i in range(0, len(vals), per):
        lines.append("    " + ", ".join(fmt % v for v in vals[i:i + per]) + ",")
    return "\n".join(lines)


def main():
    sine = [int(round(32767 * math.sin(2 * math.pi * i / 256))) for i in range(256)]

    inc = []
    for n in range(129):
        f = 440.0 * 2.0 ** ((n - 69) / 12.0)
        inc.append(int(round(f * 2.0 ** 32 / AUDIO_RATE)))

    ratio_q8 = [int(round(r * 256)) for r in RATIOS]
    ratio_name = [("%g" % r) for r in RATIOS]

    ms = env_ms()
    atk = []
    coef = []
    for t in ms:
        blocks = t * BLOCK_RATE / 1000.0
        if t == 0:
            atk.append(65535)
            coef.append(0)
            continue
        atk.append(max(1, min(65535, int(round(65535 / blocks)))))
        coef.append(coef_for(max(1, int(round(blocks)))))

    with open(OUT, "w", newline="\n") as f:
        f.write("// fm_tables.h - GENERATED by gen_fm_tables.py. Do not edit by hand.\n")
        f.write("//\n")
        f.write("// Every table here replaces a divide, an exp() or a sin() that the payload cannot do at\n")
        f.write("// run time: it links -nostdlib and the ARM7TDMI has neither a divider nor an FPU.\n")
        f.write("#pragma once\n\n")
        f.write("#include <stdint.h>\n")
        f.write("#include \"gba.h\"\n\n")
        f.write("#define AUDIO_RATE  %d\n" % AUDIO_RATE)
        f.write("#define AUDIO_BLOCK %d      // samples per DMA FIFO block, and per envelope step\n" % AUDIO_BLOCK)
        f.write("#define ENV_SNAP    %d      // a decaying stage snaps to its target below this\n\n" % ENV_SNAP)
        f.write("#define NOTE_MIN 0\n")
        f.write("#define NOTE_MAX 127\n\n")

        f.write("// One sine cycle, Q15. In IWRAM: the renderer reads it twice per sample, and an EWRAM\n")
        f.write("// halfword load costs two wait states more than an IWRAM one.\n")
        f.write("static const int16_t SINE256[256] IWRAM_RODATA = {\n")
        f.write(rows(sine, 8, "%6d") + "\n};\n\n")

        f.write("// Q32 phase increment per sample at AUDIO_RATE, per MIDI note. Entry 128 is a guard so\n")
        f.write("// note 127 can interpolate upward without an out-of-range read.\n")
        f.write("static const uint32_t NOTE_INC[129] = {\n")
        f.write(rows(inc, 6, "%10du") + "\n};\n\n")

        f.write("#define RATIO_COUNT %d\n" % len(RATIOS))
        f.write("static const uint16_t RATIO_Q8[RATIO_COUNT] = {\n")
        f.write(rows(ratio_q8, 8, "%4d") + "\n};\n")
        f.write("static const char *const RATIO_NAME[RATIO_COUNT] = {\n")
        f.write(rows(ratio_name, 8, "\"%s\"") + "\n};\n\n")

        pmul = [int(round(65536 * 2.0 ** ((i - 48) / 12.0))) for i in range(98)]
        f.write("// Q16 frequency multiplier, index = semitones + 48 (-48..+49; the last is a guard).\n")
        f.write("#define PITCH_MUL_ZERO 48\n")
        f.write("static const uint32_t PITCH_MUL[98] IWRAM_RODATA = {\n")
        f.write(rows(pmul, 7, "%8du") + "\n};\n\n")

        # Level 1..15 in 2.5 dB steps (15 = 0 dB, 1 = -35 dB), 0 = off. Linear was tried first:
        # 15 -> 8 took only 5.5 dB off, so most of the fader's travel did almost nothing, and a
        # bright drum could not be pulled back into the mix.
        lvl = [0] + [min(65535, int(round(65536 * 10 ** (-(15 - l) * 2.5 / 20)))) for l in range(1, 16)]
        f.write("// Mixer level law, Q16: 0 = off, then 2.5 dB per step up to 15 = 0 dB.\n")
        f.write("static const uint16_t LEVEL_GAIN[16] = {\n")
        f.write(rows(lvl, 8, "%5d") + "\n};\n\n")

        # 4-op operator output level 0..99, DX-style: 0.75 dB per step, 99 = full scale, 0 = off.
        # For a modulator full scale is 8 pi radians of deviation, so 91 is ~12.6 rad, 83 ~6.3,
        # 75 ~3.1, 67 ~1.6: the useful range sits where DX programmers expect it, 60-95.
        opl = [0] + [min(65535, int(round(65536 * 10 ** (-(99 - l) * 0.75 / 20)))) for l in range(1, 100)]
        f.write("// 4-op operator level 0..99, Q16: 0 = off, then 0.75 dB per step up to 99 = full scale.\n")
        f.write("static const uint16_t OP_LEVEL[100] = {\n")
        f.write(rows(opl, 10, "%5d") + "\n};\n\n")

        pan_l, pan_r = [], []
        for p in range(-7, 8):
            a = (p + 7) / 14.0 * math.pi / 2
            pan_l.append(int(round(256 * math.cos(a))))
            pan_r.append(int(round(256 * math.sin(a))))
        f.write("// Constant-power pan, Q8, index = pan + 7 (pan -7..+7).\n")
        f.write("#define PAN_STEPS 15\n")
        f.write("static const uint16_t PAN_L[PAN_STEPS] = {\n")
        f.write(rows(pan_l, 8, "%3d") + "\n};\n")
        f.write("static const uint16_t PAN_R[PAN_STEPS] = {\n")
        f.write(rows(pan_r, 8, "%3d") + "\n};\n\n")

        f.write("#define ENV_STEPS %d\n" % ENV_STEPS)
        f.write("static const uint16_t ENV_MS[ENV_STEPS] = {\n")
        f.write(rows(ms, 8, "%5d") + "\n};\n")
        f.write("// Linear attack: Q16 full scale per envelope block.\n")
        f.write("static const uint16_t ENV_ATK[ENV_STEPS] = {\n")
        f.write(rows(atk, 8, "%5d") + "\n};\n")
        f.write("// Exponential decay/release: Q16 multiplier per envelope block, reaching ENV_SNAP in ENV_MS.\n")
        f.write("static const uint32_t ENV_COEF[ENV_STEPS] = {\n")
        f.write(rows(coef, 8, "%5du") + "\n};\n")

    print("wrote %s" % OUT)


if __name__ == "__main__":
    main()

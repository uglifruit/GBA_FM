// fm.c — the FM renderer, its envelopes and the stereo mix. Runs from IWRAM, inside the DMA
// interrupt.
//
// FIXED POINT THROUGHOUT. There is no FPU and no divider, so every quantity is an integer with an
// agreed scale, and every product is arranged to fit 32 bits:
//
//   phase       Q32: the whole circle is 2^32, so the accumulator wraps for free. The top eight
//               bits index the 256-entry sine.
//   sine        Q15, +-32767.
//   amplitudes  Q16, 0..65535 (envelope x level, envelope x index).
//
//   modulator -> phase offset:  m * mA fits 31 bits (32767 * 65535 < 2^31); "<< 3" turns the
//                               full-scale product into four whole cycles, a peak deviation of
//                               8*pi radians (an FM index of about 25).
//   noise blend:                (n - c) * w >> 4 with |n - c| < 2^16 and w <= 16.
//   carrier -> voice output:    c * cA >> 16, +-32767 (c may reach -32768 with noise; still fits).
//   voice -> mix:               output x pan gain (Q8, <= 256) summed over at most four slots,
//                               < 2^25; ">> 16" lands on the signed 8-bit range, then saturates.
//   feedback:                   (f1 + f2) * fbAmt with fbAmt <= 2^15 still fits 31 bits; the two
//                               most recent modulator outputs are averaged, as the DX7 does, to
//                               stop the feedback loop hunting at high settings.

#include "fm.h"
#include "fm_tables.h"

#define ENV_IDLE 0
#define ENV_ATK  1
#define ENV_DEC  2
#define ENV_SUS  3
#define ENV_REL  4
#define ENV_TOP  65535

typedef struct {
    int32_t  env[2];              // Q16 levels, [FM_MOD] index, [FM_CAR] amplitude
    uint32_t stage[2];
    uint32_t seenOns;
    uint32_t cPh, mPh;
    int32_t  fb1, fb2;            // the last two modulator outputs, newest first
    int32_t  amp[2];              // the level each block's ramp starts from
    int32_t  pe;                  // pitch envelope level, Q16
} VoiceState;

volatile FmControl g_fm[FM_SLOTS];
volatile uint32_t  g_fmActive;
volatile uint16_t  g_fmEnv[FM_SLOTS][2];

static IWRAM_BSS VoiceState s_vs[FM_SLOTS];
static IWRAM_BSS uint32_t   s_noise;       // xorshift32 state, shared by every slot

// always_inline, not merely static inline: if GCC declined to inline this it would emit the body
// in .text, and the interrupt would call out to EWRAM for it.
//
// AN ATTACK ALWAYS COMPLETES. A gate that falls during the attack is acted on when the attack
// reaches the top, not before. Otherwise a short trigger pulse - a millisecond from a sequencer -
// would release a note that had only climbed a fraction of the way, and a percussive patch driven
// from triggers would barely sound.
//
// Decay and release are exponential: each block multiplies the distance to the target by a Q16
// coefficient, so the level falls in equal decibel steps, which is how an acoustic sound dies and
// what an ear hears as an even fade. They snap to the target at ENV_SNAP (about -60 dB), which is
// where the coefficient tables are calibrated to arrive at the time shown on screen.
static inline __attribute__((always_inline))
void env_step(int32_t *pv, uint32_t *ps, int trig, int gate,
              uint32_t atk, uint32_t dec, int32_t sus, uint32_t rel)
{
    int32_t  v = *pv;
    uint32_t s = *ps;

    // Retrigger from wherever the level is, never from zero: a jump to zero mid-note is a click.
    if (trig) s = ENV_ATK;
    else if (!gate && (s == ENV_DEC || s == ENV_SUS)) s = ENV_REL;

    switch (s) {
    case ENV_ATK:
        v += (int32_t)atk;
        if (v >= ENV_TOP) { v = ENV_TOP; s = ENV_DEC; }
        break;
    case ENV_DEC: {
        // 64-bit product: SMULL, inline, no libgcc. A signed 32-bit product of a 17-bit
        // difference and a 16-bit coefficient does not fit.
        int32_t d = (int32_t)(((int64_t)(v - sus) * dec) >> 16);
        v = sus + d;
        if (d < ENV_SNAP && d > -ENV_SNAP) { v = sus; s = ENV_SUS; }
        break;
    }
    case ENV_SUS:
        v = sus;                  // follows a live sustain edit; the block ramp smooths the step
        break;
    case ENV_REL:
        v = (int32_t)(((int64_t)v * rel) >> 16);
        if (v < ENV_SNAP) { v = 0; s = ENV_IDLE; }
        break;
    default:
        v = 0;
        break;
    }

    *pv = v;
    *ps = s;
}

// Q16 frequency multiplier for a Q8 semitone offset (-48..+48), interpolated between semitones.
// Arithmetic shift and mask split a negative offset correctly: -1.5 st is -2 plus 128/256.
static inline __attribute__((always_inline)) uint32_t pitch_mul(int32_t offQ8)
{
    int32_t  i    = (offQ8 >> 8) + PITCH_MUL_ZERO;
    uint32_t frac = (uint32_t)(offQ8 & 0xFF);
    uint32_t a = PITCH_MUL[i], b = PITCH_MUL[i + 1];
    return a + (((b - a) * frac) >> 8);
}

static inline __attribute__((always_inline)) uint32_t scale_inc(uint32_t inc, uint32_t mulQ16)
{
    uint64_t r = ((uint64_t)inc * mulQ16) >> 16;           // UMULL, inline
    return r > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)r;    // past Nyquist only aliases
}

// One modulator/carrier pair. The feedback history is not shuffled through f1/f2 per sample: each
// sample names the two outputs before it directly (m0 uses f1+f2, m1 uses m0+f1, ...), which costs
// no instructions once unrolled.
#define FM_PAIR(m, cs, newer, older)                                           \
    int32_t m  = SINE256[(mp + (uint32_t)(((newer) + (older)) * fbAmt)) >> 24]; \
    int32_t cs = SINE256[(cp + ((uint32_t)(m * mA) << 3)) >> 24];              \
    mp += mi;                                                                  \
    cp += ci;

// xorshift32, then blend: cs moves toward the noise sample by w/16. The noise is HALF scale
// (">> 17"): full-scale white noise at the same peak as a sine puts all its energy where the ear
// is most sensitive, and the noisy presets came out far louder than anything tonal at the same
// level - OP HAT most of all.
#define NOISE_MIX(cs)                                                          \
    nz ^= nz << 13;                                                            \
    nz ^= nz >> 17;                                                            \
    nz ^= nz << 5;                                                             \
    cs += ((((int32_t)nz >> 17) - cs) * nw) >> 4;

// Render one slot's AUDIO_BLOCK samples into out[] (voice output, +-32767). Two copies of the
// unrolled loop rather than a branch inside it: the noise-free path is the common one and keeps
// its register budget.
static IWRAM_CODE void render_voice(VoiceState *s, uint32_t ci, uint32_t mi, int32_t fbAmt,
                                    uint32_t nw, int32_t mTo, int32_t cTo, int32_t *out)
{
    // Linear ramps across the block, stepped once per word (every four samples, 8 kHz - far too
    // fast to hear as steps). Stepping per sample as well cost more live registers than the ARM
    // has, and GCC paid for them with stack traffic on every sample.
    int32_t  mA = s->amp[FM_MOD], cA = s->amp[FM_CAR];
    int32_t  mStep = (mTo - mA) >> 2, cStep = (cTo - cA) >> 2;   // AUDIO_BLOCK / 4 words
    uint32_t mp = s->mPh, cp = s->cPh;
    int32_t  f1 = s->fb1, f2 = s->fb2;

    if (nw == 0) {
        for (int w = 0; w < AUDIO_BLOCK / 4; w++) {
            FM_PAIR(m0, c0, f1, f2)
            FM_PAIR(m1, c1, m0, f1)
            FM_PAIR(m2, c2, m1, m0)
            FM_PAIR(m3, c3, m2, m1)
            f1 = m3;
            f2 = m2;
            out[0] = (c0 * cA) >> 16;
            out[1] = (c1 * cA) >> 16;
            out[2] = (c2 * cA) >> 16;
            out[3] = (c3 * cA) >> 16;
            out += 4;
            mA += mStep;
            cA += cStep;
        }
    } else {
        uint32_t nz = s_noise;
        for (int w = 0; w < AUDIO_BLOCK / 4; w++) {
            FM_PAIR(m0, c0, f1, f2)
            FM_PAIR(m1, c1, m0, f1)
            FM_PAIR(m2, c2, m1, m0)
            FM_PAIR(m3, c3, m2, m1)
            f1 = m3;
            f2 = m2;
            NOISE_MIX(c0)
            NOISE_MIX(c1)
            NOISE_MIX(c2)
            NOISE_MIX(c3)
            out[0] = (c0 * cA) >> 16;
            out[1] = (c1 * cA) >> 16;
            out[2] = (c2 * cA) >> 16;
            out[3] = (c3 * cA) >> 16;
            out += 4;
            mA += mStep;
            cA += cStep;
        }
        s_noise = nz;
    }

    s->mPh = mp;
    s->cPh = cp;
    s->fb1 = f1;
    s->fb2 = f2;
    // Land exactly on the targets rather than keeping the accumulated ramp: the shift above
    // rounds toward minus infinity, and four of those can carry a falling level below zero,
    // where a negative amplitude would flip the waveform.
    s->amp[FM_MOD] = mTo;
    s->amp[FM_CAR] = cTo;
}

IWRAM_CODE void fm_render_block(int8_t *left, int8_t *right)
{
    int32_t accL[AUDIO_BLOCK], accR[AUDIO_BLOCK], vo[AUDIO_BLOCK];
    for (int i = 0; i < AUDIO_BLOCK; i++) { accL[i] = 0; accR[i] = 0; }
    if (s_noise == 0) s_noise = 0x2545F491u;    // xorshift's one forbidden state is zero

    uint32_t active = g_fmActive;

    for (int v = 0; v < FM_SLOTS; v++) {
        VoiceState         *s = &s_vs[v];
        volatile FmControl *c = &g_fm[v];

        // ---- control rate: once per block ----
        // An inactive slot cannot start notes (its note-ons are swallowed) and has its gate held
        // low, so whatever it was playing releases naturally instead of being cut off.
        uint32_t ons  = c->noteOns;
        int      live = (active >> v) & 1;
        int      trig = live && (ons != s->seenOns);
        s->seenOns = ons;                        // several edges inside one block are one note
        int      gate = live && c->gate != 0;

        if (!trig && s->stage[FM_CAR] == ENV_IDLE && s->amp[FM_CAR] == 0) {
            // Silent: nothing to render. Keep the screen honest and move on.
            g_fmEnv[v][FM_MOD] = 0;
            g_fmEnv[v][FM_CAR] = 0;
            continue;
        }

        env_step(&s->env[FM_MOD], &s->stage[FM_MOD], trig, gate, c->atk[FM_MOD],
                 c->dec[FM_MOD], (int32_t)c->sus[FM_MOD], c->rel[FM_MOD]);
        env_step(&s->env[FM_CAR], &s->stage[FM_CAR], trig, gate, c->atk[FM_CAR],
                 c->dec[FM_CAR], (int32_t)c->sus[FM_CAR], c->rel[FM_CAR]);
        g_fmEnv[v][FM_MOD] = (uint16_t)s->env[FM_MOD];
        g_fmEnv[v][FM_CAR] = (uint16_t)s->env[FM_CAR];

        uint32_t ci = c->cInc, mi = c->mInc;

        // Pitch envelope: full on a note start, decaying toward zero; the offset scales both
        // increments by the same factor, so the C:M ratio holds through the sweep.
        int32_t depth = c->pitchEnv;
        if (depth) {
            if (trig) s->pe = ENV_TOP;
            else {
                s->pe = (int32_t)(((uint32_t)s->pe * c->pitchCoef) >> 16);
                if (s->pe < ENV_SNAP) s->pe = 0;
            }
            if (s->pe) {
                uint32_t mul = pitch_mul((s->pe * depth) >> 8);   // +-48 semitones, Q8
                ci = scale_inc(ci, mul);
                mi = scale_inc(mi, mul);
            }
        }

        // Where each amplitude must be by the end of this block. Both products are 16 x 16 bits,
        // unsigned, so they fit before the shift.
        int32_t mTo = (int32_t)(((uint32_t)s->env[FM_MOD] * c->idxAmt) >> 16);
        int32_t cTo = (int32_t)(((uint32_t)s->env[FM_CAR] * c->level)  >> 16);

        render_voice(s, ci, mi, c->fbAmt, c->noise, mTo, cTo, vo);

        int32_t gl = (int32_t)c->gainL, gr = (int32_t)c->gainR;
        for (int i = 0; i < AUDIO_BLOCK; i++) {
            accL[i] += vo[i] * gl;
            accR[i] += vo[i] * gr;
        }
    }

    // ---- the mix: saturate to int8 and pack four samples per word into each ring ----
    // Saturate, never wrap: a wrapped sum is a full-scale spike, a clipped one merely loud.
    uint32_t *outL = (uint32_t *)left, *outR = (uint32_t *)right;
    for (int w = 0; w < AUDIO_BLOCK / 4; w++) {
        uint32_t wl = 0, wr = 0;
        for (int k = 0; k < 4; k++) {
            int32_t l = accL[w * 4 + k] >> 16, r = accR[w * 4 + k] >> 16;
            if (l > 127) l = 127; else if (l < -128) l = -128;
            if (r > 127) r = 127; else if (r < -128) r = -128;
            wl |= ((uint32_t)l & 0xFFu) << (8 * k);
            wr |= ((uint32_t)r & 0xFFu) << (8 * k);
        }
        outL[w] = wl;
        outR[w] = wr;
    }
}

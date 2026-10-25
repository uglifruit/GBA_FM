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

// ---- the 4-operator voice -------------------------------------------------------------------
volatile Fm4Control g_fm4;
volatile uint16_t   g_fm4Env[FM4_OPS];

//                                              alg:  1     2     3     4     5     6     7     8
const uint8_t FM4_CARRIERS[FM4_ALGOS]          = { 0x1,  0x1,  0x1,  0x1,  0x5,  0x7,  0x7,  0xF };
const uint8_t FM4_MODS[FM4_ALGOS][FM4_OPS] = {   // [algo][op1..op4]: which operators modulate it
    { 0x2, 0x4, 0x8, 0 },     // 1: 4>3>2>1
    { 0x2, 0xC, 0,   0 },     // 2: (4+3)>2>1
    { 0xA, 0x4, 0,   0 },     // 3: (4 + 3>2)>1
    { 0x6, 0,   0x8, 0 },     // 4: (4>3 + 2)>1
    { 0x2, 0,   0x8, 0 },     // 5: 4>3 + 2>1
    { 0x8, 0x8, 0x8, 0 },     // 6: 4>(3,2,1)
    { 0,   0,   0x8, 0 },     // 7: 4>3 + 2 + 1
    { 0,   0,   0,   0 },     // 8: 4 + 3 + 2 + 1
};

typedef struct {
    int32_t  env[FM4_OPS];
    uint32_t stage[FM4_OPS];
    uint32_t seenOns;
    uint32_t ph[FM4_OPS];
    int32_t  fb1, fb2;
    int32_t  amp[FM4_OPS];
    int32_t  pe;
} Voice4State;

static IWRAM_BSS Voice4State s_v4;

// Operator k's sample, the phase offset it hands on as a modulator, and its contribution as a
// carrier. All three are the 2-op voice's scales: a modulator's sine x Q16 depth, "<< 3", is up to
// four whole cycles (8 pi); a carrier's sine x Q16 amplitude ">> 16" is +-32767. Two modulators
// feeding one operator simply add their offsets: the sum wraps modulo 2^32, which is modulo one
// cycle, which is exactly right for a phase.
#define OP4_S(p)      SINE256[(p) >> 24]
#define OP4_M(s, a)   ((uint32_t)((s) * (a)) << 3)
#define OP4_O(s, a)   (((s) * (a)) >> 16)

// One sample of each algorithm. Operator 4 has always been computed first (it carries the
// feedback), into s4/m4. KEEP THESE IN STEP WITH FM4_MODS / FM4_CARRIERS ABOVE.
#define ALG1  { int32_t s3 = OP4_S(p3 + m4);                 uint32_t m3 = OP4_M(s3, a3);          \
                int32_t s2 = OP4_S(p2 + m3);                 uint32_t m2 = OP4_M(s2, a2);          \
                int32_t s1 = OP4_S(p1 + m2);                 o = OP4_O(s1, a1); }
#define ALG2  { int32_t s3 = OP4_S(p3);                      uint32_t m3 = OP4_M(s3, a3);          \
                int32_t s2 = OP4_S(p2 + m3 + m4);            uint32_t m2 = OP4_M(s2, a2);          \
                int32_t s1 = OP4_S(p1 + m2);                 o = OP4_O(s1, a1); }
#define ALG3  { int32_t s3 = OP4_S(p3);                      uint32_t m3 = OP4_M(s3, a3);          \
                int32_t s2 = OP4_S(p2 + m3);                 uint32_t m2 = OP4_M(s2, a2);          \
                int32_t s1 = OP4_S(p1 + m2 + m4);            o = OP4_O(s1, a1); }
#define ALG4  { int32_t s3 = OP4_S(p3 + m4);                 uint32_t m3 = OP4_M(s3, a3);          \
                int32_t s2 = OP4_S(p2);                      uint32_t m2 = OP4_M(s2, a2);          \
                int32_t s1 = OP4_S(p1 + m3 + m2);            o = OP4_O(s1, a1); }
#define ALG5  { int32_t s3 = OP4_S(p3 + m4);                                                       \
                int32_t s2 = OP4_S(p2);                      uint32_t m2 = OP4_M(s2, a2);          \
                int32_t s1 = OP4_S(p1 + m2);                 o = OP4_O(s3, a3) + OP4_O(s1, a1); }
#define ALG6  { int32_t s3 = OP4_S(p3 + m4);                                                       \
                int32_t s2 = OP4_S(p2 + m4);                                                       \
                int32_t s1 = OP4_S(p1 + m4);                                                       \
                o = OP4_O(s3, a3) + OP4_O(s2, a2) + OP4_O(s1, a1); }
#define ALG7  { int32_t s3 = OP4_S(p3 + m4);                                                       \
                int32_t s2 = OP4_S(p2);                                                            \
                int32_t s1 = OP4_S(p1);                                                            \
                o = OP4_O(s3, a3) + OP4_O(s2, a2) + OP4_O(s1, a1); }
#define ALG8  { int32_t s3 = OP4_S(p3);                                                            \
                int32_t s2 = OP4_S(p2);                                                            \
                int32_t s1 = OP4_S(p1);                                                            \
                o = OP4_O(s4, a4) + OP4_O(s3, a3) + OP4_O(s2, a2) + OP4_O(s1, a1); }

// The block loop around one algorithm's sample. Amplitudes ramp per word, as the 2-op voice's do.
// The carriers' amplitudes are normalised by 1/sqrt(count) (control layer), so o can exceed
// +-32767 when carriers peak together - at most +-65534 with four at full level, which the int32
// mix carries comfortably before it saturates.
#define FM4_LOOP(BODY)                                                                     \
    for (int w = 0; w < AUDIO_BLOCK / 4; w++) {                                            \
        for (int k = 0; k < 4; k++) {                                                      \
            int32_t  s4 = OP4_S(p4 + (uint32_t)((f1 + f2) * fbAmt));                       \
            uint32_t m4 = OP4_M(s4, a4);                                                   \
            int32_t  o;                                                                    \
            f2 = f1;                                                                       \
            f1 = s4;                                                                       \
            BODY                                                                           \
            (void)m4;                                                                      \
            *out++ = o;                                                                    \
            p1 += i1; p2 += i2; p3 += i3; p4 += i4;                                        \
        }                                                                                  \
        a1 += st1; a2 += st2; a3 += st3; a4 += st4;                                        \
    }

// One copy of the loop per algorithm, chosen once per block: a switch per sample would cost more
// than an operator does.
static IWRAM_CODE void render_4op(Voice4State *s, const uint32_t *inc, int32_t fbAmt,
                                  uint32_t algo, const int32_t *to, int32_t *out)
{
    uint32_t p1 = s->ph[0], p2 = s->ph[1], p3 = s->ph[2], p4 = s->ph[3];
    uint32_t i1 = inc[0], i2 = inc[1], i3 = inc[2], i4 = inc[3];
    int32_t  a1 = s->amp[0], a2 = s->amp[1], a3 = s->amp[2], a4 = s->amp[3];
    int32_t  st1 = (to[0] - a1) >> 2, st2 = (to[1] - a2) >> 2;
    int32_t  st3 = (to[2] - a3) >> 2, st4 = (to[3] - a4) >> 2;
    int32_t  f1 = s->fb1, f2 = s->fb2;

    switch (algo) {
    case 0:  FM4_LOOP(ALG1) break;
    case 1:  FM4_LOOP(ALG2) break;
    case 2:  FM4_LOOP(ALG3) break;
    case 3:  FM4_LOOP(ALG4) break;
    case 4:  FM4_LOOP(ALG5) break;
    case 5:  FM4_LOOP(ALG6) break;
    case 6:  FM4_LOOP(ALG7) break;
    default: FM4_LOOP(ALG8) break;
    }

    s->ph[0] = p1; s->ph[1] = p2; s->ph[2] = p3; s->ph[3] = p4;
    s->fb1 = f1;
    s->fb2 = f2;
    for (int k = 0; k < FM4_OPS; k++) s->amp[k] = to[k];   // land exactly, as render_voice does
}

// Control rate for the 4-op voice, then render into vo[]. Returns 0 if it is silent and was
// skipped (its carriers idle and at zero amplitude).
static IWRAM_CODE int voice4_block(int live, int32_t *vo)
{
    Voice4State        *s = &s_v4;
    volatile Fm4Control *c = &g_fm4;

    uint32_t ons  = c->noteOns;
    int      trig = live && (ons != s->seenOns);
    s->seenOns = ons;
    int      gate = live && c->gate != 0;
    uint32_t algo = c->algo & 7;
    uint32_t car  = FM4_CARRIERS[algo];

    int silent = !trig;
    for (int k = 0; k < FM4_OPS && silent; k++)
        if ((car >> k) & 1) silent = (s->stage[k] == ENV_IDLE && s->amp[k] == 0);
    if (silent) {
        for (int k = 0; k < FM4_OPS; k++) g_fm4Env[k] = 0;
        return 0;
    }

    // Static for the same reason as the mix buffers: the IRQ stack is small.
    static IWRAM_BSS int32_t  to[FM4_OPS];
    static IWRAM_BSS uint32_t inc[FM4_OPS];
    for (int k = 0; k < FM4_OPS; k++) {
        env_step(&s->env[k], &s->stage[k], trig, gate, c->atk[k], c->dec[k],
                 (int32_t)c->sus[k], c->rel[k]);
        g_fm4Env[k] = (uint16_t)s->env[k];
        to[k]  = (int32_t)(((uint32_t)s->env[k] * c->lvl[k]) >> 16);
        inc[k] = c->inc[k];
    }

    int32_t depth = c->pitchEnv;
    if (depth) {
        if (trig) s->pe = ENV_TOP;
        else {
            s->pe = (int32_t)(((uint32_t)s->pe * c->pitchCoef) >> 16);
            if (s->pe < ENV_SNAP) s->pe = 0;
        }
        if (s->pe) {
            uint32_t mul = pitch_mul((s->pe * depth) >> 8);
            for (int k = 0; k < FM4_OPS; k++) inc[k] = scale_inc(inc[k], mul);
        }
    }

    render_4op(s, inc, c->fbAmt, algo, to, vo);
    return 1;
}

// The mix buffers are STATIC, not locals. Everything here runs on the BIOS's IRQ stack, which has
// only the space between 0x03007FA0 and the user stack below it, and these three arrays alone are
// 192 bytes. As locals they took the 4-op path to ~576 bytes - past the 416 the IRQ stack then had,
// into the top of the main loop's stack, which froze the editor the moment 4-op mode came on. The
// interrupt cannot re-enter itself (IME is 0 inside it), so one static copy is safe.
static IWRAM_BSS int32_t s_accL[AUDIO_BLOCK], s_accR[AUDIO_BLOCK], s_vo[AUDIO_BLOCK];

IWRAM_CODE void fm_render_block(int8_t *left, int8_t *right)
{
    int32_t *accL = s_accL, *accR = s_accR, *vo = s_vo;
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

    // ---- the 4-operator voice ----
    if (voice4_block((active & FM_ACTIVE_4OP) != 0, vo)) {
        int32_t gl = (int32_t)g_fm4.gainL, gr = (int32_t)g_fm4.gainR;
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

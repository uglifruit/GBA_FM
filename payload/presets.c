// presets.c — the factory sounds. See presets.h for what loading one changes.
//
// Units, so the table can be read: ratio indexes RATIO_Q8 (0:0.5 1:1 2:1.5 3:2 4:2.5 5:3 6:3.5
// 7:4 8:5 9:6 10:7 11:8 12:9 13:11 14:13 15:16); index is tenths of a radian; envelope stage
// times index ENV_MS (fm_tables.h - e.g. 10 = 25 ms, 16 = 125 ms, 20 = 400 ms, 26 = 2 s, 31 = 8 s);
// sustain is 0..15. Each sound was checked by rendering it through the bit-exact model of the
// renderer before it went in.
//
// Drums set sustain 0 and decay equal to release, so they sound the same whether they are struck
// by a 1 ms trigger or held by a long gate.

#include "presets.h"

typedef struct {
    const char *name;
    uint8_t ratio;
    int8_t  detune;
    uint8_t index, fb, noise;
    int8_t  pitchEnv;
    uint8_t pitchTime;
    uint8_t note;                // 0 = leave the voice's note alone (tuned sounds)
    Adsr    mod, car;
} Preset;

static const Preset PRESET[PRESET_COUNT] = {
    //  name        rat det idx fb nz  penv pt note   mod {a d s r}      car {a d s r}
    // ---- tuned ----
    { "PLUCK",       1,  0, 35, 0, 0,   0,  0,  0, {  0, 22,  3, 18 }, {  2, 25,  9, 20 } },
    { "E.PIANO",     1,  3, 22, 0, 0,   0,  0,  0, {  0, 20,  2, 18 }, {  1, 27,  0, 21 } },
    { "BELL",        6,  0, 50, 0, 0,   0,  0,  0, {  0, 27,  0, 25 }, {  0, 29,  0, 27 } },
    { "TUBULAR",     4,  0, 40, 0, 0,   0,  0,  0, {  0, 25,  2, 24 }, {  0, 30,  0, 28 } },
    { "MARIMBA",     7,  0, 30, 0, 0,   0,  0,  0, {  0, 12,  0, 12 }, {  0, 22,  0, 20 } },
    { "VIBES",       7,  4, 15, 0, 0,   0,  0,  0, {  0, 18,  0, 18 }, {  0, 28,  0, 26 } },
    { "CLAV",        5,  0, 45, 2, 0,   0,  0,  0, {  0, 16,  2, 12 }, {  0, 21,  6, 12 } },
    { "GLASS",      10,  0, 25, 0, 0,   0,  0,  0, {  2, 24,  4, 22 }, {  3, 26,  6, 24 } },
    { "BASS",        1,  0, 40, 3, 0,   0,  0,  0, {  0, 18,  3, 14 }, {  0, 24, 10, 14 } },
    { "SUB BASS",    0,  0, 12, 0, 0,   0,  0,  0, {  0, 20,  2, 14 }, {  0, 22, 13, 14 } },
    { "BRASS",       1,  0, 45, 1, 0,   0,  0,  0, { 16, 23,  9, 18 }, { 12, 22, 12, 18 } },
    { "ORGAN",       3,  0, 12, 4, 0,   0,  0,  0, {  0, 10, 15, 10 }, {  2, 10, 15, 10 } },
    { "FLUTE",       1,  0,  6, 0, 2,   0,  0,  0, { 12, 20, 10, 18 }, { 14, 20, 13, 18 } },
    { "STRINGS",     1,  6, 18, 3, 0,   0,  0,  0, { 22, 24, 12, 23 }, { 23, 24, 13, 24 } },
    // Feedback 5, not 7: the averaged feedback doubles per step, and 7 (about 2 pi effective) is
    // chaos - it measured as broadband noise in the model. 5 is the saw-like region.
    { "SAW LEAD",    1,  0, 22, 5, 0,   0,  0,  0, {  0, 20, 12, 16 }, {  2, 20, 14, 16 } },
    { "PAD",         3,  7, 15, 2, 0,   0,  0,  0, { 25, 26, 10, 26 }, { 26, 26, 13, 27 } },
    // ---- untuned / drums ----
    { "KICK",        1,  0,  8, 0, 0,  36, 13, 33, {  0, 14,  0, 14 }, {  0, 20,  0, 20 } },
    { "SNARE",       2,  0, 20, 0, 9,  12, 10, 50, {  0, 14,  0, 14 }, {  0, 19,  0, 19 } },
    // The hats are struck at C5, not C7: at C7 the 1:11 modulator wanted 23 kHz, was pinned at the
    // 16 kHz Nyquist limit, and with feedback 7 the result was a harsh, aliasing wall of noise
    // that sounded far louder than its level. At C5 the modulator sits at 5.8 kHz.
    { "CL HAT",     13,  0, 60, 5, 10,  0,  0, 72, {  0, 12,  0, 12 }, {  0, 13,  0, 13 } },
    { "OP HAT",     13,  0, 60, 5, 10,  0,  0, 72, {  0, 19,  0, 19 }, {  0, 19,  0, 19 } },
    { "TOM",         1,  0, 10, 0, 0,  14, 16, 45, {  0, 15,  0, 15 }, {  0, 21,  0, 21 } },
    { "RIM",         6,  0, 30, 0, 0,   5,  6, 76, {  0,  8,  0,  8 }, {  0, 11,  0, 11 } },
    { "COWBELL",     2,  0, 25, 0, 0,   0,  0, 68, {  0, 16,  4, 16 }, {  0, 19,  0, 19 } },
    { "METAL",      13,  0, 60, 5, 0,   0,  0, 60, {  0, 24,  4, 24 }, {  0, 25,  0, 25 } },
    { "ZAP",         1,  0, 30, 0, 0,  48, 17, 48, {  0, 18,  0, 18 }, {  0, 19,  0, 19 } },
    { "GONG",        6,  0, 45, 3, 0,   0,  0, 40, {  6, 28,  4, 28 }, {  4, 30,  0, 30 } },
};

// ---- the 4-op voice's presets ----------------------------------------------------------------
// Classic 4-op recipes in the DX21/TX81Z style. Operators are listed op1, op2, op3, op4, each
// { coarse, fine, detune, level, { a, d, s, r } }; ratio = coarse x (1 + fine/100), coarse 0 =
// 0.5; level is 0..99 at 0.75 dB per step. Algorithms are stored 0..7 for 1..8 (see fm.h):
//   alg 5 (4>3 + 2>1) is the workhorse - two independent pairs, e.g. a piano's tine and body;
//   alg 8 is additive, an organ's drawbars; alg 1/2 are deep stacks for brass and bass.
typedef struct {
    const char *name;
    uint8_t algo, fb;
    int8_t  pitchEnv;
    uint8_t pitchTime;
    Op      op[4];
} Preset4;

static const Preset4 PRESET4[PRESET4_COUNT] = {
    // E.PIANO: the classic DX tine. Pair 2>1 is the body, 4>3 the bright "ding" (op4 at 14x,
    // gone in 75 ms); carriers detuned apart for a little chorus.
    { "E.PIANO", 4, 0, 0, 0, {
        { 1,  0,  3, 99, {  0, 26,  0, 20 } },
        { 1,  0,  0, 72, {  0, 23,  2, 18 } },
        { 1,  0, -3, 85, {  0, 19,  0, 15 } },
        { 14, 0,  0, 70, {  0, 14,  0, 12 } } } },
    // E.PIANO 2: harder, brighter tine and a stronger body bark.
    { "E.PIANO 2", 4, 0, 0, 0, {
        { 1,  0,  2, 99, {  0, 26,  0, 20 } },
        { 1,  0,  0, 78, {  0, 22,  3, 18 } },
        { 1,  0, -2, 90, {  0, 20,  0, 16 } },
        { 14, 0,  0, 78, {  0, 15,  0, 12 } } } },
    // BELL: two inharmonic pairs (3.5x and 5.64x) ringing out over seconds.
    { "BELL", 4, 0, 0, 0, {
        { 1,  0,  0, 99, {  0, 30,  0, 28 } },
        { 3, 50,  0, 80, {  0, 28,  0, 26 } },
        { 1,  0,  5, 90, {  0, 27,  0, 25 } },
        { 4, 41,  0, 75, {  0, 26,  0, 24 } } } },
    // TUBULAR: a struck tube - 3.5x for the clang, a long bright decay.
    { "TUBULAR", 4, 0, 0, 0, {
        { 1,  0,  0, 99, {  0, 30,  0, 28 } },
        { 3, 50,  0, 76, {  0, 27,  2, 25 } },
        { 1,  0,  3, 85, {  0, 28,  0, 26 } },
        { 7,  0,  0, 65, {  0, 24,  0, 22 } } } },
    // MARIMBA: woody 4x partial that dies fast over a sine body.
    { "MARIMBA", 4, 0, 0, 0, {
        { 1,  0,  0, 99, {  0, 22,  0, 20 } },
        { 4,  0,  0, 72, {  0, 10,  0, 10 } },
        { 4,  0,  0, 80, {  0, 18,  0, 16 } },
        { 10, 0,  0, 60, {  0,  8,  0,  8 } } } },
    // VIBES: soft 4x shimmer, the second pair slightly detuned for a slow beat.
    { "VIBES", 4, 0, 0, 0, {
        { 1,  0,  0, 99, {  0, 28,  0, 26 } },
        { 4,  0,  0, 60, {  0, 20,  0, 20 } },
        { 1,  0,  4, 85, {  0, 28,  0, 26 } },
        { 4,  0,  0, 55, {  0, 18,  0, 18 } } } },
    // GLOCK: high, glassy, short.
    { "GLOCK", 4, 0, 0, 0, {
        { 2,  0,  0, 99, {  0, 25,  0, 23 } },
        { 7,  0,  0, 70, {  0, 15,  0, 15 } },
        { 5,  0,  0, 85, {  0, 22,  0, 20 } },
        { 14, 0,  0, 60, {  0, 10,  0, 10 } } } },
    // HARP: plucked, an octave partial on the second pair.
    { "HARP", 4, 0, 0, 0, {
        { 1,  0,  0, 99, {  0, 25,  0, 23 } },
        { 2,  0,  0, 70, {  0, 18,  0, 18 } },
        { 1,  0,  0, 92, {  0, 24,  0, 22 } },
        { 4,  0,  0, 62, {  0, 14,  0, 14 } } } },
    // CLAV: nasal 3x and 6x, short and percussive.
    { "CLAV", 4, 0, 0, 0, {
        { 1,  0,  0, 99, {  0, 21,  6, 12 } },
        { 3,  0,  0, 80, {  0, 15,  2, 12 } },
        { 1,  0,  0, 85, {  0, 20,  4, 12 } },
        { 6,  0,  0, 70, {  0, 12,  0, 10 } } } },
    // BASS: punchy - alg 4, with a fed-back 3x into op3 for growl.
    { "BASS", 3, 5, 0, 0, {
        { 1,  0,  0, 99, {  0, 22, 10, 14 } },
        { 1,  0,  0, 75, {  0, 18,  4, 14 } },
        { 1,  0,  0, 70, {  0, 16,  2, 12 } },
        { 3,  0,  0, 60, {  0, 14,  0, 12 } } } },
    // SYN BASS: a full stack with feedback - thick, saw-like, the filter sweep in the envelopes.
    // (Levels 80/72/66 with feedback 5 tipped the stack into noise: it measured BRIGHTER after the
    // attack than during it.)
    { "SYN BASS", 0, 4, 0, 0, {
        { 1,  0,  0, 99, {  0, 22, 11, 14 } },
        { 1,  0,  0, 74, {  0, 19,  6, 14 } },
        { 1,  0,  0, 64, {  0, 17,  4, 14 } },
        { 1,  0,  0, 58, {  0, 15,  2, 12 } } } },
    // BRASS: alg 2, two modulators into one, swelling in behind the carrier.
    { "BRASS", 1, 5, 0, 0, {
        { 1,  0,  0, 99, { 10, 22, 12, 18 } },
        { 1,  0,  3, 78, { 14, 22, 10, 18 } },
        { 1,  0, -3, 65, { 16, 20,  8, 18 } },
        { 1,  0,  0, 60, { 16, 20,  8, 18 } } } },
    // STRINGS: two identical 1:1 pairs detuned +-6 cents, slow attack - ensemble from detuning.
    { "STRINGS", 4, 3, 0, 0, {
        { 1,  0,  6, 95, { 23, 24, 13, 24 } },
        { 1,  0,  0, 70, { 22, 24, 12, 24 } },
        { 1,  0, -6, 95, { 23, 24, 13, 24 } },
        { 1,  0,  0, 70, { 22, 24, 12, 24 } } } },
    // PAD: alg 6, one modulator into three detuned carriers at 1x, 2x and 0.5x.
    { "PAD", 5, 2, 0, 0, {
        { 1,  0,  0, 99, { 26, 26, 13, 27 } },
        { 2,  0,  7, 96, { 26, 26, 13, 27 } },
        { 0,  0, -7, 94, { 26, 26, 13, 27 } },
        { 1,  0,  0, 65, { 27, 26, 10, 27 } } } },
    // ORGAN: alg 8, four drawbars - 16' (0.5x), 8', 4', 2' - with a little feedback grit on the 2'.
    { "ORGAN", 7, 3, 0, 0, {
        { 1,  0,  0, 99, {  2, 10, 15,  8 } },
        { 0,  0,  0, 92, {  2, 10, 15,  8 } },
        { 2,  0,  0, 92, {  2, 10, 15,  8 } },
        { 4,  0,  0, 88, {  2, 10, 15,  8 } } } },
    // FLUTE: alg 4; operator 4 at full feedback is a NOISE source - breath - into op3.
    { "FLUTE", 3, 7, 0, 0, {
        { 1,  0,  0, 99, { 12, 20, 13, 18 } },
        { 1,  0,  0, 55, { 12, 20, 10, 18 } },
        { 3,  0,  0, 50, { 12, 20, 10, 18 } },
        { 1,  0,  0, 45, { 10, 20,  8, 18 } } } },
};

const char *preset4_name(int i)
{
    if (i < 0 || i >= PRESET4_COUNT) return "---";
    return PRESET4[i].name;
}

void preset4_load(Voice4 *v, int i)
{
    if (i < 0 || i >= PRESET4_COUNT) return;
    const Preset4 *p = &PRESET4[i];
    v->preset    = (uint8_t)i;
    v->algo      = p->algo;
    v->fb        = p->fb;
    v->pitchEnv  = p->pitchEnv;
    v->pitchTime = p->pitchTime;
    // Field by field: a struct copy would be a memcpy call, which -nostdlib cannot resolve.
    for (int k = 0; k < 4; k++) {
        const Op *s = &p->op[k];
        Op *d = &v->op[k];
        d->coarse = s->coarse; d->fine = s->fine; d->detune = s->detune; d->level = s->level;
        d->env.atk = s->env.atk; d->env.dec = s->env.dec;
        d->env.sus = s->env.sus; d->env.rel = s->env.rel;
    }
}

const char *preset_name(int i)
{
    if (i < 0 || i >= PRESET_COUNT) return "---";
    return PRESET[i].name;
}

void preset_load(Voice *v, int i)
{
    if (i < 0 || i >= PRESET_COUNT) return;
    const Preset *p = &PRESET[i];
    v->preset    = (uint8_t)i;
    v->ratio     = p->ratio;
    v->detune    = p->detune;
    v->index     = p->index;
    v->fb        = p->fb;
    v->noise     = p->noise;
    v->pitchEnv  = p->pitchEnv;
    v->pitchTime = p->pitchTime;
    if (p->note) v->note = p->note;
    // Field by field rather than struct assignment: GCC turns a struct copy into a memcpy call,
    // which -nostdlib cannot resolve.
    v->env[FM_MOD].atk = p->mod.atk; v->env[FM_MOD].dec = p->mod.dec;
    v->env[FM_MOD].sus = p->mod.sus; v->env[FM_MOD].rel = p->mod.rel;
    v->env[FM_CAR].atk = p->car.atk; v->env[FM_CAR].dec = p->car.dec;
    v->env[FM_CAR].sus = p->car.sus; v->env[FM_CAR].rel = p->car.rel;
}

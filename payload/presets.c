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

// fm.h — the FM voices: four 2-operator (phase modulation) slots, mixed to stereo.
//
//   modulator:  m = sin(mPhase + feedback)                 scaled by the index envelope
//   carrier:    c = sin(cPhase + m * index), noise-blended  scaled by the amplitude envelope
//   mix:        left += c * gainL,  right += c * gainR
//
// Four SLOTS, at most three sounding: V1 always, then either V2 (normal mode) or DRUM A + DRUM B
// (drum mode). Which ones are live is g_fmActive. A slot that is switched off is not cut: it keeps
// rendering, gate forced low, until its release has finished, so changing mode never clicks. A
// slot that is silent - carrier envelope idle, amplitude zero - is skipped entirely.
//
// THE SPLIT BETWEEN CONTROL AND AUDIO. synth_tick() runs at ~1 kHz in the main loop and decides
// everything musical: pitch, ratio, index, level, pan and the envelope rates. It writes the results
// into g_fm[]. The audio interrupt reads each slot once per 16-sample block and owns everything that
// moves at audio rate: phases, feedback history, both envelopes and the pitch envelope, which
// advance once per block (2048 Hz) and are ramped across it so a level change never steps.
//
// Every field has exactly one writer (the control tick) and is 32-bit, so a read is never torn.
// Note-ons are a COUNTER rather than a flag, for the same reason as g_gateEdges in link.h: a flag
// set by one context and cleared by another loses the edge that lands between the two.
#pragma once

#include <stdint.h>

#define FM_SLOTS  4
#define FV_V1     0
#define FV_V2     1
#define FV_DRUM_A 2
#define FV_DRUM_B 3

#define FM_MOD 0
#define FM_CAR 1

typedef struct {
    uint32_t cInc, mInc;          // Q32 phase increment per sample, carrier and modulator
    uint32_t idxAmt;              // Q16: 65535 = a peak phase deviation of 8*pi radians
    uint32_t level;               // Q16 output level
    int32_t  fbAmt;               // modulator self-feedback multiplier, 0 = off
    uint32_t noise;               // noise blend into the carrier, 0..16 (16 = all noise)
    int32_t  pitchEnv;            // pitch envelope depth in semitones, -48..+48, 0 = off
    uint32_t pitchCoef;           // its per-block decay, ENV_COEF
    uint32_t gainL, gainR;        // Q8 pan gains, PAN_L / PAN_R
    uint32_t noteOns;             // advances by one per note start
    uint32_t gate;                // nonzero while the note is held
    // Envelopes, in per-block units from fm_tables.h: [0] modulator (index), [1] carrier (level).
    uint32_t atk[2];              // ENV_ATK
    uint32_t dec[2];              // ENV_COEF
    uint32_t sus[2];              // Q16 sustain level
    uint32_t rel[2];              // ENV_COEF
} FmControl;

extern volatile FmControl g_fm[FM_SLOTS];
extern volatile uint32_t  g_fmActive;        // bit per slot: may start notes

// Published by the audio interrupt, for the screen: the current envelope levels, Q16.
extern volatile uint16_t g_fmEnv[FM_SLOTS][2];

// ---- the 4-operator voice ---------------------------------------------------------------------
// One voice of four sine operators in one of the eight classic Yamaha 4-op algorithms (DX21 /
// TX81Z / YM2151 numbering: operator 4 carries the feedback, operator 1 is always a carrier):
//
//   1: 4>3>2>1            2: (4+3)>2>1          3: (4 + 3>2)>1        4: (4>3 + 2)>1
//   5: 4>3 + 2>1          6: 4>(3,2,1)          7: 4>3 + 2 + 1        8: 4 + 3 + 2 + 1
//
// It is rendered as a fifth slot alongside the four 2-op slots, gated by FM_ACTIVE_4OP in
// g_fmActive, with the same release-don't-cut rule when the mode changes.
#define FM4_OPS        4
#define FM4_ALGOS      8
#define FM_ACTIVE_4OP  (1u << 4)

typedef struct {
    uint32_t inc[FM4_OPS];        // per-operator phase increment, [0] = operator 1
    uint32_t lvl[FM4_OPS];        // Q16 output level: modulators' depth (full = 8 pi), carriers'
                                  // amplitude with the carrier-count normalisation and mixer
                                  // level already folded in
    uint32_t algo;                // 0..7 = algorithms 1..8
    int32_t  fbAmt;               // operator 4 feedback multiplier on its OUTPUT, 0 = off
    int32_t  pitchEnv;            // semitones, -48..+48
    uint32_t pitchCoef;
    uint32_t gainL, gainR;        // Q8 pan gains
    uint32_t noteOns;
    uint32_t gate;
    uint32_t atk[FM4_OPS], dec[FM4_OPS], sus[FM4_OPS], rel[FM4_OPS];
} Fm4Control;

extern volatile Fm4Control g_fm4;
extern volatile uint16_t   g_fm4Env[FM4_OPS];     // live envelope levels, for the screen

// The algorithms as data, for the control layer (carrier normalisation) and the screen (the
// diagram). The renderer does NOT read these - each algorithm is hand-written there for speed -
// so the two must be kept in step; fm.c lists the eight bodies next to each other for that reason.
extern const uint8_t FM4_CARRIERS[FM4_ALGOS];          // bit k = operator k+1 is a carrier
extern const uint8_t FM4_MODS[FM4_ALGOS][FM4_OPS];      // [algo][op]: bit k = operator k+1 modulates it

// AUDIO_BLOCK samples into each ring; called from the DMA interrupt only.
void fm_render_block(int8_t *left, int8_t *right);

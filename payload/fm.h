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

// AUDIO_BLOCK samples into each ring; called from the DMA interrupt only.
void fm_render_block(int8_t *left, int8_t *right);

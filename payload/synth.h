// synth.h — the instrument: voices, the modulation matrix, the buttons and the patch.
//
// Everything musical lives on this side of the link, as it did in 108. The Workshop streams its
// raw inputs; what they DO is decided here, which is why a patch is a struct rather than a
// conversation and the whole editor costs no protocol.
//
// Four voice slots (fm.h): V1, V2, DRUM A, DRUM B. Normal mode plays V1 and V2; drum mode gives
// up V2 and turns AU1/AU2 into the two drum triggers. Which input does what to which voice is the
// MAP page (the matrix below); what each GBA button does is the BTN page. The factory map:
//
//   CV1  -> INDEX V1          CV2  -> PITCH V1 (1V/oct)     PU2 -> GATE V1
//   AU1  -> GATE V2           AU2  -> PITCH V2 (1V/oct)
//   MAIN -> INDEX V1+V2       X    -> RATIO V1              Y   -> DECAY+RELEASE V1+V2
//
// Knobs are CENTRED, as in 108: noon leaves the patch as it is, either side moves it.
#pragma once

#include <stdint.h>
#include "fm.h"

// ---- scales ----
// FREE is index 0 and the factory default: no quantisation at all, so a continuously-varying CV
// glides rather than being pulled onto a grid. It never reaches scale_mask_for(), which is why the
// built-in masks are indexed from 1.
#define SCALE_FREE    0
#define SCALE_BUILTIN 15
#define SCALE_COUNT   (1 + SCALE_BUILTIN)
extern const char *scale_name[SCALE_COUNT];
extern const char *key_name[12];

// ---- modulation sources, in MAP-page order ----
// The first four are g_in[] in LINK_IN_* order. PU2 takes the place 108 gave the switch: its
// value is the gate (full or nothing), so it can open an index as easily as play a note. The
// switch has its own setting (SET page) instead.
#define SRC_CV1   0
#define SRC_CV2   1
#define SRC_AU1   2
#define SRC_AU2   3
#define SRC_PU2   4
#define SRC_MAIN  5
#define SRC_X     6
#define SRC_Y     7
#define SRC_COUNT 8
extern const char *src_name[SRC_COUNT];

// ---- modulation destinations ----
#define DEST_NONE     0
#define DEST_PITCH    1    // depth 32 = unity 1V/oct, through the source's calibration
#define DEST_GATE     2    // depth ignored: the source plays notes
#define DEST_INDEX    3
#define DEST_RATIO    4
#define DEST_LEVEL    5
#define DEST_FB       6
#define DEST_DETUNE   7
#define DEST_NOISE    8
#define DEST_ATTACK   9
#define DEST_DECAY   10
#define DEST_RELEASE 11
#define DEST_DR      12    // decay and release together
#define DEST_GLIDE   13
#define DEST_PAN     14
#define DEST_PENV    15    // pitch envelope depth
#define DEST_COUNT   16
extern const char *dest_name[DEST_COUNT];

// Which voices a mapping reaches. In drum mode the V2 bit reaches both drums.
#define MASK_V1 1
#define MASK_V2 2

typedef struct {
    uint8_t dest;
    int8_t  depth;               // -64..+63
    uint8_t mask;                // MASK_V1 | MASK_V2
} ModSlot;

// ---- GBA button actions (BTN page) ----
#define ACT_NONE     0
#define ACT_TRIG_V1  1
#define ACT_TRIG_V2  2
#define ACT_HOLD_V1  3
#define ACT_HOLD_V2  4
#define ACT_OCTUP_V1 5
#define ACT_OCTDN_V1 6
#define ACT_OCTUP_V2 7
#define ACT_OCTDN_V2 8
#define ACT_RATUP_V1 9
#define ACT_RATDN_V1 10
#define ACT_IDXUP_V1 11
#define ACT_IDXDN_V1 12
#define ACT_FBUP_V1  13
#define ACT_FBDN_V1  14
#define ACT_PREUP_V1 15
#define ACT_PREDN_V1 16
#define ACT_PREUP_V2 17
#define ACT_PREDN_V2 18
#define ACT_TRIG_DA  19
#define ACT_TRIG_DB  20
#define ACT_DRUMS    21
#define ACT_COUNT    22
extern const char *act_name[ACT_COUNT];

#define BTN_SLOTS 8        // A, B, L, R, Up, Down, Left, Right
extern const char *btn_name[BTN_SLOTS];

// The switch: DOWN is a momentary trigger, UP holds the gate open (a drone), for these voices.
// In drum mode, V2 means DRUM A.
#define SW_OFF  0
#define SW_V1   1
#define SW_V2   2
#define SW_BOTH 3
extern const char *sw_name[4];

// ---- the patch ----
typedef struct {
    uint8_t atk, dec, sus, rel;  // atk/dec/rel index ENV_MS (0..ENV_STEPS-1); sus 0..15
} Adsr;

typedef struct {
    uint8_t preset;              // the preset last loaded into this voice (display only)
    uint8_t ratio;               // index into RATIO_Q8: modulator frequency = carrier x ratio
    int8_t  detune;              // modulator detune in cents, -99..+99
    uint8_t index;               // FM index, 0..99 = 0..9.9 radians of peak deviation
    uint8_t fb;                  // modulator self-feedback, 0..7
    uint8_t noise;               // noise blended into the carrier, 0..15
    int8_t  pitchEnv;            // pitch sweep at note start, semitones, -48..+48
    uint8_t pitchTime;           // its decay, an ENV_MS index
    uint8_t note;                // melodic: the note 0 V plays. Drums: the note it is struck at
    int8_t  octave;              // -3..+3
    uint8_t glide;               // portamento, 0..15 (melodic voices)
    uint8_t level;               // mixer level, 0..15
    int8_t  pan;                 // mixer pan, -7..+7
    Adsr    env[2];              // [FM_MOD] index envelope, [FM_CAR] amplitude envelope
} Voice;

// 'FM', not 108's 'GB'. The Workshop's sixteen flash slots are shared by every program that uses
// them, so a 108 patch can be sitting in a slot this program loads. A different magic means it is
// rejected ("BAD DATA - NOT LOADED") instead of being read as an FM patch it never was.
#define PATCH_MAGIC   0x464Du
// BUMP THIS WHENEVER A STORED FIELD CHANGES ITS LAYOUT *OR* ITS MEANING - see CLAUDE.md on why
// that is checked rather than assumed, and on migrating rather than rejecting where possible.
//   v2  two voices, drums, the matrix, the buttons, the mixer. A LAYOUT change: v1 (the
//       single-voice beta, never released) is rejected.
#define PATCH_VERSION 2

typedef struct {
    uint16_t magic;
    uint8_t  version;
    uint8_t  drumMode;           // 0 = V1 + V2, 1 = V1 + DRUM A + DRUM B

    Voice    voice[FM_SLOTS];    // FV_V1, FV_V2, FV_DRUM_A, FV_DRUM_B
    ModSlot  mod[SRC_COUNT];
    uint8_t  btnAct[BTN_SLOTS];
    uint8_t  swTarget;           // SW_*
    uint8_t  scale;
    uint8_t  key;
    int8_t   tuneCents;

    // COUNTS PER SEMITONE IN 1/256ths (Q8), and plain-count offsets - 108's units. Two sets,
    // because the CV and audio inputs are different circuits: CV for CV1/CV2 (and anything not an
    // audio jack), AU for AU1/AU2.
    int16_t  cvScale, cvOffset;
    int16_t  auScale, auOffset;
} Patch;

extern Patch g_patch;

// ---- live state, for the UI ----
extern int32_t  g_pitchQ8;               // V1's tracked pitch before glide, Q8 semitones
extern uint8_t  g_note;                  // V1's nearest (quantised) note, also what goes upstream
extern uint8_t  g_hold[2];               // HOLD latches, V1 and V2
extern uint8_t  g_gateOn[FM_SLOTS];      // per slot: any gate source high
extern uint8_t  g_anyNoteOn;             // V1 gate high or still sounding (CV Out 1, status)
extern uint8_t  g_btnTrigHeld;           // a TRIG button is held
extern uint8_t  g_slotNote[FM_SLOTS];    // the note each slot is playing
extern uint8_t  g_liveRatio[FM_SLOTS];   // ratio index after modulation
extern uint8_t  g_liveIndex[FM_SLOTS];   // index after modulation, tenths of a radian, 0..250

void synth_init(void);
void synth_update(void);
void synth_default_patch(void);       // reset to the factory patch
int  synth_patch_bytes(void);         // sizeof(Patch), for the transfer
int  synth_patch_valid(const Patch *p);
void synth_patch_migrate(Patch *p);   // bring an older-but-compatible patch up to date, in place
void synth_patch_applied(void);       // re-derive anything cached from the patch

// The CAL page's live tuners: what CV In 2 (au = 0) or Audio In 2 (au = 1) means through the
// CURRENT calibration alone, from V1's or V2's 0 V note, ignoring quantisation.
void synth_tuner(int au, int *note, int *cents);

// Button state, sampled at the control rate; the UI drains the latches at frame rate.
extern uint16_t g_btn;
extern volatile uint16_t g_btnEdgeLatch;
extern volatile uint16_t g_btnStepLatch;
uint16_t synth_take_edges(void);
uint16_t synth_take_steps(void);

extern uint8_t g_editMode;

// synth.c — the instrument: turns the patch and the live inputs into the voices' parameters.
//
// The pitch path (calibration, quantiser, glide), the button scanner and the patch housekeeping
// are 108's. The modulation matrix is 108's model - one mapping per source, a depth, a voice mask
// - with FM destinations. What is new is the end of the tick: instead of writing PSG registers it
// fills g_fm[], which the audio interrupt reads once per block.

#include "synth.h"
#include "presets.h"
#include "fm.h"
#include "fm_tables.h"
#include "audio.h"
#include "link.h"
#include "gba.h"

// One patch must fit one flash slot on the Workshop, because the flash sector is the erase unit
// and sixteen slots are one sector.
typedef char patch_fits_a_slot[(sizeof(Patch) <= GBA_PATCH_SLOT_BYTES) ? 1 : -1];

#define REG_BASE     0x04000000
#define REG_KEYINPUT (*(volatile uint16_t *)(REG_BASE + 0x0130))
// TIMER 2, NOT 0. 108 ran its control clock on Timer 0; here Timer 0 is the audio sample clock
// (Direct Sound can only be paced by Timer 0 or 1), so the control clock moved here unchanged.
#define REG_TM2CNT_L (*(volatile uint16_t *)(REG_BASE + 0x0108))
#define REG_TM2CNT_H (*(volatile uint16_t *)(REG_BASE + 0x010A))
// Timer 3 free-runs at the CPU clock for the load meters (audio.c starts it).
#define REG_TM3CNT_L (*(volatile uint16_t *)(REG_BASE + 0x010C))

#define KEY_A      (1u << 0)
#define KEY_B      (1u << 1)
#define KEY_SELECT (1u << 2)
#define KEY_START  (1u << 3)
#define KEY_RIGHT  (1u << 4)
#define KEY_LEFT   (1u << 5)
#define KEY_UP     (1u << 6)
#define KEY_DOWN   (1u << 7)
#define KEY_R      (1u << 8)
#define KEY_L      (1u << 9)

static const uint16_t BTN_BIT[BTN_SLOTS] = {
    KEY_A, KEY_B, KEY_L, KEY_R, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT
};
const char *btn_name[BTN_SLOTS] = { "A", "B", "L", "R", "UP", "DOWN", "LEFT", "RIGHT" };

const char *src_name[SRC_COUNT] = {
    "CV 1", "CV 2", "AUD 1", "AUD 2", "PULSE 2", "MAIN", "KNOB X", "KNOB Y"
};
const char *dest_name[DEST_COUNT] = {
    "---", "PITCH", "GATE", "INDEX", "RATIO", "LEVEL", "FEEDBACK", "DETUNE",
    "NOISE", "ATTACK", "DECAY", "RELEASE", "DEC+REL", "GLIDE", "PAN", "P.ENV"
};
const char *act_name[ACT_COUNT] = {
    "---", "TRIG V1", "TRIG V2", "HOLD V1", "HOLD V2",
    "OCT + V1", "OCT - V1", "OCT + V2", "OCT - V2",
    "RATIO + V1", "RATIO - V1", "INDEX + V1", "INDEX - V1", "FB + V1", "FB - V1",
    "PRESET + V1", "PRESET - V1", "PRESET + V2", "PRESET - V2",
    "TRIG DRUM A", "TRIG DRUM B", "DRUM MODE", "4-OP MODE"
};
const char *sw_name[4] = { "OFF", "V1", "V2", "BOTH" };

const char *key_name[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

const char *scale_name[SCALE_COUNT] = {
    "FREE",
    "CHROMATIC", "MAJOR", "DORIAN", "PHRYGIAN", "LYDIAN", "MIXOLYD", "MINOR", "LOCRIAN",
    "HARM MIN", "PENTA MAJ", "PENTA MIN", "BLUES", "HIRAJOSHI", "IN SEN", "WHOLE"
};

// Scale membership as a 12-bit mask: bit n set if semitone n above the key is in the scale.
static const uint16_t SCALE_MASK[SCALE_BUILTIN] = {
    0x0FFF,   // chromatic    everything
    0x0AB5,   // major        0 2 4 5 7 9 11
    0x06AD,   // dorian       0 2 3 5 7 9 10
    0x05AB,   // phrygian     0 1 3 5 7 8 10
    0x0AD5,   // lydian       0 2 4 6 7 9 11
    0x06B5,   // mixolydian   0 2 4 5 7 9 10
    0x05AD,   // aeolian      0 2 3 5 7 8 10
    0x056B,   // locrian      0 1 3 5 6 8 10
    0x09AD,   // harm minor   0 2 3 5 7 8 11
    0x0295,   // penta major  0 2 4 7 9
    0x04A9,   // penta minor  0 3 5 7 10
    0x04E9,   // blues        0 3 5 6 7 10
    0x018D,   // hirajoshi    0 2 3 7 8
    0x04A3,   // in sen       0 1 5 7 10
    0x0555,   // whole tone   0 2 4 6 8 10
};

// In IWRAM, with the tick that reads it a thousand times a second. bss, so crt0 zeroes it.
IWRAM_BSS Patch g_patch;

int32_t g_pitchQ8     = 60 << 8;
uint8_t g_note        = 60;
uint8_t g_hold[2]     = { 0, 0 };
uint8_t g_gateOn[FM_SLOTS];
uint8_t g_anyNoteOn   = 0;
uint8_t g_btnTrigHeld = 0;
uint8_t g_slotNote[FM_SLOTS];
uint8_t g_liveRatio[FM_SLOTS];
uint8_t g_liveIndex[FM_SLOTS];

// After glide: one per 2-op slot, and [FM_SLOTS] for the 4-op voice.
static int32_t g_voicePitch[FM_SLOTS + 1];
#define GLIDE_4OP FM_SLOTS

uint16_t g_btn = 0;
static uint16_t g_btnEdge = 0, g_btnRep = 0, g_btnStep = 0;
volatile uint16_t g_btnEdgeLatch = 0;
volatile uint16_t g_btnStepLatch = 0;
uint8_t g_editMode = 0;

uint16_t synth_take_edges(void) { uint16_t v = g_btnEdgeLatch; g_btnEdgeLatch = 0; return v; }
uint16_t synth_take_steps(void) { uint16_t v = g_btnStepLatch; g_btnStepLatch = 0; return v; }

// ---- integer divide ---------------------------------------------------------------------
// We link -nostdlib, so libgcc is absent: a divide by a VARIABLE is an undefined reference at
// LINK time rather than merely slow code. Divides by compile-time constants are fine.
static uint32_t udiv32(uint32_t n, uint32_t d)
{
    if (d == 0) return 0;
    uint32_t q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= (1u << i); }
    }
    return q;
}

// ---- calibration ------------------------------------------------------------------------------
// Two sets, because the CV and audio inputs are different circuits: [0] CV (CV1, CV2, and the
// knobs/PU2 if anyone maps them to pitch), [1] AU (AU1, AU2).
#define CAL_CV 0
#define CAL_AU 1
static int32_t g_recip[2]    = { 0, 0 };
static int16_t g_recipFor[2] = { 0, 0 };

// Helpers the IWRAM tick calls are IWRAM_CODE or ALWAYS inlined: a plain static inline can be left
// out of line in EWRAM (CLAUDE.md, GBA gotcha 3).
#define ALWAYS_INLINE static inline __attribute__((always_inline))
ALWAYS_INLINE int16_t cal_scale(int set)  { return set ? g_patch.auScale  : g_patch.cvScale; }
ALWAYS_INLINE int16_t cal_offset(int set) { return set ? g_patch.auOffset : g_patch.cvOffset; }

IWRAM_CODE static void refresh_recips(void)
{
    for (int set = 0; set < 2; set++) {
        int16_t sc = cal_scale(set);
        if (sc == g_recipFor[set]) continue;
        g_recipFor[set] = sc;
        int32_t s = sc;
        // Scale is counts per semitone in 1/256ths (Q8), so semiQ8 = raw * 256 * 256 / scale,
        // and the reciprocal that ">> 12" then divides by is (65536 << 12) / scale = 2^28 / s.
        //
        // Floor of 1024 is an OVERFLOW guard: the pitch maths multiplies the reciprocal by up to
        // 4095 counts, and at 1024 that peaks near 1.07e9, inside int32.
        if (s < 1024) s = 1024;
        g_recip[set] = (int32_t)udiv32(65536u << 12, (uint32_t)s);
    }
}

void synth_patch_applied(void) { g_recipFor[0] = g_recipFor[1] = 0; refresh_recips(); }

ALWAYS_INLINE int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

ALWAYS_INLINE int32_t semis_q8(int set, int32_t raw)
{
    return ((raw - cal_offset(set)) * g_recip[set]) >> 12;
}

void synth_tuner(int au, int *note, int *cents)
{
    refresh_recips();       // pick up a SCALE edit immediately, not next tick
    int     set     = au ? CAL_AU : CAL_CV;
    int32_t raw     = (int32_t)g_in[au ? LINK_IN_AUD2 : LINK_IN_CV2] - 2048;
    int32_t pitchQ8 = ((int32_t)g_patch.voice[au ? FV_V2 : FV_V1].note << 8) + semis_q8(set, raw);
    int32_t n       = clampi((pitchQ8 + 128) >> 8, 0, 127);
    *note  = (int)n;
    *cents = (int)(((pitchQ8 - (n << 8)) * 100) >> 8);
}

int synth_patch_bytes(void) { return (int)sizeof(Patch); }

// v2 is accepted as well as v3: v3 only APPENDED the 4-op voice, so a v2 patch's bytes are all in
// the right places and synth_patch_migrate() fills in the rest. v1 (a different layout) is not.
int synth_patch_valid(const Patch *p)
{
    return p->magic == PATCH_MAGIC && p->version >= 2 && p->version <= PATCH_VERSION;
}

static void default_v4(Voice4 *v)
{
    v->note   = 36;
    v->octave = 0;
    v->glide  = 0;
    v->level  = 14;
    v->pan    = 0;
    preset4_load(v, 0);                        // E.PIANO
}

// Bring a loaded patch up to the current version IN PLACE.
//
// v2 -> v3: everything after auOffset is new. A v2 slot arrives zero-padded past the end of its
// patch, so those bytes read as zeros - voiceMode 0 is already right, but an all-zero 4-op voice
// would be silent (every level 0), so it gets the factory one.
void synth_patch_migrate(Patch *p)
{
    if (p->version == 2) {
        p->voiceMode = VMODE_2OP;
        default_v4(&p->v4);
    }
    p->version = PATCH_VERSION;
}

// ---- the factory patch ----------------------------------------------------------------------
static void set_map(Patch *p, int src, int dest, int depth, int mask)
{
    p->mod[src].dest  = (uint8_t)dest;
    p->mod[src].depth = (int8_t)depth;
    p->mod[src].mask  = (uint8_t)mask;
}

void synth_default_patch(void)
{
    Patch *p = &g_patch;

    for (unsigned i = 0; i < sizeof(Patch); i++) ((uint8_t *)p)[i] = 0;
    p->magic   = PATCH_MAGIC;
    p->version = PATCH_VERSION;

    // Voices: a pluck on V1 and a bass on V2, slightly apart in the stereo field; a kick and a
    // snare waiting in the drum slots. Levels leave headroom for both voices peaking together -
    // the mixer saturates rather than wraps, but saturation is still distortion.
    static const uint8_t preset[FM_SLOTS] = { 0, 8, 16, 17 };    // PLUCK, BASS, KICK, SNARE
    // 14 each (-2.5 dB on the level law). The model's drum-mode groove at these levels saturates
    // 0.07% of samples, where kick and pluck peak together; any hotter and it gets worse.
    static const uint8_t level[FM_SLOTS]  = { 14, 14, 14, 14 };
    static const int8_t  pan[FM_SLOTS]    = { -2, 2, 0, 1 };
    for (int v = 0; v < FM_SLOTS; v++) {
        Voice *vc = &p->voice[v];
        vc->note   = 36;          // 0 V = C2; drum presets set their own
        vc->level  = level[v];
        vc->pan    = pan[v];
        preset_load(vc, preset[v]);
    }

    // The map (synth.h has the table). MAIN rides the index of both melodic voices - and of both
    // drums in drum mode - so the most obvious knob is a brightness control for the whole
    // instrument. Knobs are centred: noon is the patch as stored.
    for (int s = 0; s < SRC_COUNT; s++) set_map(p, s, DEST_NONE, 0, MASK_V1);
    set_map(p, SRC_CV1,  DEST_INDEX, 63, MASK_V1);
    set_map(p, SRC_CV2,  DEST_PITCH, 32, MASK_V1);            // unity 1V/oct
    set_map(p, SRC_AU1,  DEST_GATE,   0, MASK_V2);
    set_map(p, SRC_AU2,  DEST_PITCH, 32, MASK_V2);            // unity 1V/oct
    set_map(p, SRC_PU2,  DEST_GATE,   0, MASK_V1);
    set_map(p, SRC_MAIN, DEST_INDEX, 63, MASK_V1 | MASK_V2);
    set_map(p, SRC_X,    DEST_RATIO, 63, MASK_V1);
    set_map(p, SRC_Y,    DEST_DR,    32, MASK_V1 | MASK_V2);

    // Buttons: phase 1's fixed layout, now just the default.
    p->btnAct[0] = ACT_TRIG_V1;    // A
    p->btnAct[1] = ACT_HOLD_V1;    // B
    p->btnAct[2] = ACT_FBDN_V1;    // L
    p->btnAct[3] = ACT_FBUP_V1;    // R
    p->btnAct[4] = ACT_OCTUP_V1;   // Up
    p->btnAct[5] = ACT_OCTDN_V1;   // Down
    p->btnAct[6] = ACT_RATDN_V1;   // Left
    p->btnAct[7] = ACT_RATUP_V1;   // Right

    p->voiceMode = VMODE_2OP;
    default_v4(&p->v4);

    p->swTarget  = SW_V1;
    p->scale     = SCALE_FREE;     // no quantiser: the factory patch glides straight off the CV
    p->key       = 0;
    p->tuneCents = 0;

    // COUNTS PER SEMITONE, Q8, AND THE DC TRIM - for Andy's module (CV offset -27, measured on
    // the CAL page). A starting point, not a calibration: TRIM THEM ON EVERY UNIT with the CAL
    // page's live tuners (two readings a known interval apart give SCALE directly as
    // 256 * (high - low) / semitones). The AU pair starts from the CV values and needs its own trim.
    //
    // AU OFFSET -37, MEASURED BY ANDY ON THIS MODULE (2026-10-25). Not 0: shipped at 0 it put V1 93
    // cents above V2 at every voltage (~29 counts per semitone). Not the CV value either: -27 was
    // the first guess on the grounds that both pairs share one ADC, but the audio inputs' own
    // circuit adds its own offset, and -37 is what plays in tune. Trim it per unit.
    p->cvScale  = 7422;
    p->cvOffset = -27;
    p->auScale  = 7422;
    p->auOffset = -37;
}

void synth_init(void)
{
    synth_default_patch();
    synth_patch_applied();
    audio_init();

    // Free-running control clock: 16.78 MHz / 1024 = 16384 Hz, so 16 ticks is 1.024 kHz. Paced
    // off a timer rather than VBlank because 60 Hz modulation on a CV-driven voice is steppy.
    REG_TM2CNT_L = 0;
    REG_TM2CNT_H = 0x0083;
}

// ---- buttons ----
static uint16_t g_btnPrev = 0;
static uint16_t g_repTimer[16];

IWRAM_CODE static void scan_buttons(void)
{
    uint16_t now = (uint16_t)((~REG_KEYINPUT) & 0x03FFu);
    g_btnEdge = (uint16_t)(now & ~g_btnPrev);
    g_btnRep  = 0;

    for (int b = 0; b < 10; b++) {
        uint16_t m = (uint16_t)(1u << b);
        if (!(now & m)) { g_repTimer[b] = 0; continue; }
        if (g_btnEdge & m) { g_repTimer[b] = 300; continue; }
        if (g_repTimer[b]) {
            if (--g_repTimer[b] == 0) { g_btnRep |= m; g_repTimer[b] = 40; }
        }
    }

    g_btn     = now;
    g_btnPrev = now;
    g_btnStep = (uint16_t)(g_btnEdge | g_btnRep);
    g_btnEdgeLatch |= g_btnEdge;
    g_btnStepLatch |= g_btnStep;
    link_set_buttons(now);
}

// ---- pitch ----------------------------------------------------------------------------------
// The Q32 phase increment for a Q8 pitch. NOTE_INC is exponential in the note number, so
// interpolating linearly between neighbours is off by at most ~0.7 cents mid-semitone. The 64-bit
// product compiles to UMULL - one instruction, no libgcc.
IWRAM_CODE static uint32_t inc_for(int32_t pitchQ8)
{
    pitchQ8 = clampi(pitchQ8, (int32_t)NOTE_MIN << 8, (int32_t)NOTE_MAX << 8);
    int32_t  n    = pitchQ8 >> 8;
    uint32_t frac = (uint32_t)(pitchQ8 & 0xFF);
    uint32_t a = NOTE_INC[n], b = NOTE_INC[n + 1];
    return a + (uint32_t)(((uint64_t)(b - a) * frac) >> 8);
}

// idx is never SCALE_FREE here - the call sites skip quantise() for it, since it would otherwise
// land on SCALE_MASK[-1].
ALWAYS_INLINE uint16_t scale_mask_for(int idx)
{
    return SCALE_MASK[(idx - 1) % SCALE_BUILTIN];    // constant divisor
}

// Snap a note to the NEAREST degree of the current key and scale, not the one below it. Rounding
// down makes a slow upward CV sweep hang on each degree until it is a full step past.
IWRAM_CODE static int32_t quantise(int32_t note, int scaleIdx, int key)
{
    uint16_t mask = scale_mask_for(scaleIdx);
    int32_t  rel  = note - key;
    int32_t  oct  = rel / 12;                  // constant divisor
    int32_t  deg  = rel - oct * 12;
    if (deg < 0) { deg += 12; oct--; }

    for (int d = 0; d < 12; d++) {
        int32_t up = deg + d, dn = deg - d;
        int32_t k, adj;
        // Down first on a tie, so a note exactly between two degrees resolves consistently.
        k = dn; adj = 0;
        if (k < 0) { k += 12; adj = -12; }
        if (mask & (1u << k)) return key + oct * 12 + k + adj;
        k = up; adj = 0;
        if (k > 11) { k -= 12; adj = 12; }
        if (mask & (1u << k)) return key + oct * 12 + k + adj;
    }
    return note;
}

// A modulation amount read as whole steps of 2^shift, with hysteresis. A stepped control on a
// noisy 12-bit reading flickers between neighbours wherever the knob or CV is parked on a
// boundary - for the ratio that is the pitch of the modulator jumping back and forth. The step
// only moves once the reading is a quarter of a step past the edge of the current one.
ALWAYS_INLINE int32_t stepped(int32_t x, int shift, int32_t *cur)
{
    int32_t margin = (1 << shift) >> 2;
    if (margin < 1) margin = 1;
    int32_t lo = *cur * (1 << shift);          // *cur can be negative: multiply, never <<
    int32_t hi = lo + (1 << shift);
    if (x < lo - margin || x >= hi + margin) *cur = x >> shift;
    return *cur;
}

// ---- one voice slot -------------------------------------------------------------------------
// Turns one Voice, its share of the matrix (md[], indexed by DEST_*) and a pitch into g_fm[slot].
// Every destination's amount is the matrix value v (about +-63 at full depth) scaled to its own
// units, chosen so full depth sweeps the whole useful range of the control.
static int32_t g_ratioStep[FM_SLOTS];

IWRAM_CODE static void voice_out(int slot, const Voice *vc, const int32_t *md, int32_t pitchQ8,
                      int gate, int edge)
{
    volatile FmControl *c = &g_fm[slot];

    int32_t ratio = clampi((int32_t)vc->ratio + stepped(md[DEST_RATIO], 3, &g_ratioStep[slot]),
                           0, RATIO_COUNT - 1);
    int32_t index = clampi((int32_t)vc->index + md[DEST_INDEX], 0, 250);     // tenths of a radian
    int32_t det   = clampi((int32_t)vc->detune + md[DEST_DETUNE] * 2, -99, 99);
    int32_t fb    = clampi((int32_t)vc->fb + (md[DEST_FB] >> 3), 0, 7);
    int32_t noise = clampi((int32_t)vc->noise + (md[DEST_NOISE] >> 2), 0, 15);
    int32_t level = clampi((int32_t)vc->level + (md[DEST_LEVEL] >> 2), 0, 15);
    int32_t pan   = clampi((int32_t)vc->pan + (md[DEST_PAN] >> 2), -7, 7);
    int32_t penv  = clampi((int32_t)vc->pitchEnv + (md[DEST_PENV] >> 1), -48, 48);
    int32_t atkOf = md[DEST_ATTACK] >> 1;                                    // +-32 ladder steps
    int32_t decOf = (md[DEST_DECAY] + md[DEST_DR]) >> 1;
    int32_t relOf = (md[DEST_RELEASE] + md[DEST_DR]) >> 1;

    g_liveRatio[slot] = (uint8_t)ratio;
    g_liveIndex[slot] = (uint8_t)index;

    uint32_t cInc  = inc_for(pitchQ8);
    uint32_t mBase = inc_for(pitchQ8 + ((det * 256) / 100));                 // constant divisor
    uint64_t mInc  = ((uint64_t)mBase * RATIO_Q8[ratio]) >> 8;
    if (mInc > 0x7FFFFFFFu) mInc = 0x7FFFFFFFu;   // past Nyquist it would only alias back down

    // Timing parameters before the gate, the gate before the note-on, so the interrupt never sees
    // a note start against a stale gate.
    c->cInc      = cInc;
    c->mInc      = (uint32_t)mInc;
    // 65536 / (8 * pi) / 10 = 260.8 per tenth of a radian; 261 is 0.07% high, checked against
    // Bessel sideband levels in a bit-exact model of the renderer. 250 * 261 = 65250 (25 rad).
    c->idxAmt    = (uint32_t)index * 261u;
    c->level     = LEVEL_GAIN[level];            // 2.5 dB per step, not linear - see the table
    c->fbAmt     = fb ? (1 << (8 + fb)) : 0;
    c->noise     = (noise >= 15) ? 16u : (uint32_t)noise;
    c->pitchEnv  = penv;
    c->pitchCoef = ENV_COEF[clampi(vc->pitchTime, 1, ENV_STEPS - 1)];
    c->gainL     = PAN_L[pan + 7];
    c->gainR     = PAN_R[pan + 7];

    for (int e = 0; e < 2; e++) {
        const Adsr *a = &vc->env[e];
        c->atk[e] = ENV_ATK[clampi((int32_t)a->atk + atkOf, 0, ENV_STEPS - 1)];
        c->dec[e] = ENV_COEF[clampi((int32_t)a->dec + decOf, 0, ENV_STEPS - 1)];
        c->sus[e] = (uint32_t)clampi(a->sus, 0, 15) * 4369u;
        c->rel[e] = ENV_COEF[clampi((int32_t)a->rel + relOf, 0, ENV_STEPS - 1)];
    }

    c->gate = (uint32_t)gate;
    if (edge) c->noteOns = c->noteOns + 1;
    g_gateOn[slot] = (uint8_t)gate;
}

// The 4-op voice's counterpart of voice_out(). The matrix destinations mean the same things where
// they can: INDEX moves every MODULATOR's level (half a step per unit, so full depth is about
// +-23 dB of modulation), RATIO steps every modulator's coarse ratio, DETUNE detunes the
// modulators, FEEDBACK is operator 4's, and the envelope offsets reach all four operators. NOISE
// has nothing to act on: the 4-op voice is sine only.
static int32_t g_ratioStep4 = 0;

IWRAM_CODE static void voice4_out(const Voice4 *v, const int32_t *md, int32_t pitchQ8, int gate, int edge)
{
    volatile Fm4Control *c = &g_fm4;
    uint32_t algo = v->algo & 7;
    uint8_t  car  = FM4_CARRIERS[algo];
    int      ncar = 0;
    for (int k = 0; k < 4; k++) ncar += (car >> k) & 1;
    // Carriers are scaled by 1/sqrt(count): partials at different frequencies add in POWER, so this
    // keeps a two- or four-carrier sound about as loud as a one-carrier sound. 1/count (tried
    // first) never clipped but left ORGAN and the alg 5 pianos 6-9 dB quieter than BASS. The cost
    // is that carriers peaking together can exceed full scale; the mix saturates, it never wraps.
    static const uint16_t NORM[5] = { 0, 65535, 46341, 37837, 32768 };

    int32_t idxOf = md[DEST_INDEX] >> 1;
    int32_t ratOf = stepped(md[DEST_RATIO], 3, &g_ratioStep4);
    int32_t detOf = md[DEST_DETUNE] * 2;
    int32_t fb    = clampi((int32_t)v->fb + (md[DEST_FB] >> 3), 0, 7);
    int32_t level = clampi((int32_t)v->level + (md[DEST_LEVEL] >> 2), 0, 15);
    int32_t pan   = clampi((int32_t)v->pan + (md[DEST_PAN] >> 2), -7, 7);
    int32_t penv  = clampi((int32_t)v->pitchEnv + (md[DEST_PENV] >> 1), -48, 48);
    int32_t atkOf = md[DEST_ATTACK] >> 1;
    int32_t decOf = (md[DEST_DECAY] + md[DEST_DR]) >> 1;
    int32_t relOf = (md[DEST_RELEASE] + md[DEST_DR]) >> 1;
    uint32_t mix  = LEVEL_GAIN[level];

    for (int k = 0; k < 4; k++) {
        const Op *o   = &v->op[k];
        int       isC = (car >> k) & 1;
        int32_t coarse = clampi((int32_t)o->coarse + (isC ? 0 : ratOf), 0, 31);
        int32_t det    = clampi((int32_t)o->detune + (isC ? 0 : detOf), -99, 99);
        int32_t lv     = clampi((int32_t)o->level + (isC ? 0 : idxOf), 0, 99);

        // Ratio x 256: coarse (0 = 0.5) x (1 + fine/100). Constant divisor.
        uint32_t rq8 = ((coarse ? (uint32_t)coarse * 256u : 128u) * (100u + o->fine)) / 100u;
        uint64_t inc = ((uint64_t)inc_for(pitchQ8 + ((det * 256) / 100)) * rq8) >> 8;
        c->inc[k] = inc > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)inc;

        uint32_t l = OP_LEVEL[lv];
        if (isC) l = (((l * NORM[ncar]) >> 16) * mix) >> 16;
        c->lvl[k] = l;

        c->atk[k] = ENV_ATK[clampi((int32_t)o->env.atk + atkOf, 0, ENV_STEPS - 1)];
        c->dec[k] = ENV_COEF[clampi((int32_t)o->env.dec + decOf, 0, ENV_STEPS - 1)];
        c->sus[k] = (uint32_t)clampi(o->env.sus, 0, 15) * 4369u;
        c->rel[k] = ENV_COEF[clampi((int32_t)o->env.rel + relOf, 0, ENV_STEPS - 1)];
    }

    c->algo      = algo;
    // YM2151/YM2414 feedback: the SUM of operator 4's last two outputs, shifted down by (10 - FB)
    // into a 10-bit phase - so FB 7 at full output is 4 pi, FB 5 is pi, each step halves it - and
    // a quiet operator 4 feeds back gently. fm.c multiplies that sum (+-65534, two +-32767
    // outputs) into a Q32 phase: 2^(10+n) puts FB 7 at 2^32 x 2 = 4 pi. (ymfm's opm.cpp is the
    // reference used.)
    c->fbAmt     = fb ? (1 << (10 + fb)) : 0;
    c->pitchEnv  = penv;
    c->pitchCoef = ENV_COEF[clampi(v->pitchTime, 1, ENV_STEPS - 1)];
    c->gainL     = PAN_L[pan + 7];
    c->gainR     = PAN_R[pan + 7];
    c->gate      = (uint32_t)gate;
    if (edge) c->noteOns = c->noteOns + 1;
}

// The pitch a melodic voice plays: its 0 V note, octave, master tuning and the matrix's PITCH
// sum, quantised (the quantiser snaps the TARGET; portamento then slides to it, so a scale still
// glides) and glided. FREE skips the quantiser, which is what lets a continuously-varying CV glide
// smoothly.
static int32_t g_target[FM_SLOTS + 1];     // each melodic voice's tracked pitch, before glide

IWRAM_CODE static int32_t melodic_pitch(int slot, int32_t noteBase, int8_t octave, uint8_t glide,
                             const int32_t *md, uint8_t *noteOut)
{
    const Patch *p = &g_patch;
    int32_t t = ((int32_t)noteBase << 8)
              + ((int32_t)octave * 12 << 8)
              + (((int32_t)p->tuneCents * 256) / 100)                         // constant divisor
              + md[DEST_PITCH];
    t = clampi(t, (int32_t)NOTE_MIN << 8, (int32_t)NOTE_MAX << 8);
    g_target[slot] = t;                      // before quantising: the CAL page's OUT reading

    uint8_t note = (uint8_t)clampi((t + 128) >> 8, 0, 127);
    if (p->scale != SCALE_FREE) {
        note = (uint8_t)clampi(quantise(note, p->scale, p->key % 12), 0, 127);
        t = (int32_t)note << 8;
    }
    *noteOut = note;

    int32_t g = clampi((int32_t)glide + (md[DEST_GLIDE] >> 2), 0, 15);
    if (g == 0) {
        g_voicePitch[slot] = t;
    } else {
        int32_t step = (t - g_voicePitch[slot]) >> g;
        if (step == 0) g_voicePitch[slot] = t;    // without this the one-pole stalls short for ever
        else           g_voicePitch[slot] += step;
    }
    return g_voicePitch[slot];
}

// ---- the control tick -------------------------------------------------------------------------
// RUNS FROM IWRAM, with everything it calls every tick. From EWRAM - 16-bit bus, two wait states,
// about six cycles per ARM instruction - its ~1900 instructions took most of each millisecond, and
// with the audio on top the main loop was left a sliver: the editor painted row by row (Andy,
// 2026-10-26). The rare calls (preset loads, link_post_note, udiv32) stay in EWRAM.
IWRAM_CODE static void synth_tick(void)
{
    Patch *p = &g_patch;
    scan_buttons();
    refresh_recips();
    int drums = p->drumMode != 0;

    // ---- GBA buttons ----------------------------------------------------------------------------
    // IN THE EDITOR NO BUTTON CARRIES ITS PERFORMANCE MEANING. Every key belongs to the editor
    // while a menu is open; performance meanings belong to the performance screen.
    int btnLvl[2] = { 0, 0 }, btnEdge[2] = { 0, 0 }, drumBtn[2] = { 0, 0 };
    for (int b = 0; b < BTN_SLOTS && !g_editMode; b++) {
        uint16_t bit = BTN_BIT[b];
        int lvl = (g_btn & bit) != 0;
        int edg = (g_btnEdge & bit) != 0;
        int stp = (g_btnStep & bit) != 0;
        Voice  *v1 = &p->voice[FV_V1], *v2 = &p->voice[FV_V2];
        Voice4 *v4 = &p->v4;
        uint8_t act = p->btnAct[b];

        // In 4-op mode the V1 actions play the 4-op voice. RATIO steps the ALGORITHM (there is no
        // single ratio to step), INDEX moves every modulator's level, FB is operator 4's.
        if (p->voiceMode == VMODE_4OP) {
            int hit = 1;
            switch (act) {
            case ACT_OCTUP_V1: if (stp) v4->octave = (int8_t)clampi(v4->octave + 1, -3, 3); break;
            case ACT_OCTDN_V1: if (stp) v4->octave = (int8_t)clampi(v4->octave - 1, -3, 3); break;
            case ACT_RATUP_V1: if (stp) v4->algo = (uint8_t)clampi(v4->algo + 1, 0, FM4_ALGOS - 1); break;
            case ACT_RATDN_V1: if (stp) v4->algo = (uint8_t)clampi(v4->algo - 1, 0, FM4_ALGOS - 1); break;
            case ACT_IDXUP_V1: case ACT_IDXDN_V1:
                if (stp) {
                    int d = (act == ACT_IDXUP_V1) ? 3 : -3;
                    uint8_t car = FM4_CARRIERS[v4->algo & 7];
                    for (int k = 0; k < 4; k++)
                        if (!((car >> k) & 1))
                            v4->op[k].level = (uint8_t)clampi(v4->op[k].level + d, 0, 99);
                }
                break;
            case ACT_FBUP_V1:  if (stp) v4->fb = (uint8_t)clampi(v4->fb + 1, 0, 7); break;
            case ACT_FBDN_V1:  if (stp) v4->fb = (uint8_t)clampi(v4->fb - 1, 0, 7); break;
            case ACT_PREUP_V1: if (stp) preset4_load(v4, v4->preset + 1 >= PRESET4_COUNT ? 0 : v4->preset + 1); break;
            case ACT_PREDN_V1: if (stp) preset4_load(v4, v4->preset == 0 ? PRESET4_COUNT - 1 : v4->preset - 1); break;
            default: hit = 0; break;
            }
            if (hit) continue;
        }

        switch (act) {
        case ACT_TRIG_V1:  btnLvl[0] |= lvl; btnEdge[0] |= edg; break;
        case ACT_TRIG_V2:  btnLvl[1] |= lvl; btnEdge[1] |= edg; break;
        case ACT_HOLD_V1:  if (edg) g_hold[0] = (uint8_t)!g_hold[0]; break;
        case ACT_HOLD_V2:  if (edg) g_hold[1] = (uint8_t)!g_hold[1]; break;
        case ACT_OCTUP_V1: if (stp) v1->octave = (int8_t)clampi(v1->octave + 1, -3, 3); break;
        case ACT_OCTDN_V1: if (stp) v1->octave = (int8_t)clampi(v1->octave - 1, -3, 3); break;
        case ACT_OCTUP_V2: if (stp) v2->octave = (int8_t)clampi(v2->octave + 1, -3, 3); break;
        case ACT_OCTDN_V2: if (stp) v2->octave = (int8_t)clampi(v2->octave - 1, -3, 3); break;
        case ACT_RATUP_V1: if (stp) v1->ratio = (uint8_t)clampi(v1->ratio + 1, 0, RATIO_COUNT - 1); break;
        case ACT_RATDN_V1: if (stp) v1->ratio = (uint8_t)clampi(v1->ratio - 1, 0, RATIO_COUNT - 1); break;
        case ACT_IDXUP_V1: if (stp) v1->index = (uint8_t)clampi(v1->index + 5, 0, 99); break;
        case ACT_IDXDN_V1: if (stp) v1->index = (uint8_t)clampi(v1->index - 5, 0, 99); break;
        case ACT_FBUP_V1:  if (stp) v1->fb = (uint8_t)clampi(v1->fb + 1, 0, 7); break;
        case ACT_FBDN_V1:  if (stp) v1->fb = (uint8_t)clampi(v1->fb - 1, 0, 7); break;
        // Presets wrap, so one button can walk the whole list.
        case ACT_PREUP_V1: if (stp) preset_load(v1, v1->preset + 1 >= PRESET_COUNT ? 0 : v1->preset + 1); break;
        case ACT_PREDN_V1: if (stp) preset_load(v1, v1->preset == 0 ? PRESET_COUNT - 1 : v1->preset - 1); break;
        case ACT_PREUP_V2: if (stp) preset_load(v2, v2->preset + 1 >= PRESET_COUNT ? 0 : v2->preset + 1); break;
        case ACT_PREDN_V2: if (stp) preset_load(v2, v2->preset == 0 ? PRESET_COUNT - 1 : v2->preset - 1); break;
        case ACT_TRIG_DA:  drumBtn[0] |= edg; break;
        case ACT_TRIG_DB:  drumBtn[1] |= edg; break;
        case ACT_DRUMS:    if (edg) p->drumMode = (uint8_t)!p->drumMode; break;
        case ACT_4OP:      if (edg) p->voiceMode = (uint8_t)(p->voiceMode == VMODE_4OP ? VMODE_2OP : VMODE_4OP); break;
        default: break;
        }
    }
    g_btnTrigHeld = (uint8_t)(btnLvl[0] | btnLvl[1]);
    drums = p->drumMode != 0;

    // ---- every source's value, level and edges, whatever it is mapped to ----------------------
    // Edges are tracked for every source on every tick, so remapping a source mid-performance
    // never fires a stale backlog of them.
    int32_t raw[SRC_COUNT];
    int     sLvl[SRC_COUNT], sEdge[SRC_COUNT];
    static uint8_t  schmitt[SRC_COUNT];
    static uint32_t seenPu2 = 0, seenAu[2] = { 0, 0 };
    int hostEdges = (g_hostCaps & GBA_CAP_AUD_EDGES) != 0;

    for (int s = 0; s < SRC_COUNT; s++) {
        raw[s] = (s < 4)        ? (int32_t)g_in[s] - 2048
               : (s == SRC_PU2) ? (g_gate ? 2047 : 0)
                                : (int32_t)g_knob[s - SRC_MAIN] - 2048;
        sEdge[s] = 0;

        if (s == SRC_PU2) {
            // Pulse In 2's edges are counted on the Workshop at 48 kHz. One per tick, so a burst
            // arrives in order instead of collapsing into one.
            sLvl[s] = g_gate;
            if (g_gateEdges != seenPu2) { seenPu2++; sEdge[s] = 1; }
        } else if ((s == SRC_AU1 || s == SRC_AU2) && hostEdges) {
            // Counted on the Workshop too (GBA_OP_AUDEDGE), so a 1 ms trigger is not lost
            // between two ~440 Hz snapshots.
            int i = s - SRC_AU1;
            sLvl[s] = (g_audLevel >> i) & 1;
            if (g_audEdges[i] != seenAu[i]) { seenAu[i]++; sEdge[s] = 1; }
        } else {
            // A Schmitt trigger on the snapshot - up through about +1 V, down through about
            // +0.5 V - so a slow CV crossing the threshold counts once.
            if (!schmitt[s] && raw[s] > 341)     { schmitt[s] = 1; sEdge[s] = 1; }
            else if (schmitt[s] && raw[s] < 170) { schmitt[s] = 0; }
            sLvl[s] = schmitt[s];
        }
    }

    // ---- the matrix -----------------------------------------------------------------------------
    // md[t][dest] accumulates every mapping's amount for target t: 0 = V1, 1 = V2 (or the drums).
    int32_t md[2][DEST_COUNT];
    int     gLvl[2] = { 0, 0 }, gEdge[2] = { 0, 0 };
    for (int t = 0; t < 2; t++) for (int d = 0; d < DEST_COUNT; d++) md[t][d] = 0;

    for (int s = 0; s < SRC_COUNT; s++) {
        // In drum mode AU1 and AU2 ARE the drum triggers, whatever their rows say.
        if (drums && (s == SRC_AU1 || s == SRC_AU2)) continue;
        const ModSlot *m = &p->mod[s];
        if (m->dest == DEST_NONE) continue;

        if (m->dest == DEST_GATE) {
            // DEPTH IS NOT A DEPTH HERE: a gate is on or off.
            for (int t = 0; t < 2; t++) {
                if (!(m->mask & (1u << t))) continue;
                gLvl[t]  |= sLvl[s];
                gEdge[t] |= sEdge[s];
            }
            continue;
        }

        int32_t v;
        if (m->dest == DEST_PITCH) {
            int set = (s == SRC_AU1 || s == SRC_AU2) ? CAL_AU : CAL_CV;
            v = (semis_q8(set, raw[s]) * m->depth) >> 5;      // depth 32 is unity 1V/oct
        } else {
            v = (raw[s] * m->depth) >> 11;                    // about +-63 at full depth
        }
        for (int t = 0; t < 2; t++) if (m->mask & (1u << t)) md[t][m->dest] += v;
    }

    // ---- the switch and HOLD ----------------------------------------------------------------------
    static uint8_t swPrev = 1, holdPrev[2] = { 0, 0 };
    int swEdge = (g_switch == 0 && swPrev != 0) || (g_switch == 2 && swPrev != 2);
    int swLvl  = (g_switch == 0 || g_switch == 2);      // DOWN = momentary, UP = drone
    swPrev = g_switch;

    int lvl[2], edge[2];
    for (int t = 0; t < 2; t++) {
        int swHere = (p->swTarget == SW_BOTH) || (p->swTarget == (t ? SW_V2 : SW_V1));
        int holdEdge = g_hold[t] && !holdPrev[t];
        holdPrev[t] = g_hold[t];
        lvl[t]  = gLvl[t]  | btnLvl[t]  | g_hold[t] | (swHere ? swLvl : 0);
        edge[t] = gEdge[t] | btnEdge[t] | holdEdge  | (swHere ? swEdge : 0);
    }

    // ---- the voices -------------------------------------------------------------------------------
    // An inactive slot still gets its parameters (it may be releasing) but no gate and no edge.
    // In 4-op mode the 4-op voice takes V1's place - its gate, its pitch, its mappings - and V2 is
    // off. The 2-op V1 and V2 slots are still computed, gated off, so they release naturally.
    //
    // ONCE AN INACTIVE SLOT IS SILENT IT IS NOT RECOMPUTED AT ALL - V1 and V2 in 4-op mode, the 4-op
    // voice in 2-op mode, the drums outside drum mode. A slot is refreshed while it is active, on the
    // tick it stops being active (which writes its gate low), and while its release still sounds;
    // the tick it becomes active again computes it, note-on included. melodic_pitch() still runs
    // for every melodic voice, so glide state carries over.
    int four = p->voiceMode == VMODE_4OP;
    uint32_t active = (four ? FM_ACTIVE_4OP : (1u << FV_V1))
                    | (drums ? ((1u << FV_DRUM_A) | (1u << FV_DRUM_B)) : (four ? 0u : (1u << FV_V2)));
    uint32_t live = active | g_fmActive;
    for (int s = 0; s < FM_SLOTS; s++) if (g_fmEnv[s][FM_CAR] | g_fmEnv[s][FM_MOD]) live |= 1u << s;
    if (g_fm4Env[0] | g_fm4Env[1] | g_fm4Env[2] | g_fm4Env[3]) live |= FM_ACTIVE_4OP;

    const Voice *vv1 = &p->voice[FV_V1], *vv2 = &p->voice[FV_V2];
    uint8_t n1 = 60, n2 = 60, n4 = 60;
    int32_t p1 = melodic_pitch(FV_V1, vv1->note, vv1->octave, vv1->glide, md[0], &n1);
    if (live & (1u << FV_V1)) voice_out(FV_V1, vv1, md[0], p1, four ? 0 : lvl[0], four ? 0 : edge[0]);
    g_slotNote[FV_V1] = n1;

    int v2on = !drums && !four;
    int32_t p2 = melodic_pitch(FV_V2, vv2->note, vv2->octave, vv2->glide, md[1], &n2);
    if (live & (1u << FV_V2)) voice_out(FV_V2, vv2, md[1], p2, v2on ? lvl[1] : 0, v2on ? edge[1] : 0);
    g_slotNote[FV_V2] = n2;

    const Voice4 *v4 = &p->v4;
    int32_t p4 = melodic_pitch(GLIDE_4OP, (int32_t)v4->note + v4->transpose, v4->octave, v4->glide,
                               md[0], &n4);
    if (live & FM_ACTIVE_4OP) voice4_out(v4, md[0], p4, four ? lvl[0] : 0, four ? edge[0] : 0);
    g_pitchQ8 = g_target[four ? GLIDE_4OP : FV_V1];

    // Drums: struck at their own note, plus any PITCH mapped to V2 (so one CV can tune the kit);
    // no quantiser, no glide. Anything that would play V2 plays DRUM A; AU2 and its button are the
    // only way to DRUM B.
    for (int d = 0; d < 2; d++) {
        int slot = FV_DRUM_A + d;
        const Voice *vc = &p->voice[slot];
        int32_t pq = ((int32_t)vc->note << 8) + ((int32_t)vc->octave * 12 << 8) + md[1][DEST_PITCH];
        pq = clampi(pq, (int32_t)NOTE_MIN << 8, (int32_t)NOTE_MAX << 8);
        int dl = 0, de = 0;
        if (drums) {
            int src = d ? SRC_AU2 : SRC_AU1;
            dl = sLvl[src] | (d ? 0 : lvl[1]);
            de = sEdge[src] | drumBtn[d] | (d ? 0 : edge[1]);
        }
        if (live & (1u << slot)) voice_out(slot, vc, md[1], pq, dl, de);
        g_slotNote[slot] = (uint8_t)clampi((pq + 128) >> 8, 0, 127);
    }

    g_fmActive = active;

    // The main voice is what CV Out 2 / CV Out 1 follow: the 4-op voice in 4-op mode. Operator 1
    // is a carrier in every algorithm, so its envelope stands for "still sounding".
    if (four) n1 = n4;
    g_note = n1;
    uint16_t envC = four ? g_fm4Env[0] : g_fmEnv[FV_V1][FM_CAR];
    g_anyNoteOn = (uint8_t)(lvl[0] || envC != 0);

    // ---- publish upstream: V1 is what CV Out 2 and CV Out 1 follow ------------------------------
    {
        static uint8_t lastNote = 0xFF, lastGate = 0xFF;
        uint8_t lv = (uint8_t)(envC >> 12);
        uint8_t gu = (uint8_t)(g_anyNoteOn ? (lv ? lv : 1) : 0);
        if (n1 != lastNote || gu != lastGate) {
            lastNote = n1; lastGate = gu;
            link_post_note(n1, gu);
        }
    }
}

// TICK LOAD (CAL page): wall time inside synth_tick() as a share of its ~1 ms period, percent x 16,
// smoothed like g_dspAvg. Wall time, so it includes any audio interrupt that lands inside the tick -
// which is exactly the time the main loop, and so the screen, does not get.
volatile uint16_t g_tickAvg  = 0;
volatile uint16_t g_tickPeak = 0;
static uint32_t   s_tickAcc  = 0;

void synth_update(void)
{
    static uint16_t last = 0;
    static int primed = 0;
    if (!primed) { last = REG_TM2CNT_L; primed = 1; }

    uint16_t now = REG_TM2CNT_L;
    // Bounded catch-up: after a long stall we do NOT run every owed tick, or the glide jumps. (The
    // envelopes do not depend on this clock at all - they run in the audio interrupt.)
    int budget = 8;
    while ((uint16_t)(now - last) >= 16 && budget--) {
        last = (uint16_t)(last + 16);
        uint16_t t0 = REG_TM3CNT_L;
        synth_tick();
        // The period is 16 Timer 2 ticks of 1024 cycles = 16384 cycles, so x1600/16384 is x25/256.
        uint32_t pct = ((uint32_t)(uint16_t)(REG_TM3CNT_L - t0) * 25u) >> 8;
        s_tickAcc += pct - (s_tickAcc >> 6);
        g_tickAvg = (uint16_t)(s_tickAcc >> 6);
        if (pct > g_tickPeak) g_tickPeak = (uint16_t)pct;
    }
    if ((uint16_t)(now - last) >= 16) last = now;
}

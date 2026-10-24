// ui.c — the screen.
//
// 108's editor, rebuilt around FM voices. The drawing rules are 108's and every one of them was
// learned the hard way - see CLAUDE.md, "Payload UI rules":
//
// ─────────────────────────────────────────────────────────────────────────────────────────────
// THE FLICKER RULE: NEVER ERASE THEN DRAW.
//
//   1. Anything that changes continuously (meters, faders, the envelope level tick) is painted in
//      ONE pass, overwriting: background segment, then value segment. No moment where it is blank.
//   2. Anything textual is compared against what is already on screen and skipped when it has
//      not changed.
//
// Static legends are drawn ONCE in edit_static(), never per frame. Large clears use rect_clear(),
// which keeps the link and the control tick serviced while it fills.
// ─────────────────────────────────────────────────────────────────────────────────────────────
//
// THE EDITOR OWNS EVERY BUTTON. Inside a menu no key carries its performance meaning.
//
//   D-pad alone           move the cursor (row, and on MIX/MAP a column)
//   A + Up/Down           change the value
//   A + Left/Right        change the value coarsely, or the second field (MIX pan)
//   SELECT + Left/Right   change page
// MAP's tickboxes are one bit, so A on its own toggles them.

#include "ui.h"
#include "gfx.h"
#include "link.h"
#include "synth.h"
#include "presets.h"
#include "fm.h"
#include "fm_tables.h"
#include "audio.h"

#define KEY_A      (1u << 0)
#define KEY_B      (1u << 1)
#define KEY_SELECT (1u << 2)
#define KEY_START  (1u << 3)
#define KEY_RIGHT  (1u << 4)
#define KEY_LEFT   (1u << 5)
#define KEY_UP     (1u << 6)
#define KEY_DOWN   (1u << 7)

uint8_t g_page = PAGE_MEM;

static uint8_t g_row      = 0;
static uint8_t g_scroll   = 0;
static uint8_t g_selVoice = FV_V1;   // the voice FM and ENV edit
static uint8_t g_selOp    = FM_CAR;  // which envelope the ENV page edits
static uint8_t g_mixCol   = 0;       // MIX column cursor (a voice slot)
static uint8_t g_mapCol   = 0;       // MAP column cursor: 0 dest, 1 depth, 2..3 voice ticks
static uint8_t g_slot     = 0;       // patch slot
static uint8_t g_repaint  = 1;
static uint8_t g_lastMode = 0xFF;
static uint8_t g_navBlank = 0;       // stepping pages with SELECT held: body blanked, tabs live
static int     g_tabPage  = -1;

static const char *page_tab[PAGE_COUNT] = { "MEM", "FM", "ENV", "MIX", "MAP", "BTN", "SET", "CAL" };

static const char *NOTE_NAME[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};
static const char *OP_NAME[2]           = { "MOD INDEX", "CARRIER" };
static const char *VOICE_NAME[FM_SLOTS] = { "VOICE 1", "VOICE 2", "DRUM A", "DRUM B" };
static const char *VOICE_TAG[FM_SLOTS]  = { "V1", "V2", "DR A", "DR B" };
static const char *ONOFF[2]             = { "OFF", "ON" };

// ---- helpers ---------------------------------------------------------------------------------
static int scopy(char *dst, int at, const char *src)
{
    while (*src) dst[at++] = *src++;
    dst[at] = 0;
    return at;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Is this slot sounding in the current mode? V1 always; V2 or the drums.
static int slot_live(int slot)
{
    if (slot == FV_V1) return 1;
    return g_patch.drumMode ? (slot >= FV_DRUM_A) : (slot == FV_V2);
}

static void note_text(char *buf, int note)
{
    int n   = (int)clampi(note, 0, 127);
    int oct = (n / 12) - 1;                  // constant divisors: no libcall
    int at  = scopy(buf, 0, NOTE_NAME[n % 12]);
    if (oct < 0) { buf[at++] = '-'; oct = -oct; }
    dec_at(buf, at, (uint32_t)oct);
}

static void env_time_text(char *buf, int idx)
{
    uint16_t ms = ENV_MS[clampi(idx, 0, ENV_STEPS - 1)];
    if (ms == 0) { scopy(buf, 0, "INSTANT"); return; }
    if (ms < 1000) {
        scopy(buf, dec_at(buf, 0, ms), " MS");
    } else {
        int at = dec_at(buf, 0, ms / 1000);            // 2500 ms -> "2.5 S"
        at = scopy(buf, at, ".");
        at = dec_at(buf, at, (ms / 100) % 10);
        scopy(buf, at, " S");
    }
}

// Index is stored in tenths of a radian: 35 -> "3.5".
static int tenths_at(char *buf, int at, uint32_t v)
{
    at = dec_at(buf, at, v / 10);                      // constant divisors
    at = scopy(buf, at, ".");
    return dec_at(buf, at, v % 10);
}

static void ratio_text(char *buf, int r)
{
    scopy(buf, scopy(buf, 0, "1:"), RATIO_NAME[clampi(r, 0, RATIO_COUNT - 1)]);
}

// Pan -7..+7 -> "L3", "C", "R7".
static void pan_text(char *buf, int pan)
{
    if (pan == 0) { scopy(buf, 0, "C"); return; }
    dec_at(buf, scopy(buf, 0, pan < 0 ? "L" : "R"), (uint32_t)(pan < 0 ? -pan : pan));
}

// Percent x 16 -> "7.5%".
static int pct_at(char *buf, int at, uint32_t p16)
{
    at = dec_at(buf, at, p16 >> 4);
    at = scopy(buf, at, ".");
    at = dec_at(buf, at, ((p16 & 15) * 10) >> 4);
    return scopy(buf, at, "%");
}

// ---- layout ----------------------------------------------------------------------------------
#define TAB_Y     3
#define LIST_TOP  22
#define LIST_ROW  11
#define LIST_MAX  10
#define VAL_X    150
#define HINT_Y   150

// FM page rows
#define FR_VOICE   0
#define FR_PRESET  1
#define FR_RATIO   2
#define FR_DETUNE  3
#define FR_INDEX   4
#define FR_FB      5
#define FR_NOISE   6
#define FR_PENV    7
#define FR_PTIME   8
#define FR_NOTE    9
#define FR_OCTAVE 10
#define FR_GLIDE  11
#define FR_ROWS   12

// MAP carries column headings above its rows, which need somewhere to live that is not on top
// of the tab bar. It is the only page with that problem, so it is the only page that moves.
static int list_top(int page)
{
    return (page == PAGE_MAP) ? LIST_TOP + 8 : LIST_TOP;
}

static int page_rows(int page)
{
    switch (page) {
    case PAGE_FM:  return FR_ROWS;
    case PAGE_ENV: return 6;                   // VOICE, ENVELOPE, then A D S R
    case PAGE_MAP: return SRC_COUNT;
    case PAGE_BTN: return BTN_SLOTS;
    case PAGE_SET: return 5;
    case PAGE_CAL: return 9;
    default:       return 1;                   // MEM and MIX: the page is a grid
    }
}

static int visible_rows(int page)
{
    int r = page_rows(page);
    return r < LIST_MAX ? r : LIST_MAX;
}

// ---- labels ------------------------------------------------------------------------------------
static const char *field_label(int page, int row)
{
    switch (page) {
    case PAGE_FM:
        switch (row) {
        case FR_VOICE:  return "VOICE";
        case FR_PRESET: return "PRESET";
        case FR_RATIO:  return "RATIO C:M";
        case FR_DETUNE: return "MOD DETUNE";
        case FR_INDEX:  return "INDEX";
        case FR_FB:     return "FEEDBACK";
        case FR_NOISE:  return "NOISE";
        case FR_PENV:   return "PITCH ENV";
        case FR_PTIME:  return "P.ENV TIME";
        // A melodic voice's note is where 0 V sits; a drum's is the note it is struck at.
        case FR_NOTE:   return g_selVoice >= FV_DRUM_A ? "DRUM NOTE" : "0V NOTE";
        case FR_OCTAVE: return "OCTAVE";
        default:        return "PORTAMENTO";
        }
    case PAGE_ENV:
        switch (row) {
        case 0: return "VOICE";
        case 1: return "ENVELOPE";
        case 2: return "ATTACK";
        case 3: return "DECAY";
        case 4: return "SUSTAIN";
        default: return "RELEASE";
        }
    case PAGE_MAP: return src_name[row % SRC_COUNT];
    case PAGE_BTN: return btn_name[row % BTN_SLOTS];
    case PAGE_SET:
        switch (row) {
        case 0: return "DRUM MODE";
        case 1: return "SWITCH PLAYS";
        case 2: return "SCALE";
        case 3: return "KEY";
        default: return "TUNING";
        }
    case PAGE_CAL:
        switch (row) {
        case 0: return "CV SCALE";
        case 1: return "CV OFFSET";
        case 2: return "AUD SCALE";
        case 3: return "AUD OFFSET";
        case 4: return "CV 2 TUNER";
        case 5: return "AUD 2 TUNER";
        case 6: return "AUD EDGES";
        case 7: return "LINK";
        default: return "DSP LOAD";
        }
    default:
        return "";
    }
}

// The tuner rows: note, cents and the raw count (masked so ADC dither alone cannot repaint it).
// The raw count is what the pitch maths works on, so two readings a known interval apart give
// SCALE directly - that is what makes this a calibration page rather than a guess.
static void tuner_text(char *buf, int au)
{
    char tmp[8];
    int note, cents;
    synth_tuner(au, &note, &cents);
    note_text(tmp, note);
    int at = scopy(buf, 0, tmp);
    sdec32(tmp, cents, 3);
    at = scopy(buf, at, tmp);
    at = scopy(buf, at, " R");
    dec_at(buf, at, (uint32_t)(g_in[au ? LINK_IN_AUD2 : LINK_IN_CV2] & 0xFF0u));
}

// ---- values -------------------------------------------------------------------------------------
static void field_value(int page, int row, char *buf)
{
    Patch *p = &g_patch;
    Voice *v = &p->voice[g_selVoice & 3];
    Adsr  *a = &v->env[g_selOp & 1];
    buf[0] = 0;

    switch (page) {
    case PAGE_FM:
        switch (row) {
        case FR_VOICE:
            // Name the voice, and say when it is not sounding in this mode - editing V2 while
            // drum mode is on is allowed, but it should not be a mystery why nothing changes.
            scopy(buf, scopy(buf, 0, VOICE_NAME[g_selVoice & 3]), slot_live(g_selVoice) ? "" : " (OFF)");
            return;
        case FR_PRESET: scopy(buf, 0, preset_name(v->preset)); return;
        case FR_RATIO:  ratio_text(buf, v->ratio); return;
        case FR_DETUNE: sdec32(buf, v->detune, 3); return;
        case FR_INDEX:  tenths_at(buf, 0, v->index); return;
        case FR_FB:     dec32(buf, v->fb, 1); return;
        case FR_NOISE:  dec32(buf, v->noise, 2); return;
        case FR_PENV: {
            char tmp[8];
            sdec32(tmp, v->pitchEnv, 3);
            scopy(buf, scopy(buf, 0, tmp), " ST");
            return;
        }
        case FR_PTIME:  env_time_text(buf, v->pitchTime); return;
        case FR_NOTE:   note_text(buf, v->note); return;
        case FR_OCTAVE: sdec32(buf, v->octave, 2); return;
        default:        dec32(buf, v->glide, 2); return;
        }
    case PAGE_ENV:
        switch (row) {
        case 0: scopy(buf, 0, VOICE_NAME[g_selVoice & 3]); return;
        case 1: scopy(buf, 0, OP_NAME[g_selOp & 1]); return;
        case 2: env_time_text(buf, a->atk); return;
        case 3: env_time_text(buf, a->dec); return;
        case 4: dec32(buf, a->sus, 2); return;
        default: env_time_text(buf, a->rel); return;
        }
    case PAGE_MAP: {
        // Comparison string only; edit_list paints the columns itself.
        ModSlot *m  = &p->mod[row % SRC_COUNT];
        int      at = dec_at(buf, 0, m->dest);
        at = scopy(buf, at, ":");
        at = dec_at(buf, at, (uint32_t)(m->depth + 64));
        at = scopy(buf, at, ":");
        at = dec_at(buf, at, m->mask);
        at = dec_at(buf, at, p->drumMode);
        // The column cursor only changes how the SELECTED row looks. Folding it into every row's
        // comparison string made a left/right press redraw the whole table.
        if (row == g_row) dec_at(buf, at, g_mapCol);
        return;
    }
    case PAGE_BTN: scopy(buf, 0, act_name[p->btnAct[row % BTN_SLOTS] % ACT_COUNT]); return;
    case PAGE_SET:
        switch (row) {
        case 0: scopy(buf, 0, ONOFF[p->drumMode ? 1 : 0]); return;
        case 1: scopy(buf, 0, sw_name[p->swTarget & 3]); return;
        case 2: scopy(buf, 0, scale_name[p->scale % SCALE_COUNT]); return;
        case 3: scopy(buf, 0, key_name[p->key % 12]); return;
        default: sdec32(buf, p->tuneCents, 3); return;
        }
    case PAGE_CAL:
        switch (row) {
        case 0: dec32(buf, (uint32_t)p->cvScale, 5); return;
        case 1: sdec32(buf, p->cvOffset, 5); return;
        case 2: dec32(buf, (uint32_t)p->auScale, 5); return;
        case 3: sdec32(buf, p->auOffset, 5); return;
        case 4: tuner_text(buf, 0); return;
        case 5: tuner_text(buf, 1); return;
        case 6: {
            // 48 KHZ when the Workshop is counting AU1/AU2 edges itself; SNAPSHOT when the
            // firmware predates that and the GBA is thresholding ~440 Hz snapshots instead.
            int at = scopy(buf, 0, (g_hostCaps & GBA_CAP_AUD_EDGES) ? "48 KHZ" : "SNAPSHOT");
            if (g_audBad) dec_at(buf, scopy(buf, at, " BAD "), g_audBad);
            return;
        }
        case 7: scopy(buf, 0, link_irq_alive() ? "IRQ" : "POLLED"); return;
        default: {
            // Smoothed load, then the worst single block since boot. The meter is the audio
            // interrupt's render time against its 16-sample budget; see audio.h.
            int at = pct_at(buf, 0, g_dspAvg);
            at = scopy(buf, at, " PK ");
            pct_at(buf, at, g_dspPeak);
            return;
        }
        }
    default:
        return;
    }
}

static void field_adjust(int page, int row, int delta)
{
    Patch *p = &g_patch;
    Voice *v = &p->voice[g_selVoice & 3];
    Adsr  *a = &v->env[g_selOp & 1];

    switch (page) {
    case PAGE_FM:
        switch (row) {
        case FR_VOICE:  g_selVoice = (uint8_t)clampi(g_selVoice + delta, 0, FM_SLOTS - 1); return;
        // Choosing a preset LOADS it, so browsing the list is auditioning it.
        case FR_PRESET: preset_load(v, (int)clampi(v->preset + delta, 0, PRESET_COUNT - 1)); return;
        case FR_RATIO:  v->ratio  = (uint8_t)clampi(v->ratio + delta, 0, RATIO_COUNT - 1); return;
        case FR_DETUNE: v->detune = (int8_t)clampi(v->detune + delta, -99, 99); return;
        case FR_INDEX:  v->index  = (uint8_t)clampi(v->index + delta, 0, 99); return;
        case FR_FB:     v->fb     = (uint8_t)clampi(v->fb + delta, 0, 7); return;
        case FR_NOISE:  v->noise  = (uint8_t)clampi(v->noise + delta, 0, 15); return;
        case FR_PENV:   v->pitchEnv  = (int8_t)clampi(v->pitchEnv + delta, -48, 48); return;
        case FR_PTIME:  v->pitchTime = (uint8_t)clampi(v->pitchTime + delta, 1, ENV_STEPS - 1); return;
        // Coarse is an octave here: eight semitones is not a musical quantity.
        case FR_NOTE:   v->note = (uint8_t)clampi(v->note + (delta == 8 ? 12 : delta == -8 ? -12 : delta),
                                                  0, 120); return;
        case FR_OCTAVE: v->octave = (int8_t)clampi(v->octave + delta, -3, 3); return;
        default:        v->glide  = (uint8_t)clampi(v->glide + delta, 0, 15); return;
        }
    case PAGE_ENV:
        switch (row) {
        case 0: g_selVoice = (uint8_t)clampi(g_selVoice + delta, 0, FM_SLOTS - 1); return;
        case 1: g_selOp = (uint8_t)(delta > 0 ? FM_CAR : FM_MOD); return;
        case 2: a->atk = (uint8_t)clampi(a->atk + delta, 0, ENV_STEPS - 1); return;
        case 3: a->dec = (uint8_t)clampi(a->dec + delta, 0, ENV_STEPS - 1); return;
        case 4: a->sus = (uint8_t)clampi(a->sus + delta, 0, 15); return;
        default: a->rel = (uint8_t)clampi(a->rel + delta, 0, ENV_STEPS - 1); return;
        }
    case PAGE_BTN: {
        int b = row % BTN_SLOTS;
        p->btnAct[b] = (uint8_t)clampi(p->btnAct[b] + delta, 0, ACT_COUNT - 1);
        return;
    }
    case PAGE_SET:
        switch (row) {
        case 0: p->drumMode = (uint8_t)(delta > 0); return;
        case 1: p->swTarget = (uint8_t)clampi(p->swTarget + delta, 0, 3); return;
        case 2: p->scale = (uint8_t)clampi(p->scale + delta, 0, SCALE_COUNT - 1); return;
        case 3: p->key   = (uint8_t)clampi(p->key + delta, 0, 11); return;
        default: p->tuneCents = (int8_t)clampi(p->tuneCents + delta, -50, 50); return;
        }
    case PAGE_CAL: {
        // COARSE IS x8 FOR THE SCALES. They are Q8, so a step of 1 is 1/256 of a count per
        // semitone - the point of the precision, but it would also make a first calibration
        // hundreds of presses. A+Left/Right therefore moves them by 64.
        int ds = (delta == 8 || delta == -8) ? delta * 8 : delta;
        switch (row) {
        case 0: p->cvScale  = (int16_t)clampi(p->cvScale + ds, 1024, 32767); return;
        case 1: p->cvOffset = (int16_t)clampi(p->cvOffset + delta, -2048, 2047); return;
        case 2: p->auScale  = (int16_t)clampi(p->auScale + ds, 1024, 32767); return;
        case 3: p->auOffset = (int16_t)clampi(p->auOffset + delta, -2048, 2047); return;
        default: return;
        }
    }
    default:
        return;
    }
}

// ---- PLAY view -----------------------------------------------------------------------------------
#define PLAY_NOTE_Y 26
#define PLAY_V_Y    56
#define PLAY_V_ROW  11
#define PLAY_INFO_Y 92
#define PLAY_IN_Y   112

static int g_playVoices = -1;      // the drum mode the voice rows were laid out for

static void play_static(void)
{
    // rect_clear(), not rect(): this fires once on every PLAY<->EDIT mode switch, and a plain
    // full-screen rect() blocks gfx_idle() for the several milliseconds the fill takes.
    rect_clear(0, 0, SCREEN_W, SCREEN_H, COL_BG);
    text_centre(4, "MTM WORKSHOP COMPUTER  //  GBA FM VOICE", COL_TITLE, 1);
    rect(14, 16, SCREEN_W - 28, 1, COL_DIM);

    static const char *inn[7] = { "CV1", "CV2", "AU1", "AU2", "MAIN", "X", "Y" };
    for (int i = 0; i < 7; i++) text(10 + i * 33, PLAY_IN_Y, inn[i], COL_DIM, 1);

    text(14, HINT_Y, "START EDIT", COL_DIM, 1);
    g_playVoices = -1;
}

// One meter, painted in a single overwriting pass (the flicker rule).
static void meter(int x, int y, int w, int full, uint16_t c)
{
    if (w > full) w = full;
    if (w < 0) w = 0;
    if (w > 0) rect(x, y, w, 6, c);
    rect(x + w, y, full - w, 6, COL_PANEL);
}

static void play_frame(void)
{
    char buf[40];
    const Patch *p = &g_patch;

    static uint8_t lastNote = 0xFF, lastOn = 0xFF;
    if (g_note != lastNote || g_anyNoteOn != lastOn) {
        lastNote = g_note; lastOn = g_anyNoteOn;
        note_text(buf, g_note);
        srect(12, PLAY_NOTE_Y, 120, 24, COL_BG);
        text(14, PLAY_NOTE_Y, buf, g_anyNoteOn ? COL_TITLE : COL_MID, 3);
    }

    {
        static uint8_t lastFlags = 0xFF;
        uint8_t f = (uint8_t)((g_gate ? 1 : 0) | (g_switch != 1 ? 2 : 0)
                            | (g_btnTrigHeld ? 4 : 0) | ((g_hold[0] | g_hold[1]) ? 8 : 0)
                            | (p->drumMode ? 16 : 0));
        if (f != lastFlags) {
            lastFlags = f;
            srect(134, PLAY_NOTE_Y, 102, 18, COL_BG);
            if (f & 1)  text(134, PLAY_NOTE_Y, "GATE", COL_OK, 1);
            if (f & 2)  text(166, PLAY_NOTE_Y, "SW", COL_OK, 1);
            if (f & 4)  text(184, PLAY_NOTE_Y, "BTN", COL_OK, 1);
            if (f & 8)  text(206, PLAY_NOTE_Y, "HOLD", COL_WAIT, 1);
            if (f & 16) text(134, PLAY_NOTE_Y + 9, "DRUMS", COL_WAIT, 1);
        }
    }

    // One row per sounding voice: its tag, its preset, and a meter of what it is putting out
    // right now (amplitude envelope x level). V1 and V2, or V1 and the two drums.
    static uint8_t lastPreset[FM_SLOTS];
    int drums = p->drumMode ? 1 : 0;
    if (drums != g_playVoices) {
        g_playVoices = drums;
        srect(10, PLAY_V_Y, SCREEN_W - 20, 3 * PLAY_V_ROW, COL_BG);
        for (int i = 0; i < FM_SLOTS; i++) lastPreset[i] = 0xFF;
    }
    int row = 0;
    for (int s = 0; s < FM_SLOTS; s++) {
        if (!slot_live(s)) continue;
        int y = PLAY_V_Y + row * PLAY_V_ROW;
        row++;
        const Voice *v = &p->voice[s];
        if (v->preset != lastPreset[s]) {
            lastPreset[s] = v->preset;
            srect(10, y, 86, 9, COL_BG);
            text(14, y, VOICE_TAG[s], COL_DIM, 1);
            text(44, y, preset_name(v->preset), COL_MID, 1);
        }
        int w = ((g_fmEnv[s][FM_CAR] >> 8) * v->level * 130) / (255 * 15);
        meter(98, y + 1, w, 130, s == FV_V1 ? COL_OK : COL_BAR);
        gfx_idle();
    }

    {
        static char last[40] = { 1, 0 };
        char tmp[12];
        int at = scopy(buf, 0, "V1 RATIO ");
        ratio_text(tmp, g_liveRatio[FV_V1]);
        at = scopy(buf, at, tmp);
        at = scopy(buf, at, "  INDEX ");
        at = tenths_at(buf, at, g_liveIndex[FV_V1]);
        at = scopy(buf, at, "  FB ");
        dec_at(buf, at, p->voice[FV_V1].fb);
        if (!streq(buf, last)) {
            scopy(last, 0, buf);
            srect(10, PLAY_INFO_Y, SCREEN_W - 20, 9, COL_BG);
            text(14, PLAY_INFO_Y, buf, COL_MID, 1);
        }
    }

    for (int i = 0; i < 7; i++) {
        int v = (i < 4) ? (int)g_in[i] : (int)g_knob[i - 4];
        int w = (v * 28) / 4095;
        int x = 10 + i * 33;
        if (w > 0) rect(x, PLAY_IN_Y + 10, w, 8, COL_BAR);
        rect(x + w, PLAY_IN_Y + 10, 28 - w, 8, COL_PANEL);
        gfx_idle();
    }

    static uint32_t lastBad = 0xFFFFFFFFu;
    if (g_streamBad != lastBad) {
        lastBad = g_streamBad;
        srect(150, HINT_Y, 84, 8, COL_BG);
        dec32(buf, g_streamBad, 5);
        text(150, HINT_Y, "REJ", COL_DIM, 1);
        text(176, HINT_Y, buf, g_streamBad ? COL_WAIT : COL_DIM, 1);
    }
}

// ---- EDIT chrome ---------------------------------------------------------------------------------
static void draw_tabs(void)
{
    int x = 2;
    for (int i = 0; i < PAGE_COUNT; i++) {
        int w = text_width(page_tab[i], 1);
        rect(x - 2, TAB_Y - 2, w + 7, 11, (i == g_page) ? COL_PANEL : COL_BG);
        text(x, TAB_Y, page_tab[i], (i == g_page) ? COL_TITLE : COL_DIM, 1);
        x += w + 10;
    }
    rect(0, 15, SCREEN_W, 1, COL_DIM);
}

static void arrow(int x, int y, int up, uint16_t c)
{
    for (int i = 0; i < 4; i++) {
        int w = up ? (i * 2 + 1) : (7 - i * 2);
        rect(x + 3 - (w >> 1), y + i, w, 1, c);
    }
}

static void edit_static(void)
{
    rect_clear(0, 0, SCREEN_W, SCREEN_H, COL_BG);   // see play_static()'s note on rect_clear()
    draw_tabs();
    text(4, HINT_Y, "SEL+L/R PAGE", COL_DIM, 1);
    text(96, HINT_Y, "A+PAD EDIT", COL_DIM, 1);
    text(174, HINT_Y, "START PLAY", COL_DIM, 1);

    // Page legends, drawn ONCE. HINT LINES ARE HARD-LIMITED TO 40 CHARACTERS from x=0 (39 from
    // x=6): glyph() clips silently at the right edge rather than wrapping.
    if (g_page == PAGE_MAP) {
        // Column headings sit ABOVE the rows, clear of the list's clear region.
        int hy = list_top(PAGE_MAP) - 9;
        text(14, hy, "SRC", COL_DIM, 1);
        text(64, hy, "DESTINATION", COL_DIM, 1);
        text(136, hy, "AMT", COL_DIM, 1);
        text(170, hy, "V1", COL_DIM, 1);
        text(194, hy, "V2", COL_DIM, 1);
        text(0, 124, "L/R COLUMN  A+UP/DN VALUE  A=TOGGLE TICK", COL_DIM, 1);
        text(6, 136, "KNOBS: NOON = AS PATCHED. 32 = 1V/OCT", COL_DIM, 1);
    }
    if (g_page == PAGE_MIX) text(6, 140, "L/R VOICE  A+UP/DN LEVEL  A+L/R PAN", COL_DIM, 1);
    if (g_page == PAGE_SET) text(6, 140, "DRUMS: AUD 1 HITS DRUM A, AUD 2 DRUM B", COL_DIM, 1);
    if (g_page == PAGE_CAL) text(6, 140, "GBA FM VOICE    BY ANDY JENKINSON 2026", COL_DIM, 1);
    if (g_page == PAGE_MEM) text(0, 140, "D-PAD PICKS SLOT. A+UP=SAVE. A+DOWN=LOAD", COL_DIM, 1);
}

// ---- the envelope graph --------------------------------------------------------------------------
static int g_envShape = -1;

static void draw_env_graph(int x, int y, int w, int h)
{
    // Segment width per ladder step, roughly logarithmic like the times themselves.
    static const uint8_t SEGW[ENV_STEPS] = {
         1,  2,  3,  4,  5,  6,  8,  9, 11, 12, 14, 15, 17, 18, 20, 21,
        23, 24, 26, 27, 29, 30, 32, 33, 35, 36, 38, 40, 43, 46, 49, 52
    };
    Adsr *a = &g_patch.voice[g_selVoice & 3].env[g_selOp & 1];

    int base = y + h - 2;
    int top  = y + 2;
    int span = base - top;
    int lane = x + w - 6;

    int shape = (g_selVoice << 22) | (g_selOp << 20) | (a->atk << 15) | (a->dec << 10)
              | (a->sus << 5) | a->rel;
    if (shape != g_envShape) {
        g_envShape = shape;
        rect(x - 1, y - 1, w + 2, h + 2, COL_DIM);
        rect(x, y, w, h, COL_BG);

        int sus = base - (span * a->sus) / 15;
        int wa = SEGW[a->atk % ENV_STEPS], wd = SEGW[a->dec % ENV_STEPS], wr = SEGW[a->rel % ENV_STEPS];
        int ws = w - 10 - wa - wd - wr;
        if (ws < 6) ws = 6;

        int x0 = x + 2;
        int x1 = (int)clampi(x0 + wa, x0, lane - 2);
        int x2 = (int)clampi(x1 + wd, x1, lane - 2);
        int x3 = (int)clampi(x2 + ws, x2, lane - 2);
        int x4 = (int)clampi(x3 + wr, x3, lane - 2);

        // Decay and release are drawn straight although they are exponential: at this size the
        // difference reads as noise, and the graph's job is the shape of the four stages.
        line(x0, base, x1, top, COL_OK);
        line(x1, top,  x2, sus, COL_OK);
        line(x2, sus,  x3, sus, COL_OK);
        line(x3, sus,  x4, base, COL_OK);
        if (x4 < lane - 2) line(x4, base, lane - 2, base, COL_DIM);
    }

    int lvl = base - (span * (g_fmEnv[g_selVoice & 3][g_selOp & 1] >> 8)) / 255;
    if (lvl < y) lvl = y;
    if (lvl > y + h - 3) lvl = y + h - 3;
    rect(lane, y, 5, lvl - y, COL_BG);
    rect(lane, lvl, 5, 3, COL_WAIT);
    rect(lane, lvl + 3, 5, (y + h) - (lvl + 3), COL_BG);
}

// ---- the list --------------------------------------------------------------------------------------
static int g_editCacheValid = 0;
// How many rows the list last occupied, so a layout change clears exactly what was there. Reset to
// zero by invalidate(): after a full repaint nothing of the old list is left, and remembering its
// height anyway made the first clear on the new page wipe that page's legend.
static int g_listVis = 0;

// The MAP page's row, painted column by column. In drum mode the AU1/AU2 rows are the drum
// triggers whatever they say, so they show that, dimmed, instead of a mapping that is not live.
static void map_row(int r, int y, int sel)
{
    const ModSlot *m = &g_patch.mod[r % SRC_COUNT];
    uint16_t base = sel ? COL_TITLE : COL_MID;

    if (g_patch.drumMode && (r == SRC_AU1 || r == SRC_AU2)) {
        text(64, y, r == SRC_AU1 ? "DRUM A HIT" : "DRUM B HIT", COL_DIM, 1);
        return;
    }

    char d[10];
    text(64, y, dest_name[m->dest % DEST_COUNT], (sel && g_mapCol == 0) ? COL_SEL : base, 1);
    if (m->dest == DEST_GATE || m->dest == DEST_NONE) scopy(d, 0, "--");
    else                                             sdec32(d, m->depth, 3);
    text(136, y, d, (sel && g_mapCol == 1) ? COL_SEL : base, 1);

    // The tickboxes: which voices this mapping reaches.
    for (int c = 0; c < 2; c++) {
        int bx  = 170 + c * 24;
        int on  = (m->mask >> c) & 1;
        int cur = (sel && g_mapCol == 2 + c);
        rect(bx, y, 11, 9, cur ? COL_SEL : COL_DIM);
        rect(bx + 1, y + 1, 9, 7, on ? COL_OK : COL_BG);
    }
}

static void edit_list(void)
{
    static char cacheVal[LIST_MAX][24];
    static int  cacheSel = -1;
    static int  cacheTop = -1;
    static int  cacheKey = -1;
    char buf[24];

    int rows = page_rows(g_page);
    int vis  = visible_rows(g_page);
    int key  = (g_page << 4) | (g_selVoice << 1) | g_selOp;
    int top  = list_top(g_page);

    // The voice and operator are part of the cache key because FM and ENV show a different
    // voice's values under the same labels.
    if (!g_editCacheValid || cacheTop != g_scroll || cacheKey != key) {
        g_editCacheValid = 1;
        cacheTop = g_scroll;
        cacheKey = key;
        cacheSel = -1;
        for (int r = 0; r < LIST_MAX; r++) cacheVal[r][0] = 1;
        int clearRows = (vis > g_listVis) ? vis : g_listVis;
        rect_clear(0, top - 2, SCREEN_W, clearRows * LIST_ROW + 4, COL_BG);
        g_listVis = vis;
    }

    for (int i = 0; i < vis; i++) {
        int r = g_scroll + i;
        if (r >= rows) break;
        int y   = top + i * LIST_ROW;
        int sel = (r == g_row);

        field_value(g_page, r, buf);
        if (sel == (i == cacheSel) && streq(buf, cacheVal[i])) continue;

        srect(8, y - 1, SCREEN_W - 16, LIST_ROW - 1, sel ? COL_PANEL : COL_BG);
        text(14, y, field_label(g_page, r), sel ? COL_SEL : COL_DIM, 1);
        if (g_page == PAGE_MAP) map_row(r, y, sel);
        else                    text(VAL_X, y, buf, sel ? COL_TITLE : COL_MID, 1);
        scopy(cacheVal[i], 0, buf);
    }
    cacheSel = g_row - g_scroll;

    {
        static int lastArrows = -1;
        int a = ((g_scroll > 0) ? 1 : 0) | ((g_scroll + vis < rows) ? 2 : 0) | (g_page << 2);
        if (a != lastArrows) {
            lastArrows = a;
            rect(230, top, 7, 4, COL_BG);
            rect(230, top + vis * LIST_ROW - 6, 7, 4, COL_BG);
            if (a & 1) arrow(230, top, 1, COL_TITLE);
            if (a & 2) arrow(230, top + vis * LIST_ROW - 6, 0, COL_TITLE);
        }
    }
}

// ---- MIX -------------------------------------------------------------------------------------------
// Four faders, one per voice slot. The two not sounding in the current mode are drawn dim - they
// keep their settings for when the mode comes back.
static int g_mixCache[FM_SLOTS] = { -1, -1, -1, -1 };

static void draw_mixer(void)
{
    char buf[8];

    for (int i = 0; i < FM_SLOTS; i++) {
        const Voice *v = &g_patch.voice[i];
        int x    = 20 + i * 54;
        int sel  = (i == g_mixCol);
        int live = slot_live(i);

        int c = (sel << 13) | (live << 12) | (v->level << 4) | (v->pan + 7);
        if (c != g_mixCache[i]) {
            g_mixCache[i] = c;
            rect(x - 4, 20, 48, 112, sel ? COL_PANEL : COL_BG);
            text(x + 2, 22, VOICE_TAG[i], sel ? COL_SEL : (live ? COL_MID : COL_DIM), 1);
            rect(x + 5, 33, 18, 82, COL_DIM);
            dec32(buf, v->level, 2);
            text(x + 6, 117, buf, sel ? COL_TITLE : COL_MID, 1);
            pan_text(buf, v->pan);
            text(x + 6, 125, buf, live ? COL_OK : COL_DIM, 1);
        }

        // The fader in one overwriting pass: background above the set level, the set level, and
        // the live output inside it.
        int hset  = (v->level * 78) / 15;
        int hlive = live ? ((g_fmEnv[i][FM_CAR] >> 8) * hset) / 255 : 0;
        const int floorY = 114;
        rect(x + 6, 34, 16, 80 - hset, COL_BG);
        if (hset > hlive) rect(x + 6, floorY - hset, 16, hset - hlive, COL_MID);
        if (hlive > 0)    rect(x + 6, floorY - hlive, 16, hlive, COL_OK);
        gfx_idle();
    }
}

// ---- MEM -----------------------------------------------------------------------------------------
// The sixteen slots, and how far a transfer has got. 108's page, unchanged.
static int     g_memInit = 0;
static uint8_t g_memCell[GBA_PATCH_SLOTS];
static int     g_memProg = -1;

static void draw_mem_page(void)
{
    char buf[8];
    const int x0 = 14, y0 = 40, bw = 26, bh = 22, by = 100;

    if (!g_memInit) {
        g_memInit = 1;
        // Stops at 138, clear of the legend edit_static() draws at 140.
        rect_clear(0, LIST_TOP - 2, SCREEN_W, 138 - LIST_TOP, COL_BG);
        text(14, LIST_TOP, "PATCH SLOTS", COL_DIM, 1);
        for (int i = 0; i < GBA_PATCH_SLOTS; i++) g_memCell[i] = 0xFF;
        g_memProg = -1;
        rect(x0, by, 208, 10, COL_DIM);          // the bar's frame, drawn once
    }

    // Per-box caching: moving the cursor repaints two boxes, not the whole grid.
    for (int i = 0; i < GBA_PATCH_SLOTS; i++) {
        uint8_t st = (uint8_t)(((g_slotMask >> i) & 1) | ((i == g_slot) ? 2 : 0));
        if (g_memCell[i] == st) continue;
        g_memCell[i] = st;

        int col = i & 7, row = i >> 3;
        int x = x0 + col * bw, y = y0 + row * (bh + 4);
        rect(x, y, bw - 4, bh, (st & 2) ? COL_SEL : COL_DIM);
        rect(x + 1, y + 1, bw - 6, bh - 2, (st & 1) ? COL_BAR : COL_BG);
        dec_at(buf, 0, (uint32_t)i + 1);
        text(x + 5, y + 7, buf, (st & 1) ? COL_TITLE : COL_MID, 1);
        gfx_idle();
    }

    int busy = (g_xferState != LINK_XFER_NONE);
    int prog = link_xfer_progress();
    int key  = (prog << 4) | (busy << 2) | (g_xferResult & 3);
    if (key != g_memProg) {
        g_memProg = key;
        int w = (prog * 206) / 100;              // constant divisor
        rect(x0 + 1, by + 1, w, 8, busy ? COL_OK : COL_PANEL);
        rect(x0 + 1 + w, by + 1, 206 - w, 8, COL_PANEL);

        rect(x0, by + 13, 208, 9, COL_BG);
        if (busy) {
            text(x0, by + 14, (g_xferState == LINK_XFER_SAVE) ? "SAVING" : "LOADING", COL_WAIT, 1);
        } else {
            if (g_xferResult == LINK_RESULT_OK)         text(x0, by + 14, "DONE", COL_OK, 1);
            else if (g_xferResult == LINK_RESULT_EMPTY) text(x0, by + 14, "SLOT IS EMPTY", COL_WAIT, 1);
            else if (g_xferResult == LINK_RESULT_BAD)   text(x0, by + 14, "BAD DATA - NOT LOADED", COL_WAIT, 1);
        }
    }
}

// ---- dispatch -----------------------------------------------------------------------------------------
void ui_init(void)
{
    g_page = PAGE_MEM;
    g_row = 0;
    g_scroll = 0;
    g_repaint = 1;
}

static void invalidate(void)
{
    g_editCacheValid = 0;
    g_listVis  = 0;
    g_envShape = -1;
    g_memInit  = 0;
    for (int i = 0; i < FM_SLOTS; i++) g_mixCache[i] = -1;
}

static void change_page(int delta)
{
    int n = (int)g_page + delta;
    if (n < 0) n = PAGE_COUNT - 1;
    if (n >= PAGE_COUNT) n = 0;
    g_page = (uint8_t)n;
    g_row = 0;
    g_scroll = 0;
    g_mapCol = 0;
    // Deliberately does NOT set g_repaint: while SELECT is held the body is blanked and only the
    // tab bar is redrawn - see the dispatch at the end of ui_frame.
}

void ui_frame(void)
{
    uint16_t edges = synth_take_edges();
    uint16_t steps = synth_take_steps();

    if (edges & KEY_START) {
        g_editMode = (uint8_t)!g_editMode;
        // Entering the editor always lands on MEM: START is a way in, not a bookmark.
        if (g_editMode) { g_page = PAGE_MEM; g_row = 0; g_scroll = 0; g_mapCol = 0; }
        g_repaint = 1;
    }

    if (g_editMode) {
        int rows = page_rows(g_page);
        int vis  = visible_rows(g_page);
        int adj  = (g_btn & KEY_A) != 0;

        if (g_btn & KEY_SELECT) {
            if (steps & KEY_RIGHT) change_page(+1);
            if (steps & KEY_LEFT)  change_page(-1);

        } else if (g_page == PAGE_MEM) {
            // Bare D-pad walks the grid the way it looks. Save and load are different gestures on
            // different directions, so neither can happen by accident while looking around.
            if (!adj) {
                if (steps & KEY_RIGHT) g_slot = (uint8_t)clampi(g_slot + 1, 0, GBA_PATCH_SLOTS - 1);
                if (steps & KEY_LEFT)  g_slot = (uint8_t)clampi(g_slot - 1, 0, GBA_PATCH_SLOTS - 1);
                if (steps & KEY_DOWN)  g_slot = (uint8_t)clampi(g_slot + 8, 0, GBA_PATCH_SLOTS - 1);
                if (steps & KEY_UP)    g_slot = (uint8_t)clampi(g_slot - 8, 0, GBA_PATCH_SLOTS - 1);
            }
            if (adj && (steps & KEY_UP) && g_xferState == LINK_XFER_NONE)
                link_begin_save(g_slot, (const uint8_t *)&g_patch, synth_patch_bytes());
            if (adj && (steps & KEY_DOWN) && g_xferState == LINK_XFER_NONE)
                link_begin_load(g_slot);

        } else if (g_page == PAGE_MIX) {
            Voice *v = &g_patch.voice[g_mixCol & 3];
            if (!adj) {
                if (steps & KEY_RIGHT) g_mixCol = (uint8_t)((g_mixCol + 1 >= FM_SLOTS) ? 0 : g_mixCol + 1);
                if (steps & KEY_LEFT)  g_mixCol = (uint8_t)((g_mixCol == 0) ? FM_SLOTS - 1 : g_mixCol - 1);
            } else {
                if (steps & KEY_UP)    v->level = (uint8_t)clampi(v->level + 1, 0, 15);
                if (steps & KEY_DOWN)  v->level = (uint8_t)clampi(v->level - 1, 0, 15);
                if (steps & KEY_RIGHT) v->pan   = (int8_t)clampi(v->pan + 1, -7, 7);
                if (steps & KEY_LEFT)  v->pan   = (int8_t)clampi(v->pan - 1, -7, 7);
            }

        } else if (g_page == PAGE_MAP) {
            ModSlot *m = &g_patch.mod[g_row % SRC_COUNT];
            // In drum mode the AU1/AU2 rows are the drum triggers; their mappings are kept for
            // normal mode but cannot be edited while they are not what the jack does.
            int locked = g_patch.drumMode && (g_row == SRC_AU1 || g_row == SRC_AU2);
            if (!adj) {
                if (steps & KEY_DOWN)  g_row = (uint8_t)((g_row + 1 >= rows) ? 0 : g_row + 1);
                if (steps & KEY_UP)    g_row = (uint8_t)((g_row == 0) ? rows - 1 : g_row - 1);
                if (steps & KEY_RIGHT) g_mapCol = (uint8_t)((g_mapCol + 1 >= 4) ? 0 : g_mapCol + 1);
                if (steps & KEY_LEFT)  g_mapCol = (uint8_t)((g_mapCol == 0) ? 3 : g_mapCol - 1);
            } else if (locked) {
                // nothing
            } else if (g_mapCol == 0) {
                if (steps & KEY_UP)   m->dest = (uint8_t)clampi(m->dest + 1, 0, DEST_COUNT - 1);
                if (steps & KEY_DOWN) m->dest = (uint8_t)clampi(m->dest - 1, 0, DEST_COUNT - 1);
            } else if (g_mapCol == 1) {
                if (steps & KEY_UP)    m->depth = (int8_t)clampi(m->depth + 1, -64, 63);
                if (steps & KEY_DOWN)  m->depth = (int8_t)clampi(m->depth - 1, -64, 63);
                if (steps & KEY_RIGHT) m->depth = (int8_t)clampi(m->depth + 8, -64, 63);
                if (steps & KEY_LEFT)  m->depth = (int8_t)clampi(m->depth - 8, -64, 63);
            } else {
                uint8_t bit = (uint8_t)(1u << (g_mapCol - 2));
                if (steps & KEY_UP)   m->mask |= bit;
                if (steps & KEY_DOWN) m->mask &= (uint8_t)~bit;
            }
            // A tick is one bit, so a bare A press flips it. Holding A and hunting for a
            // direction to express "on" is busywork.
            if ((edges & KEY_A) && g_mapCol >= 2 && !locked)
                m->mask ^= (uint8_t)(1u << (g_mapCol - 2));

        } else if (adj) {
            if (steps & KEY_UP)    field_adjust(g_page, g_row, +1);
            if (steps & KEY_DOWN)  field_adjust(g_page, g_row, -1);
            if (steps & KEY_RIGHT) field_adjust(g_page, g_row, +8);
            if (steps & KEY_LEFT)  field_adjust(g_page, g_row, -8);

        } else {
            // Wrap by comparison, not by %. The divisor is a variable and we link -nostdlib.
            if (steps & KEY_DOWN) g_row = (uint8_t)((g_row + 1 >= rows) ? 0 : g_row + 1);
            if (steps & KEY_UP)   g_row = (uint8_t)((g_row == 0) ? rows - 1 : g_row - 1);
        }

        rows = page_rows(g_page);
        if (g_row >= rows) g_row = (uint8_t)(rows - 1);
        vis = visible_rows(g_page);
        if (g_row < g_scroll)        g_scroll = g_row;
        if (g_row >= g_scroll + vis) g_scroll = (uint8_t)(g_row - vis + 1);
        if (g_scroll + vis > rows)   g_scroll = (uint8_t)((rows > vis) ? rows - vis : 0);
    }

    // A completed LOAD replaces the whole patch, so everything cached from it has to go.
    if (g_loadReady) {
        g_loadReady = 0;
        const Patch *in = (const Patch *)g_patchBuf;
        if (!synth_patch_valid(in)) {
            g_xferResult = LINK_RESULT_BAD;
        } else {
            // Byte loop, not a struct assignment: GCC turns `g_patch = *in` into a call to
            // memcpy, which -nostdlib cannot resolve.
            uint8_t *dst = (uint8_t *)&g_patch;
            const uint8_t *src = (const uint8_t *)in;
            for (int i = 0; i < synth_patch_bytes(); i++) dst[i] = src[i];
            // Migrate BEFORE applying: synth_patch_applied() derives cached values from fields.
            synth_patch_migrate(&g_patch);
            synth_patch_applied();
            g_repaint = 1;
        }
    }

    // WHILE SELECT IS HELD YOU ARE NAVIGATING, NOT READING: a step blanks the body once and then
    // redraws nothing but the tab bar. The page is drawn when SELECT comes back up.
    int selecting = g_editMode && (g_btn & KEY_SELECT);
    if (!selecting && g_navBlank) { g_navBlank = 0; g_repaint = 1; }

    if (g_editMode != g_lastMode) { g_lastMode = g_editMode; g_repaint = 1; }

    // Drum mode changes what MIX, MAP and the FM page's voice row show, and it can be flipped by a
    // button from any page.
    {
        static uint8_t lastDrums = 0xFF;
        if (g_patch.drumMode != lastDrums) {
            if (lastDrums != 0xFF && g_editMode) { g_editCacheValid = 0; for (int i = 0; i < FM_SLOTS; i++) g_mixCache[i] = -1; }
            lastDrums = g_patch.drumMode;
        }
    }

    if (g_repaint) {
        g_repaint = 0;
        invalidate();
        g_tabPage = -1;
        if (g_editMode) edit_static(); else play_static();
    }

    if (g_editMode) {
        if (selecting) {
            if (!g_navBlank) {
                g_navBlank = 1;
                rect_clear(0, 17, SCREEN_W, HINT_Y - 19, COL_BG);
            }
            if (g_tabPage != g_page) { g_tabPage = g_page; draw_tabs(); }
        }
        else if (g_page == PAGE_MEM) draw_mem_page();
        else if (g_page == PAGE_MIX) draw_mixer();
        else {
            edit_list();
            if (g_page == PAGE_ENV) draw_env_graph(14, 104, 212, 38);
        }
    } else {
        play_frame();
    }

    link_post_status(g_page, g_editMode ? GBA_MODE_EDIT : GBA_MODE_PLAY,
                     (uint8_t)((g_anyNoteOn ? GBA_FLAG_NOTE_ON : 0) |
                               (g_gate      ? GBA_FLAG_GATE    : 0) |
                               ((g_hold[0] | g_hold[1]) ? GBA_FLAG_LATCH : 0)));
}

// presets.h — factory sounds, because a 2-operator FM voice is hard to program from nothing.
//
// A preset is a SOUND, not a whole voice: loading one replaces ratio, detune, index, feedback,
// noise, the pitch envelope and both envelopes, and leaves the voice's level, pan, octave and
// portamento alone, so browsing never moves a mix. Drum presets also set the note they are struck
// at (a kick is a pitch as much as a timbre); tuned presets leave the voice's 0 V note where it was.
#pragma once

#include <stdint.h>
#include "synth.h"

#define PRESET_TUNED 16          // 0..15 tuned, then the untuned/drum sounds
#define PRESET_COUNT 26

const char *preset_name(int i);
void preset_load(Voice *v, int i);

// The 4-op voice's own list. Loading replaces the algorithm, feedback, pitch envelope and all four
// operators; level, pan, note, octave and portamento stay.
#define PRESET4_COUNT 16

const char *preset4_name(int i);
void preset4_load(Voice4 *v, int i);

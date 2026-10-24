// payload/main.c — GBA-side multiboot payload for the Workshop Computer link.
//
// Runs from EWRAM after the RP2040 uploads it via BIOS Multiboot (the audio path runs from IWRAM,
// copied there by crt0.s). It is a 2-operator FM voice played by the modular: the Workshop
// streams its inputs down, this side owns pitch tracking, the envelopes, the sound and the UI.
//
// Order of business, and the order matters:
//   1. LCD up.
//   2. The interrupt dispatcher installed, before anything that enables an interrupt source.
//   3. Sound up (audio_init sets SOUNDCNT_X bit 7 first, without which every other sound register
//      write is silently discarded) - from here the audio runs on its own, from the DMA interrupt.
//   4. Serial slave up, armed, and its interrupt enabled.
//   5. Loop: run the control tick, draw a frame, keep the link serviced throughout.
//
// Dependency-free (no libgba, no libc) so it builds with a bare arm-none-eabi toolchain
// targeting armv4t. See build.sh.

#include <stdint.h>

#include "gfx.h"
#include "irq.h"
#include "link.h"
#include "synth.h"
#include "ui.h"
#include "diag.h"

#define REG_BASE   0x04000000
#define REG_VCOUNT (*(volatile uint16_t *)(REG_BASE + 0x0006))

// Called from inside every drawing routine and from the frame wait. The synth's control tick has
// to keep running while the screen is being drawn, or its rate becomes the frame rate and every
// portamento time changes depending on which page you happen to be looking at. (The audio and
// the envelopes do not depend on this at all: they run from the DMA interrupt.)
void gfx_idle(void)
{
    link_service();
    synth_update();
}

int main(void)
{
    // ---- 1. LCD up ----
    gfx_init();

    // ---- 2. interrupts, 3. sound, 4. link ----
    irq_init();
    synth_init();      // calls audio_init(), which sets the master enable first
    link_init();
    ui_init();

    // NO SPLASH IN THE NORMAL CASE. The green screen crt0.s paints is the boot proof, and the host
    // starts talking within milliseconds. A message is drawn only if the host stays silent.
    //
    // The voice is already live while this waits: A triggers a note and B holds one, so the sound
    // can be checked with no host at all (an emulator, or a console on its own).
    {
        uint32_t guard = 0;
        while (g_rx == 0 && ++guard < 200000u) gfx_idle();
        if (g_rx == 0) {
            rect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
            text_centre(56, "MTM WORKSHOP COMPUTER", COL_TITLE, 1);
            text_centre(72, "GBA FM VOICE", COL_OK, 1);
            rect(40, 88, SCREEN_W - 80, 1, COL_DIM);
            text_centre(100, "WAITING FOR HOST", COL_WAIT, 1);
            text_centre(116, "A PLAYS  B HOLDS", COL_DIM, 1);
            while (g_rx == 0) gfx_idle();
        }
    }

    for (;;) {
        synth_update();

        // The diagnostics take over the whole screen when their magic words arrive. Their
        // magics live under the protocol tag reserved for them, so a live stream word can
        // never fall into this by accident.
        if (diag_active()) {
            diag_frame();
            continue;
        }

        ui_frame();

        // Pace to roughly one frame without ever blocking the link or stalling the synth.
        while (REG_VCOUNT <  SCREEN_H) gfx_idle();
        while (REG_VCOUNT >= SCREEN_H) gfx_idle();
    }
}

// audio.h — Direct Sound, stereo: Timer 0 clocks samples out of FIFO A (left) and FIFO B
// (right); DMA1 keeps A fed from the left ring, DMA2 keeps B fed from the right ring.
//
// THE RINGS AND THE INTERRUPTS. Each DMA runs in sound-FIFO mode: each time its FIFO drains to
// half full it moves one block of four words (sixteen 8-bit samples) from its ring, and with its
// IRQ bit set it raises an interrupt after every block. Both FIFOs drain on the same timer, so
// both request on the same tick; DMA1 has priority and goes first, DMA2 straight after.
//
//   DMA1's interrupt: if its block was the ring's last, restart DMA1 at the ring's start. In FIFO
//      mode the source address never wraps on its own; left alone it walks off the end of the ring
//      into whatever follows. The next request is a whole block away, so the window is wide, and
//      because the restart is driven by the fetches themselves rather than by a playback timer it
//      cannot land on the wrong side of one.
//   DMA2's interrupt: the same restart for DMA2, then render sixteen fresh samples into the block
//      BOTH channels have just finished with. Rendering here rather than on DMA1 is what makes
//      that true: DMA2 is the lower priority, so when its block is done DMA1's already was.
//
// Rendering from the interrupt, one block at a time, is what makes the audio independent of the
// main loop: a page clear that stalls gfx_idle() for milliseconds cannot starve it. The serial
// interrupt waits at most one block render behind it - well under 100 us with three voices.
#pragma once

#include <stdint.h>
#include "fm_tables.h"

#define AUDIO_BLOCKS 16                       // each ring = 256 bytes = two 128-sample halves

void audio_init(void);
void audio_on_dma1_irq(void);                 // called from irq_dispatch only
void audio_on_dma2_irq(void);                 // called from irq_dispatch only

// DSP load, as a percentage of one block's time budget spent rendering it. Measured on Timer 3
// inside the handler, so it excludes the BIOS dispatch overhead (a few dozen cycles per block).
extern volatile uint16_t g_dspAvg;            // smoothed, percent x 16
extern volatile uint16_t g_dspPeak;           // worst block since boot, percent x 16

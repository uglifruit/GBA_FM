// irq.c — the shared interrupt dispatcher.

#include "irq.h"
#include "gba.h"
#include "audio.h"
#include "link.h"

#define REG_IE  R16(0x0200)
#define REG_IF  R16(0x0202)
#define REG_IME R16(0x0208)

// The BIOS reads the user IRQ handler address from here — multiboot images included.
#define BIOS_IRQ_VECTOR (*(volatile uint32_t *)0x03007FFC)

// The BIOS handler saves r0-r3/r12/lr and calls this in IRQ mode with IME already 0, so nothing
// here can be interrupted. That is what makes the ORDER the priority scheme:
//
//   1. AUDIO FIRST, DMA1 then DMA2. Each one's hard deadline is restarting its DMA before its
//      FIFO's next request, sixteen samples (~488 us) after the block that raised it. Missing it
//      plays whatever memory follows the ring. DMA2's handler also renders the next block.
//   2. SERIAL SECOND. Its deadline is re-arming the slave before the host clocks the next word,
//      about 680 us at the 1 kHz poll. The audio render ahead of it costs well under 100 us even
//      with three voices sounding, so an interrupt that finds everything pending still meets
//      every deadline.
//
// Only the bits read on entry are acknowledged. A source that fires while this runs stays pending
// and re-enters the handler on return, rather than being cleared unseen.
IWRAM_CODE void irq_dispatch(void)
{
    uint16_t pending = REG_IF;
    if (pending & IRQ_DMA1)   audio_on_dma1_irq();
    if (pending & IRQ_DMA2)   audio_on_dma2_irq();
    if (pending & IRQ_SERIAL) link_on_serial_irq();
    REG_IF = pending;
}

void irq_init(void)
{
    REG_IME = 0;
    REG_IE  = 0;
    BIOS_IRQ_VECTOR = (uint32_t)irq_dispatch;
    REG_IF  = 0xFFFF;                          // clear anything stale before enabling
    REG_IME = 1;
}

void irq_enable(uint16_t mask)
{
    uint16_t ime = REG_IME;
    REG_IME = 0;
    REG_IE |= mask;
    REG_IME = ime;
}

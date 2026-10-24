// irq.h — the one interrupt handler, shared by the serial link and the audio DMA.
//
// The BIOS has a single user IRQ vector (0x03007FFC), so two interrupt-driven subsystems need one
// dispatcher between them. In 108 the link owned the vector outright; here it is installed once,
// before either subsystem starts, and each of them only enables its own bit.
#pragma once

#include <stdint.h>

#define IRQ_SERIAL (1u << 7)
#define IRQ_DMA1   (1u << 9)
#define IRQ_DMA2   (1u << 10)

void irq_init(void);               // install the dispatcher; call before link_init/audio_init
void irq_enable(uint16_t mask);    // OR bits into IE, interrupts masked around the write

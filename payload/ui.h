// ui.h — the screen: a PLAY readout and a paged editor.
#pragma once

#include <stdint.h>

// Page order is the order they appear on the tab bar. MEM COMES FIRST AND START LANDS ON IT,
// because recalling a patch is the one editor action that happens mid-performance. Then the sound
// (FM, ENV), the balance (MIX), the routing (MAP, BTN), and the housekeeping (SET, CAL).
//
// These values ARE the index into page_tab[] in ui.c, and the status word sends the page number
// upstream to the LEDs. Renumbering here without reordering that array gives a tab bar that lies
// rather than a compile error.
#define PAGE_MEM   0
#define PAGE_FM    1
#define PAGE_ENV   2
#define PAGE_MIX   3
#define PAGE_MAP   4
#define PAGE_BTN   5
#define PAGE_SET   6
#define PAGE_CAL   7
#define PAGE_COUNT 8

void ui_init(void);
void ui_frame(void);      // one frame: handle buttons, redraw what changed

extern uint8_t g_page;

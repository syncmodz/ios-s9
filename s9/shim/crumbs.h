#pragma once
#include <stdint.h>
#include "fb.h"

/*
 * SWEET: "crumbs" — the state of the previous boot, drawn on the next one.
 *
 * Why
 * ---
 * The persistent log lives on the UFS and only gets written once the kernel
 * has mounted the root; a hang before that leaves nothing behind, and the
 * pstore zone dies with the reset. So the only channel for a failed early boot
 * has been a photo of the screen, taken by hand, at the right moment.
 *
 * The Galaxy S5 port (Quantitative101/iOS7-KLTE-GalaxyS5, s5/stub/stub.c) does
 * this instead: the stub writes 64 words into a fixed page of RAM, and on the
 * next boot it draws those words on the screen as a grid of squares -- 24 px
 * for a 1, 6 px for a 0 -- and waits about ten seconds for a photo. RAM
 * survives a warm reset, so the boot that failed reports itself on the boot
 * that follows, with nobody watching the first one.
 *
 * Layout
 * ------
 * The page sits at 0x9d000000, 4 MB below the splash framebuffer at
 * 0x9d400000, inside the same 32 MB Normal-NC block that start.s maps. It is
 * outside the kernel's RAM (which starts at 0x80000000) and outside the
 * framebuffer, so nothing else writes there.
 *
 *   word 0   magic 0x53574352 ("SWCR") | stage in the low byte
 *   word 1   stage counter (monotonic across boots)
 *   word 2   last mark from the kernel's early console
 *   word 3   ESR_EL1 at the last fault, if any
 *   word 4   ELR_EL1 at the last fault
 *   word 5   FAR_EL1 at the last fault
 *   word 6   boot count
 *   word 7   reserved
 *   words 8..63 free for the kernel to fill
 *
 * The shim only reads and draws; the kernel writes. A word of 0 draws as a
 * small dot, so an untouched page is a readable "nothing happened" pattern.
 */

#define CRUMB_PA        0x9d000000UL
#define CRUMB_MAGIC     0x53574300U     /* "SWC" in the high bytes; low byte is the stage */
#define CRUMB_MASK      0xffffff00U
#define CRUMB_WORDS     64
#define CRUMB_SHOW_MS   10000           /* how long the grid stays up */

static inline volatile uint32_t *
crumb_page(void)
{
	return (volatile uint32_t *)(uintptr_t)CRUMB_PA;
}

/* Called by the shim before it draws the boot scene. */
static inline void
crumbs_show_previous(void)
{
	volatile uint32_t *c = crumb_page();
	uint32_t magic = c[0] & CRUMB_MASK;

	if (magic != CRUMB_MAGIC) {
		return;         /* first boot, or RAM was not kept */
	}
	/* Show it once: a reset while the grid is up must not repeat it. */
	c[0] = 0;

	fb_rect(0, 0, fb.width, fb.height, BLACK);

	/* 64 words as 64 rows of 32 bits, most significant bit first. */
	const int cell = 24, gap = 6, pitch = cell + gap;
	const int x0 = (fb.width - 32 * pitch) / 2;
	const int y0 = (fb.height - CRUMB_WORDS * pitch) / 2;

	for (int v = 0; v < CRUMB_WORDS; v++) {
		uint32_t w = c[v];
		for (int b = 0; b < 32; b++) {
			int big = (w >> (31 - b)) & 1;
			int sz = big ? cell : gap;
			fb_rect(x0 + b * pitch + (cell - sz) / 2,
			    y0 + v * pitch + (cell - sz) / 2, sz, sz,
			    big ? WHITE : GREY);
		}
	}
}

/* Called by the shim right before it jumps to the kernel. */
static inline void
crumbs_begin_boot(void)
{
	volatile uint32_t *c = crumb_page();

	if ((c[0] & CRUMB_MASK) != CRUMB_MAGIC) {
		for (int i = 0; i < CRUMB_WORDS; i++) {
			c[i] = 0;
		}
		c[6] = 0;
	}
	c[6]++;                         /* boot count */
	c[0] = CRUMB_MAGIC | 1;         /* stage 1: shim is about to jump */
}

/* True when the page already belongs to us (the shim's magic is present). */
static inline int
crumbs_valid(void)
{
	return (crumb_page()[0] & CRUMB_MASK) == CRUMB_MAGIC;
}

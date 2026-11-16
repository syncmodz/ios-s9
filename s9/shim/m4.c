/*
 * Galaxy S9 milestone 4: boot XNU. The ramdisk of the boot image holds the
 * SWEETXNU payload (tools/mkpayload.py: kernel + ADT + boot-args); loader.c
 * plays iBoot and jumps to it with the MMU off. The APSS watchdog stays off:
 * XNU does not feed it.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "loader.h"
#include "plat.h"
#include "bootlogo.h"
#include "crumbs.h"

#define RUN_SECONDS	90

static const char *stage;	/* set at runtime: no absolute pointers in data */

void exc_handler(uint64_t slot, uint64_t *regs)
{
	panic_screen(stage, slot, regs);
}

void shim_main(uint64_t dtb)
{
	/* M1.1: 0x9d400000, 1440x2960, stride 5760, B G R A, autorefresh on */
	fb_default();

	/*
	 * One static frame: the apple, centred, no spinner. The kernel takes this
	 * framebuffer over at the jump and its early console paints the same
	 * artwork in the band below the log, so anything animated here would
	 * freeze on the first frame anyway.
	 */
	/*
	 * SWEET: if the previous boot wrote crumbs, show them before anything
	 * else. A boot that dies before the UFS log exists leaves its state here,
	 * and this is the only channel that survives the reset. See crumbs.h and
	 * the Galaxy S5 port's stub (s5/stub/stub.c).
	 */
	crumbs_show_previous();

	draw_boot_scene();
	crumbs_begin_boot();

	stage = "loader";
	const char *why = loader_boot(dtb);	/* returns only if it could not jump */

	/* Only reach here on failure — switch to diagnostic view */
	con_clear(GREY);
	con_title("SHIM M4: XNU");
	con_hex2("SHIM", "dtb=", dtb, "img=", (uint32_t)(uintptr_t)shim_main);
	con_str("LOADER", why);
	con_skip();
	con_str("", "Tire uma foto desta tela!");
	countdown_reboot(con_row(), RUN_SECONDS);
}

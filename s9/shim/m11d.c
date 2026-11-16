/*
 * M11.1 (2026-10-29), build.sh m11d = V3 below + per-register probes on a worker
 * core. Still READ-ONLY: no SMMU write, the GMU is not released (default build).
 * V3 froze on the S9 at 2b: the first GPU-SMMU reads (IDR0, IDR1, sCR0, issued
 * back to back before anything was shown) never answered, while the S9's own
 * Linux reads and resets this SMMU (TWRP dmesg: 5 SMRs, 5 context banks; DT asks
 * only gpu_cx_gdsc + gcc_gpu_memnoc_gfx_clk). And qcom,hyp_secure_alloc sits in
 * qcom,kgsl-iommu: a KGSL flag for SECURE buffers (kgsl_sharedmem.c), not
 * hypervisor ownership of the stream table. So M11.1 [M11.1 tags]:
 *   - shows the CBCR of every clock stage 1 turns on (did each really start?);
 *   - runs each GPU-SMMU read on a secondary core (PSCI CPU_ON, as M2) while CPU0
 *     waits 100 ms and reports: a read that never answers freezes only that
 *     worker (7 of them), so one boot maps which registers answer;
 *   - a worker's abort is reported (ESR/FAR), not drawn as a panic.
 * Evidence: logs/evidence-20261029/m11-v3-result.md.
 */
/*
 * ============================================================================
 * M11 CANONICAL DRAFT V3 (fajita-informed, FAIL-SAFE) - UNVALIDATED, SOURCE-ONLY
 * ============================================================================
 * Galaxy S9 SM-G9600 / starqltechn / SDM845 / Adreno 630 - GPU SMMU + HFI.
 * NOT built, NOT linked, NOT flashed, NOT run. Rebuild on Linux/CI.
 *
 * Line-spliced from the COMMITTED shim/m11.c (git blob 7d8a1c53): every line not
 * in the edit regions is byte-identical. The committed context-bank body is
 * present in full (compiled only in the translate experiment).
 *
 * WHY V3 (the safety gate V2 lacked): V2's default proceeded to HFI via bypass
 * (physical addresses). fajita's device log on the IDENTICAL A630 (NOTES L1079)
 * shows the boot loader leaves the GMU streams in FAULT (smr=0, s2cr=0x200ff),
 * NOT bypass, so a physical HFI address is UNREACHABLE; and the only way to
 * change the stream table is the hyp-owned write that FROZE the phone (0112).
 * So neither bypass nor translate is established safe for the S9. V3 therefore:
 *   - DEFAULT: writes NO SMMU register and does NOT release the GMU; it is a
 *     READ-ONLY diagnostic (power, SMMU IDs, and the smr/s2cr routing - the
 *     decisive datum). shim_main stops before gmu_boot when stage 3 = HFI_NONE.
 *   - -DM11_TRY_TRANSLATE=1 : qcom micro-MMU/impl-def reset + the committed
 *     context-bank + stream-table programming, then HFI (freeze risk; watchdog).
 *   - -DM11_TRY_BYPASS_HFI=1 : release the GMU with PHYSICAL HFI addresses to
 *     CONFIRM the fault on the S9 (bounded: SGFSR + timeout, no brick).
 * Changes vs committed are tagged V2/V3/[S9-DT]/[FAJITA-DEV]/[EXPERIMENT].
 * See M11_FAJITA_CANONICAL_RATIONALE_V3.md.
 * ============================================================================
 */
/*
 * Galaxy S9 milestone 11 (GPU stage B): give the GMU its memory through the
 * GPU's own SMMU and get it talking to us over its message channel (HFI).
 *
 * M10 proved the GMU's firmware boots (0xBABEFACE) with no SMMU: the boot does
 * not reach RAM. HFI does: the GMU reads and writes a table of message queues
 * in memory of ours, which it reaches by the addresses 0x60000000.. through the
 * GPU's SMMU (a memory translator, like a CPU's MMU but for the GPU block). So
 * the SMMU must be programmed and the queue table told to the GMU (HFI_QTBL_ADDR)
 * BEFORE the processor is let go -- this stage therefore redoes M10's power +
 * firmware + boot, now with the SMMU in place, and then sends the GMU two
 * messages (INIT, FW_VER) and reads back its firmware version.
 *
 * The one S9-specific unknown is the hypervisor. On the Poco F1 (no Samsung RKP)
 * both reference projects programmed this SMMU directly. The S9's hypervisor may
 * emulate or refuse its global registers (stream table, context base). So stage
 * 3 READS the SMMU first (layout, and what the boot loader/hyp left) and stage 4
 * WRITES it and reads every write back -- if the hyp blocks it, the photo shows
 * exactly which register did not take, and HFI then faults (logged), never a brick.
 *
 * The GX rail (the 3D core's power) and the CP are NOT here: those are stage C
 * (M12), isolated because powering GX is the step that can reset the SoC.
 *
 * MMU off: all memory is Device, so our page tables and the GMU's queue memory
 * are static buffers whose physical address equals their address. Register
 * numbers and sequences from the S9's Android kernel (drivers/gpu/msm) and the
 * live device tree; the SMMU programming follows the smmu-v2 order of Linux
 * arm-smmu.c / io-pgtable-arm.c (facts, no code copied).
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "plat.h"

#if defined(__has_include)
#if __has_include("fw_a630_gmu.h")
#include "fw_a630_gmu.h"
#endif
#endif

#define END_SECONDS		120	/* [M11.1] more time for the photo */

/* ---- register blocks (absolute physical addresses; MMU off) ---------------------------------------- */

#define GPUCC			0x05090000UL
#define GCC			0x00100000UL
#define GMU_BASE		0x0506a000UL
#define PDC_BASE		0x0b200000UL
#define SMMU_BASE		0x05040000UL
#define SMMU_SIZE		0x10000UL

#define GMU(n)			(GMU_BASE + (((uint32_t)(n) - 0x1A800u) << 2))
#define PDC(w)			(PDC_BASE + ((uint32_t)(w) << 2))

/* gpucc / GCC (as M10) */
#define GPUCC_CX_GDSCR		0x106c
#define GPUCC_CX_HWCTRL		0x1540
#define GPUCC_GX_GDSCR		0x100c
#define GPUCC_GMU_RCG_CMD	0x1120
#define GPUCC_GMU_RCG_CFG	0x1124
#define GPUCC_CBCR_AHB		0x1078
#define GPUCC_CBCR_CX_GMU	0x1098
#define GPUCC_CBCR_CXO		0x109c
#define GPUCC_CBCR_CXO_AON	0x1004		/* V2 (14:23/14:26): always-on CXO */
#define GPUCC_CBCR_CRC_AHB	0x107c		/* V2: GPU reg-access AHB; SMMU 0x5040000 behind it */
#define GPUCC_CBCR_CX_APB	0x1088		/* V2 */
#define GPUCC_CBCR_CX_SNOC_DVM	0x108c		/* V2 */
#define GCC_GPLL0_VOTE		0x52004
#define GCC_CFG_AHB		0x71004
#define GCC_MEMNOC_GFX		0x7100c
#define GCC_SNOC_DVM_GFX	0x71018
#define GCC_DDRSS_GPU_AXI	0x44038

#define SW_COLLAPSE		(1u << 0)
#define PWR_ON			(1u << 31)
#define CBCR_ENABLE		(1u << 0)
#define CBCR_OFF		(1u << 31)
#define CMD_UPDATE		(1u << 0)

/* GMU registers (as M10, plus HFI) */
#define GMU_CM3_SYSRESET	0x1F800
#define GMU_CM3_BOOT_CONFIG	0x1F801
#define GMU_CX_GMU_WFI_CONFIG	0x1F802
#define GMU_CM3_FW_INIT_RESULT	0x1F81C
#define GMU_CM3_ITCM_START	0x1B400
#define GMU_SYS_BUS_CONFIG	0x1F40F
#define GMU_PWR_COL_FRAME_CTRL	0x1F8C0
#define GMU_RPMH_CTRL		0x1F8E8
#define GMU_RPMH_POWER_STATE	0x1F8EC
#define GMU_HFI_SFR_ADDR	0x1F982
#define GMU_HFI_MMAP_ADDR	0x1F983
#define GMU_HFI_QTBL_INFO	0x1F984
#define GMU_HFI_QTBL_ADDR	0x1F985
#define GMU_HFI_CTRL_INIT	0x1F986
#define GMU_HFI_CTRL_STATUS	0x1F980
#define GMU_HFI_VERSION_INFO	0x1F981
#define GMU_GMU2HOST_INTR_INFO	0x1F992
#define GMU_GMU2HOST_INTR_CLR	0x1F991
#define GMU_GMU2HOST_INTR_MASK	0x1F993
#define GMU_HOST2GMU_INTR_SET	0x1F994
#define GMU_GENERAL_7		0x1F9CC
#define GMU_AHB_FENCE_RANGE_0	0x23B11
#define GMU_AO_AHB_FENCE_CTRL	0x23B10
#define GMU_AO_HOST_INTR_STATUS	0x23B05

/* RSC/PDC sequencer (as M10) */
#define GPU_RSCC_RSC_STATUS0	0x23404
#define RSCC_PDC_SEQ_START_ADDR	0x23408
#define RSCC_PDC_MATCH_VALUE_LO	0x23409
#define RSCC_PDC_MATCH_VALUE_HI	0x2340A
#define RSCC_PDC_SLAVE_ID_DRV0	0x2340B
#define RSCC_HIDDEN_TCS_CMD0_ADDR 0x2340D
#define RSCC_HIDDEN_TCS_CMD0_DATA 0x2340E
#define RSCC_OVERRIDE_START_ADDR 0x23500
#define RSCC_SEQ_MEM_0_DRV0	0x23580
#define RSC_CMD_OFFSET		2
#define PDC_GPU_ENABLE_PDC	0x21140
#define PDC_GPU_SEQ_START_ADDR	0x21148
#define PDC_GPU_SEQ_MEM_0	0xA0000
#define PDC_GPU_TCS1_CONTROL	0x21572
#define PDC_GPU_TCS1_CMD_ENABLE_BANK 0x21573
#define PDC_GPU_TCS1_CMD_WAIT_FOR_CMPL_BANK 0x21574
#define PDC_GPU_TCS1_CMD0_MSGID	0x21575
#define PDC_GPU_TCS1_CMD0_ADDR	0x21576
#define PDC_GPU_TCS1_CMD0_DATA	0x21577
#define PDC_GPU_TCS3_CONTROL	0x215D6
#define PDC_GPU_TCS3_CMD_ENABLE_BANK 0x215D7
#define PDC_GPU_TCS3_CMD_WAIT_FOR_CMPL_BANK 0x215D8
#define PDC_GPU_TCS3_CMD0_MSGID	0x215D9
#define PDC_GPU_TCS3_CMD0_ADDR	0x215DA
#define PDC_GPU_TCS3_CMD0_DATA	0x215DB
#define PDC_CMD_OFFSET		4

#define GMU_RCG_CFG_200MHZ	0x602
#define GMU_CHIPID		0x06030100
#define GMU_BOOT_CONFIG_TCM	2
#define FENCE_RANGE_MASK	((1u << 31) | ((0xAu << 2) << 18) | 0x8A0u)
#define GMU_BABEFACE		0xBABEFACEu

/* ---- GPU SMMU (arm,smmu-kgsl@5040000, qcom,smmu-v2) ------------------------------------------------- */

#define SMMU_SCR0		0x000
#define SMMU_IDR0		0x020		/* [7:0] stream match registers */
#define SMMU_IDR1		0x024		/* bit31 64K pages, [30:28] log2(pages)-1, [7:0] contexts */
#define SMMU_IDR2		0x028
#define SMMU_SGFSR		0x048
#define SMMU_SMR(n)		(0x800 + 4 * (n))	/* bit31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)		(0xc00 + 4 * (n))	/* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define SMMU_CBAR(n)		(4 * (n))		/* second page */
#define SMMU_CBA2R(n)		(0x800 + 4 * (n))	/* second page */
#define SMMU_CB_SCTLR		0x00
#define SMMU_CB_TCR2		0x10
#define SMMU_CB_TTBR0		0x20
#define SMMU_CB_TTBR1		0x28
#define SMMU_CB_TCR		0x30
#define SMMU_CB_MAIR0		0x38
#define SMMU_CB_MAIR1		0x3c
#define SMMU_CB_FSR		0x58
#define SMMU_CB_FAR		0x60
#define SMMU_CB_FSYNR0		0x68
#define SMMU_CB_TLBIALL		0x618
#define SMMU_CB_TLBSYNC		0x7f0
#define SMMU_CB_TLBSTATUS	0x7f4

#define SMMU_SMR_VALID		(1u << 31)
#define SMMU_S2CR_FAULT		(2u << 16)
#define SMMU_CBAR_STAGE_1	((1u << 16) | (3u << 8) | (0xfu << 12))	/* type S1, weakest defaults */
#define SMMU_CBA2R_64BIT	1u
#define SMMU_CB_SCTLR_ON	((1u << 0) | (1u << 1) | (1u << 2) | (1u << 5) | (1u << 12)) /* on, faults recorded, no irq */
#define SMMU_TCR_39BITS		(25u | (1u << 8) | (1u << 10) | (3u << 12) | (1u << 23))
#define SMMU_TCR2_VAL		(1u | (1u << 4) | (7u << 15))	/* 36-bit PA, 16-bit ASID */
#define SMMU_MAIR0_VAL		0xf404ff44u	/* 0 uncached, 1 cached, 2 device */
#define SMMU_FSR_ALL		0xc00001feu

/* V2: qcom qsmmuv2 impl-defs + the hypervisor bit (S9 DT + fajita 0112/0125) */
#define SMMU_MICRO_MMU_CTRL	0x6000		/* S9 DT: qcom,micro-mmu-control = <0x6000> */
#define SMMU_MICRO_HALT		(1u << 2)
#define SMMU_MICRO_IDLE		(1u << 3)
#define SMMU_SCR0_HYP		(1u << 21)	/* fajita 0112: QCOM hyp owns the stream table */
#define APPS_SMMU_BASE		0x15000000u	/* S9 DT: apps-smmu@0x15000000 (control read only) */

/* Page tables for the GMU's SMMU context (3-level, 4K pages, MMU off so PA = address). */
#define PTE_TABLE		3ull
#define PTE_PAGE		3ull
#define PTE_ACCESSED		(1ull << 10)
#define PTE_SHARED		(2ull << 8)
#define PTE_NOT_GLOBAL		(1ull << 11)
#define PTE_GMU			(PTE_SHARED | PTE_ACCESSED | PTE_NOT_GLOBAL)
#define PTE_ADDR		0x0000fffffffff000ull

/* GMU memory layout (Samsung a630: HFI at 0x60000000, dump at +1 MiB). */
#define GMU_HFI_ADDR		0x60000000u
#define GMU_DUMP_ADDR		0x60100000u
#define GMU_MEM_SIZE		0x4000u		/* 16 KiB each */
#define GMU_STREAM_USER		4
#define GMU_STREAM_KERNEL	5
#define GMU_CB			0		/* context bank the GMU gets */

/* HFI message protocol (kgsl_hfi.h) */
#define HFI_QUEUES		3		/* CMD(0), MSG(1), DBG(2) */
#define HFI_QUEUE_WORDS		1024
#define HFI_MSG_CMD		0
#define H2F_MSG_INIT		0
#define H2F_MSG_FW_VER		1
#define F2H_MSG_ACK		126
#define HFI_IRQ_MSGQ		(1u << 0)

static const char *stage;
static int step_row;

/* Static memory, 4K-aligned; with MMU off, physical address == address. */
static uint8_t smmu_pool[8][4096] __attribute__((aligned(4096)));
static unsigned smmu_pool_used;
static uint8_t gmu_hfi_mem[GMU_MEM_SIZE] __attribute__((aligned(4096)));
static uint8_t gmu_dump_mem[GMU_MEM_SIZE] __attribute__((aligned(4096)));

static uint32_t smmu_page = 0x1000, smmu_pages = 8, smmu_contexts, smmu_smrs;

/* V2/V3: which HFI address path stage 3 established. */
#define HFI_NONE	0	/* no reachable path: do NOT release the GMU for HFI */
#define HFI_TRANSLATE	1	/* SMMU context programmed; GMU-VA 0x60000000 */
#define HFI_BYPASS	2	/* physical addressing (experiment; expected to fault) */
static int smmu_translate;
static int smmu_stream_is_hyp;
static int hfi_path;
static uint32_t gmu_hfi_va, gmu_dump_va;	/* GMU-facing addresses of the HFI/dump buffers */

/* V2: translate EXPERIMENT (context-bank + stream-table writes). OFF by default:
 * fajita 0112 saw a direct stream-table write FREEZE the identical A630, and the
 * S9 DT confirms the same hyp ownership. Build -DM11_TRY_TRANSLATE=1 to try it. */
#ifndef M11_TRY_TRANSLATE
#define M11_TRY_TRANSLATE 0
#endif

/* V3: bypass-HFI EXPERIMENT (release the GMU with PHYSICAL buffer addresses). OFF
 * by default: fajita device evidence (NOTES L1079) shows the GMU streams are left
 * in FAULT (s2cr=0x200ff), NOT bypass, so a physical HFI address is unreachable
 * and cannot be made reachable without the hyp-owned write that freezes. Build
 * -DM11_TRY_BYPASS_HFI=1 only to confirm the fault on the S9. */
#ifndef M11_TRY_BYPASS_HFI
#define M11_TRY_BYPASS_HFI 0
#endif

#if M11_TRY_TRANSLATE
/* V3: S9 DT (dtb/live-twrp.dts:11861) qcom,kgsl-iommu attach-impl-defs, 12 pairs.
 * Byte-identical to fajita 0125 impl_defs[] (an A630-SMMU property, not board).
 * Used only by the translate experiment's qcom reset. */
static const uint32_t smmu_impl_defs[][2] = {
	{ 0x6000, 0x2378 }, { 0x6060, 0x1055 }, { 0x678c, 0x8 },	{ 0x6794, 0x28 },
	{ 0x6800, 0x6 },    { 0x6900, 0x3ff },  { 0x6924, 0x204 },	{ 0x6928, 0x11000 },
	{ 0x6930, 0x800 },  { 0x6960, 0xffffffff }, { 0x6b64, 0x1a5551 }, { 0x6b68, 0x9a82a382 },
};
#endif

/* ---- [M11.1] risky reads on a worker core ------------------------------------------------------- */

#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_CPU_OFF		0x84000002UL
#define NR_WORKERS		7
#define PROBE_TIMEOUT_MS	100

extern char secondary_entry[];

/* Shared with the worker. MMU off: Device memory, nothing cached, accesses in order. */
static volatile uint64_t probe_addr;
static volatile uint32_t probe_val, probe_seq, probe_ack, probe_quit, probe_ready;
static volatile uint32_t probe_fault, probe_fault_esr;
static volatile uint64_t probe_fault_far;
static unsigned worker, workers_lost;

static uint64_t mpidr(void)
{
	uint64_t v;

	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
	return v;
}

void secondary_main(uint64_t idx)
{
	uint32_t seen = probe_seq;

	probe_ready = (uint32_t)idx;
	dsb();
	while (!probe_quit) {
		uint32_t seq = probe_seq;

		if (seq == seen)
			continue;
		probe_val = rd32(probe_addr);	/* may never return: then only this core is lost */
		dsb();
		{
			uint64_t isr;	/* SError is masked here: a bus error stays pending (ISR_EL1.A) */

			__asm__ volatile("isb; mrs %0, isr_el1" : "=r"(isr));
			if (isr & (1u << 8)) {
				probe_fault_esr = 0x5e5e5e5eu;	/* marker: pending SError, not an ESR */
				probe_fault_far = probe_addr;
				dsb();
				probe_fault = seq;
				dsb();
				for (;;)
					__asm__ volatile("wfe");
			}
		}
		probe_ack = seq;
		dsb();
		seen = seq;
	}
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if ((mpidr() >> 8) & 0xff) {	/* [M11.1] a worker: report the abort to CPU0, park */
		uint64_t esr, far;

		__asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
		__asm__ volatile("mrs %0, far_el1" : "=r"(far));
		probe_fault_esr = (uint32_t)esr;
		probe_fault_far = far;
		dsb();
		probe_fault = probe_seq;
		dsb();
		(void)slot;
		(void)regs;
		for (;;)
			__asm__ volatile("wfe");
	}
	panic_screen(stage, slot, regs);
}

static void step(const char *s)
{
	char b[52], *p = put(b, s);
	int row = con_row();

	while (p < b + 50)
		*p++ = ' ';
	*p = 0;
	con_set_row(step_row);
	con_str("PASSO", b);
	con_set_row(row);
	wdog_pet();
}

static void showhex(const char *label, uint64_t v)
{
	con_hex(label, v);
	wdog_pet();
}

/* [M11.1] Read `addr` on the worker core. 1 = answered (*v); 0 = no answer in time or an
 * abort: that core is left behind and the next probe starts a new one. */
static int probe(const char *label, uintptr_t addr, uint32_t *v)
{
	static uint32_t seq;
	char b[52], *p;

	if (!worker) {
		if (workers_lost >= NR_WORKERS) {
			con_str(label, "sem nucleo livre: nao lido");
			return 0;
		}
		worker = workers_lost + 1;
		probe_ready = 0;
		dsb();
		if (smc(PSCI_CPU_ON, (uint64_t)worker << 8, (uintptr_t)secondary_entry, worker) != 0) {
			con_str(label, "CPU_ON falhou: nao lido");
			worker = 0;
			workers_lost++;
			return 0;
		}
		for (int ms = 0; ms < 500 && probe_ready != worker; ms++)
			delay_ms(1);
		if (probe_ready != worker) {
			con_str(label, "nucleo nao subiu: nao lido");
			worker = 0;
			workers_lost++;
			return 0;
		}
	}
	p = put_hex(put(b, "2b le 0x"), addr, 8);
	put_dec(put(p, " no nucleo "), worker);
	step(b);
	probe_fault = 0;
	probe_addr = addr;
	dsb();
	probe_seq = ++seq;
	dsb();
	for (int ms = 0; ms < PROBE_TIMEOUT_MS; ms++) {
		if (probe_ack == seq) {
			*v = probe_val;
			showhex(label, *v);
			return 1;
		}
		if (probe_fault == seq)
			break;
		delay_ms(1);
	}
	if (probe_fault == seq) {
		p = put_hex(put(b, "ABORT esr "), probe_fault_esr, 8);
		put_hex(put(p, " far "), probe_fault_far, 8);
	} else {
		p = put_hex(put(b, "TRAVOU: sem resposta (0x"), addr, 8);
		put_dec(put(p, ") nucleo "), worker);
	}
	con_str(label, b);
	worker = 0;
	workers_lost++;
	return 0;
}

static void zero(void *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		((volatile uint8_t *)p)[i] = 0;
}

static uint64_t pa(const void *p)
{
	return (uint64_t)(uintptr_t)p;
}

/* ---- power (M10 stages 1-2, condensed) ------------------------------------------------------------- */

static int clk_branch_on(uintptr_t cbcr)
{
	wr32(cbcr, rd32(cbcr) | CBCR_ENABLE);
	dsb();
	for (int i = 0; i < 2000 && (rd32(cbcr) & CBCR_OFF); i++)
		delay_ms(1);
	return !(rd32(cbcr) & CBCR_OFF);
}

static int cx_power_on(void)
{
	uint32_t v = rd32(GPUCC + GPUCC_CX_GDSCR);

	if (v & SW_COLLAPSE) {
		wr32(GPUCC + GPUCC_CX_GDSCR, v & ~SW_COLLAPSE);
		dsb();
	}
	for (int i = 0; i < 2000; i++) {
		if ((rd32(GPUCC + GPUCC_CX_GDSCR) & PWR_ON) || (rd32(GPUCC + GPUCC_CX_HWCTRL) & PWR_ON))
			break;
		delay_ms(1);
	}
	return (rd32(GPUCC + GPUCC_CX_GDSCR) & PWR_ON) || (rd32(GPUCC + GPUCC_CX_HWCTRL) & PWR_ON);
}

static int power_on(void)
{
	stage = "1 energia";
	step("1 GPLL0 + CX GDSC + relogios");
	wr32(GCC + GCC_GPLL0_VOTE, rd32(GCC + GCC_GPLL0_VOTE) | (1u << 15) | (1u << 16));
	dsb();
	if (!clk_branch_on(GCC + GCC_CFG_AHB))
		return 0;
	if (!cx_power_on()) {
		con_str("1", "CX GDSC nao liga: parando");
		return 0;
	}
	showhex("cx gdscr", rd32(GPUCC + GPUCC_CX_GDSCR));
	/* GMU RCG 200 MHz */
	wr32(GPUCC + GPUCC_GMU_RCG_CFG, GMU_RCG_CFG_200MHZ);
	dsb();
	wr32(GPUCC + GPUCC_GMU_RCG_CMD, rd32(GPUCC + GPUCC_GMU_RCG_CMD) | CMD_UPDATE);
	dsb();
	for (int i = 0; i < 2000 && (rd32(GPUCC + GPUCC_GMU_RCG_CMD) & CMD_UPDATE); i++)
		delay_ms(1);
	clk_branch_on(GPUCC + GPUCC_CBCR_CXO_AON);	/* V2: 0x1004 */
	clk_branch_on(GPUCC + GPUCC_CBCR_CXO);
	clk_branch_on(GPUCC + GPUCC_CBCR_AHB);
	clk_branch_on(GPUCC + GPUCC_CBCR_CRC_AHB);	/* V2: the SMMU at 0x5040000 is behind this AHB */
	clk_branch_on(GPUCC + GPUCC_CBCR_CX_APB);	/* V2 */
	clk_branch_on(GPUCC + GPUCC_CBCR_CX_SNOC_DVM);	/* V2 */
	if (!clk_branch_on(GPUCC + GPUCC_CBCR_CX_GMU))
		return 0;
	clk_branch_on(GCC + GCC_MEMNOC_GFX);
	clk_branch_on(GCC + GCC_DDRSS_GPU_AXI);
	clk_branch_on(GCC + GCC_SNOC_DVM_GFX);
	/* [M11.1] CBCR bit 31 = clock OFF: did each one really start? */
	showhex("c cfgahb", rd32(GCC + GCC_CFG_AHB));
	showhex("c mnocgx", rd32(GCC + GCC_MEMNOC_GFX));
	showhex("c gpuaxi", rd32(GCC + GCC_DDRSS_GPU_AXI));
	showhex("c snocdv", rd32(GCC + GCC_SNOC_DVM_GFX));
	showhex("g ahb", rd32(GPUCC + GPUCC_CBCR_AHB));
	showhex("g crcahb", rd32(GPUCC + GPUCC_CBCR_CRC_AHB));
	showhex("g cxgmu", rd32(GPUCC + GPUCC_CBCR_CX_GMU));
	con_str("1", "GPU com energia e relogios (+crc_ahb/apb)");
	return 1;
}

/* ---- stage 2: read the SMMU as the boot loader / hypervisor left it --------------------------------- */

static void smmu_probe(void)
{
	uint32_t idr0, idr1, scr0;

	stage = "2 smmu leitura";
	/* 2a: a read-only control on the apps SMMU (known-good) before the GPU
	 * SMMU, to prove the SMMU-read path. Harmless; aids triage. [14:23 build] */
	step("2a le a SMMU dos apps (controle, 0x15000000)");
	showhex("a idr0", rd32(APPS_SMMU_BASE + SMMU_IDR0));

	stage = "2 smmu da GPU";
	step("2b le a SMMU da GPU (0x5040000)");
	/* [M11.1] one register at a time, each on the worker core, shown before the next:
	 * first those the hyp emulates on the apps SMMU (M7: ID0/ID1/SMR/S2CR/CBAR), then
	 * the rest; sCR0 (froze on the apps SMMU in M7) and the context bank last. */
	idr0 = idr1 = scr0 = 0;
	if (probe("s idr0", SMMU_BASE + SMMU_IDR0, &idr0))
		smmu_smrs = idr0 & 0xff;
	if (probe("s idr1", SMMU_BASE + SMMU_IDR1, &idr1)) {
		smmu_page = (idr1 >> 31) ? 0x10000 : 0x1000;
		smmu_pages = 2u << ((idr1 >> 28) & 7);
		smmu_contexts = idr1 & 0xff;
	}
	if (2 * smmu_pages * smmu_page != SMMU_SIZE) {
		con_str("2", "idr1 nao confiavel: uso 4K/8 (geometria conhecida do A630)");
		smmu_page = 0x1000;
		smmu_pages = 8;
	}
	{
		uint32_t v;

		probe("s idr2", SMMU_BASE + SMMU_IDR2, &v);
		probe("s smr0", SMMU_BASE + SMMU_SMR(0), &v);
		probe("s s2cr0", SMMU_BASE + SMMU_S2CR(0), &v);
		probe("s smr1", SMMU_BASE + SMMU_SMR(1), &v);
		probe("s s2cr1", SMMU_BASE + SMMU_S2CR(1), &v);
		probe("s cbar0", SMMU_BASE + smmu_page + SMMU_CBAR(0), &v);
		probe("s ummu", SMMU_BASE + SMMU_MICRO_MMU_CTRL, &v);
		probe("s sgfsr", SMMU_BASE + SMMU_SGFSR, &v);
		if (probe("s scr0", SMMU_BASE + SMMU_SCR0, &scr0))
			showhex("scr0 b21", scr0 & SMMU_SCR0_HYP);
		probe("s cb0sct", SMMU_BASE + smmu_pages * smmu_page + SMMU_CB_SCTLR, &v);
	}
	showhex("perdidos", workers_lost);
	smmu_stream_is_hyp = 0;		/* [M11.1] no DT basis (see header) */
}

/* ---- stage 3: page tables + the GMU's SMMU context -------------------------------------------------- */

static uint64_t *smmu_new_table(void)
{
	if (smmu_pool_used >= sizeof(smmu_pool) / sizeof(smmu_pool[0]))
		return 0;
	uint64_t *t = (uint64_t *)smmu_pool[smmu_pool_used++];
	zero(t, 4096);
	return t;
}

static uint64_t *smmu_root;

/* Map one 4K page of GMU address `va` to physical `phys`. */
static int smmu_map(uint64_t va, uint64_t phys)
{
	uint64_t *l1 = smmu_root, *l2, *l3;
	uint64_t e;

	e = l1[(va >> 30) & 0x1ff];
	if (e & 1)
		l2 = (uint64_t *)(uintptr_t)(e & PTE_ADDR);
	else {
		l2 = smmu_new_table();
		if (!l2)
			return 0;
		l1[(va >> 30) & 0x1ff] = pa(l2) | PTE_TABLE;
	}
	e = l2[(va >> 21) & 0x1ff];
	if (e & 1)
		l3 = (uint64_t *)(uintptr_t)(e & PTE_ADDR);
	else {
		l3 = smmu_new_table();
		if (!l3)
			return 0;
		l2[(va >> 21) & 0x1ff] = pa(l3) | PTE_TABLE;
	}
	l3[(va >> 12) & 0x1ff] = phys | PTE_GMU | PTE_PAGE;
	return 1;
}

#if M11_TRY_TRANSLATE
/* V2/V3 NEW (translate experiment only): the qcom qsmmuv2 reset the GPU SMMU
 * needs (no qcom,skip-init). Halt the micro-MMU, write the S9's attach-impl-defs,
 * resume. Impl-def window 0x6000..0x6b68 is OUTSIDE the DT's qcom,protect
 * <0x40000 0xc000> and NOT the hyp stream table. [FAJITA-DEV 0125; S9 DT values.]
 * Bounded 300 ms, watchdog-petted. [HYP for the S9: these writes are unproven.] */
static int smmu_qcom_reset(void)
{
	uint32_t ctrl = rd32(SMMU_BASE + SMMU_MICRO_MMU_CTRL);
	int idle = 0;

	showhex("micro-mmu ctrl", ctrl);
	wr32(SMMU_BASE + SMMU_MICRO_MMU_CTRL, ctrl | SMMU_MICRO_HALT);
	dsb();
	for (int i = 0; i < 300; i++) {
		if (rd32(SMMU_BASE + SMMU_MICRO_MMU_CTRL) & SMMU_MICRO_IDLE) {
			idle = 1;
			break;
		}
		delay_ms(1);
		wdog_pet();
	}
	for (unsigned i = 0; i < sizeof(smmu_impl_defs) / sizeof(smmu_impl_defs[0]); i++)
		wr32(SMMU_BASE + smmu_impl_defs[i][0], smmu_impl_defs[i][1]);
	wr32(SMMU_BASE + SMMU_MICRO_MMU_CTRL,
	     rd32(SMMU_BASE + SMMU_MICRO_MMU_CTRL) & ~SMMU_MICRO_HALT);
	dsb();
	showhex("micro-mmu idle", (uint32_t)idle);
	return idle;
}
#endif

static int smmu_context_on(void)
{
	uint32_t cb = GMU_CB, ctx = 0;
	uint64_t ttbr = 0;

	/* Prepare bypass addressing (physical); the translate path overrides it. */
	smmu_translate = 0;
	gmu_hfi_va = (uint32_t)pa(gmu_hfi_mem);
	gmu_dump_va = (uint32_t)pa(gmu_dump_mem);

	stage = "3 smmu contexto";
	step("3a tabelas p/ HFI 0x60000000 + dump (para o modo translate)");
	smmu_root = smmu_new_table();
	if (!smmu_root)
		return HFI_NONE;
	for (uint32_t off = 0; off < GMU_MEM_SIZE; off += 4096)
		if (!smmu_map(GMU_HFI_ADDR + off, pa(gmu_hfi_mem) + off) ||
		    !smmu_map(GMU_DUMP_ADDR + off, pa(gmu_dump_mem) + off)) {
			con_str("3", "sem tabelas");
			return HFI_NONE;
		}
	dsb();	/* the SMMU reads the tables from memory (Device: no cache to flush) */

	/* 3b: the stream table as the boot loader/hyp left it (READ ONLY). Decisive:
	 * on the identical A630 fajita saw s2cr=0x200ff (FAULT) - the GMU streams are
	 * NOT in bypass, so a physical HFI address is unreachable. These are the S9's
	 * own values; NO SMMU register is written on the default path. */
	step("3b estado da tabela de streams (so leitura)");
	{
		uint32_t v;	/* [M11.1] via the worker, like 2b */

		probe("3b smr0", SMMU_BASE + SMMU_SMR(0), &v);
		probe("3b s2cr0", SMMU_BASE + SMMU_S2CR(0), &v);
	}

#if M11_TRY_TRANSLATE
	/* 3c-3d EXPERIMENT (OFF by default): qcom reset, then program a context + the
	 * stream table. fajita 0112 saw the stream write FREEZE the identical A630; the
	 * watchdog is the only net. The programming below is the committed body verbatim. */
	step("3c reset do qcom (micro-mmu + impl-defs do DT do S9)");
	if (!smmu_qcom_reset())
		con_str("3c", "micro-mmu nao ficou idle (sigo mesmo assim)");
	step("3d EXPERIMENTO: programa contexto + tabela (pode travar)");
	ctx = smmu_pages * smmu_page + cb * smmu_page;		/* CB0 register page */
	ttbr = pa(smmu_root) | ((uint64_t)cb << 48);		/* tagged with the ASID */
	wr32(SMMU_BASE + SMMU_SCR0, rd32(SMMU_BASE + SMMU_SCR0) & ~(1u << 0));	/* CLIENTPD=0 */
	dsb();
	wr32(SMMU_BASE + SMMU_CB_SCTLR + ctx, 0);
	wr32(SMMU_BASE + smmu_page + SMMU_CBA2R(cb), SMMU_CBA2R_64BIT);
	wr32(SMMU_BASE + smmu_page + SMMU_CBAR(cb), SMMU_CBAR_STAGE_1);
	wr32(SMMU_BASE + SMMU_CB_TCR2 + ctx, SMMU_TCR2_VAL);
	wr32(SMMU_BASE + SMMU_CB_TCR + ctx, SMMU_TCR_39BITS);
	wr32(SMMU_BASE + SMMU_CB_TTBR0 + ctx, (uint32_t)ttbr);
	wr32(SMMU_BASE + SMMU_CB_TTBR0 + 4 + ctx, (uint32_t)(ttbr >> 32));
	wr32(SMMU_BASE + SMMU_CB_TTBR1 + ctx, 0);
	wr32(SMMU_BASE + SMMU_CB_TTBR1 + 4 + ctx, 0);
	wr32(SMMU_BASE + SMMU_CB_MAIR0 + ctx, SMMU_MAIR0_VAL);
	wr32(SMMU_BASE + SMMU_CB_MAIR1 + ctx, 0);
	wr32(SMMU_BASE + SMMU_CB_FSR + ctx, SMMU_FSR_ALL);
	wr32(SMMU_BASE + SMMU_CB_TLBIALL + ctx, 0);
	wr32(SMMU_BASE + SMMU_CB_TLBSYNC + ctx, 0);
	for (int i = 0; i < 100 && (rd32(SMMU_BASE + SMMU_CB_TLBSTATUS + ctx) & 1); i++)
		delay_ms(1);
	wr32(SMMU_BASE + SMMU_CB_SCTLR + ctx, SMMU_CB_SCTLR_ON);
	wr32(SMMU_BASE + SMMU_S2CR(0), cb);
	wr32(SMMU_BASE + SMMU_SMR(0), SMMU_SMR_VALID | GMU_STREAM_KERNEL);
	wr32(SMMU_BASE + SMMU_S2CR(1), cb);
	wr32(SMMU_BASE + SMMU_SMR(1), SMMU_SMR_VALID | GMU_STREAM_USER);
	dsb();
	showhex("cb0 sctlr<-", rd32(SMMU_BASE + SMMU_CB_SCTLR + ctx));
	showhex("smr0<-", rd32(SMMU_BASE + SMMU_SMR(0)));
	showhex("s2cr0<-", rd32(SMMU_BASE + SMMU_S2CR(0)));
	if ((rd32(SMMU_BASE + SMMU_CB_SCTLR + ctx) & 1) &&
	    rd32(SMMU_BASE + SMMU_SMR(0)) == (SMMU_SMR_VALID | GMU_STREAM_KERNEL)) {
		smmu_translate = 1;
		gmu_hfi_va = GMU_HFI_ADDR;
		gmu_dump_va = GMU_DUMP_ADDR;
		con_str("3", "SMMU traduzindo (contexto + tabela aceitos)");
		return HFI_TRANSLATE;
	}
	con_str("3", "a tabela nao aceitou -> sem caminho HFI provado");
	return HFI_NONE;
#elif M11_TRY_BYPASS_HFI
	/* [EXPERIMENT] release the GMU with PHYSICAL buffer addresses. fajita evidence
	 * says the streams are in FAULT, so expect the GMU's HFI reads to fault
	 * (bounded: SGFSR + timeout, no brick). gmu_hfi_va/gmu_dump_va keep physical. */
	(void)cb;
	(void)ctx;
	(void)ttbr;
	con_str("3", "EXPERIMENTO bypass: HFI fisico (deve faltar se S2CR=FAULT)");
	return HFI_BYPASS;
#else
	/* FAIL-SAFE DEFAULT: neither path is established for the S9, so M11 stops as a
	 * READ-ONLY diagnostic and does NOT release the GMU. bypass is unreachable
	 * (streams left in FAULT, fajita NOTES L1079); translate needs the hyp-owned
	 * stream write that froze the identical A630 (fajita 0112). The smr/s2cr above
	 * are the datum the next decision needs. */
	(void)cb;
	(void)ctx;
	(void)ttbr;
	con_str("3", "DIAGNOSTICO (so leitura): sem caminho HFI provado; GMU nao liberado");
	return HFI_NONE;
#endif
}

static int smmu_faulted(void)
{
	uint32_t ctx = smmu_pages * smmu_page + GMU_CB * smmu_page;
	uint32_t fsr = rd32(SMMU_BASE + SMMU_CB_FSR + ctx);

	if ((fsr & SMMU_FSR_ALL) == 0)
		return 0;
	showhex("SMMU RECUSOU fsr", fsr);
	showhex("  far lo", rd32(SMMU_BASE + SMMU_CB_FAR + ctx));
	return 1;
}

/* ---- the HFI queue table + polled send/receive ----------------------------------------------------- */

/* kgsl_hfi.h: 6-word table header, then one 12-word header per queue. */
#define HFI_TBL_HDR_WORDS	6
#define HFI_QHDR_WORDS		12
#define HFI_TABLE_WORDS		(HFI_TBL_HDR_WORDS + HFI_QUEUES * HFI_QHDR_WORDS)
#define HFI_QUEUE_OFFSET(i)	((HFI_TABLE_WORDS * 4u) + (i) * (4096u + 16u))	/* bytes */
/* queue header word indices */
#define QH_STATUS		0
#define QH_START		1
#define QH_TYPE			2
#define QH_SIZE			3
#define QH_MSG_SIZE		4
#define QH_RX_REQ		8
#define QH_READ			10
#define QH_WRITE		11

static volatile uint32_t *hfi;	/* = gmu_hfi_mem as words */

static uint32_t hfi_number;
static uint32_t hfi_payload0;	/* first payload word of the last answer (e.g. the firmware version) */

static volatile uint32_t *hfi_qhdr(unsigned q)
{
	return hfi + HFI_TBL_HDR_WORDS + q * HFI_QHDR_WORDS;
}

static void hfi_table_init(void)
{
	zero(gmu_hfi_mem, GMU_MEM_SIZE);
	hfi = (volatile uint32_t *)gmu_hfi_mem;
	hfi[0] = 0;				/* version */
	hfi[1] = HFI_TABLE_WORDS;		/* size (dwords) */
	hfi[2] = HFI_TBL_HDR_WORDS;		/* qhdr0 offset */
	hfi[3] = HFI_QHDR_WORDS;		/* qhdr size */
	hfi[4] = HFI_QUEUES;			/* num_q */
	hfi[5] = HFI_QUEUES;			/* num_active_q */
	{
		static const uint8_t qid[HFI_QUEUES] = { 0, 4, 5 };
		static const uint8_t qprio[HFI_QUEUES] = { 10, 10, 40 };
		for (unsigned i = 0; i < HFI_QUEUES; i++) {
			volatile uint32_t *h = hfi_qhdr(i);
			h[QH_STATUS] = 1;
			h[QH_START] = gmu_hfi_va + HFI_QUEUE_OFFSET(i);	/* V2: GMU-VA (translate) or physical (bypass) */
			h[QH_TYPE] = qid[i] | ((uint32_t)qprio[i] << 8);
			h[QH_SIZE] = HFI_QUEUE_WORDS;
			h[QH_MSG_SIZE] = 0;
			h[QH_RX_REQ] = 1;
			h[QH_READ] = 0;
			h[QH_WRITE] = 0;
		}
	}
	dsb();
}

/* Receive from queue `which`; true if the answer to `number` was seen. */
static int hfi_receive(unsigned which, uint32_t number, uint32_t *verdict)
{
	volatile uint32_t *h = hfi_qhdr(which);
	volatile uint32_t *q = (volatile uint32_t *)(gmu_hfi_mem + HFI_QUEUE_OFFSET(which));
	int answered = 0;

	while (h[QH_READ] != h[QH_WRITE]) {
		uint32_t at = h[QH_READ] % HFI_QUEUE_WORDS, msg0 = q[at], words = (msg0 >> 8) & 0xff;
		uint32_t m[8] = { 0 };

		for (uint32_t i = 0; i < words; i++) {
			if (i < 8)
				m[i] = q[at];
			at = (at + 1) % HFI_QUEUE_WORDS;
		}
		h[QH_READ] = (at + 3) / 4 * 4 % HFI_QUEUE_WORDS;
		if (((m[0] >> 16) & 0xf) == 1 /* ACK */ && (m[1] >> 20) == number) {
			*verdict = m[2];	/* error (0 = ok) */
			hfi_payload0 = m[3];	/* payload[0], e.g. the firmware version */
			answered = 1;
		}
		if (words == 0)
			break;
	}
	return answered;
}

/* Send `words` to the CMD queue and wait for the GMU's verdict. */
static int hfi_send(const char *what, uint32_t id, uint32_t *msg, uint32_t words, uint32_t *verdict)
{
	volatile uint32_t *h = hfi_qhdr(0);
	volatile uint32_t *q = (volatile uint32_t *)(gmu_hfi_mem + HFI_QUEUE_OFFSET(0));
	uint32_t at = h[QH_WRITE] % HFI_QUEUE_WORDS, number = ++hfi_number & 0xfff;
	int answered = 0;

	msg[0] = id | (words << 8) | (number << 20);
	for (uint32_t i = 0; i < (words + 3) / 4 * 4; i++) {
		q[at] = i < words ? msg[i] : 0xfafafafau;
		at = (at + 1) % HFI_QUEUE_WORDS;
	}
	h[QH_WRITE] = at;
	dsb();
	wr32(GMU(GMU_HOST2GMU_INTR_SET), 1);
	for (int i = 0; i < 5000 && !answered; i++) {
		if (rd32(GMU(GMU_GMU2HOST_INTR_INFO)) & HFI_IRQ_MSGQ) {
			wr32(GMU(GMU_GMU2HOST_INTR_CLR), HFI_IRQ_MSGQ);
			answered = hfi_receive(1, number, verdict);
		}
		if (smmu_faulted())
			break;
		wdog_pet();
		delay_ms(1);
	}
	con_str(what, answered ? (*verdict ? "GMU respondeu NAO" : "GMU aceitou") : "GMU nao respondeu");
	return answered && *verdict == 0;
}

/* ---- the RSC/PDC micro-code (M10) ------------------------------------------------------------------- */

#ifdef HAVE_GMU_FW
static void load_rpmh_ucode(void)
{
	static const uint32_t rsc_seq[5] = { 0xA7A506A0u, 0xA1E6A6E7u, 0xA2E081E1u, 0xE9A982E2u, 0x0020E8A8u };
	static const uint32_t pdc_seq[5] = { 0xFEBEA1E1u, 0xA5A4A3A2u, 0x8382A6E0u, 0xBCE3E284u, 0x002081FCu };

	wr32(GMU(GPU_RSCC_RSC_STATUS0), 1u << 24);
	wr32(GMU(RSCC_PDC_SLAVE_ID_DRV0), 1);
	wr32(GMU(RSCC_HIDDEN_TCS_CMD0_DATA), 0);
	wr32(GMU(RSCC_HIDDEN_TCS_CMD0_ADDR), 0);
	wr32(GMU(RSCC_HIDDEN_TCS_CMD0_DATA + RSC_CMD_OFFSET), 0);
	wr32(GMU(RSCC_HIDDEN_TCS_CMD0_ADDR + RSC_CMD_OFFSET), 0);
	wr32(GMU(RSCC_HIDDEN_TCS_CMD0_DATA + RSC_CMD_OFFSET * 2), 0x80000000u);
	wr32(GMU(RSCC_HIDDEN_TCS_CMD0_ADDR + RSC_CMD_OFFSET * 2), 0);
	wr32(GMU(RSCC_OVERRIDE_START_ADDR), 0);
	wr32(GMU(RSCC_PDC_SEQ_START_ADDR), 0x4520);
	wr32(GMU(RSCC_PDC_MATCH_VALUE_LO), 0x4510);
	wr32(GMU(RSCC_PDC_MATCH_VALUE_HI), 0x4514);
	for (unsigned i = 0; i < 5; i++)
		wr32(GMU(RSCC_SEQ_MEM_0_DRV0) + 4 * i, rsc_seq[i]);
	for (unsigned i = 0; i < 5; i++)
		wr32(PDC(PDC_GPU_SEQ_MEM_0) + 4 * i, pdc_seq[i]);
	wr32(PDC(PDC_GPU_TCS1_CMD_ENABLE_BANK), 7);
	wr32(PDC(PDC_GPU_TCS1_CMD_WAIT_FOR_CMPL_BANK), 0);
	wr32(PDC(PDC_GPU_TCS1_CONTROL), 0);
	wr32(PDC(PDC_GPU_TCS1_CMD0_MSGID), 0x10108);
	wr32(PDC(PDC_GPU_TCS1_CMD0_ADDR), 0x30010);
	wr32(PDC(PDC_GPU_TCS1_CMD0_DATA), 1);
	wr32(PDC(PDC_GPU_TCS1_CMD0_MSGID + PDC_CMD_OFFSET), 0x10108);
	wr32(PDC(PDC_GPU_TCS1_CMD0_ADDR + PDC_CMD_OFFSET), 0x30000);
	wr32(PDC(PDC_GPU_TCS1_CMD0_DATA + PDC_CMD_OFFSET), 0x0);
	wr32(PDC(PDC_GPU_TCS1_CMD0_MSGID + PDC_CMD_OFFSET * 2), 0x10108);
	wr32(PDC(PDC_GPU_TCS1_CMD0_ADDR + PDC_CMD_OFFSET * 2), 0x30080);
	wr32(PDC(PDC_GPU_TCS1_CMD0_DATA + PDC_CMD_OFFSET * 2), 0x0);
	wr32(PDC(PDC_GPU_TCS3_CMD_ENABLE_BANK), 7);
	wr32(PDC(PDC_GPU_TCS3_CMD_WAIT_FOR_CMPL_BANK), 0);
	wr32(PDC(PDC_GPU_TCS3_CONTROL), 0);
	wr32(PDC(PDC_GPU_TCS3_CMD0_MSGID), 0x10108);
	wr32(PDC(PDC_GPU_TCS3_CMD0_ADDR), 0x30010);
	wr32(PDC(PDC_GPU_TCS3_CMD0_DATA), 2);
	wr32(PDC(PDC_GPU_TCS3_CMD0_MSGID + PDC_CMD_OFFSET), 0x10108);
	wr32(PDC(PDC_GPU_TCS3_CMD0_ADDR + PDC_CMD_OFFSET), 0x30000);
	wr32(PDC(PDC_GPU_TCS3_CMD0_DATA + PDC_CMD_OFFSET), 0x3);
	wr32(PDC(PDC_GPU_TCS3_CMD0_MSGID + PDC_CMD_OFFSET * 2), 0x10108);
	wr32(PDC(PDC_GPU_TCS3_CMD0_ADDR + PDC_CMD_OFFSET * 2), 0x30080);
	wr32(PDC(PDC_GPU_TCS3_CMD0_DATA + PDC_CMD_OFFSET * 2), 0x3);
	wr32(PDC(PDC_GPU_SEQ_START_ADDR), 0);
	wr32(PDC(PDC_GPU_ENABLE_PDC), 0x80000001u);
	dsb();
}

/* ---- stage 4: firmware + boot (with HFI_QTBL set) --------------------------------------------------- */

static int gmu_boot_with_hfi(void)
{
	uint32_t result = 0, wrong = 0;
	unsigned words = sizeof(a630_gmu_fw) / sizeof(a630_gmu_fw[0]);

	stage = "4 firmware+boot";
	step("4a RSC/PDC + firmware no ITCM");
	load_rpmh_ucode();
	wr32(GMU(GMU_CM3_SYSRESET), 1);
	dsb();
	wr32(GMU(GMU_GENERAL_7), 1);
	for (unsigned i = 0; i < words; i++)
		wr32(GMU(GMU_CM3_ITCM_START) + 4 * i, a630_gmu_fw[i]);
	dsb();
	for (unsigned i = 0; i < words; i++)
		if (rd32(GMU(GMU_CM3_ITCM_START) + 4 * i) != a630_gmu_fw[i])
			wrong++;
	if (wrong) {
		showhex("itcm errados", wrong);
		con_str("4", "ITCM nao guarda o firmware");
		return 0;
	}

	step("4b config (HFI_QTBL = modo do estagio 3) e solta o GMU");
	hfi_table_init();
	wr32(GMU(GMU_CM3_FW_INIT_RESULT), 0);
	wr32(GMU(GMU_CM3_BOOT_CONFIG), GMU_BOOT_CONFIG_TCM);
	wr32(GMU(GMU_HFI_QTBL_ADDR), gmu_hfi_va);	/* V2: matches the stage-3 mode */
	wr32(GMU(GMU_HFI_QTBL_INFO), 1);
	wr32(GMU(GMU_AHB_FENCE_RANGE_0), FENCE_RANGE_MASK);
	wr32(GMU(GMU_HFI_SFR_ADDR), GMU_CHIPID);
	wr32(GMU(GMU_SYS_BUS_CONFIG), 1);
	wr32(GMU(GMU_PWR_COL_FRAME_CTRL), 0x9C40400u);
	wr32(GMU(GMU_RPMH_CTRL), 0);
	wr32(GMU(GMU_GMU2HOST_INTR_CLR), 0xFFFFFFFFu);
	wr32(GMU(GMU_GMU2HOST_INTR_MASK), (uint32_t)~HFI_IRQ_MSGQ);	/* let the message-queue interrupt through */
	wr32(GMU(GMU_CX_GMU_WFI_CONFIG), 0);
	dsb();
	wr32(GMU(GMU_CM3_SYSRESET), 0);
	dsb();
	for (int i = 0; i < 1000 && result != GMU_BABEFACE; i++) {
		result = rd32(GMU(GMU_CM3_FW_INIT_RESULT));
		wdog_pet();
		delay_ms(1);
	}
	showhex("gmu fw_init_result", result);
	if (result != GMU_BABEFACE) {
		con_str("4", "o GMU nao subiu");
		return 0;
	}
	showhex("smmu sgfsr (pos-GMU)", rd32(SMMU_BASE + SMMU_SGFSR));	/* V2 (fajita 0112): GMU access fault? */
	con_str("4", smmu_translate ? "GMU rodando (0xBABEFACE), SMMU traduzindo" : "GMU rodando (0xBABEFACE), SMMU em bypass");
	return 1;
}

/* ---- stage 5: HFI up + INIT + FW_VER ---------------------------------------------------------------- */

static void hfi_talk(void)
{
	uint32_t msg[16], verdict = 0;

	stage = "5 HFI";
	step("5a liga as filas de mensagem (HFI_CTRL_INIT)");
	wr32(GMU(GMU_HFI_CTRL_INIT), 1);
	dsb();
	{
		int up = 0;
		for (int i = 0; i < 500 && !up; i++) {
			if (rd32(GMU(GMU_HFI_CTRL_STATUS)) & 1)
				up = 1;
			else
				delay_ms(1);
		}
		showhex("hfi_ctrl_status", rd32(GMU(GMU_HFI_CTRL_STATUS)));
		showhex("hfi version_info", rd32(GMU(GMU_HFI_VERSION_INFO)));
		if (!up) {
			con_str("5", "as filas HFI nao sobem");
			smmu_faulted();
			return;
		}
	}

	step("5b manda H2F_MSG_INIT");
	zero(msg, sizeof(msg));
	/* hfi_gmu_init_cmd: seg_id, dbg_buffer_addr, dbg_buffer_size, boot_state (cold=1) */
	msg[1] = 0;
	msg[2] = gmu_dump_va;	/* V2: dump buffer, GMU-VA or physical per mode */
	msg[3] = GMU_MEM_SIZE;
	msg[4] = 1;
	if (!hfi_send("5b INIT", H2F_MSG_INIT, msg, 5, &verdict))
		return;

	step("5c pede a versao do firmware (FW_VER)");
	zero(msg, sizeof(msg));
	msg[1] = (1u << 28) | (3u << 16) | 0;	/* expected FW_VERSION(major=1,minor=3) */
	if (hfi_send("5c FW_VER", H2F_MSG_FW_VER, msg, 2, &verdict))
		showhex("GMU fw version", hfi_payload0);
	con_str("5", "O GMU CONVERSA POR HFI: canal de mensagens de pe");
}
#endif /* HAVE_GMU_FW */

void shim_main(uint64_t dtb)
{
	(void)dtb;
	fb_default();
	con_clear(GREY);
	con_title("SHIM M11.1: SMMU DA GPU");
	stage = "watchdog";
	wdog_arm();
	step_row = con_row();
	con_skip();
	con_skip();	/* [M11.1] a full row: the first line no longer runs under PASSO */

	if (power_on()) {
		con_skip();
		smmu_probe();
		con_skip();
#ifdef HAVE_GMU_FW
		/* V3 fail-safe: release the GMU only if stage 3 established a reachable HFI
		 * path (translate programmed, or an explicit bypass experiment). The default
		 * is a read-only diagnostic that stops here without touching the GMU. */
		if (smmu_context_on() != HFI_NONE) {
			con_skip();
			if (gmu_boot_with_hfi())
				hfi_talk();
		} else {
			con_skip();
			con_str("M11", "diagnostico so leitura concluido (GMU nao liberado)");
		}
#else
		stage = "firmware";
		step("firmware do GMU");
		con_str("", "sem fw/a630_gmu.bin: rode tools/gpu-fw-embed.py e recompile");
#endif
	}

	probe_quit = 1;		/* [M11.1] live workers power off; frozen ones stay */
	dsb();
	stage = "fim";
	step("fim");
	con_skip();
	con_str("", "Tire uma foto desta tela!");
	countdown_reboot(con_row(), END_SECONDS);
}

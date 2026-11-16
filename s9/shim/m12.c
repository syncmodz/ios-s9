/*
 * M11.3 (2026-10-29), build.sh m11f = the M11.2 baseline (shim/m11e.c is unchanged) plus two
 * READ-ONLY discovery stages that extract the S9's own RPMh power-vote data and GX power state,
 * the inputs a6xx_gfx_rail_on needs. Nothing new is written to any register; the SMMU/GMU/HFI
 * path is byte-for-byte the m11e sequence (power -> SMMU translate -> GMU boot -> HFI_CTRL_INIT
 * -> INIT -> FW_VER -> GMU reset -> SMMU restore).
 *
 * WHY only discovery (not the full GX power sequence): the M11.2 review listed gfx_rail_on,
 * SPTPRAC, RPMh enable and the PERF/BW HFI tables as the missing S9 steps. Tracing them in the
 * S9 kernel shows every one depends on gmu->rpmh_votes, built at runtime from frameworks the
 * shim does not have:
 *   - a6xx_gfx_rail_on (adreno_a6xx.c L1634-1665): writes GX_VOTE_IDX/MX_VOTE_IDX = the boot
 *     level's arc_vote_desc.pri_idx/sec_idx, then a boot OOB (HOST2GMU BIT22, poll GMU2HOST
 *     BIT30, GPU_START_TIMEOUT 100 ms). pri_idx/sec_idx come from setup_volt_dependency_tbl
 *     (kgsl_gmu.c L559-603): the GPU boot level's VLVL corner matched into the gfx.lvl / mx.lvl
 *     ARC tables. The corner is dev_pm_opp_get_voltage()-1 from the GPU OPP table, which is NOT
 *     in the flattened S9 DT (no opp-microvolt/RPMH_REGULATOR_LEVEL) and needs the OPP +
 *     regulator-level framework. So the index is NOT derivable from shim-visible data, and the
 *     fajita value (3/3) is another device's cmd-db -> must not be assumed. => stage 4 SKIP.
 *   - a6xx_sptprac_enable (L1538-1560): SP/TP RAC sits in the GX domain and is enabled right
 *     after gfx_rail_on, so it cannot be exercised before the GX rail is up. => folded into 4.
 *   - hfi_send_perftbl / hfi_send_bwtbl (kgsl_hfi.c L369-462): PERF_TBL needs the full
 *     gx_votes[]/cx_votes[] (cmd-db + per-level OPP corners); BW_VOTE_TBL needs the DDR/CNOC TCS
 *     command table from build_rpmh_bw_votes (kgsl_gmu.c L686+) via the msm_bus / interconnect
 *     framework (struct msm_bus_tcs_handle) -- absent from the shim. => stage 9 SKIP.
 *
 * WHAT IS NEW AND SOURCE-SUPPORTED: the cmd-db itself is a plain always-on SRAM region, and the
 * S9 kernel reads it from EL1 (drivers/soc/qcom/cmd-db.c). The DT node qcom,cmd-db@861e0000 has
 * reg = <0x0c3f000c 0x8>: a 2-word dictionary whose word0 is the real cmd-db base and word1 its
 * size (cmd_db_dev_probe L349-371). The header (cmd_db_header, 144 B) is version, magic
 * 0x0C0330DB, then 8 rsc_hdr (16 B: slv_id, header_offset, data_offset, cnt, version). Entry
 * headers (24 B: res_id u64, priority[2], addr, len u16, offset u16) live at base+144+header_off;
 * the aux VLVL table (u16 array) at base+144+data_off+entry.offset (L97-199). M11.3 walks this,
 * prints base/size/magic/version and the gfx.lvl / mx.lvl / cx.lvl VLVL tables -- the real S9
 * ARC corners a later stage needs to compute the vote index -- plus a read-only snapshot of the
 * GX GDSC, SPTPRAC and RPMh power-state registers. Every cmd-db read is on the worker core with
 * a timeout (a new, unproven physical region); an abort is contained, never a brick. Read-only,
 * so it adds no risk over m11e: the default remains safe and recoverable. Not hardware-tested.
 * Lineage: shim/m11d.c (M11.1), shim/m11e.c (M11.2), logs/evidence-20261029/.
 */
/*
 * ====================================================================================================
 * M12 (2026-10-30) == GPU stage C: command-processor bring-up.  Base = this file's inherited m11g
 * sequence (power -> SMMU translate -> GMU boot -> HFI), UNCHANGED, then the new CP stages.  Every CP
 * constant is from the S9's own KGSL (~/Downloads/android_kernel_samsung_sdm845-lineage-20/drivers/
 * gpu/msm) with file:line citations inline; nothing is invented (directive section 7).
 *
 *   Cp  cp_prepare()  -- SAFE, ALWAYS runs: copy SQE microcode, map ring/sqe/scratch into the GPU
 *                        SMMU CB0 table, build+validate the CP_ME_INIT / NOP / MEM_WRITE packets.
 *                        Touches only our own RAM and page tables -- no GPU-core register access.
 *   C   cp_bringup()  -- GATED (-DM12_TRY_CP=1) and only if the GX rail actually came up: SQE base,
 *                        CP start, CP_ME_INIT (a6xx_send_cp_init), bounded NOP + MEM_WRITE, CPU
 *                        readback, SMMU-fault check.  The 3D core is in the GX domain, so the first
 *                        touch is a fault-isolated canary; a single attempt, bounded, never retried.
 *   Cz  pas_zap_probe() -- GATED: Samsung-signed ZAP via TrustZone PAS id 13 (a630_zap.mdt).  A failed
 *                        PAS call stops the secure stages and reports the exact SMC return.
 *
 * STATUS: [IMPL] compiled + host-tested (tools + shim/host_test_m12.c), NOT hardware-tested.  The CP
 * path depends on the M11.4 GX rail and sec_idx/teardown, which are themselves NOT hardware-confirmed
 * (directive). cp_bringup runs only under a supervised flash with -DM12_TRY_CP=1 -DM11G_TRY_GX_RAIL=1.
 * ====================================================================================================
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
 * [M11.2] M11.1 showed the GPU SMMU readable from EL1 in the boot loader's state (streams
 * FAULT, CB0 off) and the S9's Linux programs it itself, so stage 3 READS the SMMU first,
 * checks it is still in that state, and only then WRITES it, reading every write back.
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
#if __has_include("fw_a630_cp.h")
#include "fw_a630_cp.h"		/* [M12] SQE microcode + Samsung ZAP (tools/cp-fw-embed.py) */
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
#define GMU_SPTPRAC_PWR_CLK_STATUS	0x1F8D0	/* [M11.3] a6xx_reg.h (read-only snapshot) */
/* [M11.4] GX-rail vote + boot OOB (a6xx_reg.h; a6xx_gfx_rail_on adreno_a6xx.c L1634-1665) */
#define GMU_BOOT_SLUMBER_OPTION		0x1CBF8
#define GMU_GX_VOTE_IDX			0x1CBF9
#define GMU_MX_VOTE_IDX			0x1CBFA
#define GMU_GX_SPTPRAC_POWER_CONTROL	0x1A881
#define OOB_BOOT_OPTION			0u		/* kgsl_gmu.h */
#define OOB_BOOT_SLUMBER_SET_MASK	(1u << 22)	/* HOST2GMU set */
#define OOB_BOOT_SLUMBER_CHECK_MASK	(1u << 30)	/* GMU2HOST ack */
#define OOB_BOOT_SLUMBER_CLEAR_MASK	(1u << 30)
#define SPTPRAC_POWERON_CTRL_MASK	0x00778000u	/* a6xx_sptprac_enable */
#define SPTPRAC_POWERON_STATUS_MASK	(1u << 3)
#define SPTPRAC_CTRL_TIMEOUT_MS		10
/* [M11.4] boot GX corner: initial-pwrlevel 6 = 257 MHz -> gpucc-sdm845.c v2 fixup VDD_GX_LOWER
 * -> vdd_gx_corner[] LOW_SVS -> rpmh-regulator.h 64+1, minus rpmh_arc_votes_init -1 = raw VLVL 0x40.
 * Full trace: logs/evidence-20261030/m11g-opp-source-trace.md. Not assumed; derived from S9 source. */
#define GX_BOOT_VLVL			0x40u
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
/* [M11.2] arm-smmu.c write_context_bank: M|TRE|AFE|CFRE|CFIE|ASIDPNE|SHCFG NSH, minus CFCFG
 * (terminate, not stall: see header). == fajita's 0x00c01067. CFIE: IRQs are masked here. */
#define SMMU_CB_SCTLR_ON	((1u << 0) | (1u << 1) | (1u << 2) | (1u << 5) | (1u << 6) | (1u << 12) | (3u << 22))
/* [M11.2] io-pgtable-arm.c, no quirk: T0SZ 64-39 (use-3-lvl-tables), NC walks, SH_OS, 4K, EPD1 */
#define SMMU_TCR_39BITS		(25u | (2u << 12) | (1u << 23))
#define SMMU_CB_ACTLR		0x04
#define SMMU_ACTLR_QCOM		((1u << 28) | (1u << 29) | (1u << 30))	/* qsmmuv2: OSH|ISH|NSH */
#define SMMU_TLBIALLNSNH	0x068
#define SMMU_STLBGSYNC		0x070
#define SMMU_STLBGSTATUS	0x074
#define SMMU_S2CR_TRANS(cb)	((3u << 8) | (cb))	/* TYPE 0 translate, SHCFG NSH, PRIVCFG 0 */
#define SMMU_S2CR_FAULT_IDLE	0x000200ffu		/* boot loader state (M11.1 photo) */
#define SMMU_GMU_ASID		(GMU_CB + 1)		/* arm_smmu_init_asid: cbndx + 1 */
#define SMMU_TCR2_VAL		(1u | (1u << 4) | (7u << 15))	/* 36-bit PA, 16-bit ASID */
#define SMMU_MAIR0_VAL		0xf404ff44u	/* 0 uncached, 1 cached, 2 device */
#define SMMU_FSR_ALL		0xc00001feu

/* V2: qcom qsmmuv2 impl-defs + the hypervisor bit (S9 DT + fajita 0112/0125) */
#define SMMU_MICRO_MMU_CTRL	0x6000		/* S9 DT: qcom,micro-mmu-control = <0x6000> */
#define SMMU_MICRO_HALT		(1u << 2)
#define SMMU_MICRO_IDLE		(1u << 3)
#define SMMU_SCR0_HYP		(1u << 21)	/* [M11.2] = sCR0.SMCFCFG (old name kept for m11d parity) */
#define APPS_SMMU_BASE		0x15000000u	/* S9 DT: apps-smmu@0x15000000 (control read only) */

/* Page tables for the GMU's SMMU context (3-level, 4K pages, MMU off so PA = address). */
#define PTE_TABLE		3ull
#define PTE_PAGE		3ull
#define PTE_ACCESSED		(1ull << 10)
#define PTE_SHARED		(2ull << 8)
#define PTE_NOT_GLOBAL		(1ull << 11)
#define PTE_AP_RW		(1ull << 6)	/* [M11.2] AP unpriv RW: kgsl maps GMU kmem READ|WRITE, not PRIV */
#define PTE_GMU			(PTE_SHARED | PTE_ACCESSED | PTE_NOT_GLOBAL | PTE_AP_RW)	/* + page = 0xE43 */
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
#define F2H_MSG_ERR		100
#define HFI_IRQ_MSGQ		(1u << 0)
#define HFI_RSP_WORDS		19		/* sizeof(struct hfi_msg_rsp) >> 2 */
#define HFI_RSP_TIMEOUT_MS	5000		/* kgsl_hfi.h HFI_RSP_TIMEOUT */
#define GMU_START_TIMEOUT_MS	100		/* adreno_a6xx.c GMU_START_TIMEOUT */
#define GMU_FW_WANTED		((1u << 28) | (3u << 16))	/* FW_VERSION(gpmu_major 1, gpmu_minor 3), A630v2 */

/* ================================================================================================
 * [M12] GPU stage C: command-processor registers, PM4 packets, PAS (ZAP) and buffers.  Active only
 * when the firmware header is present; sized/typed from fw_a630_cp.h.
 * ============================================================================================== */
#ifdef HAVE_CP_FW

#ifndef M12_TRY_CP
#define M12_TRY_CP 0		/* default: no GPU-core access; cp_bringup/zap are not even compiled */
#endif

/* GPU 3D-core register block.  S9 DT kgsl_3d0_reg_memory reg = <0x5000000 0x40000> (sdm845-gpu.dtsi
 * L51).  _regwrite (adreno_a6xx.c L1186): byte address = regbase + (offsetwords << 2). */
#define GPU_BASE			0x05000000UL
#define GPU(w)				(GPU_BASE + ((uint32_t)(w) << 2))

/* a6xx_reg.h register word offsets. */
#define A6XX_CP_RB_BASE			0x800
#define A6XX_CP_RB_BASE_HI		0x801
#define A6XX_CP_RB_CNTL			0x802
#define A6XX_CP_RB_RPTR_ADDR_LO		0x804
#define A6XX_CP_RB_RPTR_ADDR_HI		0x805
#define A6XX_CP_RB_RPTR			0x806
#define A6XX_CP_RB_WPTR			0x807
#define A6XX_CP_SQE_CNTL		0x808
#define A6XX_CP_SQE_INSTR_BASE_LO	0x830
#define A6XX_CP_SQE_INSTR_BASE_HI	0x831
#define A6XX_RBBM_STATUS		0x210
#define A6XX_RBBM_SECVID_TSB_ADDR_MODE_CNTL	0xF810	/* a6xx_start L487: = 0x1 */

/* a6xx_rb_start (adreno_a6xx.c L1082-1104) ring control.  CP_RB_CNTL for a 32 KiB ring (KGSL_RB_SIZE,
 * adreno_ringbuffer.h L29) = A6XX_CP_RB_CNTL_DEFAULT (adreno_a6xx.h L106)
 *   = (1<<27) | ((ilog2(4)<<8)&0x1F00) | (ilog2(8192>>1)&0x3F) = 0x0800020C. */
#define CP_RB_DWORDS			8192u
#define CP_RB_CNTL_DEFAULT		0x0800020Cu

/* PM4 packet headers (adreno_pm4types.h L18-21, L166-205): odd-parity bits exactly as the kernel. */
#define CP_TYPE7_PKT			(7u << 28)
#define CP_NOP				0x10
#define CP_ME_INIT			0x48
#define CP_MEM_WRITE			0x3d
#define CP_SET_SECURE_MODE		0x66

/* CP_ME_INIT ordinal mask bits (adreno_a6xx.c L876-895).  _set_ordinals (L921-971) gives each ordinal
 * value verbatim.  The full KGSL mask also sets REGISTER_INIT_LIST_WITH_SPINLOCK (BIT8) -> 0x16F, which
 * needs the a6xx_pwrup_reglist structure in GPU memory; the diagnostic uses the minimal subset (0x6F)
 * whose ordinals are fully known, so no structure is guessed. */
#define CP_INIT_MAX_CONTEXT		(1u << 0)
#define CP_INIT_ERROR_DETECTION_CONTROL	(1u << 1)
#define CP_INIT_HEADER_DUMP		(1u << 2)
#define CP_INIT_DEFAULT_RESET_STATE	(1u << 3)
#define CP_INIT_UCODE_WORKAROUND_MASK	(1u << 5)
#define CP_INIT_OPERATION_MODE_MASK	(1u << 6)
#define CP_INIT_MASK_MIN		(CP_INIT_MAX_CONTEXT | CP_INIT_ERROR_DETECTION_CONTROL | \
					 CP_INIT_HEADER_DUMP | CP_INIT_DEFAULT_RESET_STATE | \
					 CP_INIT_UCODE_WORKAROUND_MASK | CP_INIT_OPERATION_MODE_MASK)

/* TrustZone PAS (zap auth).  qcom_scm.h: SVC_PIL=0x2, INIT_IMAGE=1, MEM_SETUP=2, AUTH_AND_RESET=5.
 * FNID=(svc<<8)|cmd; SMC id = ARM_SMCCC(STD,SMC64,SIP=2,fnid) = 0x42000000|fnid (qcom_scm-64.c
 * L131-138).  arginfo = QCOM_SCM_ARGS(num, types): INIT_IMAGE(2,VAL,RW)=0x82, MEM_SETUP(3)=0x3,
 * AUTH(1)=0x1.  GPU zap PAS id = 13 (msm8953-gpu.dtsi L16; A630 zap_name "a630_zap", adreno-gpulist.h
 * L348; directive section 6). */
#define SCM_SIP_SMC64			0x42000000u
#define SCM_FNID(s, c)			((((s) & 0xffu) << 8) | ((c) & 0xffu))
#define SCM_SVC_PIL			0x2u
#define SCM_PAS_INIT_IMAGE		0x1u
#define SCM_PAS_MEM_SETUP		0x2u
#define SCM_PAS_AUTH_AND_RESET		0x5u
#define SCM_ARGINFO_INIT_IMAGE		0x82u	/* QCOM_SCM_ARGS(2, VAL, RW) */
#define SCM_ARGINFO_MEM_SETUP		0x3u	/* QCOM_SCM_ARGS(3) */
#define SCM_ARGINFO_AUTH		0x1u	/* QCOM_SCM_ARGS(1) */
#define SCM_INTERRUPTED			1	/* qcom_scm.h L66 */
#define GPU_PAS_ID			13

/* CP buffers.  GPU virtual addresses are shim-chosen: the CP only needs them mapped in the context it
 * runs under -- CB0, the global/kernel context m11 already programmed (TTBR0 = smmu_root).  Backed by
 * 4K-aligned static RAM, mapped with smmu_map() into that same CB0 table. */
#define CP_RING_VA			0x01000000u	/* 8 pages (32 KiB) */
#define CP_SQE_VA			0x01100000u	/* ceil(SQE) ~34 KiB */
#define CP_SCRATCH_VA			0x01200000u	/* 1 page */
#define CP_SCRATCH_RPTR			0u		/* scratch dword 0: CP_RB_RPTR_ADDR shadow */
#define CP_SCRATCH_MEMW			4u		/* scratch dword 4: CP_MEM_WRITE target */
#define CP_MEMW_SENTINEL		0x12CA7E12u	/* value the CP must write; CPU reads it back */

static uint8_t cp_ring_mem[CP_RB_DWORDS * 4] __attribute__((aligned(4096)));
static uint8_t cp_sqe_mem[A630_SQE_UCODE_DWORDS * 4 + 4096] __attribute__((aligned(4096)));
static uint8_t cp_scratch_mem[4096] __attribute__((aligned(4096)));

#endif /* HAVE_CP_FW */

static const char *stage;
static int step_row;

/* Static memory, 4K-aligned; with MMU off, physical address == address. */
static uint8_t smmu_pool[8][4096] __attribute__((aligned(4096)));
static unsigned smmu_pool_used;
static uint8_t gmu_hfi_mem[GMU_MEM_SIZE] __attribute__((aligned(4096)));
static uint8_t gmu_dump_mem[GMU_MEM_SIZE] __attribute__((aligned(4096)));

static uint32_t smmu_page = 0x1000, smmu_pages = 8, smmu_contexts, smmu_smrs;

/* [M11.2] which HFI address path stage 3 established (no bypass path exists). */
#define HFI_NONE	0	/* no proven path: do NOT release the GMU */
#define HFI_TRANSLATE	1	/* CB0 + SMR/S2CR programmed and read back; GMU-VA 0x60000000 */
static int smmu_translate;
static int smmu_written;	/* [M11.2] something was written: restore at the end */
static uint32_t gmu_hfi_va, gmu_dump_va;	/* GMU-facing addresses of the HFI/dump buffers */

/* S9 DT (dtb/live-twrp.dts:11861) arm,smmu-kgsl@5040000 attach-impl-defs, 12 pairs,
 * written by qsmmuv2_device_reset between halt and resume. */
static const uint32_t smmu_impl_defs[][2] = {
	{ 0x6000, 0x2378 }, { 0x6060, 0x1055 }, { 0x678c, 0x8 },	{ 0x6794, 0x28 },
	{ 0x6800, 0x6 },    { 0x6900, 0x3ff },  { 0x6924, 0x204 },	{ 0x6928, 0x11000 },
	{ 0x6930, 0x800 },  { 0x6960, 0xffffffff }, { 0x6b64, 0x1a5551 }, { 0x6b68, 0x9a82a382 },
};

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
static volatile uint32_t probe_op;		/* [M11.2] see PROBE_* */
static volatile uint64_t probe_wval, probe_val64;
static unsigned worker, workers_lost;
static const char *acc_tag;	/* [M11.2] prefix of the access shown on the PASSO line (set at run
				 * time: a static pointer initializer is an ABS64 relocation) */

#define PROBE_READ	0	/* rd32 */
#define PROBE_WRITE	1	/* wr32, dsb, rd32 (read back) */
#define PROBE_WRITE64	2	/* 64-bit store, dsb, 64-bit load (TTBRn: writeq in arm-smmu.c) */
#define PROBE_CMD	3	/* wr32, dsb: write-only registers (TLBI, sync) */

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
		switch (probe_op) {	/* may never return: then only this core is lost */
		case PROBE_WRITE:
			wr32(probe_addr, (uint32_t)probe_wval);
			dsb();
			probe_val = rd32(probe_addr);
			break;
		case PROBE_WRITE64:
			*(volatile uint64_t *)probe_addr = probe_wval;
			dsb();
			probe_val64 = *(volatile uint64_t *)probe_addr;
			break;
		case PROBE_CMD:
			wr32(probe_addr, (uint32_t)probe_wval);
			probe_val = 0;
			break;
		default:
			probe_val = rd32(probe_addr);
		}
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

/* [M11.2] Run one access `op` on the worker core. 1 = answered (*rb = value read / read back);
 * 0 = no answer in time or an abort, shown under `label`: that core is left behind and the
 * next access starts a new one. */
static int worker_do(const char *label, uint32_t op, uintptr_t addr, uint64_t wval, uint64_t *rb)
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
	p = put(put(b, acc_tag ? acc_tag : "?"), op == PROBE_READ ? " le 0x" : " escreve 0x");
	p = put_hex(p, addr, 8);
	if (op != PROBE_READ)
		p = put_hex(put(p, "="), wval, op == PROBE_WRITE64 ? 16 : 8);
	put_dec(put(p, " n"), worker);
	step(b);
	probe_fault = 0;
	probe_op = op;
	probe_wval = wval;
	probe_addr = addr;
	dsb();
	probe_seq = ++seq;
	dsb();
	for (int ms = 0; ms < PROBE_TIMEOUT_MS; ms++) {
		if (probe_ack == seq) {
			*rb = op == PROBE_WRITE64 ? probe_val64 : probe_val;
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
	con_hex2("c cfg/mnoc", "", rd32(GCC + GCC_CFG_AHB), "", rd32(GCC + GCC_MEMNOC_GFX));	/* [M11.2] 2 per row */
	con_hex2("c axi/snoc", "", rd32(GCC + GCC_DDRSS_GPU_AXI), "", rd32(GCC + GCC_SNOC_DVM_GFX));
	con_hex2("g ahb/gmu", "", rd32(GPUCC + GPUCC_CBCR_AHB), "", rd32(GPUCC + GPUCC_CBCR_CX_GMU));
	con_str("1", "GPU com energia e relogios (+crc_ahb/apb)");
	return 1;
}

/* ---- stage 2: read the SMMU as the boot loader / hypervisor left it --------------------------------- */

static int s_rd(uint32_t off, uint32_t *v);	/* [M11.2] worker read, below */

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
	/* [M11.1] one register at a time on the worker core; [M11.2] shown two per row. */
	acc_tag = "2b";
	idr0 = idr1 = scr0 = 0;
	if (s_rd(SMMU_IDR0, &idr0))
		smmu_smrs = idr0 & 0xff;
	if (s_rd(SMMU_IDR1, &idr1)) {
		smmu_page = (idr1 >> 31) ? 0x10000 : 0x1000;
		smmu_pages = 2u << ((idr1 >> 28) & 7);
		smmu_contexts = idr1 & 0xff;
	}
	if (2 * smmu_pages * smmu_page != SMMU_SIZE) {
		con_str("2", "idr1 nao confiavel: uso 4K/8 (geometria conhecida do A630)");
		smmu_page = 0x1000;
		smmu_pages = 8;
		smmu_contexts = 0;	/* [M11.2] makes the 3b gate refuse */
	}
	s_rd(SMMU_SCR0, &scr0);
	con_hex2("s idr0/1", "", idr0, "", idr1);
	{
		uint32_t g = 0, u = 0;

		s_rd(SMMU_SGFSR, &g);
		s_rd(SMMU_MICRO_MMU_CTRL, &u);
		con_hex2("s scr0", "", scr0, "sgfsr ", g);
		showhex("s ummu", u);
	}
	showhex("perdidos", workers_lost);
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

/* ---- [M11.2] SMMU accesses: all on the worker core, every write read back ------------------------ */

static int smmu_dead;	/* an SMMU access did not answer or aborted: touch the SMMU no more */

#define SMMU_GR1(off)		(smmu_page + (off))
#define SMMU_CB(n, off)		(smmu_pages * smmu_page + (n) * smmu_page + (off))

static int s_access(const char *label, uint32_t op, uint32_t off, uint64_t val, uint64_t *rb)
{
	if (smmu_dead || !worker_do(label, op, SMMU_BASE + off, val, rb)) {
		smmu_dead = 1;
		return 0;
	}
	if (op != PROBE_READ)
		smmu_written = 1;
	return 1;
}

static int s_rd(uint32_t off, uint32_t *v)
{
	uint64_t r;

	if (!s_access(acc_tag, PROBE_READ, off, 0, &r))
		return 0;
	*v = (uint32_t)r;
	return 1;
}

static int s_cmd(uint32_t off, uint32_t val)
{
	uint64_t r;

	return s_access(acc_tag, PROBE_CMD, off, val, &r);
}

/* Write `val` and read it back: 1 only if the bits in `mask` read back as written. */
static int s_set(const char *label, uint32_t off, uint32_t val, uint32_t mask)
{
	uint64_t r;
	char b[52], *p;

	if (!s_access(label, PROBE_WRITE, off, val, &r))
		return 0;
	if (((uint32_t)r & mask) == (val & mask))
		return 1;
	p = put_hex(put(b, "ERRO escrito "), val, 8);
	put_hex(put(p, " lido "), (uint32_t)r, 8);
	con_str(label, b);
	return 0;
}

static int s_set64(const char *label, uint32_t off, uint64_t val)
{
	uint64_t r;

	if (!s_access(label, PROBE_WRITE64, off, val, &r))
		return 0;
	if (r == val)
		return 1;
	con_str(label, "ERRO leitura de volta (64 bits):");
	showhex("  escrito", val);
	showhex("  lido", r);
	return 0;
}

/* Global TLBIALLNSNH, then __arm_smmu_tlb_sync (arm-smmu.c L1193-1208). */
static int smmu_tlb_sync(void)
{
	uint32_t st = 1;

	if (!s_cmd(SMMU_TLBIALLNSNH, 0) || !s_cmd(SMMU_STLBGSYNC, 0))
		return 0;
	for (int ms = 0; ms < 10; ms++) {
		if (!s_rd(SMMU_STLBGSTATUS, &st))
			return 0;
		if (!(st & 1))
			return 1;
		delay_ms(1);
	}
	con_str(acc_tag, "ERRO sTLBGSTATUS nao zerou em 10 ms");
	return 0;
}

/* Clean+invalidate `n` bytes by VA (== PA, MMU off): no stale boot-loader line may later be
 * written back over what the GMU or the SMMU reads or writes there. */
static void cache_clean_inval(const void *p, uint32_t n)
{
	uint64_t ctr, line;

	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	line = 4ull << ((ctr >> 16) & 0xf);
	for (uint64_t a = (uintptr_t)p & ~(line - 1); a < (uintptr_t)p + n; a += line)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	dsb();
}

/* Software walk of our own tables (what the SMMU will do): the PA `va` maps to, or ~0. */
static uint64_t smmu_walk(uint64_t va)
{
	uint64_t e = smmu_root[(va >> 30) & 0x1ff];

	if ((e & 3) != PTE_TABLE)
		return ~0ull;
	e = ((uint64_t *)(uintptr_t)(e & PTE_ADDR))[(va >> 21) & 0x1ff];
	if ((e & 3) != PTE_TABLE)
		return ~0ull;
	e = ((uint64_t *)(uintptr_t)(e & PTE_ADDR))[(va >> 12) & 0x1ff];
	if ((e & 0xfff) != (PTE_GMU | PTE_PAGE))
		return ~0ull;
	return (e & PTE_ADDR) | (va & 0xfff);
}

/* 3b: write nothing unless the SMMU is still exactly as M11.1 photographed it. */
static int smmu_state_as_left(void)
{
	uint32_t scr0, smr0, smr1, s2cr0, s2cr1, cbar, sctlr;

	acc_tag = "3b";
	step("3b confere o estado do boot loader (so leitura)");
	if (!s_rd(SMMU_SCR0, &scr0) || !s_rd(SMMU_SMR(0), &smr0) || !s_rd(SMMU_S2CR(0), &s2cr0) ||
	    !s_rd(SMMU_SMR(1), &smr1) || !s_rd(SMMU_S2CR(1), &s2cr1) ||
	    !s_rd(SMMU_GR1(SMMU_CBAR(GMU_CB)), &cbar) || !s_rd(SMMU_CB(GMU_CB, SMMU_CB_SCTLR), &sctlr))
		return 0;
	con_hex2("3b smr/s2cr", "0:", smr0, "", s2cr0);
	con_hex2("3b smr/s2cr", "1:", smr1, "", s2cr1);
	if (workers_lost || smmu_smrs < 2 || smmu_contexts < 1 || smmu_page != 0x1000 || smmu_pages != 8 ||
	    (scr0 & 1) || smr0 || smr1 || s2cr0 != SMMU_S2CR_FAULT_IDLE || s2cr1 != SMMU_S2CR_FAULT_IDLE ||
	    cbar || sctlr) {
		con_str("3b", "ERRO estado diferente do M11.1: nada escrito");
		return 0;
	}
	return 1;
}

/* 3c: arm_smmu_device_reset's sGFSR clear + qsmmuv2_device_reset (halt, impl-defs, resume). */
static int smmu_qcom_reset(void)
{
	uint32_t gfsr, ctrl, v;
	unsigned differ = 0;
	int idle = 0;

	acc_tag = "3c";
	step("3c halt do micro-MMU + 12 impl-defs do DT + resume");
	if (!s_rd(SMMU_SGFSR, &gfsr) || (gfsr && !s_cmd(SMMU_SGFSR, gfsr)))	/* write-1-to-clear */
		return 0;
	if (!s_rd(SMMU_MICRO_MMU_CTRL, &ctrl) || !s_cmd(SMMU_MICRO_MMU_CTRL, ctrl | SMMU_MICRO_HALT))
		return 0;
	for (int ms = 0; ms <= 30 && !idle; ms++) {	/* qsmmuv2_wait_for_halt: 30 ms */
		if (!s_rd(SMMU_MICRO_MMU_CTRL, &v))
			return 0;
		if (v & SMMU_MICRO_IDLE)
			idle = 1;
		else
			delay_ms(1);
	}
	if (!idle) {
		con_str("3c", "ERRO micro-MMU nao ficou IDLE em 30 ms");
		s_cmd(SMMU_MICRO_MMU_CTRL, ctrl & ~SMMU_MICRO_HALT);
		return 0;
	}
	for (unsigned i = 0; i < sizeof(smmu_impl_defs) / sizeof(smmu_impl_defs[0]); i++) {
		uint64_t r;

		if (!s_access("3c impl-def", PROBE_WRITE, smmu_impl_defs[i][0], smmu_impl_defs[i][1], &r))
			return 0;
		if ((uint32_t)r != smmu_impl_defs[i][1])
			differ++;	/* reported only: Linux does not read these back */
	}
	if (!s_rd(SMMU_MICRO_MMU_CTRL, &ctrl) || !s_cmd(SMMU_MICRO_MMU_CTRL, ctrl & ~SMMU_MICRO_HALT) ||
	    !s_rd(SMMU_MICRO_MMU_CTRL, &ctrl))
		return 0;
	con_hex2("3c", "gfsr ", gfsr, "ummu ", ctrl);
	showhex("3c impl-defs !=", differ);
	if (ctrl & SMMU_MICRO_HALT) {
		con_str("3c", "ERRO o micro-MMU continua em halt");
		return 0;
	}
	return 1;
}

/* 3d: CB0 as arm_smmu_write_context_bank (L1776-1862), after disabling it and clearing its
 * faults + a TLB invalidate as the device reset does. */
static int smmu_context_program(uint64_t ttbr0)
{
	uint32_t fsr;

	acc_tag = "3d";
	step("3d CB0: S1, 4K, 39 bits, ASID 1, walks nao cacheaveis");
	if (!s_set("3d sctlr 0", SMMU_CB(GMU_CB, SMMU_CB_SCTLR), 0, ~0u) ||
	    !s_cmd(SMMU_CB(GMU_CB, SMMU_CB_FSR), SMMU_FSR_ALL) ||
	    !s_rd(SMMU_CB(GMU_CB, SMMU_CB_FSR), &fsr))
		return 0;
	if (fsr & SMMU_FSR_ALL) {
		showhex("3d ERRO fsr", fsr);
		return 0;
	}
	if (!smmu_tlb_sync() ||
	    !s_set("3d cba2r", SMMU_GR1(SMMU_CBA2R(GMU_CB)), SMMU_CBA2R_64BIT, ~0u) ||
	    !s_set("3d cbar", SMMU_GR1(SMMU_CBAR(GMU_CB)), SMMU_CBAR_STAGE_1, 0x3ffffu) ||
	    !s_set("3d tcr2", SMMU_CB(GMU_CB, SMMU_CB_TCR2), SMMU_TCR2_VAL, ~0u) ||
	    !s_set("3d tcr", SMMU_CB(GMU_CB, SMMU_CB_TCR), SMMU_TCR_39BITS, ~0u) ||
	    !s_set64("3d ttbr0", SMMU_CB(GMU_CB, SMMU_CB_TTBR0), ttbr0) ||
	    !s_set64("3d ttbr1", SMMU_CB(GMU_CB, SMMU_CB_TTBR1), (uint64_t)SMMU_GMU_ASID << 48) ||
	    !s_set("3d mair0", SMMU_CB(GMU_CB, SMMU_CB_MAIR0), SMMU_MAIR0_VAL, ~0u) ||
	    !s_set("3d mair1", SMMU_CB(GMU_CB, SMMU_CB_MAIR1), 0, ~0u) ||
	    !s_set("3d actlr", SMMU_CB(GMU_CB, SMMU_CB_ACTLR), SMMU_ACTLR_QCOM, SMMU_ACTLR_QCOM) ||
	    !s_set("3d sctlr", SMMU_CB(GMU_CB, SMMU_CB_SCTLR), SMMU_CB_SCTLR_ON, ~0u))
		return 0;
	showhex("3d ttbr0", ttbr0);
	con_hex2("3d", "sctlr ", SMMU_CB_SCTLR_ON, "tcr ", SMMU_TCR_39BITS);
	return 1;
}

/* 3e: arm_smmu_write_sme (S2CR, then SMR; exact match, mask 0), then a TLB invalidate. */
static int smmu_streams_on(void)
{
	uint32_t gfsr;

	acc_tag = "3e";
	step("3e streams 5 (gmu_kernel) e 4 (gmu_user) -> CB0");
	if (!s_set("3e s2cr0", SMMU_S2CR(0), SMMU_S2CR_TRANS(GMU_CB), ~0u) ||
	    !s_set("3e smr0", SMMU_SMR(0), SMMU_SMR_VALID | GMU_STREAM_KERNEL, ~0u) ||
	    !s_set("3e s2cr1", SMMU_S2CR(1), SMMU_S2CR_TRANS(GMU_CB), ~0u) ||
	    !s_set("3e smr1", SMMU_SMR(1), SMMU_SMR_VALID | GMU_STREAM_USER, ~0u) ||
	    !smmu_tlb_sync() || !s_rd(SMMU_SGFSR, &gfsr))
		return 0;
	if (gfsr) {
		showhex("3e ERRO sgfsr", gfsr);
		return 0;
	}
	con_hex2("3e", "s2cr ", SMMU_S2CR_TRANS(GMU_CB), "smr0 ", SMMU_SMR_VALID | GMU_STREAM_KERNEL);
	return 1;
}

static int smmu_context_on(void)
{
	uint64_t lo = pa(smmu_pool), hi = pa(smmu_pool) + sizeof(smmu_pool);
	uint64_t ttbr0;

	smmu_translate = 0;
	gmu_hfi_va = gmu_dump_va = 0;
	stage = "3 smmu contexto";
	acc_tag = "3a";
	step("3a tabelas: HFI 0x60000000 + dump 0x60100000");
	if (pa(gmu_hfi_mem) < lo)
		lo = pa(gmu_hfi_mem);
	if (pa(gmu_dump_mem) + GMU_MEM_SIZE > hi)
		hi = pa(gmu_dump_mem) + GMU_MEM_SIZE;
	if (((pa(smmu_pool) | pa(gmu_hfi_mem) | pa(gmu_dump_mem)) & 0xfff) || (hi >> 36) || lo < 0x1000) {
		con_str("3a", "ERRO memoria fora de 4K / 36 bits: nada escrito");
		return HFI_NONE;
	}
	cache_clean_inval(smmu_pool, sizeof(smmu_pool));
	cache_clean_inval(gmu_hfi_mem, GMU_MEM_SIZE);
	cache_clean_inval(gmu_dump_mem, GMU_MEM_SIZE);
	smmu_pool_used = 0;
	smmu_root = smmu_new_table();
	if (!smmu_root)
		return HFI_NONE;
	for (uint32_t off = 0; off < GMU_MEM_SIZE; off += 4096)
		if (!smmu_map(GMU_HFI_ADDR + off, pa(gmu_hfi_mem) + off) ||
		    !smmu_map(GMU_DUMP_ADDR + off, pa(gmu_dump_mem) + off)) {
			con_str("3a", "ERRO sem tabelas");
			return HFI_NONE;
		}
	dsb();	/* Device stores: in memory before the SMMU walks them */
	for (uint32_t off = 0; off < GMU_MEM_SIZE; off += 4096)
		if (smmu_walk(GMU_HFI_ADDR + off) != pa(gmu_hfi_mem) + off ||
		    smmu_walk(GMU_DUMP_ADDR + off) != pa(gmu_dump_mem) + off) {
			con_str("3a", "ERRO as tabelas nao resolvem: nada escrito");
			return HFI_NONE;
		}
	ttbr0 = pa(smmu_root) | ((uint64_t)SMMU_GMU_ASID << 48);
	con_hex2("3a", "l1 ", (uint32_t)pa(smmu_root), "hfi ", (uint32_t)pa(gmu_hfi_mem));

	stage = "3 smmu escrita";
	if (!smmu_state_as_left() || !smmu_qcom_reset() || !smmu_context_program(ttbr0) || !smmu_streams_on()) {
		con_str("3", "ERRO SMMU: o GMU NAO sera liberado");
		return HFI_NONE;
	}
	smmu_translate = 1;
	gmu_hfi_va = GMU_HFI_ADDR;
	gmu_dump_va = GMU_DUMP_ADDR;
	con_str("3", "OK SMMU traduz o GMU (tudo lido de volta)");
	return HFI_TRANSLATE;
}

/* 3z: GMU already in reset -> streams back to FAULT and CB0 off (the boot loader's state). */
static void smmu_restore(void)
{
	int ok, s2 = 0;

	if (!smmu_written)
		return;
	stage = "3z smmu restaura";
	if (smmu_dead) {
		con_str("3z", "SMMU sem resposta: nada restaurado (o reboot zera)");
		return;
	}
	acc_tag = "3z";
	/* [M11.4] CRITICAL revert (verified by readback): invalidate both SMRs and disable CB0, then TLB
	 * sync. With SMR=0 no stream matches CB0 and the context bank is off, so the GMU SIDs 4/5 no longer
	 * translate -- this is what actually makes the SMMU safe, independent of the S2CR TYPE field. */
	step("3z invalida streams (SMR=0) + desliga CB0 + TLB sync");
	ok = s_set("3z smr0", SMMU_SMR(0), 0, ~0u) &&
	     s_set("3z smr1", SMMU_SMR(1), 0, ~0u) &&
	     s_set("3z sctlr", SMMU_CB(GMU_CB, SMMU_CB_SCTLR), 0, ~0u) &&
	     smmu_tlb_sync();
	/* COSMETIC best-effort: try to put S2CR back to the boot-loader FAULT value. On the S9 this
	 * translate->fault write is rejected (M11.3 + M11.4: reads back 0x300, even with SMR already 0),
	 * which is harmless now that SMR=0 neutralised the streams. Read back transparently, no hard error. */
	{
		uint64_t r0 = 0, r1 = 0;

		s_access("3z", PROBE_WRITE, SMMU_S2CR(0), SMMU_S2CR_FAULT_IDLE, &r0);
		s_access("3z", PROBE_WRITE, SMMU_S2CR(1), SMMU_S2CR_FAULT_IDLE, &r1);
		con_hex2("3z s2cr rb", "0:", (uint32_t)r0, "1:", (uint32_t)r1);
		s2 = ((uint32_t)r0 == SMMU_S2CR_FAULT_IDLE) && ((uint32_t)r1 == SMMU_S2CR_FAULT_IDLE);
	}
	if (!ok)
		con_str("3z", "ERRO: revert essencial (SMR/CB0) incompleto");
	else
		con_str("3z", s2 ? "SMMU de volta ao estado do boot loader"
				 : "streams off + CB0 off OK (S2CR TYPE nao reverte: cosmetico)");
}

/* CB0 fault status (worker); 1 = a fault was recorded or the SMMU stopped answering. */
static int smmu_faulted(void)
{
	uint32_t fsr, v;

	if (!s_rd(SMMU_CB(GMU_CB, SMMU_CB_FSR), &fsr))
		return 1;
	if (!(fsr & SMMU_FSR_ALL))
		return 0;
	showhex("ERRO SMMU fsr", fsr);
	if (s_rd(SMMU_CB(GMU_CB, SMMU_CB_FAR), &v))
		showhex("  far", v);
	if (s_rd(SMMU_CB(GMU_CB, SMMU_CB_FSYNR0), &v))
		showhex("  fsynr0", v);
	return 1;
}

/* ---- the HFI queue table + polled send/receive (kgsl_hfi.c, 3 queues) ------------------------------ */

/* kgsl_hfi.h: 6-word table header, then one 12-word header per queue. */
#define HFI_TBL_HDR_WORDS	6
#define HFI_QHDR_WORDS		12
#define HFI_TABLE_WORDS		(HFI_TBL_HDR_WORDS + HFI_QUEUES * HFI_QHDR_WORDS)
#define HFI_QUEUE_OFFSET(i)	((HFI_TABLE_WORDS * 4u) + (i) * (4096u + 16u))	/* bytes, 16 B guard */
#define HFI_CMD_Q		0
#define HFI_MSG_Q		1
/* queue header word indices (struct hfi_queue_header) */
#define QH_STATUS		0
#define QH_START		1
#define QH_TYPE			2
#define QH_SIZE			3
#define QH_MSG_SIZE		4
#define QH_DROP			5
#define QH_RX_WM		6
#define QH_TX_WM		7
#define QH_RX_REQ		8
#define QH_TX_REQ		9
#define QH_READ			10
#define QH_WRITE		11

_Static_assert(HFI_QUEUE_OFFSET(HFI_QUEUES) <= GMU_MEM_SIZE, "HFI queues exceed HFIMEM_SIZE");
_Static_assert(HFI_QUEUE_WORDS * 4 == 4096, "HFI_QUEUE_SIZE is SZ_4K");

static volatile uint32_t *hfi;	/* = gmu_hfi_mem as words */
static uint32_t hfi_seqnum;
static uint32_t hfi_payload0;	/* payload[0] of the last ACK (FW_VER: the firmware version) */

static volatile uint32_t *hfi_qhdr(unsigned q)
{
	return hfi + HFI_TBL_HDR_WORDS + q * HFI_QHDR_WORDS;
}

/* hfi_init (kgsl_hfi.c L134-170). */
static void hfi_table_init(void)
{
	static const uint8_t qid[HFI_QUEUES] = { 0, 4, 5 };
	static const uint8_t qprio[HFI_QUEUES] = { 10, 10, 40 };

	zero(gmu_hfi_mem, GMU_MEM_SIZE);
	zero(gmu_dump_mem, GMU_MEM_SIZE);
	hfi = (volatile uint32_t *)gmu_hfi_mem;
	hfi[0] = 0;				/* version */
	hfi[1] = HFI_TABLE_WORDS;		/* size (dwords) */
	hfi[2] = HFI_TBL_HDR_WORDS;		/* qhdr0 offset */
	hfi[3] = HFI_QHDR_WORDS;		/* qhdr size */
	hfi[4] = HFI_QUEUES;			/* num_q */
	hfi[5] = HFI_QUEUES;			/* num_active_q */
	for (unsigned i = 0; i < HFI_QUEUES; i++) {
		volatile uint32_t *h = hfi_qhdr(i);

		h[QH_STATUS] = 1;
		h[QH_START] = gmu_hfi_va + HFI_QUEUE_OFFSET(i);
		h[QH_TYPE] = qid[i] | ((uint32_t)qprio[i] << 8);	/* QUEUE_HDR_TYPE(id, prio, 0, 0) */
		h[QH_SIZE] = HFI_QUEUE_WORDS;
		h[QH_MSG_SIZE] = 0;
		h[QH_DROP] = 0;
		h[QH_RX_WM] = 1;
		h[QH_TX_WM] = 1;
		h[QH_RX_REQ] = 1;
		h[QH_TX_REQ] = 0;
		h[QH_READ] = 0;
		h[QH_WRITE] = 0;
	}
	dsb();
}

/* hfi_msgq_read + hfi_receiver (kgsl_hfi.c L18-62, L547-580) on the MSG queue.
 * 1 = the ACK of (id, seq) was read (*err = its error word); 0 = not yet; -1 = stop. */
static int hfi_receive(uint32_t id, uint32_t seq, uint32_t *err)
{
	volatile uint32_t *h = hfi_qhdr(HFI_MSG_Q);
	volatile uint32_t *q = (volatile uint32_t *)(gmu_hfi_mem + HFI_QUEUE_OFFSET(HFI_MSG_Q));
	int got = 0;

	for (int n = 0; n < 64 && h[QH_READ] != h[QH_WRITE]; n++) {
		uint32_t rd = h[QH_READ], m[HFI_RSP_WORDS] = { 0 }, size;

		if (rd >= HFI_QUEUE_WORDS) {
			showhex("5 ERRO read idx", rd);
			return -1;
		}
		size = (q[rd] >> 8) & 0xff;
		if (size == 0 || size > HFI_RSP_WORDS) {
			showhex("5 ERRO msg hdr", q[rd]);
			return -1;
		}
		for (uint32_t i = 0; i < size; i++) {
			m[i] = q[rd];
			rd = (rd + 1) % HFI_QUEUE_WORDS;
		}
		h[QH_READ] = rd;
		dsb();
		switch (m[0] & 0xff) {
		case F2H_MSG_ACK:
			con_hex2("5 rx ACK", "hdr ", m[0], "req ", m[1]);
			if ((m[1] & 0xff) == id && (m[1] >> 20) == seq) {
				*err = m[2];
				hfi_payload0 = m[3];
				got = 1;
			}
			break;
		case F2H_MSG_ERR:
			con_hex2("5 ERRO F2H_ERR", "hdr ", m[0], "code ", m[1]);
			return -1;
		default:
			showhex("5 rx id ?", m[0]);
		}
	}
	if (h[QH_READ] == h[QH_WRITE])
		h[QH_RX_REQ] = 1;
	return got;
}

/* hfi_send_msg + hfi_cmdq_write (kgsl_hfi.c L66-125, L214-270): exactly `words` dwords at the
 * write index (no padding), barrier, HOST2GMU_INTR_SET; then up to HFI_RSP_TIMEOUT for the ACK. */
static int hfi_send(const char *what, uint32_t id, uint32_t *msg, uint32_t words, uint32_t *err)
{
	volatile uint32_t *h = hfi_qhdr(HFI_CMD_Q);
	volatile uint32_t *q = (volatile uint32_t *)(gmu_hfi_mem + HFI_QUEUE_OFFSET(HFI_CMD_Q));
	uint32_t wr = h[QH_WRITE], rd = h[QH_READ], space, seq = ++hfi_seqnum & 0xfff;
	int got = 0, ms;

	space = wr >= rd ? HFI_QUEUE_WORDS - (wr - rd) : rd - wr;
	if (wr >= HFI_QUEUE_WORDS || space < words) {
		con_str(what, "ERRO fila CMD sem espaco");
		return 0;
	}
	msg[0] = id | (words << 8) | ((uint32_t)HFI_MSG_CMD << 16) | (seq << 20);
	for (uint32_t i = 0; i < words; i++) {
		q[wr] = msg[i];
		wr = (wr + 1) % HFI_QUEUE_WORDS;
	}
	h[QH_WRITE] = wr;
	dsb();		/* wmb(): packet + write index before the interrupt */
	wr32(GMU(GMU_HOST2GMU_INTR_SET), 1);
	dsb();
	showhex("5 tx hdr", msg[0]);
	for (ms = 0; ms < HFI_RSP_TIMEOUT_MS && got == 0; ms++) {
		if (rd32(GMU(GMU_GMU2HOST_INTR_INFO)) & HFI_IRQ_MSGQ)
			wr32(GMU(GMU_GMU2HOST_INTR_CLR), HFI_IRQ_MSGQ);
		got = hfi_receive(id, seq, err);
		if (got == 0 && ms % 100 == 99 && smmu_faulted())
			got = -1;
		wdog_pet();
		if (got == 0)
			delay_ms(1);
	}
	if (got == 1 && *err == 0) {
		showhex("5 ACK ok, ms", ms);
		return 1;
	}
	if (got == 1)
		showhex("5 ERRO ACK error", *err);
	else if (got == 0)
		con_str(what, "ERRO sem ACK em 5 s");
	else
		con_str(what, "ERRO parado (ver acima)");
	return 0;
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

static int gmu_released;

/* adreno_a6xx.c a6xx_gmu_fw_start L1839-1898 + a6xx_gmu_start L1403-1426 (M10 order). */
static int gmu_boot_with_hfi(void)
{
	uint32_t result = 0, wrong = 0, gfsr = 0;
	unsigned words = sizeof(a630_gmu_fw) / sizeof(a630_gmu_fw[0]);
	int ms;

	stage = "4 firmware+boot";
	step("4a RSC/PDC + GMU em reset + firmware no ITCM");
	load_rpmh_ucode();	/* M10/M11.1 order, proven on the S9 */
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
		showhex("4a ERRO itcm errados", wrong);
		return 0;
	}

	step("4b HFI_QTBL = 0x60000000 (GMU-VA) e solta o GMU");
	hfi_table_init();
	wr32(GMU(GMU_CM3_FW_INIT_RESULT), 0);
	wr32(GMU(GMU_CM3_BOOT_CONFIG), GMU_BOOT_CONFIG_TCM);
	wr32(GMU(GMU_HFI_QTBL_ADDR), gmu_hfi_va);
	wr32(GMU(GMU_HFI_QTBL_INFO), 1);
	wr32(GMU(GMU_AHB_FENCE_RANGE_0), FENCE_RANGE_MASK);
	wr32(GMU(GMU_HFI_SFR_ADDR), GMU_CHIPID);
	wr32(GMU(GMU_SYS_BUS_CONFIG), 1);
	wr32(GMU(GMU_PWR_COL_FRAME_CTRL), 0x9C40400u);
	wr32(GMU(GMU_RPMH_CTRL), 0);
	wr32(GMU(GMU_GMU2HOST_INTR_CLR), 0xFFFFFFFFu);
	wr32(GMU(GMU_CX_GMU_WFI_CONFIG), 0);
	dsb();
	if (rd32(GMU(GMU_HFI_QTBL_ADDR)) != gmu_hfi_va) {
		showhex("4b ERRO qtbl_addr lido", rd32(GMU(GMU_HFI_QTBL_ADDR)));
		return 0;
	}
	gmu_released = 1;
	wr32(GMU(GMU_CM3_SYSRESET), 0);
	dsb();
	for (ms = 0; ms < 1000 && result != GMU_BABEFACE; ms++) {
		result = rd32(GMU(GMU_CM3_FW_INIT_RESULT));
		wdog_pet();
		if (result != GMU_BABEFACE)
			delay_ms(1);
	}
	con_hex2("4c", "fw_init ", result, "ms ", (uint32_t)ms);
	if (result != GMU_BABEFACE) {
		con_str("4c", "ERRO o GMU nao subiu (sem 0xBABEFACE)");
		return 0;
	}
	acc_tag = "4c";
	if (smmu_faulted() || !s_rd(SMMU_SGFSR, &gfsr) || gfsr) {
		showhex("4c ERRO sgfsr", gfsr);
		return 0;
	}
	con_str("4c", "OK GMU pronto (0xBABEFACE), SMMU sem falha");
	return 1;
}

/* ---- stage 5: HFI up + INIT + FW_VER (hfi_start order, stopped before PERF_TBL) ------------------- */

static void hfi_talk(void)
{
	uint32_t msg[8], err = 0, st = 0;
	int up = 0;

	stage = "5 HFI";
	acc_tag = "5";
	step("5a filas HFI: mascara MSGQ + HFI_CTRL_INIT");
	/* a6xx_gmu_hfi_start L1433-1449 */
	wr32(GMU(GMU_GMU2HOST_INTR_MASK), rd32(GMU(GMU_GMU2HOST_INTR_MASK)) & ~HFI_IRQ_MSGQ);
	wr32(GMU(GMU_HFI_CTRL_INIT), 1);
	dsb();
	for (int ms = 0; ms < GMU_START_TIMEOUT_MS && !up; ms++) {
		st = rd32(GMU(GMU_HFI_CTRL_STATUS));
		if (st & 1)
			up = 1;
		else
			delay_ms(1);
	}
	con_hex2("5a", "status ", st, "version ", rd32(GMU(GMU_HFI_VERSION_INFO)));
	if (!up) {
		con_str("5a", "ERRO as filas HFI nao subiram em 100 ms");
		smmu_faulted();
		return;
	}

	step("5b H2F_MSG_INIT (cold boot, dump 0x60100000)");
	zero(msg, sizeof(msg));
	msg[1] = 0;			/* seg_id */
	msg[2] = gmu_dump_va;		/* dbg_buffer_addr */
	msg[3] = GMU_MEM_SIZE;		/* dbg_buffer_size (DUMPMEM_SIZE) */
	msg[4] = 1;			/* GMU_COLD_BOOT */
	if (!hfi_send("5b INIT", H2F_MSG_INIT, msg, 5, &err))
		return;

	step("5c H2F_MSG_FW_VER (pede 1.3)");
	zero(msg, sizeof(msg));
	msg[1] = GMU_FW_WANTED;		/* supported_ver */
	if (!hfi_send("5c FW_VER", H2F_MSG_FW_VER, msg, 2, &err))
		return;
	showhex("5c fw version", hfi_payload0);
	if ((hfi_payload0 >> 28) != 1)
		con_str("5c", "AVISO major != 1 (o kgsl so avisa)");
	con_str("5", "OK INIT e FW_VER com ACK (PERF/BW/TEST nao enviados)");
}
#endif /* HAVE_GMU_FW */

/* ---- [M11.3] RPMh cmd-db discovery + GX power-state readout (READ-ONLY) -------------------------- */

#define CMDDB_DICT_PHYS		0x0c3f000cUL	/* S9 DT qcom,cmd-db@861e0000 reg = <0x0c3f000c 0x8> */
#define CMDDB_MAGIC		0x0c0330dbUL	/* cmd-db.c CMD_DB_MAGIC */
#define CMDDB_HDR_SIZE		144u		/* sizeof(struct cmd_db_header): 4+4+8*16+4+4 */
#define CMDDB_RSC_MAX		8u		/* MAX_SLV_ID */
#define CMDDB_ENT_SIZE		24u		/* sizeof(struct entry_header) */
#define CMDDB_SCAN_CAP		256u		/* bound on total entries scanned */
/* res_id = cmd_db_get_u64_id("name"): the ASCII bytes packed little-endian. */
#define RES_GFX_LO		0x2e786667u	/* "gfx." */
#define RES_GFX_HI		0x006c766cu	/* "lvl\0" */
#define RES_MX_LO		0x6c2e786du	/* "mx.l" */
#define RES_MX_HI		0x00006c76u	/* "vl\0\0" */
#define RES_CX_LO		0x6c2e7863u	/* "cx.l" */
#define RES_CX_HI		0x00006c76u	/* "vl\0\0" */

/* read a cmd-db u32 on the worker core (bounded); 0 = no answer -> caller SKIPs. */
static int cmddb_rd(uintptr_t addr, uint32_t *v)
{
	uint64_t r;

	if (!worker_do("P", PROBE_READ, addr, 0, &r))
		return 0;
	*v = (uint32_t)r;
	return 1;
}

static uint32_t cmddb_last;

/* [M11.4] captured ARC VLVL tables (raw, as stored in cmd-db). */
#define ARC_CAP		20u
static uint16_t gfx_vlvl[ARC_CAP], mx_vlvl[ARC_CAP];
static uint32_t gfx_raw_n, mx_raw_n;

/* Read one aux table into dst[] (cap) on the worker core and print the first 10 entries.
 * Returns the number of u16 read (<= cap). dst/cap may be 0 to only print (cx.lvl). */
static uint32_t cmddb_aux(const char *name, uintptr_t aux, uint32_t len_bytes, uint16_t *dst, uint32_t cap)
{
	char b[52], *p = put(put(b, name), ": ");
	uint32_t n = len_bytes / 2, cnt = 0;

	for (uint32_t i = 0; i < n; i++) {
		uint32_t w;
		uint16_t v;

		if ((i & 1) == 0) {
			if (!cmddb_rd(aux + (i / 2) * 4, &w))
				break;
			cmddb_last = w;
		}
		v = (i & 1) ? (cmddb_last >> 16) : (cmddb_last & 0xffff);
		if (dst && cnt < cap)
			dst[cnt] = v;
		if (i < 10) {
			p = put_hex(p, v, 4);
			*p++ = ' ';
		}
		cnt++;
	}
	*p = 0;
	con_str("P arc", b);
	return cnt > cap ? cap : cnt;
}

/* Effective ARC entry count = strictly-ascending prefix, exactly as the kernel determines it
 * (kgsl_gmu.c rpmh_read_arc_values L538-541: stop when val[n-1] >= val[n]). This drops the
 * trailing zero padding and any non-monotonic tail from a bad read. */
static uint32_t arc_count(const uint16_t *v, uint32_t n)
{
	uint32_t c = 1;

	if (n == 0)
		return 0;
	while (c < n && v[c - 1] < v[c])
		c++;
	return c;
}
static void cmddb_discover(void)
{
	uint32_t base, size, ver, magic, scanned = 0;

	stage = "P cmd-db (RPMh)";
	acc_tag = "P";
	step("P le dicionario cmd-db @0x0c3f000c");
	if (!cmddb_rd(CMDDB_DICT_PHYS, &base) || !cmddb_rd(CMDDB_DICT_PHYS + 4, &size)) {
		con_str("P", "SKIP: dicionario cmd-db nao respondeu");
		return;
	}
	con_hex2("P base/size", "", base, "", size);
	if (!base || (base & 0xfff) || size < CMDDB_HDR_SIZE) {
		con_str("P", "SKIP: ponteiro cmd-db invalido");
		return;
	}
	step("P le cabecalho cmd-db");
	if (!cmddb_rd(base, &ver) || !cmddb_rd(base + 4, &magic)) {
		con_str("P", "SKIP: cabecalho cmd-db nao respondeu");
		return;
	}
	con_hex2("P ver/magic", "", ver, "", magic);
	if (magic != CMDDB_MAGIC) {
		con_str("P", "SKIP: magic != 0x0C0330DB");
		return;
	}
	step("P procura gfx.lvl / mx.lvl / cx.lvl");
	for (uint32_t i = 0; i < CMDDB_RSC_MAX; i++) {
		uint32_t w0, w1, slv, hoff, doff, cnt;

		if (!cmddb_rd(base + 8 + i * 16, &w0) || !cmddb_rd(base + 8 + i * 16 + 4, &w1))
			return;
		slv = w0 & 0xffff;
		hoff = w0 >> 16;
		doff = w1 & 0xffff;
		cnt = w1 >> 16;
		if (!slv)
			break;
		for (uint32_t j = 0; j < cnt && scanned < CMDDB_SCAN_CAP; j++, scanned++) {
			uintptr_t ent = base + CMDDB_HDR_SIZE + hoff + j * CMDDB_ENT_SIZE;
			uint32_t lo, hi, lw;
			const char *nm = 0;

			if (!cmddb_rd(ent, &lo) || !cmddb_rd(ent + 4, &hi))
				return;
			if (lo == RES_GFX_LO && hi == RES_GFX_HI)
				nm = "gfx.lvl";
			else if (lo == RES_MX_LO && hi == RES_MX_HI)
				nm = "mx.lvl";
			else if (lo == RES_CX_LO && hi == RES_CX_HI)
				nm = "cx.lvl";
			if (!nm)
				continue;
			if (!cmddb_rd(ent + 20, &lw))
				return;
			{
				uintptr_t aux = base + CMDDB_HDR_SIZE + doff + (lw >> 16);
				uint32_t len = lw & 0xffff;

				if (nm[0] == 'g')	/* gfx.lvl */
					gfx_raw_n = cmddb_aux(nm, aux, len, gfx_vlvl, ARC_CAP);
				else if (nm[0] == 'm')	/* mx.lvl */
					mx_raw_n = cmddb_aux(nm, aux, len, mx_vlvl, ARC_CAP);
				else			/* cx.lvl: print only */
					cmddb_aux(nm, aux, len, 0, 0);
			}
		}
	}
	con_str("P", scanned ? "OK cmd-db lida (tabelas ARC acima)" : "SKIP: nenhuma entrada ARC");
}

/* GX GDSC / SPTPRAC / RPMh power-state snapshot. GMU AO regs are readable once CX is on (m11e
 * already reads GMU regs post-power); GX GDSC is a GPUCC reg. All read-only. */
static void power_state_readout(void)
{
	stage = "P estado de energia";
	step("P estado GX/SPTPRAC/RPMh (so leitura)");
	con_hex2("P gx/sptp", "", rd32(GPUCC + GPUCC_GX_GDSCR), "", rd32(GMU(GMU_SPTPRAC_PWR_CLK_STATUS)));
	showhex("P rpmh pwr", rd32(GMU(GMU_RPMH_POWER_STATE)));
}

/* ---- [M11.4] derive the boot GX/MX vote indices from the live cmd-db ARC tables (READ-ONLY) ------
 * setup_volt_dependency_tbl (kgsl_gmu.c L574-598): pri_idx = index k where gfx.lvl[k] == corner;
 * sec_idx = first j where mx.lvl[j] >= corner. corner = GX_BOOT_VLVL (0x40, source-derived). No index
 * is ever guessed: a missing/duplicate/unsorted table or an absent corner fails and leaves GX off. */
static int gx_votes_valid;
static uint32_t gx_pri_idx, gx_sec_idx;

static void gx_vote_derive(void)
{
	uint32_t gn = arc_count(gfx_vlvl, gfx_raw_n), mn = arc_count(mx_vlvl, mx_raw_n);
	uint32_t k, found = 0, pri = 0, sec;

	stage = "Pv voto GX";
	acc_tag = "Pv";
	step("Pv deriva GX/MX vote (corner 0x40, so leitura)");
	gx_votes_valid = 0;
	con_hex2("Pv n", "gfx ", gn, "mx ", mn);
	if (gn < 2 || mn < 1 || gn > ARC_CAP || mn > ARC_CAP) {
		con_str("Pv", "SKIP: tabelas ARC ausentes/curtas (GX fica desligado)");
		return;
	}
	/* pri_idx: gfx.lvl[k] == corner. The ascending prefix guarantees it appears at most once. */
	for (k = 0; k < gn; k++)
		if (gfx_vlvl[k] == GX_BOOT_VLVL) {
			pri = k;
			found++;
		}
	if (found != 1) {
		showhex("Pv gfx 0x40 count", found);
		con_str("Pv", "ERRO: corner 0x40 ausente/duplicado em gfx.lvl (GX desligado)");
		return;
	}
	/* sec_idx: first mx.lvl >= corner, else the LAST index -- exactly setup_volt_dependency_tbl
	 * (kgsl_gmu.c L589-598): `if (mx[j] >= cur_vlvl || j + 1 == mx_num) { sec_idx = j; break; }`. */
	for (sec = 0; sec < mn; sec++)
		if (mx_vlvl[sec] >= GX_BOOT_VLVL || sec + 1 == mn)
			break;
	gx_pri_idx = pri;
	gx_sec_idx = sec;
	gx_votes_valid = 1;
	con_hex2("Pv idx", "pri ", pri, "sec ", sec);
	showhex("Pv mx[sec]", mx_vlvl[sec]);
	con_str("Pv", "OK voto GX derivado do S9 (GX continua desligado)");
}

#ifndef M11G_TRY_GX_RAIL
#define M11G_TRY_GX_RAIL 0	/* default: GX NEVER activated; the routine below is not even compiled */
#endif

#if M11G_TRY_GX_RAIL
/* [M11.4 EXPERIMENT, off by default] a6xx_gfx_rail_on (adreno_a6xx.c L1634-1665) + a6xx_sptprac_enable
 * (L1538-1560). Powers the 3D GX rail -- the step that can reset the SoC -- so it is compiled ONLY
 * with -DM11G_TRY_GX_RAIL=1 and still needs its own supervised test. Uses only the derived indices. */
static int gx_rail_on(void)
{
	uint32_t info = 0, sptp = 0;
	int ok = 0;

	stage = "4 GX rail";
	if (!gx_votes_valid) {
		con_str("4", "ABORT: voto GX nao derivado (GX nao ligado)");
		return 0;
	}
	step("4 GX rail: GX/MX vote + boot OOB");
	wr32(GMU(GMU_BOOT_SLUMBER_OPTION), OOB_BOOT_OPTION);
	wr32(GMU(GMU_GX_VOTE_IDX), gx_pri_idx);
	wr32(GMU(GMU_MX_VOTE_IDX), gx_sec_idx);
	dsb();
	wr32(GMU(GMU_GMU2HOST_INTR_CLR), OOB_BOOT_SLUMBER_CLEAR_MASK);
	wr32(GMU(GMU_HOST2GMU_INTR_SET), OOB_BOOT_SLUMBER_SET_MASK);
	dsb();
	for (int ms = 0; ms < GMU_START_TIMEOUT_MS && !ok; ms++) {
		info = rd32(GMU(GMU_GMU2HOST_INTR_INFO));
		if (info & OOB_BOOT_SLUMBER_CHECK_MASK)
			ok = 1;
		else {
			delay_ms(1);
			wdog_pet();
		}
	}
	wr32(GMU(GMU_GMU2HOST_INTR_CLR), OOB_BOOT_SLUMBER_CLEAR_MASK);
	dsb();
	con_hex2("4 oob", "info ", info, "rpmh ", rd32(GMU(GMU_RPMH_POWER_STATE)));
	if (!ok) {
		con_str("4", "FAIL: boot OOB timeout (GX nao subiu)");
		return 0;
	}
	/* SPTPRAC sits in the GX domain; enable only after the GX rail OOB succeeded. */
	step("4 SPTPRAC power-on");
	wr32(GMU(GMU_GX_SPTPRAC_POWER_CONTROL), SPTPRAC_POWERON_CTRL_MASK);
	dsb();
	for (int ms = 0; ms < SPTPRAC_CTRL_TIMEOUT_MS; ms++) {
		sptp = rd32(GMU(GMU_SPTPRAC_PWR_CLK_STATUS));
		if (sptp & SPTPRAC_POWERON_STATUS_MASK)
			break;
		delay_ms(1);
		wdog_pet();
	}
	con_hex2("4 sptp", "stat ", sptp, "gx ", rd32(GPUCC + GPUCC_GX_GDSCR));
	con_str("4", (sptp & SPTPRAC_POWERON_STATUS_MASK) ? "GX rail ON + SPTPRAC ON" : "GX OOB ok, SPTPRAC timeout");
	return 1;
}
#endif


#ifdef HAVE_CP_FW
/* ---- [M12] PM4 packet helpers (adreno_pm4types.h L166-205), bit-for-bit the kernel's ------------- */
static uint32_t pm4_parity(uint32_t val)
{
	return (0x9669u >> (0xfu & (val ^ (val >> 4) ^ (val >> 8) ^ (val >> 12) ^
		(val >> 16) ^ (val >> 20) ^ (val >> 24) ^ (val >> 28)))) & 1u;
}

static uint32_t cp_type7(uint32_t opcode, uint32_t cnt)
{
	return CP_TYPE7_PKT | (cnt & 0x7fffu) | (pm4_parity(cnt) << 15) |
		((opcode & 0x7fu) << 16) | (pm4_parity(opcode) << 23);
}

/* ---- [M12] Cp: prepare the CP in RAM + our SMMU tables only (SAFE, always run) ------------------ */
static void cp_prepare(void)
{
	uint32_t *sqe = (uint32_t *)cp_sqe_mem;	/* runtime pointer: no static init -> no ABS64 reloc */
	uint32_t i, bad = 0;

	stage = "Cp preparo CP";
	acc_tag = "Cp";
	step("Cp preparo CP (so RAM+SMMU, sem tocar a GPU)");

	/* SQE microcode: the embedded copy already has the 4-byte version header stripped
	 * (tools/cp-fw-embed.py, adreno_a6xx.c _load_firmware L1174). */
	for (i = 0; i < A630_SQE_UCODE_DWORDS; i++)
		sqe[i] = a630_sqe_ucode[i];
	cache_clean_inval(cp_sqe_mem, A630_SQE_UCODE_DWORDS * 4);

	/* Map ring / SQE / scratch into the GPU SMMU CB0 table (same TTBR0 the GMU uses). */
	for (i = 0; i < CP_RB_DWORDS * 4; i += 0x1000)
		if (!smmu_map(CP_RING_VA + i, pa(cp_ring_mem) + i)) bad++;
	for (i = 0; i < A630_SQE_UCODE_DWORDS * 4 + 0x1000; i += 0x1000)
		if (!smmu_map(CP_SQE_VA + i, pa(cp_sqe_mem) + i)) bad++;
	if (!smmu_map(CP_SCRATCH_VA, pa(cp_scratch_mem))) bad++;
	zero(cp_scratch_mem, 4096);
	cache_clean_inval(cp_scratch_mem, 4096);

	/* Validate by walking our own tables (what the SMMU hardware will do). */
	if (smmu_walk(CP_RING_VA) != pa(cp_ring_mem)) bad++;
	if (smmu_walk(CP_SQE_VA) != pa(cp_sqe_mem)) bad++;
	if (smmu_walk(CP_SCRATCH_VA) != pa(cp_scratch_mem)) bad++;

	con_hex2("Cp map", "ring->", (uint32_t)smmu_walk(CP_RING_VA), "sqe->", (uint32_t)smmu_walk(CP_SQE_VA));
	showhex("Cp sqe versao", A630_SQE_VERSION);
	con_hex2("Cp me_init", "hdr ", cp_type7(CP_ME_INIT, 11), "mask ", CP_INIT_MASK_MIN);
	con_hex2("Cp pkts", "nop ", cp_type7(CP_NOP, 0), "memw ", cp_type7(CP_MEM_WRITE, 3));
	con_hex2("Cp cntl", "rb ", CP_RB_CNTL_DEFAULT, "zap n", A630_ZAP_MDT_LEN);
	if (bad) {
		showhex("Cp falhas de mapeamento", bad);
		con_str("Cp", "ERRO: layout CP invalido (NAO prosseguir p/ cp_bringup)");
	} else {
		con_str("Cp", "OK layout CP pronto: SQE copiada, ring/sqe/scratch mapeados e validados");
	}
}

#if M12_TRY_CP
/* ---- [M12] TrustZone SMC conduit from EL1 (qcom_scm-64.c arm_smccc_smc), full PAS register set --- */
static uint64_t cp_smc5(uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t *out1)
{
	register uint64_t r0 __asm__("x0") = x0;
	register uint64_t r1 __asm__("x1") = x1;
	register uint64_t r2 __asm__("x2") = x2;
	register uint64_t r3 __asm__("x3") = x3;
	register uint64_t r4 __asm__("x4") = x4;
	register uint64_t r5 __asm__("x5") = 0;	/* no extended-arg buffer: all PAS args fit in x2..x4 */
	register uint64_t r6 __asm__("x6") = 0;	/* ARM_SMCCC_QUIRK_QCOM_A6 resume state */
	int guard = 0;

	do {
		__asm__ volatile("smc #0"
			: "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6)
			:
			: "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		/* qcom_scm_call loop: on QCOM_SCM_INTERRUPTED re-issue (bounded). */
	} while ((int64_t)r0 == SCM_INTERRUPTED && ++guard < 20);

	if (out1)
		*out1 = r1;
	return r0;
}

/* qcom_scm_call: ret<0 is the SCM error; otherwise the result is res.a1 (__qcom_scm_pas_* return). */
static int64_t pas_call(uint32_t cmd_id, uint64_t arginfo, uint64_t a0, uint64_t a1, uint64_t a2)
{
	uint64_t res1 = 0;
	uint64_t cmd = SCM_SIP_SMC64 | SCM_FNID(SCM_SVC_PIL, cmd_id);
	int64_t st = (int64_t)cp_smc5(cmd, arginfo, a0, a1, a2, &res1);

	return st ? st : (int64_t)res1;
}

/* Publish ring dwords [0, cp_wptr) and wait for the CP to consume them (adreno_ringbuffer_submit_spin,
 * 2000 ms).  Returns 1 if RPTR reached WPTR. */
static uint32_t cp_wptr;

static int cp_submit(const char *tag)
{
	cache_clean_inval(cp_ring_mem, cp_wptr * 4);
	dsb();
	wr32(GPU(A6XX_CP_RB_WPTR), cp_wptr);
	dsb();
	for (int ms = 0; ms < 2000; ms++) {
		if (rd32(GPU(A6XX_CP_RB_RPTR)) == cp_wptr)
			return 1;
		delay_ms(1);
		wdog_pet();
	}
	con_hex2(tag, "rptr ", rd32(GPU(A6XX_CP_RB_RPTR)), "wptr ", cp_wptr);
	return 0;
}

/* ---- [M12] C: actual CP bring-up on the 3D core (GATED; needs the GX rail up) ------------------- */
static void cp_bringup(void)
{
	uint32_t *ring = (uint32_t *)cp_ring_mem;
	uint32_t *scratch = (uint32_t *)cp_scratch_mem;
	uint64_t canary = 0, dst;
	uint32_t memw;

	stage = "C CP bring-up";
	acc_tag = "C";
	/* First 3D-core touch is fault-isolated: if GX/AHB-fence is not truly up this aborts on the
	 * worker core instead of panicking the main core. */
	step("C canary RBBM_STATUS (nucleo isolado)");
	if (!worker_do("C canary", PROBE_READ, GPU(A6XX_RBBM_STATUS), 0, &canary)) {
		con_str("C", "ABORT: 3D core nao responde (GX/fence?) -- CP nao iniciado");
		return;
	}
	showhex("C rbbm_status", canary);

	/* SECVID address mode (a6xx_start L487) + SQE microcode base (a6xx_microcode_load L838-843). */
	step("C SECVID + base da SQE");
	wr32(GPU(A6XX_RBBM_SECVID_TSB_ADDR_MODE_CNTL), 0x1);
	wr32(GPU(A6XX_CP_SQE_INSTR_BASE_LO), (uint32_t)CP_SQE_VA);
	wr32(GPU(A6XX_CP_SQE_INSTR_BASE_HI), (uint32_t)((uint64_t)CP_SQE_VA >> 32));
	dsb();

	/* Ring base / cntl / rptr-addr (a6xx_rb_start L1090-1104). */
	step("C ring base/cntl/rptr-addr");
	wr32(GPU(A6XX_CP_RB_RPTR_ADDR_LO), (uint32_t)(CP_SCRATCH_VA + CP_SCRATCH_RPTR * 4));
	wr32(GPU(A6XX_CP_RB_RPTR_ADDR_HI), 0);
	wr32(GPU(A6XX_CP_RB_CNTL), CP_RB_CNTL_DEFAULT);
	wr32(GPU(A6XX_CP_RB_BASE), (uint32_t)CP_RING_VA);
	wr32(GPU(A6XX_CP_RB_BASE_HI), 0);
	wr32(GPU(A6XX_CP_RB_WPTR), 0);
	cp_wptr = 0;
	dsb();

	/* Clear SQE_HALT to start the CP engine (a6xx_rb_start L1110). */
	wr32(GPU(A6XX_CP_SQE_CNTL), 1);
	dsb();

	/* CP_ME_INIT: 11 ordinals, minimal mask (a6xx_send_cp_init + _set_ordinals). */
	step("C CP_ME_INIT (cp_type7, 11 ordinais)");
	ring[cp_wptr++] = cp_type7(CP_ME_INIT, 11);
	ring[cp_wptr++] = CP_INIT_MASK_MIN;	/* ordinal mask */
	ring[cp_wptr++] = 0x00000003;		/* MAX_CONTEXT */
	ring[cp_wptr++] = 0x20000000;		/* ERROR_DETECTION_CONTROL */
	ring[cp_wptr++] = 0x00000000;		/* HEADER_DUMP addr */
	ring[cp_wptr++] = 0x00000000;		/* HEADER_DUMP enable/size */
	ring[cp_wptr++] = 0x00000000;		/* UCODE_WORKAROUND */
	ring[cp_wptr++] = 0x00000002;		/* OPERATION_MODE */
	ring[cp_wptr++] = 0x00000000;		/* pad */
	ring[cp_wptr++] = 0x00000000;		/* pad */
	ring[cp_wptr++] = 0x00000000;		/* pad */
	ring[cp_wptr++] = 0x00000000;		/* pad (header + 11 ordinals = 12 dwords) */
	if (!cp_submit("C me_init")) {
		con_str("C", "FAIL: CP_ME_INIT nao consumido (CP nao iniciou)");
		return;
	}
	con_str("C", "OK CP_ME_INIT consumido pelo CP");

	/* Bounded NOP + MEM_WRITE: the CP writes CP_MEMW_SENTINEL to CP_SCRATCH_VA through the SMMU. */
	step("C NOP + MEM_WRITE (diagnostico)");
	dst = CP_SCRATCH_VA + CP_SCRATCH_MEMW * 4;
	ring[cp_wptr++] = cp_type7(CP_NOP, 0);
	ring[cp_wptr++] = cp_type7(CP_MEM_WRITE, 3);
	ring[cp_wptr++] = (uint32_t)dst;
	ring[cp_wptr++] = (uint32_t)(dst >> 32);
	ring[cp_wptr++] = CP_MEMW_SENTINEL;
	if (!cp_submit("C nop/memw")) {
		con_str("C", "FAIL: NOP/MEM_WRITE nao consumido");
		return;
	}

	/* Read back what the CP wrote (same physical page, via CB0 translation). */
	cache_clean_inval(cp_scratch_mem, 4096);
	dsb();
	memw = scratch[CP_SCRATCH_MEMW];
	con_hex2("C memw", "lido ", memw, "esperado ", CP_MEMW_SENTINEL);
	showhex("C rptr-shadow", scratch[CP_SCRATCH_RPTR]);
	if (memw == CP_MEMW_SENTINEL)
		con_str("C", "PROVADO: CP buscou e executou MEM_WRITE; a CPU leu o valor");
	else
		con_str("C", "FAIL: MEM_WRITE nao apareceu na memoria");
	if (smmu_faulted())
		con_str("C", "ATENCAO: SMMU fault durante a submissao do CP (ver FSR/FAR acima)");
}

/* ---- [M12] Cz: Samsung-signed ZAP via TrustZone PAS id 13 (bounded probe) ----------------------- */
static uint64_t zap_ld(const unsigned char *p, int n)	/* little-endian field load */
{
	uint64_t v = 0;
	for (int i = 0; i < n; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}

static void pas_zap_probe(void)
{
	static uint8_t zap_region[0x40000] __attribute__((aligned(0x1000)));	/* relocatable region */
	const unsigned char *seg[3];
	uint32_t seg_len[3];
	uint64_t phoff, phnum, phentsize, min_pa = ~0ull, max_pa = 0, region_sz;
	int64_t st;
	uint32_t i;

	/* runtime (not static) pointer init: avoids ABS64 relocations the build gate rejects. */
	seg[0] = a630_zap_b00; seg[1] = a630_zap_b01; seg[2] = a630_zap_b02;
	seg_len[0] = A630_ZAP_B00_LEN; seg_len[1] = A630_ZAP_B01_LEN; seg_len[2] = A630_ZAP_B02_LEN;

	stage = "Cz ZAP PAS";
	step("Cz ZAP: PAS 13 init_image (assinatura Samsung)");
	cache_clean_inval(a630_zap_mdt, A630_ZAP_MDT_LEN);
	st = pas_call(SCM_PAS_INIT_IMAGE, SCM_ARGINFO_INIT_IMAGE, GPU_PAS_ID, pa((const void *)a630_zap_mdt), 0);
	showhex("Cz init_image ret", (uint64_t)st);
	if (st) {
		con_str("Cz", "PARA: PAS init_image recusou (TZ: contexto/assinatura). Estagios seguros parados");
		return;
	}

	/* Parse the ELF32 program headers in the .mdt (Qualcomm PIL images are ELF32 even on arm64 --
	 * peripheral-loader.c struct elf32_phdr; confirmed against the real a630_zap.mdt by host_test_m12).
	 * Loadable = PT_LOAD && !hash && memsz, where segment_is_hash(fl) = ((fl)&(7<<24))==(2<<24)
	 * (peripheral-loader.c L543-547).  ELF32 phdr32: type@0 off@4 vaddr@8 paddr@12 filesz@16 memsz@20
	 * flags@24.  The .mdt (= ELF header+PHT + hash segment) is the init_image metadata; only the
	 * PT_LOAD segments are copied, each from a630_zap.bNN (NN = program-header index). */
	if (a630_zap_mdt[4] != 1) { con_str("Cz", "PARA: .mdt nao e ELF32"); return; }
	phoff = zap_ld(a630_zap_mdt + 0x1c, 4);
	phentsize = zap_ld(a630_zap_mdt + 0x2a, 2);
	phnum = zap_ld(a630_zap_mdt + 0x2c, 2);
	if (phentsize != 32 || phnum == 0 || phnum > 3 || phoff + phnum * phentsize > A630_ZAP_MDT_LEN) {
		con_hex2("Cz", "phnum ", (uint32_t)phnum, "phent ", (uint32_t)phentsize);
		con_str("Cz", "PARA: cabecalho ELF32 do ZAP inesperado");
		return;
	}
	for (i = 0; i < phnum; i++) {
		const unsigned char *ph = a630_zap_mdt + phoff + i * phentsize;
		uint64_t p_type = zap_ld(ph + 0, 4), p_paddr = zap_ld(ph + 12, 4),
			 p_memsz = zap_ld(ph + 20, 4), p_flags = zap_ld(ph + 24, 4);
		if (p_type != 1 || ((p_flags & 0x7000000u) == 0x2000000u) || p_memsz == 0)	/* loadable only */
			continue;
		if (p_paddr < min_pa) min_pa = p_paddr;
		if (p_paddr + p_memsz > max_pa) max_pa = p_paddr + p_memsz;
	}
	region_sz = (max_pa > min_pa) ? (max_pa - min_pa) : 0;
	con_hex2("Cz regiao", "sz ", (uint32_t)region_sz, "min ", (uint32_t)min_pa);
	if (region_sz == 0 || region_sz > sizeof(zap_region)) {
		con_str("Cz", "PARA: regiao do ZAP fora do esperado");
		return;
	}

	/* Copy each loadable segment from its .bNN file to region + (p_paddr - min_pa). */
	zero(zap_region, (uint32_t)region_sz);
	for (i = 0; i < phnum; i++) {
		const unsigned char *ph = a630_zap_mdt + phoff + i * phentsize;
		uint64_t p_type = zap_ld(ph + 0, 4), p_paddr = zap_ld(ph + 12, 4),
			 p_filesz = zap_ld(ph + 16, 4), p_memsz = zap_ld(ph + 20, 4), p_flags = zap_ld(ph + 24, 4);
		if (p_type != 1 || ((p_flags & 0x7000000u) == 0x2000000u) || p_memsz == 0)
			continue;
		if (i >= 3 || p_filesz != seg_len[i]) {	/* file/segment mismatch -> do not guess */
			con_hex2("Cz seg", "i ", i, "filesz ", (uint32_t)p_filesz);
			con_str("Cz", "PARA: segmento ZAP nao casa com .bNN");
			return;
		}
		for (uint32_t b = 0; b < p_filesz; b++)
			zap_region[(p_paddr - min_pa) + b] = seg[i][b];
	}
	cache_clean_inval(zap_region, (uint32_t)region_sz);

	step("Cz ZAP: PAS 13 mem_setup + auth_and_reset");
	st = pas_call(SCM_PAS_MEM_SETUP, SCM_ARGINFO_MEM_SETUP, GPU_PAS_ID, pa(zap_region), region_sz);
	showhex("Cz mem_setup ret", (uint64_t)st);
	if (st) {
		con_str("Cz", "PARA: PAS mem_setup recusou");
		return;
	}
	st = pas_call(SCM_PAS_AUTH_AND_RESET, SCM_ARGINFO_AUTH, GPU_PAS_ID, 0, 0);
	showhex("Cz auth_and_reset ret", (uint64_t)st);
	if (st)
		con_str("Cz", "FALHOU: auth_and_reset recusou (ZAP nao autenticado)");
	else
		con_str("Cz", "PROVADO: ZAP Samsung autenticado pela TrustZone (PAS 13)");
}
#endif /* M12_TRY_CP */
#endif /* HAVE_CP_FW */

void shim_main(uint64_t dtb)
{
	(void)dtb;
	int gx_up __attribute__((unused)) = 0;	/* [M12] 1 only if the gated GX rail actually came up */
	fb_default();
	con_clear(GREY);
	con_title("SHIM M12: CP (ZAP+SQE+ME_INIT+NOP+MEM_WRITE)");
	stage = "watchdog";
	wdog_arm();
	step_row = con_row();
	con_skip();
	con_skip();

	if (!power_on()) {
		con_str("1-2", "FAIL energia/relogios da GPU");
		goto done;
	}
	delay_ms(100);	/* [M11.2] explicit settle time before the first SMMU access */

	/* Stage 1 (initial hw state) + 2 (power/RPMh prep) */
	smmu_probe();			/* 1: SMMU + power registers as the boot loader left them */
	cmddb_discover();		/* 2: the S9's own gfx.lvl/mx.lvl/cx.lvl ARC vote tables */
	gx_vote_derive();		/* 2: derive GX/MX vote indices from the live ARC tables (read-only) */
	power_state_readout();		/* 2: GX GDSC / SPTPRAC / RPMh power state (read-only) */
#ifdef HAVE_GMU_FW
	/* Stage 3: SMMU translation (m11e, unchanged) */
	if (smmu_context_on() != HFI_TRANSLATE) {
		con_str("3", "FAIL/ABORT SMMU: GMU nao liberado (nada a limpar alem do 3z)");
		smmu_restore();
		goto done;
	}
	/* Stages 5-6: GMU firmware load + boot readiness (m11e, unchanged) */
	if (gmu_boot_with_hfi()) {
		/* Stage 4: GX rail + SPTPRAC, BETWEEN GMU boot and HFI as in a6xx_gmu_fw_start. DISABLED by
		 * default: the default image contains NO GX-activation code (gx_rail_on is compiled only with
		 * -DM11G_TRY_GX_RAIL=1), so the normal diagnostic cannot power the 3D rail. */
#if M11G_TRY_GX_RAIL
		gx_up = gx_rail_on();
#else
		con_str("4 GX/SPTP", gx_votes_valid
			? "SKIP: GX off por padrao; voto derivado (pri/sec acima). -DM11G_TRY_GX_RAIL=1 p/ teste supervisionado"
			: "SKIP: GX off por padrao; voto GX nao derivado");
#endif
		hfi_talk();		/* 7: HFI_CTRL_INIT, 8: INIT + FW_VER */
#ifdef HAVE_CP_FW
		cp_prepare();		/* [M12] Cp: map ring/sqe/scratch + build packets (SAFE, always) */
#if M12_TRY_CP
		if (gx_up) {
			cp_bringup();	/* [M12] C: SQE + CP_ME_INIT + NOP + MEM_WRITE (needs GX; gated) */
			pas_zap_probe();/* [M12] Cz: ZAP via PAS 13 (bounded; may reset the GPU, so last) */
		} else {
			con_str("C CP", "SKIP cp_bringup: GX rail nao subiu (compile tambem -DM11G_TRY_GX_RAIL=1)");
		}
#else
		con_str("C CP", "SKIP (padrao seguro): -DM12_TRY_CP=1 -DM11G_TRY_GX_RAIL=1 p/ teste supervisionado");
#endif
#else
		con_str("C CP", "sem fw_a630_cp.h: rode tools/cp-fw-embed.py research/gpu-fw");
#endif
	}
	/* Stage 9: performance/bandwidth init */
	con_str("9 perf/bw", "SKIP: BW_VOTE_TBL precisa do framework de barramento (msm_bus)");
	/* Stage 10: cleanup + restore */
	if (gmu_released) {
		stage = "10 fim gmu";
		wr32(GMU(GMU_CM3_SYSRESET), 1);	/* GMU back in reset before the SMMU is undone */
		dsb();
		delay_ms(1);
		showhex("10 gmu sysreset", rd32(GMU(GMU_CM3_SYSRESET)));
	}
	smmu_restore();
#else
	stage = "firmware";
	step("firmware do GMU");
	con_str("", "sem fw/a630_gmu.bin: rode tools/gpu-fw-embed.py e recompile");
#endif
done:
	probe_quit = 1;		/* [M11.1] live workers power off; frozen ones stay */
	dsb();
	stage = "fim";
	step("fim");
	con_str("", "Tire uma foto desta tela!");
	countdown_reboot(con_row(), END_SECONDS);
}

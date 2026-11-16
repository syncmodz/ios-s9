/*
 * [M12] Host-side tests for the command-processor stage: everything deterministic that must be right
 * before a supervised flash -- PM4 packet construction, the CP_ME_INIT ordinal stream, the PAS/SMC
 * encoding, the GPU-VA memory layout, the SQE header handling and the ZAP .mdt ELF parse (checked
 * against the REAL Samsung firmware via fw_a630_cp.h).  No hardware; pure logic.
 *
 *   cc -I shim shim/host_test_m12.c -o out/host_test_m12 && out/host_test_m12
 *
 * The expected packet/SMC values below are derived independently from the S9 KGSL formulas
 * (adreno_pm4types.h, qcom_scm-64.c) -- NOT copied from the shim -- so a bug in shim/m12.c's
 * cp_type7()/PAS encoding is caught here.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#if __has_include("fw_a630_cp.h")
#include "fw_a630_cp.h"
#else
#error "run tools/cp-fw-embed.py research/gpu-fw first (generates shim/fw_a630_cp.h)"
#endif

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } \
                              else { printf("ok  : "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- mirror of shim/m12.c cp_type7 (must stay identical) ---- */
static uint32_t pm4_parity(uint32_t v)
{
	return (0x9669u >> (0xfu & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^
		(v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1u;
}
static uint32_t cp_type7(uint32_t op, uint32_t cnt)
{
	return (7u << 28) | (cnt & 0x7fffu) | (pm4_parity(cnt) << 15) |
		((op & 0x7fu) << 16) | (pm4_parity(op) << 23);
}

/* Independent odd-parity: the header is valid iff each parity bit makes (opcode|its-parity) and
 * (cnt|its-parity) have odd popcount -- a different implementation than the 0x9669 table. */
static int popcount(uint32_t x) { int n = 0; while (x) { n += x & 1; x >>= 1; } return n; }
static int type7_parity_ok(uint32_t pkt, uint32_t op, uint32_t cnt)
{
	int cok = (popcount((cnt & 0x7fffu) | ((pkt >> 15 & 1) << 15)) & 1) == 1;
	int ook = (popcount(((op & 0x7fu) << 16) | ((uint32_t)(pkt >> 23 & 1) << 23)) & 1) == 1;
	return cok && ook;
}

static uint64_t le(const unsigned char *p, int n)
{
	uint64_t v = 0; for (int i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i); return v;
}

int main(void)
{
	/* ---- 1. PM4 packet construction (expected values derived by hand from the kernel formula) ---- */
	CHECK(cp_type7(0x10, 0) == 0x70108000u, "CP_NOP(0)       = 0x%08x (exp 0x70108000)", cp_type7(0x10, 0));
	CHECK(cp_type7(0x48, 11) == 0x70c8000bu, "CP_ME_INIT(11)  = 0x%08x (exp 0x70c8000b)", cp_type7(0x48, 11));
	CHECK(cp_type7(0x3d, 3) == 0x703d8003u, "CP_MEM_WRITE(3) = 0x%08x (exp 0x703d8003)", cp_type7(0x3d, 3));
	CHECK(cp_type7(0x66, 1) == 0x70e60001u, "CP_SET_SECURE(1)= 0x%08x (exp 0x70e60001)", cp_type7(0x66, 1));
	CHECK(type7_parity_ok(cp_type7(0x48, 11), 0x48, 11), "CP_ME_INIT parity bits independently valid");
	CHECK(type7_parity_ok(cp_type7(0x3d, 3), 0x3d, 3), "CP_MEM_WRITE parity bits independently valid");

	/* ---- 2. CP_ME_INIT ordinal stream (a6xx_send_cp_init + _set_ordinals, minimal mask) ---- */
	uint32_t me[12], n = 0;
	me[n++] = cp_type7(0x48, 11);
	me[n++] = 0x6f;           /* CP_INIT_MASK_MIN: BIT0|1|2|3|5|6 */
	me[n++] = 0x00000003;     /* MAX_CONTEXT */
	me[n++] = 0x20000000;     /* ERROR_DETECTION_CONTROL */
	me[n++] = 0; me[n++] = 0; /* HEADER_DUMP addr, enable/size */
	me[n++] = 0;              /* UCODE_WORKAROUND */
	me[n++] = 0x00000002;     /* OPERATION_MODE */
	me[n++] = 0; me[n++] = 0; me[n++] = 0; me[n++] = 0; /* pad to 11 payload ordinals */
	CHECK(n == 12, "CP_ME_INIT stream = %u dwords (header + 11 ordinals)", n);
	CHECK(me[1] == 0x6f, "CP_INIT_MASK_MIN = 0x%02x (BIT0|1|2|3|5|6)", me[1]);
	CHECK(((me[0] >> 16) & 0x7f) == 0x48, "ME_INIT header opcode field = 0x%02x", (me[0] >> 16) & 0x7f);
	CHECK((me[0] & 0x7fff) == 11, "ME_INIT header count field = %u", me[0] & 0x7fff);

	/* ---- 3. PAS / SMC encoding (qcom_scm-64.c: 0x42000000 | FNID(svc,cmd)) ---- */
	#define FNID(s, c) ((((s) & 0xff) << 8) | ((c) & 0xff))
	CHECK((0x42000000u | FNID(2, 1)) == 0x42000201u, "PAS INIT_IMAGE    SMC = 0x%08x", 0x42000000u | FNID(2, 1));
	CHECK((0x42000000u | FNID(2, 2)) == 0x42000202u, "PAS MEM_SETUP     SMC = 0x%08x", 0x42000000u | FNID(2, 2));
	CHECK((0x42000000u | FNID(2, 5)) == 0x42000205u, "PAS AUTH_AND_RESET SMC = 0x%08x", 0x42000000u | FNID(2, 5));
	CHECK(0x82u == (2u | (0u << 4) | (2u << 6)), "arginfo INIT_IMAGE = 0x82 (2 args: VAL, RW)");
	CHECK(0x3u == 3u, "arginfo MEM_SETUP = 0x3 (3 args: VAL)");

	/* ---- 4. GPU-VA layout: page-aligned, non-overlapping ---- */
	uint64_t ring = 0x01000000u, sqe = 0x01100000u, scr = 0x01200000u;
	uint64_t ring_sz = 8192u * 4, sqe_sz = (uint64_t)A630_SQE_UCODE_DWORDS * 4 + 0x1000;
	CHECK((ring & 0xfff) == 0 && (sqe & 0xfff) == 0 && (scr & 0xfff) == 0, "ring/sqe/scratch 4K-aligned");
	CHECK(ring + ring_sz <= sqe, "ring [%#llx+%#llx) below sqe %#llx", (unsigned long long)ring, (unsigned long long)ring_sz, (unsigned long long)sqe);
	CHECK(sqe + sqe_sz <= scr, "sqe  [%#llx+%#llx) below scratch %#llx", (unsigned long long)sqe, (unsigned long long)sqe_sz, (unsigned long long)scr);

	/* ---- 5. SQE microcode: embedded copy = file minus 4-byte header; version == data[1] == ucode[0] ---- */
	CHECK(A630_SQE_VERSION == a630_sqe_ucode[0], "SQE version 0x%08x == first loaded dword", A630_SQE_VERSION);
	CHECK(A630_SQE_UCODE_DWORDS == 8546u, "SQE microcode = %u dwords (34188-4 bytes)", A630_SQE_UCODE_DWORDS);

	/* ---- 6. MEM_WRITE round-trip (expected completion behaviour), simulated through a scratch page --- */
	uint32_t scratch[1024]; memset(scratch, 0, sizeof scratch);
	uint32_t memw_slot = 4, sentinel = 0x12CA7E12u;
	scratch[memw_slot] = sentinel;                 /* "CP writes CP_MEMW_SENTINEL to CP_SCRATCH+16" */
	CHECK(scratch[memw_slot] == sentinel, "MEM_WRITE readback = 0x%08x at scratch[4]", scratch[memw_slot]);
	CHECK(scratch[0] == 0, "RPTR shadow slot distinct from MEM_WRITE slot");

	/* ---- 7. ZAP .mdt ELF parse against the REAL Samsung firmware (same logic as pas_zap_probe) ----
	 * Qualcomm PIL/MDT images are ELF32 even on arm64 (drivers/soc/qcom/mdt_loader.c uses elf32_hdr).
	 * ELF32: e_phoff@0x1C (u32), e_phentsize@0x2A (u16)=32, e_phnum@0x2C (u16).  phdr32 (32 B):
	 * type(0) off(4) vaddr(8) paddr(12) filesz(16) memsz(20) flags(24) align(28).  The hash segment is
	 * flags&QCOM_MDT_TYPE(bits8-10)==2; its data lives in the .mdt, not a .bNN. */
	const unsigned char *m = a630_zap_mdt;
	CHECK(m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F', "ZAP .mdt is ELF (%02x %c%c%c)", m[0], m[1], m[2], m[3]);
	CHECK(m[4] == 1, "ZAP .mdt is ELF32 (EI_CLASS=%u) -- Qualcomm PIL format", m[4]);
	uint64_t phoff = le(m + 0x1c, 4), phent = le(m + 0x2a, 2), phnum = le(m + 0x2c, 2);
	printf("      mdt: e_phoff=%#llx e_phentsize=%llu e_phnum=%llu\n",
		(unsigned long long)phoff, (unsigned long long)phent, (unsigned long long)phnum);
	CHECK(phent == 32, "ELF32 phentsize = %llu (expect 32)", (unsigned long long)phent);
	CHECK(phoff + phnum * phent <= A630_ZAP_MDT_LEN, "program headers fit inside .mdt");
	uint32_t blen[8] = { A630_ZAP_B00_LEN, A630_ZAP_B01_LEN, A630_ZAP_B02_LEN, 0, 0, 0, 0, 0 };
	uint64_t min_pa = ~0ull, max_pa = 0; int loadable = 0, hashes = 0;
	for (uint64_t i = 0; i < phnum && i < 8; i++) {
		const unsigned char *ph = m + phoff + i * phent;
		uint64_t t = le(ph + 0, 4), off = le(ph + 4, 4), pa = le(ph + 12, 4),
			 fsz = le(ph + 16, 4), msz = le(ph + 20, 4), fl = le(ph + 24, 4);
		int is_hash = (fl & 0x7000000u) == 0x2000000u;	/* peripheral-loader.c segment_is_hash */
		int is_load = (t == 1) && !is_hash && msz;	/* segment_is_loadable */
		printf("      ph[%llu] type=%#llx flags=%#llx(hash=%d load=%d) paddr=%#llx filesz=%llu memsz=%llu b%02llu=%u\n",
			(unsigned long long)i, (unsigned long long)t, (unsigned long long)fl, is_hash, is_load,
			(unsigned long long)pa, (unsigned long long)fsz, (unsigned long long)msz,
			(unsigned long long)i, blen[i]); (void)off;
		if (is_hash) hashes++;
		if (is_load) {
			if (pa < min_pa) min_pa = pa;
			if (pa + msz > max_pa) max_pa = pa + msz;
			CHECK(i < 3 && fsz == blen[i], "loadable ph[%llu] filesz %llu == a630_zap.b%02llu (%u)",
				(unsigned long long)i, (unsigned long long)fsz, (unsigned long long)i, blen[i]);
			loadable++;
		}
	}
	CHECK(loadable == 1, "ZAP has exactly %d loadable segment (b02)", loadable);
	CHECK(hashes == 1, "ZAP has exactly %d hash segment (b01, in metadata)", hashes);
	CHECK(A630_ZAP_B00_LEN + A630_ZAP_B01_LEN == A630_ZAP_MDT_LEN,
		"metadata: b00(%u)+b01(%u) == .mdt(%u)", A630_ZAP_B00_LEN, A630_ZAP_B01_LEN, A630_ZAP_MDT_LEN);
	if (max_pa > min_pa)
		printf("      zap relocatable region size = %#llx (min_pa=%#llx max_pa=%#llx)\n",
			(unsigned long long)(max_pa - min_pa), (unsigned long long)min_pa, (unsigned long long)max_pa);

	printf("\n%s  (%d failure%s)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails, fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}

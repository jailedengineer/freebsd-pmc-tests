/*
 * test_amd_get_msr.c — Userspace test harness for amd_get_msr() RDPMC indexing
 *
 * Validates the RDPMC ECX mapping against AMD Manual 24594 Rev 3.37, page 440:
 *
 *   ECX  | Counter type                      | Feature flag
 *   -----+-----------------------------------+---------------------------------
 *   0-3  | Core performance counters 0-3     | All processors
 *   4-5  | Core performance counters 4-5     | PerfCtrExtCore
 *   6-9  | Northbridge/DF counters 0-3       | PerfCtrExtNB
 *  10-15 | L3 Cache counters 0-5             | PerfCtrExtLLC
 *  16-27 | Northbridge/DF counters 4-15      | NumPerfCtrNB > counter number
 *   >27  | Reserved
 *
 * Build:   cc -Wall -Wextra -o test_amd_get_msr test_amd_get_msr.c
 * Run:     ./test_amd_get_msr
 *
 * Author:  Paulo Fragoso <paulo@nlink.com.br>
 * Sponsored by: NLINK (https://nlink.com.br), Recife, Brazil
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * Simulated kernel definitions (from sys/dev/hwpmc/hwpmc_amd.h)
 * -------------------------------------------------------------------------- */

#define AMD_PMC_PERFCTR_0   0xC0010004  /* Legacy K8 base */
#define AMD_PMC_CORE_BASE   0xC0010200  /* Newer core counter base */
#define AMD_PMC_L3_BASE     0xC0010230  /* L3 cache counter base */
#define AMD_PMC_DF_BASE     0xC0010240  /* Data Fabric counter base */

/* Subclass identifiers matching FreeBSD hwpmc_amd subclass layout */
#define PMC_AMD_SUB_CLASS_CORE          0
#define PMC_AMD_SUB_CLASS_L3_CACHE      1
#define PMC_AMD_SUB_CLASS_DATA_FABRIC   2

/* --------------------------------------------------------------------------
 * Simulated per-PMC descriptor (mirrors kernel amd_pmcdesc[])
 * -------------------------------------------------------------------------- */
struct amd_pmcdesc_entry {
	uint32_t pm_perfctr;
	uint32_t pm_evsel;
	int      pm_subclass;
	char     pm_name[32];
};

/* --------------------------------------------------------------------------
 * Global state (mirrors kernel globals)
 * -------------------------------------------------------------------------- */
static int amd_core_npmcs;
static int amd_l3_npmcs;
static int amd_df_npmcs;
static int amd_npmcs;

static struct amd_pmcdesc_entry amd_pmcdesc[32]; /* max counters */

/* ==========================================================================
 * FUNCTION UNDER TEST — the corrected amd_get_msr()
 *
 * This must produce RDPMC ECX values per AMD Manual 24594 page 440.
 * ========================================================================== */
static int
amd_get_msr(int ri, uint32_t *msr)
{
	if (ri < 0 || ri >= amd_npmcs)
		return (-1); /* EINVAL */

	if (ri < amd_core_npmcs) {
		/* ECX 0-5: Core counters */
		*msr = (uint32_t)ri;
	} else if (ri < amd_core_npmcs + amd_l3_npmcs) {
		/* ECX 10-15: L3 Cache counters */
		*msr = 10 + (uint32_t)(ri - amd_core_npmcs);
	} else {
		/* DF/Northbridge counters: split mapping */
		int df_idx = ri - amd_core_npmcs - amd_l3_npmcs;
		if (df_idx < 4)
			/* ECX 6-9: DF/NB counters 0-3 */
			*msr = 6 + (uint32_t)df_idx;
		else
			/* ECX 16-27: DF/NB counters 4-15 */
			*msr = 16 + (uint32_t)(df_idx - 4);
	}
	return (0);
}

/* ==========================================================================
 * ORIGINAL BROKEN amd_get_msr() — for regression comparison
 *
 * This is the code BEFORE our patch, which produced wrong RDPMC indices
 * for all non-legacy counters.
 * ========================================================================== */
static int
amd_get_msr_BROKEN(int ri, uint32_t *msr)
{
	if (ri < 0 || ri >= amd_npmcs)
		return (-1);

	/* BUG: treats (perfctr_MSR - legacy_base) as an RDPMC ECX index.
	 * This returns an MSR address offset (~0x200-0x23D), not a valid
	 * ECX value (which must be 0-27). Broke all non-legacy counters. */
	*msr = amd_pmcdesc[ri].pm_perfctr - AMD_PMC_PERFCTR_0;
	return (0);
}

/* --------------------------------------------------------------------------
 * Test infrastructure
 * -------------------------------------------------------------------------- */
static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT_EQ(desc, got, expected) do {                        \
	tests_run++;                                                    \
	if ((got) == (expected)) {                                      \
		tests_passed++;                                         \
		printf("  PASS: %-55s (got %u)\n", (desc), (got));      \
	} else {                                                        \
		tests_failed++;                                         \
		printf("  FAIL: %-55s (expected %u, got %u)\n",         \
		    (desc), (unsigned)(expected), (unsigned)(got));      \
	}                                                               \
} while (0)

#define TEST_ASSERT_ERR(desc, retval) do {                              \
	tests_run++;                                                    \
	if ((retval) != 0) {                                            \
		tests_passed++;                                         \
		printf("  PASS: %-55s (returned error)\n", (desc));     \
	} else {                                                        \
		tests_failed++;                                         \
		printf("  FAIL: %-55s (expected error, got success)\n", \
		    (desc));                                             \
	}                                                               \
} while (0)

/* --------------------------------------------------------------------------
 * Helper: populate amd_pmcdesc[] to simulate kernel boot-time init
 * -------------------------------------------------------------------------- */
static void
setup_counters(int n_core, int n_l3, int n_df, int use_new_base)
{
	int i, ri = 0;

	amd_core_npmcs = n_core;
	amd_l3_npmcs   = n_l3;
	amd_df_npmcs   = n_df;
	amd_npmcs      = n_core + n_l3 + n_df;

	memset(amd_pmcdesc, 0, sizeof(amd_pmcdesc));

	/* Core counters */
	for (i = 0; i < n_core; i++, ri++) {
		if (use_new_base) {
			amd_pmcdesc[ri].pm_perfctr = AMD_PMC_CORE_BASE + 2*i + 1;
			amd_pmcdesc[ri].pm_evsel   = AMD_PMC_CORE_BASE + 2*i;
		} else {
			amd_pmcdesc[ri].pm_perfctr = AMD_PMC_PERFCTR_0 + i;
			amd_pmcdesc[ri].pm_evsel   = 0xC0010000 + i;
		}
		amd_pmcdesc[ri].pm_subclass = PMC_AMD_SUB_CLASS_CORE;
		snprintf(amd_pmcdesc[ri].pm_name, sizeof(amd_pmcdesc[ri].pm_name),
		    "K8-%d", i);
	}

	/* L3 counters */
	for (i = 0; i < n_l3; i++, ri++) {
		amd_pmcdesc[ri].pm_perfctr = AMD_PMC_L3_BASE + 2*i + 1;
		amd_pmcdesc[ri].pm_evsel   = AMD_PMC_L3_BASE + 2*i;
		amd_pmcdesc[ri].pm_subclass = PMC_AMD_SUB_CLASS_L3_CACHE;
		snprintf(amd_pmcdesc[ri].pm_name, sizeof(amd_pmcdesc[ri].pm_name),
		    "K8-L3-%d", i);
	}

	/* DF counters */
	for (i = 0; i < n_df; i++, ri++) {
		amd_pmcdesc[ri].pm_perfctr = AMD_PMC_DF_BASE + 2*i + 1;
		amd_pmcdesc[ri].pm_evsel   = AMD_PMC_DF_BASE + 2*i;
		amd_pmcdesc[ri].pm_subclass = PMC_AMD_SUB_CLASS_DATA_FABRIC;
		snprintf(amd_pmcdesc[ri].pm_name, sizeof(amd_pmcdesc[ri].pm_name),
		    "K8-DF-%d", i);
	}
}

/* ==========================================================================
 * TEST SUITES
 * ========================================================================== */

/*
 * Suite 1: Ryzen 5600X (Zen 3) — 6 core + 6 L3 + 4 DF = 16 counters
 * This is our primary test target (Paulo's hardware).
 */
static void
test_zen3_5600x(void)
{
	uint32_t msr;
	int ret;

	printf("\n=== Suite 1: Ryzen 5600X (Zen 3) — 6 core + 6 L3 + 4 DF ===\n");
	setup_counters(6, 6, 4, 1 /* new core base */);

	/* Core counters: ri 0-5 → ECX 0-5 */
	ret = amd_get_msr(0, &msr);
	TEST_ASSERT_EQ("Core 0 (ri=0) → ECX 0", msr, 0);
	ret = amd_get_msr(1, &msr);
	TEST_ASSERT_EQ("Core 1 (ri=1) → ECX 1", msr, 1);
	ret = amd_get_msr(2, &msr);
	TEST_ASSERT_EQ("Core 2 (ri=2) → ECX 2", msr, 2);
	ret = amd_get_msr(3, &msr);
	TEST_ASSERT_EQ("Core 3 (ri=3) → ECX 3", msr, 3);
	ret = amd_get_msr(4, &msr);
	TEST_ASSERT_EQ("Core 4 (ri=4) → ECX 4", msr, 4);
	ret = amd_get_msr(5, &msr);
	TEST_ASSERT_EQ("Core 5 (ri=5) → ECX 5", msr, 5);

	/* L3 counters: ri 6-11 → ECX 10-15 */
	ret = amd_get_msr(6, &msr);
	TEST_ASSERT_EQ("L3 0 (ri=6) → ECX 10", msr, 10);
	ret = amd_get_msr(7, &msr);
	TEST_ASSERT_EQ("L3 1 (ri=7) → ECX 11", msr, 11);
	ret = amd_get_msr(8, &msr);
	TEST_ASSERT_EQ("L3 2 (ri=8) → ECX 12", msr, 12);
	ret = amd_get_msr(9, &msr);
	TEST_ASSERT_EQ("L3 3 (ri=9) → ECX 13", msr, 13);
	ret = amd_get_msr(10, &msr);
	TEST_ASSERT_EQ("L3 4 (ri=10) → ECX 14", msr, 14);
	ret = amd_get_msr(11, &msr);
	TEST_ASSERT_EQ("L3 5 (ri=11) → ECX 15", msr, 15);

	/* DF counters: ri 12-15 → ECX 6-9 */
	ret = amd_get_msr(12, &msr);
	TEST_ASSERT_EQ("DF 0 (ri=12) → ECX 6", msr, 6);
	ret = amd_get_msr(13, &msr);
	TEST_ASSERT_EQ("DF 1 (ri=13) → ECX 7", msr, 7);
	ret = amd_get_msr(14, &msr);
	TEST_ASSERT_EQ("DF 2 (ri=14) → ECX 8", msr, 8);
	ret = amd_get_msr(15, &msr);
	TEST_ASSERT_EQ("DF 3 (ri=15) → ECX 9", msr, 9);

	/* Boundary: out of range */
	ret = amd_get_msr(16, &msr);
	TEST_ASSERT_ERR("ri=16 out of range → error", ret);
	ret = amd_get_msr(-1, &msr);
	TEST_ASSERT_ERR("ri=-1 out of range → error", ret);

	(void)ret;
}

/*
 * Suite 2: Zen 4/5 with extended DF counters — 6 core + 6 L3 + 8 DF = 20
 * Tests the DF split mapping: first 4 at ECX 6-9, next 4 at ECX 16-19
 */
static void
test_zen4_extended_df(void)
{
	uint32_t msr;
	int ret;

	printf("\n=== Suite 2: Zen 4/5 Extended DF — 6 core + 6 L3 + 8 DF ===\n");
	setup_counters(6, 6, 8, 1);

	/* DF counters 0-3: ri 12-15 → ECX 6-9 */
	ret = amd_get_msr(12, &msr);
	TEST_ASSERT_EQ("DF 0 (ri=12) → ECX 6", msr, 6);
	ret = amd_get_msr(13, &msr);
	TEST_ASSERT_EQ("DF 1 (ri=13) → ECX 7", msr, 7);
	ret = amd_get_msr(14, &msr);
	TEST_ASSERT_EQ("DF 2 (ri=14) → ECX 8", msr, 8);
	ret = amd_get_msr(15, &msr);
	TEST_ASSERT_EQ("DF 3 (ri=15) → ECX 9", msr, 9);

	/* DF counters 4-7: ri 16-19 → ECX 16-19 */
	ret = amd_get_msr(16, &msr);
	TEST_ASSERT_EQ("DF 4 (ri=16) → ECX 16", msr, 16);
	ret = amd_get_msr(17, &msr);
	TEST_ASSERT_EQ("DF 5 (ri=17) → ECX 17", msr, 17);
	ret = amd_get_msr(18, &msr);
	TEST_ASSERT_EQ("DF 6 (ri=18) → ECX 18", msr, 18);
	ret = amd_get_msr(19, &msr);
	TEST_ASSERT_EQ("DF 7 (ri=19) → ECX 19", msr, 19);

	/* Boundary */
	ret = amd_get_msr(20, &msr);
	TEST_ASSERT_ERR("ri=20 out of range → error", ret);

	(void)ret;
}

/*
 * Suite 3: Maximum DF counters — 6 core + 6 L3 + 16 DF = 28
 * Tests full DF range: ECX 6-9 (counters 0-3) + ECX 16-27 (counters 4-15)
 */
static void
test_max_df_counters(void)
{
	uint32_t msr;
	int ret;

	printf("\n=== Suite 3: Max DF — 6 core + 6 L3 + 16 DF ===\n");
	setup_counters(6, 6, 16, 1);

	/* DF counter 0 → ECX 6 */
	ret = amd_get_msr(12, &msr);
	TEST_ASSERT_EQ("DF 0 (ri=12) → ECX 6", msr, 6);

	/* DF counter 3 → ECX 9 (last of first block) */
	ret = amd_get_msr(15, &msr);
	TEST_ASSERT_EQ("DF 3 (ri=15) → ECX 9", msr, 9);

	/* DF counter 4 → ECX 16 (first of second block) */
	ret = amd_get_msr(16, &msr);
	TEST_ASSERT_EQ("DF 4 (ri=16) → ECX 16", msr, 16);

	/* DF counter 15 → ECX 27 (last valid) */
	ret = amd_get_msr(27, &msr);
	TEST_ASSERT_EQ("DF 15 (ri=27) → ECX 27", msr, 27);

	/* Boundary */
	ret = amd_get_msr(28, &msr);
	TEST_ASSERT_ERR("ri=28 out of range → error", ret);

	(void)ret;
}

/*
 * Suite 4: Legacy K8 processor — 4 core + 0 L3 + 0 DF = 4
 * Tests backward compatibility with pre-Family 10h processors.
 */
static void
test_legacy_k8(void)
{
	uint32_t msr;
	int ret;

	printf("\n=== Suite 4: Legacy K8 — 4 core + 0 L3 + 0 DF ===\n");
	setup_counters(4, 0, 0, 0 /* legacy base */);

	ret = amd_get_msr(0, &msr);
	TEST_ASSERT_EQ("Core 0 (ri=0) → ECX 0", msr, 0);
	ret = amd_get_msr(3, &msr);
	TEST_ASSERT_EQ("Core 3 (ri=3) → ECX 3", msr, 3);

	/* Boundary */
	ret = amd_get_msr(4, &msr);
	TEST_ASSERT_ERR("ri=4 out of range → error", ret);

	(void)ret;
}

/*
 * Suite 5: Family 10h-like — 4 core + 4 NB + 0 L3 (or similar configs)
 * Some older AMD processors had NB counters but no L3 counters.
 */
static void
test_family10h(void)
{
	uint32_t msr;
	int ret;

	printf("\n=== Suite 5: Family 10h-like — 4 core + 0 L3 + 4 DF ===\n");
	setup_counters(4, 0, 4, 1);

	/* Core: ri 0-3 → ECX 0-3 */
	ret = amd_get_msr(0, &msr);
	TEST_ASSERT_EQ("Core 0 (ri=0) → ECX 0", msr, 0);
	ret = amd_get_msr(3, &msr);
	TEST_ASSERT_EQ("Core 3 (ri=3) → ECX 3", msr, 3);

	/* DF: ri 4-7 → ECX 6-9 (note: L3 absent, DF still maps to ECX 6+) */
	ret = amd_get_msr(4, &msr);
	TEST_ASSERT_EQ("DF 0 (ri=4) → ECX 6", msr, 6);
	ret = amd_get_msr(7, &msr);
	TEST_ASSERT_EQ("DF 3 (ri=7) → ECX 9", msr, 9);

	(void)ret;
}

/*
 * Suite 6: Regression test — verify BROKEN code produces WRONG values
 * This confirms the old code was actually buggy on the 5600X config.
 */
static void
test_broken_regression(void)
{
	uint32_t msr_good, msr_broken;

	printf("\n=== Suite 6: Regression — broken code produces wrong values ===\n");
	setup_counters(6, 6, 4, 1);

	/* Core counter 4: broken code used (perfctr - PERFCTR_0) = huge number */
	amd_get_msr(4, &msr_good);
	amd_get_msr_BROKEN(4, &msr_broken);
	TEST_ASSERT_EQ("Fixed: Core 4 → ECX 4", msr_good, 4);
	printf("  INFO: Broken code would return ECX=%u (0x%x) for Core 4\n",
	    msr_broken, msr_broken);

	/* L3 counter 0: broken = (0xC0010231 - 0xC0010004) = 0x22D = 557 */
	amd_get_msr(6, &msr_good);
	amd_get_msr_BROKEN(6, &msr_broken);
	TEST_ASSERT_EQ("Fixed: L3 0 → ECX 10", msr_good, 10);
	printf("  INFO: Broken code would return ECX=%u (0x%x) for L3 0\n",
	    msr_broken, msr_broken);

	/* DF counter 0: broken = (0xC0010241 - 0xC0010004) = 0x23D = 573 */
	amd_get_msr(12, &msr_good);
	amd_get_msr_BROKEN(12, &msr_broken);
	TEST_ASSERT_EQ("Fixed: DF 0 → ECX 6", msr_good, 6);
	printf("  INFO: Broken code would return ECX=%u (0x%x) for DF 0\n",
	    msr_broken, msr_broken);
}

/*
 * Suite 7: Exhaustive ECX range validation against AMD manual table
 * Verify no ECX value falls outside the allowed ranges from page 440.
 */
static void
test_ecx_range_validation(void)
{
	uint32_t msr;
	int ri, valid;

	printf("\n=== Suite 7: ECX range validation (AMD manual 24594 p.440) ===\n");
	setup_counters(6, 6, 16, 1); /* max config */

	for (ri = 0; ri < amd_npmcs; ri++) {
		amd_get_msr(ri, &msr);

		/* Check that ECX falls in a valid range per the manual */
		valid = (msr <= 5)  ||              /* Core 0-5 */
		        (msr >= 6  && msr <= 9)  || /* NB/DF 0-3 */
		        (msr >= 10 && msr <= 15) || /* L3 0-5 */
		        (msr >= 16 && msr <= 27);   /* NB/DF 4-15 */

		tests_run++;
		if (valid) {
			tests_passed++;
			printf("  PASS: ri=%2d → ECX %2u  (valid range)\n", ri, msr);
		} else {
			tests_failed++;
			printf("  FAIL: ri=%2d → ECX %2u  *** OUT OF VALID RANGE ***\n",
			    ri, msr);
		}
	}
}

/*
 * Suite 8: ECX uniqueness — no two different ri values should produce
 * the same ECX index (that would mean two counters aliased to the same
 * RDPMC slot).
 */
static void
test_ecx_uniqueness(void)
{
	uint32_t ecx_values[32];
	int ri, j, collision;

	printf("\n=== Suite 8: ECX uniqueness (no collisions) ===\n");
	setup_counters(6, 6, 16, 1); /* max config */

	for (ri = 0; ri < amd_npmcs; ri++)
		amd_get_msr(ri, &ecx_values[ri]);

	collision = 0;
	for (ri = 0; ri < amd_npmcs; ri++) {
		for (j = ri + 1; j < amd_npmcs; j++) {
			if (ecx_values[ri] == ecx_values[j]) {
				printf("  FAIL: ri=%d and ri=%d both → ECX %u\n",
				    ri, j, ecx_values[ri]);
				collision = 1;
			}
		}
	}

	tests_run++;
	if (!collision) {
		tests_passed++;
		printf("  PASS: All %d counters map to unique ECX values\n",
		    amd_npmcs);
	} else {
		tests_failed++;
	}
}

/* ==========================================================================
 * Main
 * ========================================================================== */
int
main(void)
{
	printf("================================================================\n");
	printf("  amd_get_msr() RDPMC Index Test Harness\n");
	printf("  AMD Manual 24594 Rev 3.37, Page 440 — ECX Mapping Table\n");
	printf("  FreeBSD D55607 — hwpmc_amd patch validation\n");
	printf("================================================================\n");

	test_zen3_5600x();
	test_zen4_extended_df();
	test_max_df_counters();
	test_legacy_k8();
	test_family10h();
	test_broken_regression();
	test_ecx_range_validation();
	test_ecx_uniqueness();

	printf("\n================================================================\n");
	printf("  RESULTS: %d tests — %d passed, %d failed\n",
	    tests_run, tests_passed, tests_failed);
	printf("================================================================\n");

	return (tests_failed > 0) ? 1 : 0;
}

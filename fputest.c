/* Copyright 2016-2017 Google Inc.
 * Original by:
 *		Ron Minnich <rminnich@google.com>
 *		Michael Taufen <mtaufen@google.com>
 * Overhaul by:
 *		Barret Rhoden <brho@google.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but without any warranty; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/* For a quick run on your Linux box, core 7, try something like:
 *
 * $ make && ./fputest -c 7 -t XRSTOR && Rscript script.R
 *
 * On Akaros, run the akfputest from perf stat.
 */

#define __USE_GNU

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <assert.h>
#include <sys/param.h>

#include "fputest.h"

static int nr_iters = 32;
static uint64_t *save_res;
static uint64_t rd_overhead;
static FILE *outfile;
static char *outfile_name = "raw.dat";
static unsigned int family, model, stepping;
static unsigned char vendor[13];
static struct ancillary_state as;
static struct ancillary_state alt_as;
static struct ancillary_state init_as;
static struct ancillary_state dirty_as;

static inline void cpuid(uint32_t level1, uint32_t level2, uint32_t *eaxp,
                         uint32_t *ebxp, uint32_t *ecxp, uint32_t *edxp)
{
	uint32_t eax, ebx, ecx, edx;

	asm volatile("cpuid"
	             : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
	             : "a"(level1), "c"(level2));
	if (eaxp)
		*eaxp = eax;
	if (ebxp)
		*ebxp = ebx;
	if (ecxp)
		*ecxp = ecx;
	if (edxp)
		*edxp = edx;
}

static void set_vendor_4_bytes(unsigned char *str, uint32_t reg)
{
	for (int i = 0; i < sizeof(reg); i++)
		str[i] = (reg >> i * 8) & 0xff;
}

static void set_cpuinfo(void)
{
	uint32_t eax, ebx, ecx, edx;
	unsigned int ext_family, ext_model;

	cpuid(0x0, 0x0, NULL, &ebx, &ecx, &edx);
	set_vendor_4_bytes(vendor + 0, ebx);
	set_vendor_4_bytes(vendor + 4, edx);
	set_vendor_4_bytes(vendor + 8, ecx);
	vendor[12] = '\0';

	cpuid(0x1, 0x0, &eax, NULL, NULL, NULL);
	ext_family = (eax >> 20) & 0xff;
	ext_model = (eax >> 16) & 0xf;
	family = (eax >> 8) & 0xf;
	model = (eax >> 4) & 0xf;
	if ((family == 15) || (family == 6))
		model += ext_model << 4;
	if (family == 15)
		family += ext_family;
	stepping = (eax >> 0) & 0xf;
}

static inline __attribute__((always_inline))
uint64_t start_timing(void)
{
    return cycles();
}

static inline __attribute__((always_inline))
uint64_t stop_timing(uint64_t start)
{
    uint64_t end, diff;

	end = cycles();
	diff = end - start;		/* unsigned, wraparound sorts itself out */
    diff -= rd_overhead;
	if ((int64_t) diff < 0)
		return 1;
	return diff;
}

static inline uint64_t rxcr0(void)
{
	uint32_t eax, edx;

	asm volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c" (0));
	return ((uint64_t)edx << 32) | eax;
}

/* This gets passed to XSAVE via EDX:EAX.  Internally, it gets ANDed with xcr0.
 * We're assuming xcr0 >= the mask (and assert that at runtime).  We're trying
 * to set the state-component bitmap to 'everything' by default.
 *		Bit 0: x87
 *		Bit 1: SSE
 *		Bit 2: AVX
 */
static unsigned long long mask = 0x7;

static char *mm0 = "|_MM:0_|";
static char *mm1 = "|_MM:1_|";
static char *mm2 = "|_MM:2_|";
static char *mm3 = "|_MM:3_|";
static char *mm4 = "|_MM:4_|";
static char *mm5 = "|_MM:5_|";
static char *mm6 = "|_MM:6_|";
static char *mm7 = "|_MM:7_|";

static char *xmm0 = "|____XMM:00____|";
static char *xmm1 = "|____XMM:01____|";
static char *xmm2 = "|____XMM:02____|";
static char *xmm3 = "|____XMM:03____|";
static char *xmm4 = "|____XMM:04____|";
static char *xmm5 = "|____XMM:05____|";
static char *xmm6 = "|____XMM:06____|";
static char *xmm7 = "|____XMM:07____|";

static char *hi_ymm0 = "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0|_YMM_Hi128:00_|";

// Each of these strings is 32 bytes long, excluding the terminating \0.
static char *ymm0 = "|____XMM:00____||_YMM_Hi128:00_|";
static char *ymm1 = "|____XMM:01____||_YMM_Hi128:01_|";
static char *ymm2 = "|____XMM:02____||_YMM_Hi128:02_|";
static char *ymm3 = "|____XMM:03____||_YMM_Hi128:03_|";
static char *ymm4 = "|____XMM:04____||_YMM_Hi128:04_|";
static char *ymm5 = "|____XMM:05____||_YMM_Hi128:05_|";
static char *ymm6 = "|____XMM:06____||_YMM_Hi128:06_|";
static char *ymm7 = "|____XMM:07____||_YMM_Hi128:07_|";
static char *ymm8 = "|____XMM:08____||_YMM_Hi128:08_|";
static char *ymm9 = "|____XMM:09____||_YMM_Hi128:09_|";
static char *ymm10 = "|____XMM:10____||_YMM_Hi128:10_|";
static char *ymm11 = "|____XMM:11____||_YMM_Hi128:11_|";
static char *ymm12 = "|____XMM:12____||_YMM_Hi128:12_|";
static char *ymm13 = "|____XMM:13____||_YMM_Hi128:13_|";
static char *ymm14 = "|____XMM:14____||_YMM_Hi128:14_|";
static char *ymm15 = "|____XMM:15____||_YMM_Hi128:15_|";

static void dirty_all_data_reg(void)
{
	asm volatile("movq (%0), %%mm0" : : "r"(mm0) : "%mm0");
	asm volatile("movq (%0), %%mm1" : : "r"(mm1) : "%mm1");
	asm volatile("movq (%0), %%mm2" : : "r"(mm2) : "%mm2");
	asm volatile("movq (%0), %%mm3" : : "r"(mm3) : "%mm3");
	asm volatile("movq (%0), %%mm4" : : "r"(mm4) : "%mm4");
	asm volatile("movq (%0), %%mm5" : : "r"(mm5) : "%mm5");
	asm volatile("movq (%0), %%mm6" : : "r"(mm6) : "%mm6");
	asm volatile("movq (%0), %%mm7" : : "r"(mm7) : "%mm7");

	asm volatile("vmovdqu (%0), %%ymm0" : : "r"(ymm0) : "%xmm0");
	asm volatile("vmovdqu (%0), %%ymm1" : : "r"(ymm1) : "%xmm1");
	asm volatile("vmovdqu (%0), %%ymm2" : : "r"(ymm2) : "%xmm2");
	asm volatile("vmovdqu (%0), %%ymm3" : : "r"(ymm3) : "%xmm3");
	asm volatile("vmovdqu (%0), %%ymm4" : : "r"(ymm4) : "%xmm4");
	asm volatile("vmovdqu (%0), %%ymm5" : : "r"(ymm5) : "%xmm5");
	asm volatile("vmovdqu (%0), %%ymm6" : : "r"(ymm6) : "%xmm6");
	asm volatile("vmovdqu (%0), %%ymm7" : : "r"(ymm7) : "%xmm7");

	asm volatile("vmovdqu (%0), %%ymm8" : : "r"(ymm8) : "%xmm8");
	asm volatile("vmovdqu (%0), %%ymm9" : : "r"(ymm9) : "%xmm9");
	asm volatile("vmovdqu (%0), %%ymm10" : : "r"(ymm10) : "%xmm10");
	asm volatile("vmovdqu (%0), %%ymm11" : : "r"(ymm11) : "%xmm11");
	asm volatile("vmovdqu (%0), %%ymm12" : : "r"(ymm12) : "%xmm12");
	asm volatile("vmovdqu (%0), %%ymm13" : : "r"(ymm13) : "%xmm13");
	asm volatile("vmovdqu (%0), %%ymm14" : : "r"(ymm14) : "%xmm14");
	asm volatile("vmovdqu (%0), %%ymm15" : : "r"(ymm15) : "%xmm15");
}

static void dirty_x87(void)
{
	asm volatile("movq (%0), %%mm0" : : "r"(mm0) : "%mm0");
}

static void dirty_xmm(void)
{
	asm volatile("movdqu (%0), %%xmm0" : : "r"(xmm0) : "%xmm0");
}

/* Dirtying one xmm seems to have the same effect as dirtying all. */
static void dirty_all_xmm(void)
{
	asm volatile("movdqu (%0), %%xmm0" : : "r"(xmm0) : "%xmm0");
	asm volatile("movdqu (%0), %%xmm1" : : "r"(xmm1) : "%xmm1");
	asm volatile("movdqu (%0), %%xmm2" : : "r"(xmm2) : "%xmm2");
	asm volatile("movdqu (%0), %%xmm3" : : "r"(xmm3) : "%xmm3");
	asm volatile("movdqu (%0), %%xmm4" : : "r"(xmm4) : "%xmm4");
	asm volatile("movdqu (%0), %%xmm5" : : "r"(xmm5) : "%xmm5");
	asm volatile("movdqu (%0), %%xmm6" : : "r"(xmm6) : "%xmm6");
	asm volatile("movdqu (%0), %%xmm7" : : "r"(xmm7) : "%xmm7");
}

/* Not sure if touching a ymm also touches the xmm / x87.  This applies to all
 * of the tests using hi_ymm. */
static void dirty_hi_ymm(void)
{
	asm volatile("vmovdqu (%0), %%ymm0" : : "r"(hi_ymm0) : "%xmm0");
}

static void dirty_xmm_x87(void)
{
	dirty_xmm();
	dirty_x87();
}

static void dirty_hi_ymm_xmm(void)
{
	dirty_hi_ymm();
	dirty_xmm();
}

static void dirty_hi_ymm_x87(void)
{
	dirty_hi_ymm();
	dirty_x87();
}

static void dirty_hi_ymm_xmm_x87(void)
{
	dirty_hi_ymm();
	dirty_xmm();
	dirty_x87();
}

static void noop(void)
{
}

/* Sets 'as' to represent an initialized, unmodified FP state.  Note this may
 * dirty your processors XMMs! */
static void initialize_as(struct ancillary_state *as)
{
	memcpy(as, &init_as, sizeof(struct ancillary_state));
}

/* Sets AS to represent a fully modified FP state.  Note this may dirty your
 * processors XMMs! */
static void full_dirty_as(struct ancillary_state *as)
{
	memcpy(as, &dirty_as, sizeof(struct ancillary_state));
}

/* Sets the processor's FP state to an initialized, unmodified state. */
static void reset_fp(void)
{
	__builtin_ia32_xrstor64(&init_as, mask);
}

static uint64_t abs_diff(uint64_t x, uint64_t y)
{
	return x >= y ? x - y : y - x;
}

static uint64_t compute_rd_overhead(void)
{
	uint64_t start;
	uint64_t end;
	uint64_t sum = 0;
	uint64_t opt1, opt2;
	#define NR_LOOPS 10000

	/* There's a couple ways you can compute this.  The first way is the way
	 * we'll use it: just two reads, and using the measurement of each iteration
	 * to measure that iteration. */
	for (int i = 0; i < NR_LOOPS; i++) {
		start = cycles();
		end = cycles();
		sum += (end - start);
	}
	opt1 = sum / NR_LOOPS;
	/* The second way is to just do a bunch of the calls, and only use the last
	 * measurement. */
	start = cycles();
	for (int i = 0; i < NR_LOOPS; i++)
		end = cycles();
	opt2 = (end - start) / NR_LOOPS;

	/* Note that, like with rdtsc, rdpmc's latency may hide some instructions.
	 * I was able to squeeze in a couple movqs to stack addresses before
	 * noticing a difference.  If you want to play with it, try this:
	 *
		#define JMAX 3
		long foo[JMAX];
	
		for (int j = 0; j < JMAX; j++)
			asm volatile("movq %%rax, %0;" : : "m"(foo[j]));
	 */
	/* 2 seems reasonable for rdpmc. */
	if (abs_diff(opt1, opt2) > 2) {
		fprintf(stderr,
		        "Overhead diff between %llu %llu is too great (interference?), try again!\n",
		        opt1, opt2);
		exit(-1);
	}
	fprintf(stderr,
	        "Measurement overhead is %llu, subtracted from the results\n",
	        MIN(opt1, opt2));
	return MIN(opt1, opt2);
}

/* Keep the names at the same width for easy R alignment.  clobbered_xstatebv is
 * three bits we expect the test to clobber on a clean/inited FPU.  We'll assert
 * this at runtime. */
struct dirty_test {
	char *name;
	uint64_t clobbered_xstatebv;
	void (*dirty)(void);
} dirty_tests[] = {
	{"...........noop", 0x0, noop},
	{".........reinit", 0x0, reset_fp},
	{"............x87", 0x1, dirty_x87},
	{"............xmm", 0x2, dirty_xmm},
	{"........xmm_x87", 0x3, dirty_xmm_x87},
	{".........hi_ymm", 0x6, dirty_hi_ymm},		/* touching ymm touches xmm */
	{".....hi_ymm_xmm", 0x6, dirty_hi_ymm_xmm},
	{".....hi_ymm_x87", 0x7, dirty_hi_ymm_x87},
	{"...all_data_reg", 0x7, dirty_all_data_reg},
	{".hi_ymm_xmm_x87", 0x7, dirty_hi_ymm_xmm_x87},
};

/* Measures the costs of xsave / xsaveopt during a restore-dirty-save cycle.
 *
 * opt controls whether we use xsaveopt or just xsave.
 *
 * clean controls whether we start an iteration with an all clean (initialized)
 * or all in-use state.  This ends up being the *rest* of the state that isn't
 * clobbered by dirty() that gets xsaved.  Clean shouldn't matter, according to
 * the SDM, since if it wasn't modified, xsaveopt should ignore it.  Regardless,
 * I see a difference for xmm based on 'clean' on some machines/OSs.
 *
 * This measures the effect of the 'modified' optimization, where xsaveopt
 * would only save regions that were modified since the last rstror, so long as
 * the 4-tuple of {cpl, vmx, xsave_linear_addr, xcomp_bv} hasn't changed. */
static void test_xsave(struct dirty_test *dt, bool opt, bool clean)
{
	uint64_t start;

	for (int i = 0; i < nr_iters; i++) {
		if (clean)
			initialize_as(&as);
		else
			full_dirty_as(&as);
		__builtin_ia32_xrstor64(&as, mask);
		dt->dirty();
		start = start_timing();
		if (opt)
			__builtin_ia32_xsaveopt64(&as, mask);
		else
			__builtin_ia32_xsave64(&as, mask);
		save_res[i] = stop_timing(start);
	}

	for (int i = 0; i < nr_iters; i++)
		fprintf(outfile, "%sXSAVE%s %s %llu\n",
		        clean ? "CLEAN_" : "", opt ? "OPT" : "", dt->name, save_res[i]);
}

enum {
	XRSTOR_CMD_CLEAN,
	XRSTOR_CMD_DIRTY,
	XRSTOR_CMD_NOOP,
};

/* This tests XRSTOR's speed to restore a context of varying dirtiness.  For
 * initialized FP states (an xstate_bv bit is clear), the processor should just
 * e.g. set the registers to 0 (etc), and not read from memory.
 *
 * The SDM doesn't say if the *current* FPU state matters for restore.  It could
 * be clean, fully dirty, a variety of dirtiness, etc.  cmd controls a few of
 * these options.
 *
 * Possibly if the FPU is already clean, the processor knows that and doesn't
 * even bother zeroing the registers.  Or it could use the XINUSE / modified
 * optimization info. */
static void test_xrstor(struct dirty_test *dt, int cmd)
{
	uint64_t start;
	char *title = NULL;

	reset_fp();
	dt->dirty();
	__builtin_ia32_xsaveopt64(&as, mask);

	for (int i = 0; i < nr_iters; i++) {
		switch (cmd) {
		case XRSTOR_CMD_CLEAN:
			reset_fp();
			break;
		case XRSTOR_CMD_DIRTY:
			dirty_all_data_reg();
			break;
		}
		start = start_timing();
		__builtin_ia32_xrstor64(&as, mask);
		save_res[i] = stop_timing(start);
	}

	switch (cmd) {
	case XRSTOR_CMD_CLEAN:
		title = "CLEAN";
		break;
	case XRSTOR_CMD_DIRTY:
		title = "DIRTY";
		break;
	case XRSTOR_CMD_NOOP:
		title = "NOOP_";
		break;
	}
	for (int i = 0; i < nr_iters; i++)
		fprintf(outfile, "%s_XRSTOR %s %llu\n", title, dt->name, save_res[i]);
}

/* Measures XRSTOR speed for restoring a context when the *current FPU* has been
 * dirtied in various ways.
 *
 * This attempts to see if XRSTOR does anything with the 'modified'
 * optimizization.  The SDM does not suggest this happens.  For instance, if we
 * just saved a fully dirty FP state, and then do not dirty the processor state,
 * can the XRSTOR skip reloading the registers from memory?  Or if it just
 * restored, then restored again, does it realize nothing changed?
 *
 * 'presave' controls whether or not we do a save right before the dirty.  This
 * checks if xsave has any interaction with these optimizations.  From what I've
 * seen, it makes no difference on my machine.  But that's why we test.
 *
 * The context we're restoring can be fully dirty or fully clean.  This test is
 * sort of the inverse of test_xrstor().  There, the initial state was
 * controlled by the dirty_test, and the intermediate op was clean/dirty/noop.
 * This test's initial state is clean/dirty, and the intermediate op is
 * controlled by dirty_test.  Following this to the extreme, we'd have
 * dirty_test * dirty_test combinations - these two tests are easier to deal
 * with. */
static void test_xrstor_alt(struct dirty_test *dt, bool clean, bool presave)
{
	uint64_t start;

	if (clean)
		reset_fp();
	else
		dirty_all_data_reg();
	__builtin_ia32_xsaveopt64(&as, mask);

	for (int i = 0; i < nr_iters; i++) {
		if (presave)
			__builtin_ia32_xsaveopt64(&as, mask);
		dt->dirty();
		start = start_timing();
		__builtin_ia32_xrstor64(&as, mask);
		save_res[i] = stop_timing(start);
	}

	for (int i = 0; i < nr_iters; i++)
		fprintf(outfile, "%s_%sXRSTOR %s %llu\n", clean ? "CLEAN" : "DIRTY",
		        presave ? "PRESAVE" : "", dt->name, save_res[i]);
}

/* Tests whether XSAVE does the init optimization: omit saving components in
 * their initial state.  We'll vary which components are in their init state
 * with dirty().
 *
 * opt controls whether or not we use XSAVEOPT.
 *
 * The tricky thing is that we need to not hit the modified optimization, which
 * is when we save to the same place we just restored from. */
static void test_init_xsave(struct dirty_test *dt, bool opt)
{
	uint64_t start;

	for (int i = 0; i < nr_iters; i++) {
		/* This also does an rstor, but it is from a different address than
		 * where we save later.  That means the modified optimization won't
		 * happen. */
		reset_fp();
		dt->dirty();
		start = start_timing();
		if (opt)
			__builtin_ia32_xsaveopt64(&alt_as, mask);
		else
			__builtin_ia32_xsave64(&alt_as, mask);
		save_res[i] = stop_timing(start);
	}

	for (int i = 0; i < nr_iters; i++)
		fprintf(outfile, "INIT_XSAVE%s %s %llu\n", opt ? "OPT" : "",
		        dt->name, save_res[i]);
}

enum {
	XSAVE,
	XRSTOR,
	XRSTOR_ALT,
	INIT_XSAVE,
};

static const char * const main_tests[] = {
	[XSAVE] = "XSAVE",
	[XRSTOR] = "XRSTOR",
	[XRSTOR_ALT] = "XRSTOR_ALT",
	[INIT_XSAVE] = "INIT_XSAVE",
};

static int get_test_id(const char *name)
{
	for (int i = 0; i < sizeof(main_tests) / sizeof(main_tests[0]); i++)
		if (!strcmp(main_tests[i], name))
			return i;
	return -1;
}

/* Given an initially clean FPU on the processor, xsave should only save the
 * parts we think dirty() touched.  We can see those in xstatebv. */
static void assert_clobbers(void)
{
	struct dirty_test *dt;

	for (int i = 0; i < sizeof(dirty_tests) / sizeof(dirty_tests[0]); i++) {
		dt = &dirty_tests[i];
		reset_fp();
		dt->dirty();
		__builtin_ia32_xsaveopt64(&as, 0x7);
		if (as.xstate_bv != dt->clobbered_xstatebv) {
			fprintf(stderr,
					"Test %s had unexpected clobbers: xstate_bv was %p, expected %p\n",
					dt->name, as.xstate_bv, dt->clobbered_xstatebv);
			exit(-1);
		}
	}
}

int main(int argc, char *argv[])
{
	int i;
	int core = 0;
	int opt = 0;
	static struct option long_options[] = {
	    {"samples", required_argument, 0, 's'},
	    {"savemask", required_argument, 0, 'm'},
	    {"core", required_argument, 0, 'c'},
	    {"outfile", required_argument, 0, 'o'},
	    {"test", required_argument, 0, 't'},
	    {0, 0, 0, 0}};
	int long_index = 0;
	time_t now;
	int test_id = XSAVE;

	while ((opt = getopt_long(argc, argv, "c:s:m:o:t:", long_options,
	                          &long_index)) != -1) {
		switch (opt) {
		case 'c':
			core = strtol(optarg, 0, 0);
			break;
		case 'm':
			mask = strtol(optarg, 0, 0);
			break;
		case 's':
			nr_iters = atoi(optarg);
			break;
		case 'o':
			outfile_name = optarg;
			break;
		case 't':
			test_id = get_test_id(optarg);
			if (test_id < 0) {
				fprintf(stderr, "Unknown test '%s'.  Try:\n", optarg);
				for (int i = 0;
				     i < sizeof(main_tests) / sizeof(main_tests[0]);
				     i++) {
					fprintf(stderr, "\t%s\n", main_tests[i]);
				}
				exit(1);
			}
			break;
		default:
			fprintf(stderr, "Usage: %s [-m savemask] [-s numsamples]\n",
			        argv[0]);
			exit(1);
		}
	}
	assert((mask & rxcr0()) == mask);

	if (setup(core) < 0) {
		perror("setup");
		exit(1);
	}
	enable_speed_step(core, 0);
	save_res = malloc(nr_iters * sizeof(uint64_t));
	set_cpuinfo();
	rd_overhead = compute_rd_overhead();

	outfile = fopen(outfile_name, "w");
	if (!outfile) {
		perror("opening outfile");
		exit(-1);
	}
	fprintf(stderr, "Outputting to %s\n", outfile_name);

	fprintf(outfile, "# title: %s %s Costs\n", os_name(), main_tests[test_id]);
	fprintf(outfile, "# machine: %s %d, %d, %d (F, M, S)\n", vendor, family,
	        model, stepping);
	now = time(NULL);
	fprintf(outfile, "# date: %s\n", ctime(&now));

	/* Set up an initialized state that we can use for resets.  Importantly,
	 * this has the xstate_bv[] bits set to 0. */
	memset(&init_as, 0, sizeof(struct ancillary_state));
	init_as.fp_head_64d.mxcsr = 0x1f80;

	/* Set up a fully-dirty ancillary state. */
	dirty_all_data_reg();
	__builtin_ia32_xsaveopt64(&dirty_as, 7);

	assert_clobbers();

	/* Prime it.  (not sure if this is necessary or not) */
	reset_fp();
	__builtin_ia32_xsaveopt64(&as, mask);
	__builtin_ia32_xsave64(&as, mask);
	__builtin_ia32_xrstor64(&as, mask);

	for (i = 0; i < sizeof(dirty_tests) / sizeof(dirty_tests[0]); i++) {
		switch (test_id) {
		case XSAVE:
			test_xsave(&dirty_tests[i], false, false);
			test_xsave(&dirty_tests[i], true,  false);
			test_xsave(&dirty_tests[i], false, true);
			test_xsave(&dirty_tests[i], true,  true);
			break;
		case XRSTOR:
			test_xrstor(&dirty_tests[i], XRSTOR_CMD_NOOP);
			test_xrstor(&dirty_tests[i], XRSTOR_CMD_CLEAN);
			test_xrstor(&dirty_tests[i], XRSTOR_CMD_DIRTY);
			break;
		case XRSTOR_ALT:
			test_xrstor_alt(&dirty_tests[i], false, false);
			test_xrstor_alt(&dirty_tests[i], true,  false);
			test_xrstor_alt(&dirty_tests[i], false, true);
			test_xrstor_alt(&dirty_tests[i], true,  true);
			break;
		case INIT_XSAVE:
			test_init_xsave(&dirty_tests[i], false);
			test_init_xsave(&dirty_tests[i], true);
			break;
		}
	}

	fclose(outfile);
	return 0;
}

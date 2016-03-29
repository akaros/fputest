#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <x86intrin.h>
#include <getopt.h>
#include <errno.h>
#define __USE_GNU
#include <unistd.h>

#include <sched.h>

/*

This version of the test program rearranges the ids of the tests
so that they get plotted in a different order.

The new order will be

baseline xsave64 test
xsave64 tests
baseline xsaveopt64 test
xsaveopt64 tests
baseline xrstor6464 after xsave64 test
xrstor6464 after xsasve64 tests
baseline xrstor6464 after xsaveopt64 test
xrstor6464 after xsaveopt64 tests

But still in the same order, under those categories, that the
tests occurred, so that the first test in "xsave64 tests"
corresponds to the first test in "xrstor6464 tests that followed an xsasve64"
and so forth.
*/

static inline uint64_t
rdtsc()
{
	uint32_t h, l;

	asm volatile("cpuid;"
	             "rdtsc;"
	             "mov %%edx, %0;"
	             "mov %%eax, %1" :
	             "=r"(h), "=r"(l) ::
	             "%rax", "%rbx", "%rcx", "%rdx");

	return ((uint64_t)h << 32) | l;
}

static inline uint64_t
rdtscp()
{
	uint32_t h, l;

	asm volatile("rdtscp;"
	             "mov %%edx, %0;"
	             "mov %%eax, %1;"
	             "cpuid" :
	             "=r"(h), "=r"(l) ::
	             "%rax", "%rbx", "%rcx", "%rdx");

	return ((uint64_t)h << 32) | l;
}

/*
SERIOUS TODO: We MUST pin this test to a single core, because the TSC clocks may
              not be synchronized between cores! If the process is migrated, then
              the count could be wrong.


Tests the performance of xsave64 vs xsaveopt64

xsave64/xrstor6464 will be our baseline

We will always time the xsave64 and xrstor6464 separately


There four kinds of tests:

1. Baseline tells us difference for init optimization

2. Dirtying outside the loop tells us difference
	for modified optimization

3. Dirtying at top of loop gives us a spectrum for
	xsaveopt64 with different amounts of state changed

4. Dirtying between save and restore tells us cost
	 of ext state use in vcore context (these tests
	 should be compared to baseline, as they will
	 use the init optimization)
*/

// ------------------------------------------------------------
// We treat the ancillary state the same as Akaros:
// ------------------------------------------------------------
struct fp_header_non_64bit {
	uint16_t		fcw;
	uint16_t		fsw;
	uint8_t			ftw;
	uint8_t			padding0;
	uint16_t		fop;
	uint32_t		fpu_ip;
	uint16_t		cs;
	uint16_t		padding1;
	uint32_t		fpu_dp;
	uint16_t		ds;
	uint16_t		padding2;
	uint32_t		mxcsr;
	uint32_t		mxcsr_mask;
};

/* Header for the 64-bit mode FXSAVE map with promoted operand size */
struct fp_header_64bit_promoted {
	uint16_t		fcw;
	uint16_t		fsw;
	uint8_t			ftw;
	uint8_t			padding0;
	uint16_t		fop;
	uint64_t		fpu_ip;
	uint64_t		fpu_dp;
	uint32_t		mxcsr;
	uint32_t		mxcsr_mask;
};

/* Header for the 64-bit mode FXSAVE map with default operand size */
struct fp_header_64bit_default {
	uint16_t		fcw;
	uint16_t		fsw;
	uint8_t			ftw;
	uint8_t			padding0;
	uint16_t		fop;
	uint32_t		fpu_ip;
	uint16_t		cs;
	uint16_t		padding1;
	uint32_t		fpu_dp;
	uint16_t		ds;
	uint16_t		padding2;
	uint32_t		mxcsr;
	uint32_t		mxcsr_mask;
};

/* Just for storage space, not for real use	*/
typedef struct {
	unsigned int stor[4];
} __128bits;

/*
 *  X86_MAX_XCR0 specifies the maximum set of processor extended state
 *  feature components that Akaros supports saving through the
 *  XSAVE instructions.
 *  This may be a superset of available state components on a given
 *  processor. We CPUID at boot and determine the intersection
 *  of Akaros-supported and processor-supported features, and we
 *  save this value to __proc_global_info.x86_default_xcr0 in arch/x86/init.c.
 *  We guarantee that the set of feature components specified by
 *  X86_MAX_XCR0 will fit in the ancillary_state struct.
 *  If you add to the mask, make sure you also extend ancillary_state!
 */

#define X86_MAX_XCR0 0x2ff

typedef struct ancillary_state {
	/* Legacy region of the XSAVE area */
	union { /* whichever header used depends on the mode */
		struct fp_header_non_64bit			fp_head_n64;
		struct fp_header_64bit_promoted		fp_head_64p;
		struct fp_header_64bit_default		fp_head_64d;
	};
	/* offset 32 bytes */
	__128bits		st0_mm0;	/* 128 bits: 80 for the st0, 48 reserved */
	__128bits		st1_mm1;
	__128bits		st2_mm2;
	__128bits		st3_mm3;
	__128bits		st4_mm4;
	__128bits		st5_mm5;
	__128bits		st6_mm6;
	__128bits		st7_mm7;
	/* offset 160 bytes */
	__128bits		xmm0;
	__128bits		xmm1;
	__128bits		xmm2;
	__128bits		xmm3;
	__128bits		xmm4;
	__128bits		xmm5;
	__128bits		xmm6;
	__128bits		xmm7;
	/* xmm8-xmm15 are only available in 64-bit-mode */
	__128bits		xmm8;
	__128bits		xmm9;
	__128bits		xmm10;
	__128bits		xmm11;
	__128bits		xmm12;
	__128bits		xmm13;
	__128bits		xmm14;
	__128bits		xmm15;
	/* offset 416 bytes */
	__128bits		reserv0;
	__128bits		reserv1;
	__128bits		reserv2;
	__128bits		reserv3;
	__128bits		reserv4;
	__128bits		reserv5;
	/* offset 512 bytes */

	/*
	 * XSAVE header (64 bytes, starting at offset 512 from
	 * the XSAVE area's base address)
	 */

	// xstate_bv identifies the state components in the XSAVE area
	uint64_t		xstate_bv;
	/*
	 *	xcomp_bv[bit 63] is 1 if the compacted format is used, else 0.
	 *	All bits in xcomp_bv should be 0 if the processor does not support the
	 *	compaction extensions to the XSAVE feature set.
	*/
	uint64_t		xcomp_bv;
	__128bits		reserv6;

	/* offset 576 bytes */
	/*
	 *	Extended region of the XSAVE area
	 *	We currently support an extended region of up to 2112 bytes,
	 *	for a total ancillary_state size of 2688 bytes.
	 *	This supports x86 state components up through the zmm31 register.
	 *	If you need more, please ask!
	 *	See the Intel Architecture Instruction Set Extensions Programming
	 *	Reference page 3-3 for detailed offsets in this region.
	*/
	uint8_t			extended_region[2112];

	/* ancillary state  */
} __attribute__((aligned(64))) ancillary_state_t;
// ------------------------------------------------------------
// End Akaros-specific stuff
// ------------------------------------------------------------


#define XSAVE "xsave"
#define XSAVEOPT "xsaveopt"

// The ancillary state region used for all the tests
struct ancillary_state as;
struct ancillary_state default_as;

uint32_t edx = 0x0;
uint32_t eax = 0x7;
unsigned long long mask = 2; // avoid bit 0.
char *mm0 = "|_MM:0_|";
char *xmm0  = "|____XMM:00____|";
char *hi_ymm1 = "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0|_YMM_Hi128:01_|";

char *mm1 = "|_MM:1_|";
char *mm2 = "|_MM:2_|";
char *mm3 = "|_MM:3_|";
char *mm4 = "|_MM:4_|";
char *mm5 = "|_MM:5_|";
char *mm6 = "|_MM:6_|";
char *mm7 = "|_MM:7_|";

// Each of these strings is 32 bytes long, excluding the terminating \0.
char *ymm0  = "|____XMM:00____||_YMM_Hi128:00_|";
char *ymm1  = "|____XMM:01____||_YMM_Hi128:01_|";
char *ymm2  = "|____XMM:02____||_YMM_Hi128:02_|";
char *ymm3  = "|____XMM:03____||_YMM_Hi128:03_|";
char *ymm4  = "|____XMM:04____||_YMM_Hi128:04_|";
char *ymm5  = "|____XMM:05____||_YMM_Hi128:05_|";
char *ymm6  = "|____XMM:06____||_YMM_Hi128:06_|";
char *ymm7  = "|____XMM:07____||_YMM_Hi128:07_|";
char *ymm8  = "|____XMM:08____||_YMM_Hi128:08_|";
char *ymm9  = "|____XMM:09____||_YMM_Hi128:09_|";
char *ymm10 = "|____XMM:10____||_YMM_Hi128:10_|";
char *ymm11 = "|____XMM:11____||_YMM_Hi128:11_|";
char *ymm12 = "|____XMM:12____||_YMM_Hi128:12_|";
char *ymm13 = "|____XMM:13____||_YMM_Hi128:13_|";
char *ymm14 = "|____XMM:14____||_YMM_Hi128:14_|";
char *ymm15 = "|____XMM:15____||_YMM_Hi128:15_|";

int enable_speed_step(int cpu, int on)
{
    static const uint64_t ss_bit = (uint64_t) 1 << 32;
    static const off_t perf_ctl_msr = 0x199;
    int fd, status;
    uint64_t val, xval;
    char msrdev[256];

    snprintf(msrdev, sizeof(msrdev), "/dev/cpu/%d/msr", cpu);
    fd = open(msrdev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "MSR device not available, leaving speed step as it was!\n");
        return -1;
    }
    if (pread(fd, &val, sizeof(val), perf_ctl_msr) != sizeof(val)) {
        fprintf(stderr, "Unable to read MSR device register 0x%lx: %s\n",
                perf_ctl_msr, strerror(errno));
        exit(2);
    }
    status = (val & ss_bit) ? 0 : 1;
    if (status ^ (on != 0)) {
        if (on)
            val &= ~ss_bit;
        else
            val |= ss_bit;
        if (pwrite(fd, &val, sizeof(val), perf_ctl_msr) != sizeof(val)) {
            fprintf(stderr, "Unable to write MSR device: %s\n", strerror(errno));
            exit(2);
        }
        if (pread(fd, &xval, sizeof(xval), perf_ctl_msr) != sizeof(xval)) {
            fprintf(stderr, "Unable to read MSR device: %s\n", strerror(errno));
            exit(2);
        }
        if (val != xval) {
            fprintf(stderr, "Unable to write MSR device. "
                    "Value 0x%lx did not stick at MSR 0x%lx!\n", val, perf_ctl_msr);
            exit(2);
        }
    }
    close(fd);

    return status;
}

void dirty_all_data_reg() {

	asm volatile ("movq (%0), %%mm0" : /* No Outputs */ : "r" (mm0) : "%mm0");
	asm volatile ("movq (%0), %%mm1" : /* No Outputs */ : "r" (mm1) : "%mm1");
	asm volatile ("movq (%0), %%mm2" : /* No Outputs */ : "r" (mm2) : "%mm2");
	asm volatile ("movq (%0), %%mm3" : /* No Outputs */ : "r" (mm3) : "%mm3");
	asm volatile ("movq (%0), %%mm4" : /* No Outputs */ : "r" (mm4) : "%mm4");
	asm volatile ("movq (%0), %%mm5" : /* No Outputs */ : "r" (mm5) : "%mm5");
	asm volatile ("movq (%0), %%mm6" : /* No Outputs */ : "r" (mm6) : "%mm6");
	asm volatile ("movq (%0), %%mm7" : /* No Outputs */ : "r" (mm7) : "%mm7");

	asm volatile ("vmovdqu (%0), %%ymm0" : /* No Outputs */ : "r" (ymm0) : "%xmm0");
	asm volatile ("vmovdqu (%0), %%ymm1" : /* No Outputs */ : "r" (ymm1) : "%xmm1");
	asm volatile ("vmovdqu (%0), %%ymm2" : /* No Outputs */ : "r" (ymm2) : "%xmm2");
	asm volatile ("vmovdqu (%0), %%ymm3" : /* No Outputs */ : "r" (ymm3) : "%xmm3");
	asm volatile ("vmovdqu (%0), %%ymm4" : /* No Outputs */ : "r" (ymm4) : "%xmm4");
	asm volatile ("vmovdqu (%0), %%ymm5" : /* No Outputs */ : "r" (ymm5) : "%xmm5");
	asm volatile ("vmovdqu (%0), %%ymm6" : /* No Outputs */ : "r" (ymm6) : "%xmm6");
	asm volatile ("vmovdqu (%0), %%ymm7" : /* No Outputs */ : "r" (ymm7) : "%xmm7");

	asm volatile ("vmovdqu (%0), %%ymm8"  : /* No Outputs */ : "r" (ymm8)  : "%xmm8");
	asm volatile ("vmovdqu (%0), %%ymm9"  : /* No Outputs */ : "r" (ymm9)  : "%xmm9");
	asm volatile ("vmovdqu (%0), %%ymm10" : /* No Outputs */ : "r" (ymm10) : "%xmm10");
	asm volatile ("vmovdqu (%0), %%ymm11" : /* No Outputs */ : "r" (ymm11) : "%xmm11");
	asm volatile ("vmovdqu (%0), %%ymm12" : /* No Outputs */ : "r" (ymm12) : "%xmm12");
	asm volatile ("vmovdqu (%0), %%ymm13" : /* No Outputs */ : "r" (ymm13) : "%xmm13");
	asm volatile ("vmovdqu (%0), %%ymm14" : /* No Outputs */ : "r" (ymm14) : "%xmm14");
	asm volatile ("vmovdqu (%0), %%ymm15" : /* No Outputs */ : "r" (ymm15) : "%xmm15");

}

void dirty_x87()
{
	 asm volatile ("movq (%0), %%mm0" : /* No Outputs */ : "r" (mm0) : "%mm0");
}
void dirty_xmm()
{
	asm volatile ("movdqu (%0), %%xmm0" : /* No Outputs */ : "r" (xmm0) : "%xmm0");
}
void dirty_hi_ymm()
{
	// Is there any way to dirty just the high bits?
	// I have a feeling this probably marks both AVX and SSE components 1 in XINUSE
	// TODO
	asm volatile ("vmovdqu (%0), %%ymm1" : /* No Outputs */ : "r" (hi_ymm1) : "%xmm1");
}

void dirty_xmm_x87()
{
	dirty_xmm();
	dirty_x87();
}
void dirty_hi_ymm_xmm()
{
	dirty_hi_ymm();
	dirty_xmm();
}
void dirty_hi_ymm_x87()
{
	// TODO: Not sure if there's a way to only dirty the high ymms...
	dirty_hi_ymm();
	dirty_x87();
}
void dirty_hi_ymm_xmm_x87()
{
	dirty_hi_ymm();
	dirty_xmm();
	dirty_x87();
}

void zero_as(struct ancillary_state *as)
{
	memset(as, 0x0, sizeof(struct ancillary_state));
}

void reset_fp()
{
	//asm volatile("fninit");
	__builtin_ia32_xrstor64(&as, mask);
}

void print_intro()
{
	printf("The type of test is identified by a number:\n"
	       "1. Baseline tells us difference for init optimization\n"
	       "2. Dirtying outside the loop tells us difference\n"
	       "   for modified optimization\n"
	       "3. Dirtying at top of loop gives us a spectrum for\n"
	       "   xsaveopt with different amounts of state changed\n"
		   "4. Dirtying between save and restore helps estimate cost\n"
		   "   of ext state use in vcore context (these tests\n"
		   "   should be compared to baseline, as they will\n"
		   "   use the init optimization)\n"
		);
	printf("\nThe result format is: result[tabtab]test_name-test_number-tested_instr\n");
}


void print_results(char * name, int num, char * instr, int id, double result)
{
	printf("%s-%d-%s\t%d\t%f\n", name, num, instr, id, result);
}

void print_test_result(char * name, double result)
{
	printf("%s %g\n", name, result);
}

int n = 1000000;
void tsc_test(void)
{
	uint64_t i;
	uint64_t start;
	uint64_t end;
	uint64_t sum = 0;
	for (i = 0; i < n; ++i) {
		start = rdtsc();
		end = rdtscp();
		sum += (end - start);
	}

	/*
		Timing measurements come with at least a single readTSC call
		of overhead baked in. Assuming that you have the proper
		time in the registers as soon as the rdtsc instruction completes,
		after your "start" value is measured you have the cost of a bit
		shift and or, and then a pop to return. Your "end" value has the
		cost of a function call (readTSC) and the actual rdtsc instruction
		baked in. Thus end - start contains the cost of a readTSC call in
		addition to whatever you measured.
	*/
	print_results("tsc overhead", 0, "readTSC()", 0, (double)sum/n);
	printf("You should subtract this from the rest of the timings\n");
	printf("to account for the overhead of the readTSC() function call.\n");
}


uint64_t *save_res;
uint64_t *rstor_res;

void nodirty(void)
{
}

void programtest(char *name, char *opt, int base, void dirty(void), int save /* 1 == xsave, 2 == xsaveopt */)
{
	static int first = 1;
	int i, j, iter;
	uint64_t start;
	uint64_t end;
	//for(i = 2; i < 5; i++) {
	for(i = 2; i < 3; i++) {
		for(j = 0; j < 2; j++) {
			zero_as(&as);
			reset_fp();
			if (i == 2)
				dirty();
			for (iter = 0; iter < n; iter++) {
				if (i == 3) {
					reset_fp();
					dirty();
				}
				
				start = rdtsc();
				if (save == 1)
					__builtin_ia32_xsave64(&default_as, mask);
				else
					__builtin_ia32_xsaveopt64(&default_as, mask);
				end = rdtscp();
				save_res[iter] = end - start;
				if (i == 4) {
					reset_fp();
					dirty();
				}
				start = rdtsc();
				__builtin_ia32_xrstor64(&as, mask);
				end = rdtscp();
				rstor_res[iter] = end - start;
			}
		}
		if (! first)
			printf("\n\n");
		first = 0;
		printf("#%s_xsave%s-%d-xsave%s\n", name, opt, i, opt);
		printf("%d -20 %s_xsave%s-%d-xsave%s\n", base + (i-2), name, opt, i, opt);
		for (iter = 0; iter < n; ++iter)
			printf("%d\t%ld\n", base + (i-2), save_res[iter] + rstor_res[i]);
/*
		printf("%d -20 %s_xsave%s-%d-xsave%s\n", base + (i-2), name, opt, i, opt);
		for (iter = 0; iter < n; ++iter)
			printf("%d\t%ld\n", base + (i-2), save_res[iter]);
		printf("\n\n");
		printf("#%s_xsave%s-%d-xrstor64\n", name, opt, i);
		for (iter = 0; iter < n; ++iter)
			printf("%d\t%ld\n", base + (i-2) + 50, rstor_res[iter]);
 */
	}
}

struct test {
	char *name;
	int index;
	void (*dirty)(void);} tests[] = {
	{"baseline", 1, nodirty},
	{"x87", 2, dirty_x87},
	{"xmm_x87", 3, dirty_xmm_x87},
	{"xmm", 4, dirty_xmm},
	{"hi_ymm", 5, dirty_hi_ymm},
	{"hi_ymm_xmm", 6, dirty_hi_ymm_xmm},
	{"hi_ymm_x87", 7, dirty_hi_ymm_x87},
	{"hi_ymm_xmm_x87", 8, dirty_hi_ymm_xmm_x87},
	{"all_data_reg", 9, dirty_all_data_reg}
};

int setup(int core) {
	cpu_set_t my_set;
	CPU_ZERO(&my_set);
	CPU_SET(core, &my_set);
	if (sched_setaffinity(0, sizeof(cpu_set_t), &my_set) < 0)
		return -1;
	enable_speed_step(core, 0);
	return 0;
}
int main(int argc, char *argv[])
{
	int i;
	int core = 31;
    int opt= 0;
    static struct option long_options[] = {
        {"samples",      required_argument,       0,  's' },
          {"savemask",      required_argument,       0,  'm' },
          {"core",      required_argument,       0,  'c' },
      {0,           0,                 0,  0   }
    };

    int long_index =0;
    while ((opt = getopt_long(argc, argv,"s:m:", 
                   long_options, &long_index )) != -1) {
        switch (opt) {
             case 'c' : core = strtol(optarg, 0, 0);
                 break;
             case 'm' : mask = strtol(optarg, 0, 0);
                 break;
             case 's' : n = atoi(optarg);
                 break;
             default: fprintf(stderr, "Usage: %s [-m savemask] [-s numsamples]\n", argv[0]);
                 exit(1);
        }
    }

	if (setup(core) < 0) {
		perror("setup");
		exit(1);
	}
	save_res = malloc(n * sizeof(uint64_t));
	rstor_res = malloc(n * sizeof(uint64_t));

	// Set up a default extended state that we can use for resets
	memset(&default_as, 0x00, sizeof(struct ancillary_state));
	asm volatile ("fninit");
	edx = 0x0;
	eax = 0x1;
	__builtin_ia32_xsave64(&default_as, mask);
	default_as.fp_head_64d.mxcsr = 0x1f80;
	eax = 0x7; // Set eax back to state components up to AVX


	// TODO: According to Agner, Intel has a performance
	// counter called "core clock cycles", that is apparently
	// the most accurate measure... should take a look at this.
	for(i = 0; i < sizeof(tests)/sizeof(tests[0]); i++) {
		programtest(tests[i].name, "", tests[i].index, tests[i].dirty, 1);
		programtest(tests[i].name, "opt", tests[i].index + 25, tests[i].dirty, 2);
	}
	printf("#PLEASE NOTE!: I'm not sure if my method here marks just YMM or both YMM and XMM as XINUSE!");
	return 0;
}

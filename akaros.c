#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>
#define __USE_GNU
#include <unistd.h>
#include <parlib/uthread.h>

#include "fputest.h"

void enable_speed_step(int cpu, int on)
{
}

int setup(int core)
{
	int pcoreid;

	sys_provision(getpid(), RES_CORES, core);
	/* Surprised this works.  We're still the thread0 scheduler! */
	uthread_mcp_init();
	pcoreid = __procinfo.vcoremap[vcore_id()].pcoreid;

	if (pcoreid != core)
		fprintf(stderr, "Akaros: couldn't get core %d, you're on %d\n", core,
		        pcoreid);
	if (!cycles()) {
		fprintf(stderr,
		        "Akaros: run this from perf stat (without perf's -C) to get non-zero rdpmc counts\n");
		exit(-1);
	}
	return 0;
}

const char *os_name(void)
{
	return "Akaros";
}

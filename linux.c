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

#include <sched.h>

int setup(int core)
{
	cpu_set_t my_set;
	CPU_ZERO(&my_set);
	CPU_SET(core, &my_set);
	if (sched_setaffinity(0, sizeof(cpu_set_t), &my_set) < 0)
		return -1;
	return 0;
}

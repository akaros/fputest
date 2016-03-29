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

int setup(int core) {
	cpu_set_t my_set;
	CPU_ZERO(&my_set);
	CPU_SET(core, &my_set);
	if (sched_setaffinity(0, sizeof(cpu_set_t), &my_set) < 0)
		return -1;
	return 0;
}

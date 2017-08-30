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

int enable_speed_step(int cpu, int on);
int setup(int core)
{
	return 0;
}

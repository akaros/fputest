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

int enable_speed_step(int cpu, int on);
int setup(int core) {
	return 0;
}

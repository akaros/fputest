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

void enable_speed_step(int cpu, int on)
{
	static const uint64_t ss_bit = (uint64_t)1 << 32;
	static const off_t perf_ctl_msr = 0x199;
	int fd, status;
	uint64_t val, xval;
	char msrdev[256];

	snprintf(msrdev, sizeof(msrdev), "/dev/cpu/%d/msr", cpu);
	fd = open(msrdev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr,
		        "MSR device not available, leaving speed step as it was!\n");
		return;
	}
	if (pread(fd, &val, sizeof(val), perf_ctl_msr) != sizeof(val)) {
		fprintf(stderr, "Unable to read MSR device register 0x%lx: %s\n",
		        perf_ctl_msr, strerror(errno));
		return;
	}
	status = (val & ss_bit) ? 0 : 1;
	if (status ^ (on != 0)) {
		if (on)
			val &= ~ss_bit;
		else
			val |= ss_bit;
		if (pwrite(fd, &val, sizeof(val), perf_ctl_msr) != sizeof(val)) {
			fprintf(stderr, "Unable to write MSR device: %s\n",
			        strerror(errno));
			return;
		}
		if (pread(fd, &xval, sizeof(xval), perf_ctl_msr) != sizeof(xval)) {
			fprintf(stderr, "Unable to read MSR device: %s\n", strerror(errno));
			return;
		}
		if (val != xval) {
			fprintf(stderr,
			        "Unable to write MSR device. "
			        "Value 0x%lx did not stick at MSR 0x%lx!\n",
			        val, perf_ctl_msr);
			return;
		}
	}
	close(fd);

	return;
}

int setup(int core)
{
	cpu_set_t my_set;
	CPU_ZERO(&my_set);
	CPU_SET(core, &my_set);
	if (sched_setaffinity(0, sizeof(cpu_set_t), &my_set) < 0)
		return -1;
	return 0;
}

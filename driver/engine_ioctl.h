/* The engine driver's interface to user programs (/dev/engine).
 *
 * The OS owns the hardware threads of every core: a program does not issue
 * the core's management instructions (`.insn r 0x2b, 7, <operation>`) itself,
 * it hands them to the driver, which issues them in order on the CPU the
 * calling thread is pinned to and returns each rd. The first list on a CPU
 * makes the open file the owner of that core's threads until it is closed;
 * closing it cancels and releases what the owner left running. */
#pragma once

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint64_t __u64;
#endif

struct engine_instruction {
  __u64 operation, rs1, rs2, rd;
};

struct engine_issue {
  __u64 count;
  /* struct engine_instruction[count], rd written back. */
  __u64 instructions;
};

#define ENGINE_ISSUE _IOWR('B', 1, struct engine_issue)

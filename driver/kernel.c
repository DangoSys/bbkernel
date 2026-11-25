// SPDX-License-Identifier: GPL-2.0
/*
 * The kernel driver: the OS owns the hardware threads of every hart and
 * issues their management instructions for user programs (kernel_ioctl.h).
 *
 * A list runs on the CPU the caller is pinned to, in order, with preemption
 * off and SUM set: a thread task inherits the context of the instruction
 * that starts it (the process's satp, supervisor privilege), so with SUM its
 * DDR accesses reach the process's pages. The first list on a CPU makes the
 * open file the owner of that hart's threads; another file on the same CPU
 * is refused. When the owner closes, the driver cancels the tasks it left
 * running on each CPU it owns, acknowledges them and releases the threads.
 *
 * A thread's DMA translates through the process's page tables; a burst that
 * meets a page fault stalls in the hart until its owner resolves it. The
 * driver is that owner: the hart's wait for a completion also returns when a
 * thread stalls, and the driver services each stalled burst like a user
 * access fault of the process (fixup_user_fault), retries the burst, or fails
 * it when the process may not access the page, and waits again.
 */
#include <linux/cpumask.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <asm/csr.h>

#include "kernel_ioctl.h"

/* Management operations of a hart: query, the task and TSS operations, the
 * device service (13-19), the compiled transfers (20-23) and the DMA page
 * faults (24: the fault of a thread, 25: retry or fail its stalled burst). */
#define OPERATIONS 26
#define QUERY 0
#define ACQUIRE 5
#define RELEASE 6
#define FAULT 24
#define RESOLVE 25
#define LIST_LIMIT 65536

enum { FIELD_STATUS = 1, FIELD_COUNT = 7, FIELD_WAIT_COMPLETION = 10 };
/* The status of a thread: online, owned, done, cancelled, stalled on a fault. */
enum { STATUS_OWNED = 2, STATUS_DONE = 4, STATUS_STALLED = 16 };
/* The wait's result: a thread is done, a thread is stalled; 0 when none is owned. */
enum { WAIT_DONE = 1, WAIT_STALLED = 2 };
/* The fault word: a burst is stalled, it is a store, its virtual address. */
#define FAULT_STALLED BIT_ULL(63)
#define FAULT_STORE BIT_ULL(62)
#define FAULT_ADDRESS GENMASK_ULL(61, 0)

static DEFINE_SPINLOCK(owners_lock);
static struct file *owners[NR_CPUS];
/* Whether the owner of a CPU holds its thread group (acquire without release). */
static bool acquired[NR_CPUS];

#define ISSUE(n)                                                               \
	case n:                                                                \
		asm volatile(".insn r 0x2b, 7, " #n ", %0, %1, %2"             \
			     : "=r"(rd)                                        \
			     : "r"(rs1), "r"(rs2)                              \
			     : "memory");                                      \
		break

static u64 issue(u64 operation, u64 rs1, u64 rs2)
{
	u64 rd = 0;

	switch (operation) {
	ISSUE(0); ISSUE(1); ISSUE(2); ISSUE(3); ISSUE(4); ISSUE(5);
	ISSUE(6); ISSUE(7); ISSUE(8); ISSUE(9); ISSUE(10); ISSUE(11);
	ISSUE(12); ISSUE(13); ISSUE(14); ISSUE(15); ISSUE(16); ISSUE(17);
	ISSUE(18); ISSUE(19); ISSUE(20); ISSUE(21); ISSUE(22); ISSUE(23);
	ISSUE(24); ISSUE(25);
	default:
		BUG();
	}
	return rd;
}

static u64 key(u64 thread, u64 field)
{
	return thread << 32 | field;
}

/* On the owner's CPU, in a kworker bound to it: what the owner left. */
static long clean(void *unused)
{
	u64 count = issue(0, key(0, FIELD_COUNT), 0);
	u64 thread;

	for (thread = 0; thread < count; ++thread) {
		u64 status = issue(0, key(thread, FIELD_STATUS), 0);

		if (!(status & STATUS_OWNED))
			continue;
		if (!(status & STATUS_DONE))
			issue(4, key(thread, 0), 0);
		/* The owner's address space is gone: fail its stalled bursts. */
		while (!((status = issue(0, key(thread, FIELD_STATUS), 0)) & STATUS_DONE)) {
			if (status & STATUS_STALLED)
				issue(RESOLVE, key(thread, 0), 0);
			cond_resched();
		}
		issue(3, key(thread, 0), 0);
	}
	issue(RELEASE, 0, 0);
	return 0;
}

/*
 * The hart's wait for a completion (query WAIT_COMPLETION), on the caller's
 * CPU with preemption on: 1 once a thread is done, 0 when none is owned. The
 * hart's wait also returns when a thread is stalled on a DMA page fault; the
 * driver resolves its faults in the caller's address space and waits again.
 */
static u64 wait_completion(void)
{
	for (;;) {
		u64 result, count, thread;

		preempt_disable();
		result = issue(QUERY, key(0, FIELD_WAIT_COMPLETION), 0);
		preempt_enable();
		if (!(result & WAIT_STALLED))
			return result;
		preempt_disable();
		count = issue(QUERY, key(0, FIELD_COUNT), 0);
		preempt_enable();
		for (thread = 0; thread < count; ++thread) {
			u64 fault;
			bool fixed;

			preempt_disable();
			fault = issue(FAULT, key(thread, 0), 0);
			preempt_enable();
			if (!(fault & FAULT_STALLED))
				continue;
			mmap_read_lock(current->mm);
			fixed = !fixup_user_fault(current->mm, fault & FAULT_ADDRESS,
						  fault & FAULT_STORE ? FAULT_FLAG_WRITE : 0,
						  NULL);
			mmap_read_unlock(current->mm);
			if (!fixed)
				pr_err("kernel: thread %llu DMA %s fault at 0x%llx\n", thread,
				       fault & FAULT_STORE ? "store" : "load",
				       fault & FAULT_ADDRESS);
			preempt_disable();
			issue(RESOLVE, key(thread, 0), fixed);
			preempt_enable();
		}
	}
}

static int kernel_release(struct inode *inode, struct file *file)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		if (owners[cpu] != file)
			continue;
		if (acquired[cpu])
			work_on_cpu(cpu, clean, NULL);
		spin_lock(&owners_lock);
		owners[cpu] = NULL;
		acquired[cpu] = false;
		spin_unlock(&owners_lock);
	}
	return 0;
}

static long kernel_ioctl(struct file *file, unsigned int command,
			 unsigned long argument)
{
	struct kernel_issue request;
	struct kernel_instruction *list;
	unsigned int cpu;
	size_t bytes;
	u64 i;
	long status = 0;

	if (command != KERNEL_ISSUE)
		return -ENOTTY;
	if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
		return -EFAULT;
	if (!request.count || request.count > LIST_LIMIT)
		return -EINVAL;
	/* The instructions go to the hart of one CPU: the caller stays on it. */
	if (current->nr_cpus_allowed != 1)
		return -EPERM;
	bytes = request.count * sizeof(*list);
	list = kvmalloc(bytes, GFP_KERNEL);
	if (!list)
		return -ENOMEM;
	if (copy_from_user(list, u64_to_user_ptr(request.instructions), bytes)) {
		status = -EFAULT;
		goto out;
	}
	for (i = 0; i < request.count; ++i)
		if (list[i].operation >= OPERATIONS) {
			status = -EINVAL;
			goto out;
		}

	cpu = get_cpu();
	spin_lock(&owners_lock);
	if (owners[cpu] && owners[cpu] != file) {
		spin_unlock(&owners_lock);
		put_cpu();
		status = -EBUSY;
		goto out;
	}
	owners[cpu] = file;
	spin_unlock(&owners_lock);
	csr_set(CSR_STATUS, SR_SUM);
	for (i = 0; i < request.count; ++i) {
		if (list[i].operation == QUERY &&
		    list[i].rs1 == key(0, FIELD_WAIT_COMPLETION)) {
			csr_clear(CSR_STATUS, SR_SUM);
			put_cpu();
			list[i].rd = wait_completion();
			cpu = get_cpu();
			csr_set(CSR_STATUS, SR_SUM);
			continue;
		}
		list[i].rd = issue(list[i].operation, list[i].rs1, list[i].rs2);
		if (list[i].operation == ACQUIRE)
			acquired[cpu] = true;
		if (list[i].operation == RELEASE)
			acquired[cpu] = false;
	}
	csr_clear(CSR_STATUS, SR_SUM);
	put_cpu();

	if (copy_to_user(u64_to_user_ptr(request.instructions), list, bytes))
		status = -EFAULT;
out:
	kvfree(list);
	return status;
}

static const struct file_operations kernel_operations = {
	.owner = THIS_MODULE,
	.release = kernel_release,
	.unlocked_ioctl = kernel_ioctl,
};

static struct miscdevice kernel_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "kernel",
	.fops = &kernel_operations,
	.mode = 0666,
};

module_misc_device(kernel_device);
MODULE_DESCRIPTION("Buckyball kernel: management of the hardware threads of the harts");
MODULE_LICENSE("GPL");

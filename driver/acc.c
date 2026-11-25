// SPDX-License-Identifier: GPL-2.0
/*
 * The DMA page faults of the accelerator of each CPU hart.
 *
 * Every page an accelerator instruction's DMA touches is translated before
 * any of its data moves, so a page fault stalls the instruction without
 * effect and raises the local interrupt 16 of the hart that issued it. The
 * CSR accfault (0x5c0) reads the fault: bit 63 stalled, bit 62 a store, bits
 * 61:0 the virtual address; accsatp (0xdc0) is the satp the instruction was
 * issued under. Writing accfault resolves it: bit 0 set retries the
 * translation, clear fails the instruction.
 *
 * The accelerator belongs to the hart, not to a task: the stalled
 * instruction may be of a task that is not running. The interrupt masks
 * itself and queues work on its hart; the work finds the address space whose
 * page table accsatp names, fixes the page like a user access fault of that
 * process (fixup_user_fault) and retries, or fails the instruction when the
 * process may not access the page or has exited, and unmasks the interrupt.
 */
#include <asm/csr.h>
#include <asm/irq.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/workqueue.h>

#define CSR_ACCFAULT 0x5c0
#define CSR_ACCSATP 0xdc0
#define IRQ_ACC_FAULT 16
#define FAULT_STORE BIT_ULL(62)
#define FAULT_ADDRESS GENMASK_ULL(61, 0)

static int fault_irq;
static DEFINE_PER_CPU(int, fault_cookie);
static DEFINE_PER_CPU(struct work_struct, fault_work);

/* The address space whose page table satp names, referenced; NULL once it
 * exited. */
static struct mm_struct *address_space(unsigned long satp) {
  struct task_struct *process, *thread;
  struct mm_struct *mm = NULL;

  rcu_read_lock();
  for_each_process_thread(process, thread) {
    task_lock(thread);
    if (thread->mm && virt_to_pfn(thread->mm->pgd) == (satp & SATP_PPN) &&
        mmget_not_zero(thread->mm))
      mm = thread->mm;
    task_unlock(thread);
    if (mm)
      goto found;
  }
found:
  rcu_read_unlock();
  return mm;
}

/* Runs on the hart whose accelerator is stalled. */
static void resolve(struct work_struct *work) {
  u64 fault = csr_read(CSR_ACCFAULT);
  struct mm_struct *mm = address_space(csr_read(CSR_ACCSATP));
  bool fixed = false;

  if (mm) {
    mmap_read_lock(mm);
    fixed = !fixup_user_fault(mm, fault & FAULT_ADDRESS,
                              fault & FAULT_STORE ? FAULT_FLAG_WRITE : 0, NULL);
    mmap_read_unlock(mm);
    mmput(mm);
  }
  if (!fixed)
    pr_err("acc: CPU %d DMA %s fault at 0x%llx\n", smp_processor_id(),
           fault & FAULT_STORE ? "store" : "load", fault & FAULT_ADDRESS);
  csr_write(CSR_ACCFAULT, fixed);
  enable_percpu_irq(fault_irq, IRQ_TYPE_NONE);
}

static irqreturn_t stalled(int irq, void *cookie) {
  disable_percpu_irq(irq);
  schedule_work_on(smp_processor_id(), this_cpu_ptr(&fault_work));
  return IRQ_HANDLED;
}

static void enable(void *unused) {
  enable_percpu_irq(fault_irq, IRQ_TYPE_NONE);
}

static int __init acc_init(void) {
  struct irq_domain *domain =
      irq_find_matching_fwnode(riscv_get_intc_hwnode(), DOMAIN_BUS_ANY);
  int cpu, err;

  for_each_possible_cpu(cpu) INIT_WORK(per_cpu_ptr(&fault_work, cpu), resolve);
  fault_irq = irq_create_mapping(domain, IRQ_ACC_FAULT);
  err = request_percpu_irq(fault_irq, stalled, "acc-fault", &fault_cookie);
  if (err)
    return err;
  on_each_cpu(enable, NULL, 1);
  return 0;
}
module_init(acc_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Buckyball accelerator DMA page faults");

/* Machine-mode trampoline for compiled user kernels. */
.section .text
.align 2
.globl buckyball_task_worker
.type buckyball_task_worker,@function
buckyball_task_worker:
  /* This hart belongs to the tile worker pool, never to Linux SMP. */
  li t0, -1
  csrw pmpaddr0, t0
  li t0, 15
  csrw pmpcfg0, t0
  csrw medeleg, zero
  csrw mideleg, zero
  csrw mie, zero
  li t0, 7
  csrw mcounteren, t0
  csrw scounteren, t0
.Lworker_wait:
  .insn r 0x2b, 7, 8, t0, zero, zero
  addi sp, sp, -48
  sd gp, 0(sp)
  sd tp, 8(sp)
  csrw mscratch, sp
  la t0, .Lworker_trap
  csrw mtvec, t0
  li t0, 0
  .insn r 0x2b, 7, 9, t1, t0, zero
  csrw mepc, t1
  li t0, 1
  .insn r 0x2b, 7, 9, a0, t0, zero
  li t0, 2
  .insn r 0x2b, 7, 9, t2, t0, zero
  li t0, 3
  .insn r 0x2b, 7, 9, tp, t0, zero
  li t0, 5
  .insn r 0x2b, 7, 9, gp, t0, zero
  li t0, 7
  .insn r 0x2b, 7, 9, t1, t0, zero
  csrw satp, t1
  sfence.vma
  fence.i
  /* User privilege; FP and custom-extension state enabled. */
  li t0, 0x1e000
  csrw mstatus, t0
  mv sp, t2
  mret
.Lworker_trap:
  csrr sp, mscratch
  csrr t0, mcause
  li t1, 8
  bne t0, t1, .Lworker_failed
  li t1, 0xbb01
  bne a7, t1, .Lworker_failed
  j .Lworker_complete
.Lworker_failed:
  li a0, 1
.Lworker_complete:
  .insn r 0x2b, 7, 2, t0, a0, zero
  csrw satp, zero
  sfence.vma
  ld gp, 0(sp)
  ld tp, 8(sp)
  addi sp, sp, 48
  j .Lworker_wait
.size buckyball_task_worker, .-buckyball_task_worker

// SPDX-License-Identifier: BSD-2-Clause
/*
 * Buckyball OpenSBI Platform
 *
 * Console: SCU UART (per-hart at 0x60000000 + hartid*0x40000 + 0x20000)
 * IPI:     CLINT MSWI at 0x02000000
 * Timer:   CLINT MTIMER at 0x02000000
 * IRQ:     PLIC at 0x0C000000
 */

#include <libfdt.h>
#include <sbi/riscv_asm.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_ecall_interface.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_hartmask.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_system.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <sbi_utils/ipi/aclint_mswi.h>
#include <sbi_utils/irqchip/plic.h>
#include <sbi_utils/timer/aclint_mtimer.h>

#ifndef BUCKYBALL_VISIBLE_HART_COUNT
#define BUCKYBALL_VISIBLE_HART_COUNT 64
#endif
#ifndef BUCKYBALL_TOTAL_HART_COUNT
#define BUCKYBALL_TOTAL_HART_COUNT BUCKYBALL_VISIBLE_HART_COUNT
#endif
#ifndef BUCKYBALL_HIDDEN_HART_BASE
#define BUCKYBALL_HIDDEN_HART_BASE BUCKYBALL_VISIBLE_HART_COUNT
#endif

#define BUCKYBALL_CLINT_ADDR 0x02000000
#define BUCKYBALL_PLIC_ADDR 0x0C000000
#define BUCKYBALL_PLIC_SIZE 0x04000000
#define BUCKYBALL_PLIC_NUM_SOURCES 1
#define BUCKYBALL_SCU_BASE 0x60000000UL
#define BUCKYBALL_SCU_STRIDE 0x40000UL
#define BUCKYBALL_SCU_UART_OFFSET 0x20000UL
#define BUCKYBALL_SCU_UART_RX_OFFSET 0x20004UL
#define BUCKYBALL_SCU_UART_STATUS_OFFSET 0x20005UL
#define BUCKYBALL_SCU_UART_RX_VALID 0x01
#define BUCKYBALL_MTIMER_FREQ 10000000
#define BUCKYBALL_SIM_EXIT_SUCCESS 0

#if BUCKYBALL_VISIBLE_HART_COUNT < 1
#error "BUCKYBALL_VISIBLE_HART_COUNT must be at least 1"
#endif
#if BUCKYBALL_TOTAL_HART_COUNT < BUCKYBALL_VISIBLE_HART_COUNT
#error "BUCKYBALL_TOTAL_HART_COUNT must cover visible harts"
#endif
#if BUCKYBALL_HIDDEN_HART_BASE < BUCKYBALL_VISIBLE_HART_COUNT
#error "BUCKYBALL_HIDDEN_HART_BASE must be after visible harts"
#endif

/* SCU console: per-hart UART at base + hartid*stride + offset */
static void buckyball_console_putc(char ch) {
  unsigned long hartid = current_hartid();
  volatile unsigned char *uart =
      (volatile unsigned char *)(BUCKYBALL_SCU_BASE +
                                 hartid * BUCKYBALL_SCU_STRIDE +
                                 BUCKYBALL_SCU_UART_OFFSET);
  *uart = (unsigned char)ch;
}

static bool buckyball_is_visible_hart(unsigned long hartid) {
  return hartid < BUCKYBALL_VISIBLE_HART_COUNT;
}

static void buckyball_disable_hidden_harts_in_fdt(void) {
  int cpu, cpus, err;
  u32 hartid;
  void *fdt = fdt_get_address_rw();

  if (!fdt)
    return;

  err = fdt_open_into(fdt, fdt,
                      fdt_totalsize(fdt) + 64 * BUCKYBALL_TOTAL_HART_COUNT);
  if (err < 0)
    return;

  cpus = fdt_path_offset(fdt, "/cpus");
  if (cpus < 0)
    return;

  fdt_for_each_subnode(cpu, fdt, cpus) {
    err = fdt_parse_hart_id(fdt, cpu, &hartid);
    if (err)
      continue;
    if (!buckyball_is_visible_hart(hartid))
      fdt_setprop_string(fdt, cpu, "status", "disabled");
  }
}

static bool buckyball_cold_boot_allowed(u32 hartid) {
  return hartid == 0;
}

static int buckyball_console_getc(void) {
  unsigned long hartid = current_hartid();
  unsigned long hart_base = BUCKYBALL_SCU_BASE + hartid * BUCKYBALL_SCU_STRIDE;
  volatile unsigned char *status =
      (volatile unsigned char *)(hart_base + BUCKYBALL_SCU_UART_STATUS_OFFSET);
  volatile unsigned char *rx =
      (volatile unsigned char *)(hart_base + BUCKYBALL_SCU_UART_RX_OFFSET);

  if (!(*status & BUCKYBALL_SCU_UART_RX_VALID))
    return -1;

  return *rx;
}

static struct sbi_console_device buckyball_console = {
    .name = "buckyball-scu",
    .console_putc = buckyball_console_putc,
    .console_getc = buckyball_console_getc,
};

static struct plic_data *plic;

static int buckyball_system_reset_check(u32 type, u32 reason) {
  if (type == SBI_SRST_RESET_TYPE_SHUTDOWN)
    return 1;

  return 0;
}

static void buckyball_system_reset(u32 type, u32 reason) {
  unsigned long hartid = current_hartid();
  volatile unsigned int *sim_exit =
      (volatile unsigned int *)(BUCKYBALL_SCU_BASE +
                                hartid * BUCKYBALL_SCU_STRIDE);

  (void)type;
  (void)reason;

  *sim_exit = BUCKYBALL_SIM_EXIT_SUCCESS;

  while (1)
    wfi();
}

static struct sbi_system_reset_device buckyball_reset = {
    .name = "buckyball-scu-reset",
    .system_reset_check = buckyball_system_reset_check,
    .system_reset = buckyball_system_reset,
};

static struct aclint_mswi_data mswi = {
    .addr = BUCKYBALL_CLINT_ADDR + CLINT_MSWI_OFFSET,
    .size = ACLINT_MSWI_SIZE,
    .first_hartid = 0,
    .hart_count = BUCKYBALL_VISIBLE_HART_COUNT,
};

static struct aclint_mtimer_data mtimer = {
    .mtime_freq = BUCKYBALL_MTIMER_FREQ,
    .mtime_addr = BUCKYBALL_CLINT_ADDR + CLINT_MTIMER_OFFSET +
                  ACLINT_DEFAULT_MTIME_OFFSET,
    .mtime_size = ACLINT_DEFAULT_MTIME_SIZE,
    .mtimecmp_addr = BUCKYBALL_CLINT_ADDR + CLINT_MTIMER_OFFSET +
                     ACLINT_DEFAULT_MTIMECMP_OFFSET,
    .mtimecmp_size = ACLINT_DEFAULT_MTIMECMP_SIZE,
    .first_hartid = 0,
    .hart_count = BUCKYBALL_VISIBLE_HART_COUNT,
    .has_64bit_mmio = true,
};

static int buckyball_early_init(bool cold_boot) {
  if (cold_boot) {
    sbi_console_set_device(&buckyball_console);
    sbi_system_reset_add_device(&buckyball_reset);
    aclint_mswi_cold_init(&mswi);
  }
  return 0;
}

static int buckyball_final_init(bool cold_boot) {
  if (cold_boot)
    buckyball_disable_hidden_harts_in_fdt();

  return 0;
}

static int buckyball_irqchip_init(void) {
  int i;

  plic = sbi_zalloc(PLIC_DATA_SIZE(BUCKYBALL_VISIBLE_HART_COUNT));
  if (!plic)
    return SBI_ENOMEM;

  plic->unique_id = 0;
  plic->addr = BUCKYBALL_PLIC_ADDR;
  plic->size = BUCKYBALL_PLIC_SIZE;
  plic->num_src = BUCKYBALL_PLIC_NUM_SOURCES;

  for (i = 0; i < BUCKYBALL_VISIBLE_HART_COUNT; i++) {
    plic->context_map[i][PLIC_M_CONTEXT] = i * 2;
    plic->context_map[i][PLIC_S_CONTEXT] = i * 2 + 1;
  }

  return plic_cold_irqchip_init(plic);
}

static int buckyball_timer_init(void) {
  return aclint_mtimer_cold_init(&mtimer, NULL);
}

const struct sbi_platform_operations platform_ops = {
    .cold_boot_allowed = buckyball_cold_boot_allowed,
    .early_init = buckyball_early_init,
    .final_init = buckyball_final_init,
    .irqchip_init = buckyball_irqchip_init,
    .timer_init = buckyball_timer_init,
};

const struct sbi_platform platform = {
    .opensbi_version = OPENSBI_VERSION,
    .platform_version = SBI_PLATFORM_VERSION(0x0, 0x01),
    .name = "Buckyball",
    .features = SBI_PLATFORM_DEFAULT_FEATURES,
    .hart_count = BUCKYBALL_VISIBLE_HART_COUNT,
    .hart_stack_size = SBI_PLATFORM_DEFAULT_HART_STACK_SIZE,
    .heap_size = SBI_PLATFORM_DEFAULT_HEAP_SIZE(BUCKYBALL_VISIBLE_HART_COUNT),
    .platform_ops_addr = (unsigned long)&platform_ops,
};

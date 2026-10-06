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
#include <sbi_utils/fdt/fdt_fixup.h>
#include <sbi_utils/ipi/aclint_mswi.h>
#include <sbi_utils/irqchip/plic.h>
#include <sbi_utils/timer/aclint_mtimer.h>

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
#define BUCKYBALL_SIM_EXIT_SUCCESS 0
#define BUCKYBALL_SIM_EXIT_FAILURE 1

static struct sbi_console_device buckyball_console;
extern char _payload_start[], _payload_end[];
extern char _fw_start[], _fw_end[];
// This Enable rdtime and rdcycle (in original opensbi its only rdtime, we eable 
// rdcylce to do performance counting)
#define BUCKYBALL_SCOUNTEREN_CY_TM 0x03UL

#if BUCKYBALL_VISIBLE_HART_COUNT < 1
#error "BUCKYBALL_VISIBLE_HART_COUNT must be at least 1"
#endif
#if BUCKYBALL_TOTAL_HART_COUNT < BUCKYBALL_VISIBLE_HART_COUNT
#error "BUCKYBALL_TOTAL_HART_COUNT must cover visible harts"
#endif
#if BUCKYBALL_HIDDEN_HART_BASE < BUCKYBALL_VISIBLE_HART_COUNT
#error "BUCKYBALL_HIDDEN_HART_BASE must be after visible harts"
#endif

unsigned long fw_platform_init(unsigned long arg0, unsigned long arg1,
                               unsigned long arg2, unsigned long arg3,
                               unsigned long arg4) {
  void *src = (void *)arg1;
  void *dst = (void *)BUCKYBALL_FDT_ADDR;
  int size, err;

  (void)arg0;
  (void)arg2;
  (void)arg3;
  (void)arg4;

  sbi_console_set_device(&buckyball_console);
  if (!arg1)
    sbi_panic("Buckyball: missing boot DTB\n");
  err = fdt_check_header(src);
  if (err)
    sbi_panic("Buckyball: invalid boot DTB: %s\n", fdt_strerror(err));

  size = fdt_totalsize(src);
  if (size <= 0 || size > BUCKYBALL_FDT_SIZE)
    sbi_panic("Buckyball: boot DTB exceeds staging capacity\n");
  if (BUCKYBALL_FDT_SIZE != 256UL * 1024 ||
      BUCKYBALL_FDT_ADDR < 0x80000000UL ||
      BUCKYBALL_FDT_ADDR != 0x80000000UL + BUCKYBALL_GUEST_MEMORY_BYTES - BUCKYBALL_FDT_SIZE)
    sbi_panic("Buckyball: DTB staging is outside guest RAM\n");
  if (BUCKYBALL_FDT_ADDR < (unsigned long)_payload_end &&
      BUCKYBALL_FDT_ADDR + BUCKYBALL_FDT_SIZE >
      (unsigned long)_payload_start)
    sbi_panic("Buckyball: DTB staging overlaps firmware payload\n");
  if (BUCKYBALL_FDT_ADDR < (unsigned long)_fw_end &&
      BUCKYBALL_FDT_ADDR + BUCKYBALL_FDT_SIZE > (unsigned long)_fw_start)
    sbi_panic("Buckyball: DTB staging overlaps firmware\n");
  err = fdt_open_into(src, dst, BUCKYBALL_FDT_SIZE);
  if (err)
    sbi_panic("Buckyball: DTB relocation failed: %s\n", fdt_strerror(err));
  return BUCKYBALL_FDT_ADDR;
}

/* Linux exposes one SBI hvc0 console; all callers use SCU hart 0. */
static void buckyball_console_putc(char ch) {
  volatile unsigned char *uart =
      (volatile unsigned char *)(BUCKYBALL_SCU_BASE + BUCKYBALL_SCU_UART_OFFSET);
  *uart = (unsigned char)ch;
}

static bool buckyball_is_visible_hart(unsigned long hartid) {
  return hartid < BUCKYBALL_VISIBLE_HART_COUNT;
}

static int buckyball_disable_hidden_harts_in_fdt(void) {
  int cpu, cpus, err;
  u32 hartid;
  void *fdt = fdt_get_address_rw();

  if (!fdt)
    return -FDT_ERR_BADSTATE;

  cpus = fdt_path_offset(fdt, "/cpus");
  if (cpus < 0)
    return cpus;

  fdt_for_each_subnode(cpu, fdt, cpus) {
    err = fdt_parse_hart_id(fdt, cpu, &hartid);
    if (err)
      return err;
    if (!buckyball_is_visible_hart(hartid)) {
      err = fdt_setprop_string(fdt, cpu, "status", "disabled");
      if (err)
        return err;
    }
  }
  return cpu == -FDT_ERR_NOTFOUND ? 0 : cpu;
}

static int buckyball_add_pmem_in_fdt(void *fdt) {
  fdt64_t reg[2];
  char name[40];
  int node, err;

  if (!BUCKYBALL_PMEM_SIZE)
    return BUCKYBALL_PMEM_BASE ? -FDT_ERR_BADVALUE : 0;
  if (BUCKYBALL_PMEM_BASE != 0x80000000UL + BUCKYBALL_GUEST_MEMORY_BYTES ||
      BUCKYBALL_PMEM_SIZE != (16UL << 30) - BUCKYBALL_GUEST_MEMORY_BYTES ||
      fdt_address_cells(fdt, 0) != 2 || fdt_size_cells(fdt, 0) != 2)
    return -FDT_ERR_BADVALUE;
  sbi_snprintf(name, sizeof(name), "pmem@%lx", (unsigned long)BUCKYBALL_PMEM_BASE);
  node = fdt_add_subnode(fdt, 0, name);
  if (node < 0)
    return node;
  err = fdt_setprop_string(fdt, node, "compatible", "pmem-region");
  if (err)
    return err;
  reg[0] = cpu_to_fdt64(BUCKYBALL_PMEM_BASE);
  reg[1] = cpu_to_fdt64(BUCKYBALL_PMEM_SIZE);
  return fdt_setprop(fdt, node, "reg", reg, sizeof(reg));
}

static bool buckyball_cold_boot_allowed(u32 hartid) {
  return hartid == 0;
}

static int buckyball_console_getc(void) {
  unsigned long hart_base = BUCKYBALL_SCU_BASE;
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
  (void)reason;

  /* Shutdown and reboot both end the P2E run via SCU sim_exit. */
  if (type == SBI_SRST_RESET_TYPE_SHUTDOWN ||
      type == SBI_SRST_RESET_TYPE_COLD_REBOOT ||
      type == SBI_SRST_RESET_TYPE_WARM_REBOOT)
    return 1;

  return 0;
}

static void buckyball_system_reset(u32 type, u32 reason) {
  unsigned long hartid = current_hartid();
  volatile unsigned int *sim_exit =
      (volatile unsigned int *)(BUCKYBALL_SCU_BASE +
                                hartid * BUCKYBALL_SCU_STRIDE);
  unsigned int code = BUCKYBALL_SIM_EXIT_FAILURE;

  /*
   * Clean poweroff → exit 0.
   * Reboot (incl. panic=N restart) or SYSFAIL shutdown → exit 1 so P2E fails.
   */
  if (type == SBI_SRST_RESET_TYPE_SHUTDOWN &&
      reason == SBI_SRST_RESET_REASON_NONE)
    code = BUCKYBALL_SIM_EXIT_SUCCESS;

  *sim_exit = code;

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

static int buckyball_nascent_init(void) {
#if BUCKYBALL_TILE_TASKS
  if (!buckyball_is_visible_hart(current_hartid())) {
    extern void buckyball_task_worker(void) __attribute__((noreturn));
    buckyball_task_worker();
  }
#endif
  return 0;
}

static int buckyball_early_init(bool cold_boot) {
  if (sbi_hart_priv_version(sbi_scratch_thishart_ptr()) >=
      SBI_HART_PRIV_VER_1_10)
    csr_set(CSR_SCOUNTEREN, BUCKYBALL_SCOUNTEREN_CY_TM);

  if (cold_boot) {
    sbi_console_set_device(&buckyball_console);
    sbi_system_reset_add_device(&buckyball_reset);
    aclint_mswi_cold_init(&mswi);
  }
  return 0;
}

static int buckyball_final_init(bool cold_boot) {
  if (cold_boot) {
    void *fdt = fdt_get_address_rw();
    int err;
    if (fdt != (void *)BUCKYBALL_FDT_ADDR ||
        fdt_totalsize(fdt) != BUCKYBALL_FDT_SIZE)
      return SBI_EINVAL;
    err = buckyball_disable_hidden_harts_in_fdt();
    if (!err)
      err = buckyball_add_pmem_in_fdt(fdt);
    if (err) {
      sbi_printf("Buckyball: DTB fixup failed: %s\n", fdt_strerror(err));
      return SBI_EINVAL;
    }
    fdt_fixups(fdt);
    // The staging area is 256 KiB; hand Linux the packed tree so it does not checksum the slack.
    err = fdt_pack(fdt);
    if (err) {
      sbi_printf("Buckyball: DTB pack failed: %s\n", fdt_strerror(err));
      return SBI_EINVAL;
    }
  }

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

/* mtime counts divided RTC ticks; the device tree reports that frequency. */
static int buckyball_timer_init(void) {
  unsigned long freq;
  int err = fdt_parse_timebase_frequency(fdt_get_address(), &freq);

  if (err)
    return err;
  mtimer.mtime_freq = freq;
  return aclint_mtimer_cold_init(&mtimer, NULL);
}

const struct sbi_platform_operations platform_ops = {
    .nascent_init = buckyball_nascent_init,
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
    .hart_count = BUCKYBALL_TILE_TASKS ? BUCKYBALL_TOTAL_HART_COUNT : BUCKYBALL_VISIBLE_HART_COUNT,
    .hart_stack_size = SBI_PLATFORM_DEFAULT_HART_STACK_SIZE,
    .heap_size = SBI_PLATFORM_DEFAULT_HEAP_SIZE(BUCKYBALL_VISIBLE_HART_COUNT),
    .platform_ops_addr = (unsigned long)&platform_ops,
};

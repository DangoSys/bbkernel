// SPDX-License-Identifier: BSD-2-Clause
/*
 * Buckyball OpenSBI Platform
 *
 * Console: SCU UART (per-hart at 0x60000000 + hartid*0x40000 + 0x20000)
 * IPI:     CLINT MSWI at 0x02000000
 * Timer:   CLINT MTIMER at 0x02000000
 * IRQ:     PLIC at 0x0C000000
 */

#include <sbi/riscv_asm.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_ecall_interface.h>
#include <sbi/sbi_hartmask.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_system.h>
#include <sbi_utils/ipi/aclint_mswi.h>
#include <sbi_utils/irqchip/plic.h>
#include <sbi_utils/timer/aclint_mtimer.h>

#define BUCKYBALL_HART_COUNT		64
#define BUCKYBALL_CLINT_ADDR		0x02000000
#define BUCKYBALL_PLIC_ADDR		0x0C000000
#define BUCKYBALL_PLIC_SIZE		0x04000000
#define BUCKYBALL_PLIC_NUM_SOURCES	1
#define BUCKYBALL_SCU_BASE		0x60000000UL
#define BUCKYBALL_SCU_STRIDE		0x40000UL
#define BUCKYBALL_SCU_UART_OFFSET	0x20000UL
#define BUCKYBALL_SCU_UART_RX_OFFSET	0x20004UL
#define BUCKYBALL_SCU_UART_STATUS_OFFSET 0x20005UL
#define BUCKYBALL_SCU_UART_RX_VALID	0x01
#define BUCKYBALL_MTIMER_FREQ		10000000
#define BUCKYBALL_SIM_EXIT_SUCCESS	0

/* SCU console: per-hart UART at base + hartid*stride + offset */
static void buckyball_console_putc(char ch)
{
	unsigned long hartid = current_hartid();
	volatile unsigned char *uart = (volatile unsigned char *)(
		BUCKYBALL_SCU_BASE +
		hartid * BUCKYBALL_SCU_STRIDE +
		BUCKYBALL_SCU_UART_OFFSET);
	*uart = (unsigned char)ch;
}

static int buckyball_console_getc(void)
{
	unsigned long hartid = current_hartid();
	unsigned long hart_base = BUCKYBALL_SCU_BASE +
		hartid * BUCKYBALL_SCU_STRIDE;
	volatile unsigned char *status = (volatile unsigned char *)(
		hart_base + BUCKYBALL_SCU_UART_STATUS_OFFSET);
	volatile unsigned char *rx = (volatile unsigned char *)(
		hart_base + BUCKYBALL_SCU_UART_RX_OFFSET);

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

static int buckyball_system_reset_check(u32 type, u32 reason)
{
	if (type == SBI_SRST_RESET_TYPE_SHUTDOWN)
		return 1;

	return 0;
}

static void buckyball_system_reset(u32 type, u32 reason)
{
	unsigned long hartid = current_hartid();
	volatile unsigned int *sim_exit = (volatile unsigned int *)(
		BUCKYBALL_SCU_BASE + hartid * BUCKYBALL_SCU_STRIDE);

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
	.hart_count = BUCKYBALL_HART_COUNT,
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
	.hart_count = BUCKYBALL_HART_COUNT,
	.has_64bit_mmio = true,
};

static int buckyball_early_init(bool cold_boot)
{
	if (cold_boot) {
		sbi_console_set_device(&buckyball_console);
		sbi_system_reset_add_device(&buckyball_reset);
		aclint_mswi_cold_init(&mswi);
	}
	return 0;
}

static int buckyball_final_init(bool cold_boot)
{
	return 0;
}

static int buckyball_irqchip_init(void)
{
	int i;

	plic = sbi_zalloc(PLIC_DATA_SIZE(BUCKYBALL_HART_COUNT));
	if (!plic)
		return SBI_ENOMEM;

	plic->unique_id = 0;
	plic->addr = BUCKYBALL_PLIC_ADDR;
	plic->size = BUCKYBALL_PLIC_SIZE;
	plic->num_src = BUCKYBALL_PLIC_NUM_SOURCES;

	for (i = 0; i < BUCKYBALL_HART_COUNT; i++) {
		plic->context_map[i][PLIC_M_CONTEXT] = i * 2;
		plic->context_map[i][PLIC_S_CONTEXT] = i * 2 + 1;
	}

	return plic_cold_irqchip_init(plic);
}

static int buckyball_timer_init(void)
{
	return aclint_mtimer_cold_init(&mtimer, NULL);
}

const struct sbi_platform_operations platform_ops = {
	.early_init       = buckyball_early_init,
	.final_init       = buckyball_final_init,
	.irqchip_init     = buckyball_irqchip_init,
	.timer_init       = buckyball_timer_init,
};

const struct sbi_platform platform = {
	.opensbi_version   = OPENSBI_VERSION,
	.platform_version  = SBI_PLATFORM_VERSION(0x0, 0x01),
	.name              = "Buckyball",
	.features          = SBI_PLATFORM_DEFAULT_FEATURES,
	.hart_count        = BUCKYBALL_HART_COUNT,
	.hart_stack_size   = SBI_PLATFORM_DEFAULT_HART_STACK_SIZE,
	.heap_size         = SBI_PLATFORM_DEFAULT_HEAP_SIZE(BUCKYBALL_HART_COUNT),
	.platform_ops_addr = (unsigned long)&platform_ops,
};

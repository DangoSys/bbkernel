# SPDX-License-Identifier: BSD-2-Clause

platform-cppflags-y =
platform-cflags-y =
platform-cflags-y += -DBUCKYBALL_HART_COUNT=$(BUCKYBALL_HART_COUNT)
platform-cflags-y += -DBUCKYBALL_GUEST_MEMORY_BYTES=$(BUCKYBALL_GUEST_MEMORY_BYTES)
platform-cflags-y += -DBUCKYBALL_FDT_ADDR=$(BUCKYBALL_FDT_ADDR)
platform-cflags-y += -DBUCKYBALL_FDT_SIZE=$(BUCKYBALL_FDT_SIZE)
platform-cflags-y += -DBUCKYBALL_MODEL_DDR_BASE=$(BUCKYBALL_MODEL_DDR_BASE)
platform-cflags-y += -DBUCKYBALL_MODEL_DDR_SIZE=$(BUCKYBALL_MODEL_DDR_SIZE)
platform-asflags-y =
platform-ldflags-y =

platform-objs-y += platform.o

PLATFORM_RISCV_XLEN = 64
PLATFORM_RISCV_ABI = lp64d
PLATFORM_RISCV_ISA = rv64gc
PLATFORM_RISCV_CODE_MODEL = medany

# Build fw_payload (OpenSBI + Linux Image embedded) loaded at 0x80000000.
# Linux Image is linked at offset 0x200000 (i.e. 0x80200000).
FW_TEXT_START = 0x80000000
FW_PAYLOAD = y
FW_PAYLOAD_OFFSET = 0x200000

# The BootROM passes no DTB; the kernel build embeds the chip's DTB with FW_FDT_PATH,
# which OpenSBI hands to fw_platform_init as its arg1.

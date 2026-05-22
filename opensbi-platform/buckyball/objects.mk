# SPDX-License-Identifier: BSD-2-Clause

platform-cppflags-y =
platform-cflags-y =
platform-asflags-y =
platform-ldflags-y =

platform-objs-y += platform.o

PLATFORM_RISCV_XLEN = 64
PLATFORM_RISCV_ABI = lp64d
PLATFORM_RISCV_ISA = rv64gc
PLATFORM_RISCV_CODE_MODEL = medany

# Build fw_payload (OpenSBI + Linux Image embedded) loaded at 0x80000000.
# Linux Image is linked at offset 0x200000 (i.e. 0x80200000).
FW_PAYLOAD = y
FW_PAYLOAD_OFFSET = 0x200000

# DTB is passed in a1 by the previous boot stage (Buckyball bootrom),
# so we do not need to embed an FDT.

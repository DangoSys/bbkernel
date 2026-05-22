# bbkernel — Buckyball Linux Boot

Complete OpenSBI + Linux + initramfs build for Buckyball RISC-V DSA.

## Architecture

```
Memory Layout:
  0x10000     : BootROM (+ DTB appended by Rocket Chip)
  0x02000000  : CLINT (MSWI + MTIMER)
  0x0C000000  : PLIC
  0x60000000  : SCU (per-hart UART at base + hartid*0x40000 + 0x20000)
  0x80000000  : OpenSBI fw_payload.bin
                ├─ 0x80000000: OpenSBI (M-mode, ~256KB)
                └─ 0x80200000: Linux Image (S-mode, with initramfs)

Boot Flow:
  1. BootROM (M-mode) @ 0x10040
     - Multi-core sync via CLINT MSWI
     - Jump to 0x80000000 with a0=hartid, a1=dtb
  2. OpenSBI (M-mode) @ 0x80000000
     - Init CLINT, PLIC, SCU console
     - Install SBI ecall handlers
     - mret to S-mode @ 0x80200000
  3. Linux (S-mode) @ 0x80200000
     - Parse DTB, mount initramfs
     - Console via SBI ecall (hvc0) → SCU UART
```

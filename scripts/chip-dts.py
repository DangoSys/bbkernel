#!/usr/bin/env python3
"""Generate the Linux device tree for a chip from its Chip.pb.

Every hart in the PB is listed; OpenSBI marks the hidden ones disabled at boot. Device addresses
follow the System's DeviceParams and SCU defaults (CLINT, PLIC, SCU). The timebase is the
RTC tick frequency after the CLINT clock divider.
"""

import argparse
import sys
from pathlib import Path

DRAM_BASE = 0x8000_0000
CLINT = (0x0200_0000, 0x1_0000)
PLIC = (0x0C00_0000, 0x400_0000)
PLIC_SOURCES = 1


def load_chip(repo: Path, pb: Path):
    sys.path.insert(0, str(repo / "bbdev" / "api" / "steps" / "config" / "scripts"))
    import chip_pb2

    chip = chip_pb2.Chip()
    chip.ParseFromString(pb.read_bytes())
    return chip


def isa(core) -> tuple[str, bool]:
    """ISA string and whether the core translates with Sv39."""
    cpu = core.cpu
    if cpu.WhichOneof("cpu") == "rocket":
        r = cpu.rocket
        base = "rv64i" + ("m" if r.mul_div.enable else "") + "a"
        if r.fpu.enable:
            base += "fd" if r.fpu.f_len == 64 else "f"
        base += "c"
        extensions = [name for name, on in (("zba", r.use_zba), ("zbb", r.use_zbb), ("zbs", r.use_zbs)) if on]
        return "_".join([base] + extensions), r.use_vm
    if cpu.WhichOneof("cpu") == "boom":
        return "rv64imafdc", True
    raise SystemExit(f"core {core.index}: unsupported CPU kind {cpu.kind!r}")


def cells(value: int) -> str:
    return f"0x{value >> 32:x} 0x{value & 0xFFFF_FFFF:x}"


def generate(chip, guest_bytes: int, timebase_hz: int) -> str:
    harts = sorted(chip.cores, key=lambda core: core.hart_id)
    if [core.hart_id for core in harts] != list(range(len(harts))):
        raise SystemExit("chip hart ids are not 0 until the hart count")
    cpus = []
    for core in harts:
        h = core.hart_id
        string, vm = isa(core)
        mmu = '      mmu-type = "riscv,sv39";\n' if vm else ""
        cpus.append(
            f"    cpu{h}: cpu@{h} {{\n"
            f'      device_type = "cpu";\n'
            f"      reg = <{h}>;\n"
            f'      status = "okay";\n'
            f'      compatible = "riscv";\n'
            f'      riscv,isa = "{string}";\n'
            f"{mmu}"
            f"      cpu{h}_intc: interrupt-controller {{\n"
            f"        #interrupt-cells = <1>;\n"
            f"        interrupt-controller;\n"
            f'        compatible = "riscv,cpu-intc";\n'
            f"      }};\n"
            f"    }};\n"
        )
    # CLINT: software (3) and timer (7) per hart. PLIC: context 2h is M (11), 2h+1 is S (9).
    clint_irqs = " ".join(f"&cpu{c.hart_id}_intc 3 &cpu{c.hart_id}_intc 7" for c in harts)
    plic_irqs = " ".join(f"&cpu{c.hart_id}_intc 11 &cpu{c.hart_id}_intc 9" for c in harts)
    return (
        "/dts-v1/;\n\n"
        "/ {\n"
        "  #address-cells = <2>;\n"
        "  #size-cells = <2>;\n"
        '  compatible = "buckyball,soc";\n'
        f'  model = "buckyball,{chip.name}";\n\n'
        # The kernel command line is fixed by CONFIG_CMDLINE_FORCE; OpenSBI fills /chosen.
        "  chosen {\n"
        "  };\n\n"
        "  cpus {\n"
        "    #address-cells = <1>;\n"
        "    #size-cells = <0>;\n"
        f"    timebase-frequency = <{timebase_hz}>;\n"
        + "".join(cpus)
        + "  };\n\n"
        f"  memory@{DRAM_BASE:x} {{\n"
        '    device_type = "memory";\n'
        f"    reg = <{cells(DRAM_BASE)} {cells(guest_bytes)}>;\n"
        "  };\n\n"
        "  soc {\n"
        "    #address-cells = <2>;\n"
        "    #size-cells = <2>;\n"
        '    compatible = "simple-bus";\n'
        "    ranges;\n\n"
        f"    clint@{CLINT[0]:x} {{\n"
        '      compatible = "riscv,clint0";\n'
        f"      reg = <{cells(CLINT[0])} {cells(CLINT[1])}>;\n"
        f"      interrupts-extended = <{clint_irqs}>;\n"
        "    };\n\n"
        f"    plic: interrupt-controller@{PLIC[0]:x} {{\n"
        '      compatible = "riscv,plic0";\n'
        "      #address-cells = <0>;\n"
        "      #interrupt-cells = <1>;\n"
        "      interrupt-controller;\n"
        f"      reg = <{cells(PLIC[0])} {cells(PLIC[1])}>;\n"
        f"      riscv,ndev = <{PLIC_SOURCES}>;\n"
        f"      interrupts-extended = <{plic_irqs}>;\n"
        "    };\n"
        "  };\n"
        "};\n"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--chip-pb", type=Path, required=True)
    parser.add_argument("--guest-memory-mib", type=int, required=True)
    parser.add_argument("--timebase-hz", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    chip = load_chip(args.repo, args.chip_pb)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generate(chip, args.guest_memory_mib << 20, args.timebase_hz), encoding="utf-8")


if __name__ == "__main__":
    main()

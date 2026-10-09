import argparse
import struct
from pathlib import Path

FDT_BYTES = 256 * 1024
HEADROOM = 512 * 1024 * 1024  # Kernel, page tables and userspace runtime reserve; not a measured peak.


def check(required, available, detail):
    if required > available:
        raise ValueError(f"Guest memory budget exceeded: {detail}, required={required} bytes, guest={available} bytes")
    print(f"[kernel] memory budget: {detail}, required={required} bytes, guest={available} bytes")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--guest-memory-mib", type=int, required=True)
    parser.add_argument("--resources", type=Path)
    parser.add_argument("--rootfs", type=Path)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--payload", type=Path)
    args = parser.parse_args()
    available = args.guest_memory_mib * 1024 * 1024 - FDT_BYTES
    if args.resources:
        import json
        source = args.resources
        layout = json.loads((source / "layout.json").read_text())
        paths = set()
        for value in layout["resources"].values():
            relative = Path(value)
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError(f"Invalid resource path: {value}")
            paths.add(source / relative)
        total = sum(path.stat().st_size for path in paths)
        check(2 * total + HEADROOM, available, f"resources={total}, copies=2, headroom={HEADROOM}")
    if args.rootfs:
        total = sum(p.stat().st_size for p in args.rootfs.rglob("*") if p.is_file() and not p.is_symlink())
        image = args.image.stat().st_size if args.image else total
        if args.image:
            with args.image.open("rb") as source:
                header = source.read(64)
            if len(header) != 64 or header[48:56] != b"RISCV\0\0\0":
                raise ValueError("Expected a RISC-V Linux Image header")
            span = struct.unpack_from("<Q", header, 16)[0]
            if span >= 1024 * 1024 * 1024:
                raise ValueError(f"RISC-V early kernel mapping exceeds one 1 GiB PUD: span={span}; "
                                 "compress the embedded initramfs or use external model storage")
        check(total + image + HEADROOM, available, f"rootfs={total}, image_or_second_copy={image}, headroom={HEADROOM}")
    if args.payload:
        size = args.payload.stat().st_size
        check(size, available, f"payload={size}, FDT-reserved={FDT_BYTES}")


if __name__ == "__main__":
    main()

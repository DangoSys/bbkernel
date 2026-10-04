"""Prepare the explicit P2E read-only model partition and its load manifest."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

BLOCK = 4096
DDR_BYTES = 16 * 1024**3
FDT_BYTES = 256 * 1024


def resources(source):
    layout = json.loads((source / "layout.json").read_text())
    if layout["execution"]["p2e"]["kind"] != "host-worker":
        raise ValueError("pmem requires host-worker execution")
    result = []
    for name in sorted(set(layout["resources"].values())):
        path = Path(name)
        if path.is_absolute() or ".." in path.parts or not path.parts or "\n" in name or "\t" in name or path.suffix == ".rax":
            raise ValueError(f"Invalid pmem resource: {name!r}")
        file = source / path
        if file.is_symlink() or not file.is_file():
            raise ValueError(f"pmem resource must be a regular file: {file}")
        result.append((name, file.stat().st_size))
    if not result:
        raise ValueError("Empty pmem resource list")
    return result


def geometry(files, guest_mib):
    if not 1 <= guest_mib < 16384:
        raise ValueError("pmem guest-memory-mib must be in 1..16383")
    offset = guest_mib * 1024**2
    # Explicit filesystem allowance; mke2fs failure is terminal, never resize/retry silently.
    file_blocks = sum((size + BLOCK - 1) // BLOCK for _, size in files)
    blocks = file_blocks + (file_blocks + 19) // 20 + 64 * 1024**2 // BLOCK
    size = blocks * BLOCK
    if size > DDR_BYTES - offset:
        raise ValueError(f"pmem image cannot fit: resources={sum(s for _, s in files)} image={size} capacity={DDR_BYTES-offset} offset={offset} bytes")
    return offset, size


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("check", "image", "manifest"))
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--guest-memory-mib", type=int, required=True)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--staging", type=Path)
    parser.add_argument("--boot", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    files = resources(args.source)
    offset, size = geometry(files, args.guest_memory_mib)
    print(f"[pmem] resources={sum(s for _, s in files)} image={size} offset={offset} PA={0x80000000+offset} capacity={DDR_BYTES-offset} bytes")
    if args.mode == "check":
        return
    if args.mode == "image":
        if args.image is None or args.staging is None:
            parser.error("image requires --image and --staging")
        if args.staging.exists():
            shutil.rmtree(args.staging)
        args.staging.mkdir(parents=True)
        for name, _ in files:
            target = args.staging / name
            target.parent.mkdir(parents=True, exist_ok=True)
            os.link(args.source / name, target)  # No cross-device copy fallback.
        args.image.parent.mkdir(parents=True, exist_ok=True)
        temporary = args.image.with_suffix(args.image.suffix + ".new")
        with temporary.open("wb") as stream:
            stream.truncate(size)
        inodes = len(list(args.staging.rglob("*"))) + 32
        command = ["mke2fs", "-F", "-t", "ext2", "-b", str(BLOCK), "-m", "0",
                   "-N", str(inodes), "-O", "none,filetype,sparse_super,large_file",
                   "-E", "root_owner=0:0", "-d", str(args.staging), str(temporary), str(size // BLOCK)]
        print("[pmem] " + " ".join(command), flush=True)
        subprocess.run(command, check=True)
        subprocess.run(["e2fsck", "-f", "-n", str(temporary)], check=True)
        if resources(args.source) != files or temporary.stat().st_size != size:
            raise ValueError("Resource byte list or image size changed during image creation")
        temporary.replace(args.image)
        args.image.with_suffix(".files.json").write_text(json.dumps({"block_bytes": BLOCK, "files": [{"path": n, "size": s} for n, s in files]}, indent=2) + "\n")
    else:
        if args.image is None or args.boot is None or args.out is None:
            parser.error("manifest requires --image, --boot and --out")
        recorded = json.loads(args.image.with_suffix(".files.json").read_text())
        expected = {"block_bytes": BLOCK, "files": [{"path": n, "size": s} for n, s in files]}
        if recorded != expected:
            raise ValueError("Image resource byte manifest differs from current model layout")
        if args.image.stat().st_size != size:
            raise ValueError(f"Image size mismatch: actual={args.image.stat().st_size}, expected={size} bytes")
        boot_size = args.boot.stat().st_size
        boot_limit = offset - FDT_BYTES
        if boot_size == 0 or boot_size > boot_limit:
            raise ValueError(f"Boot image must end before FDT staging: boot={boot_size}, limit={boot_limit}, reserved={FDT_BYTES} bytes")
        records = []
        for role, file, start in (("boot", args.boot, 0), ("model", args.image, offset)):
            records.append({"role": role, "file": str(file.resolve(strict=True)), "offset": start,
                            "size": file.stat().st_size, "sha256": digest(file), "format": "binary"})
        args.out.write_text(json.dumps({"version": 1, "ddr_base": 0x80000000, "ddr_size": DDR_BYTES,
            "guest_memory_bytes": offset, "pmem_base": 0x80000000 + offset,
            "fdt_base": 0x80000000 + offset - FDT_BYTES, "fdt_size": FDT_BYTES,
            "pmem_size": DDR_BYTES - offset, "loads": records}, indent=2) + "\n")


if __name__ == "__main__":
    main()

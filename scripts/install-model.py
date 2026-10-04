import argparse
import json
import shlex
import shutil
import tomllib
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("binary")
    parser.add_argument("destination", type=Path)
    parser.add_argument("config", type=Path)
    parser.add_argument("--dataset", default="")
    parser.add_argument("--guest-memory-mib", type=int, default=512)
    parser.add_argument("--model-storage", choices=("initramfs", "pmem"), default="initramfs")
    parser.add_argument("--worker-cpus", type=int, nargs="+")
    args = parser.parse_args()
    source, binary, destination, config, dataset = args.source, args.binary, args.destination, args.config, args.dataset
    layout = json.loads((source / "layout.json").read_text())
    execution = layout["execution"]
    has_p2e = "p2e" in execution
    if has_p2e:
        execution = execution["p2e"]
    if execution["kind"] == "host-worker":
        if execution.get("protocol") != "bbmux1" or execution.get("workers") != 8:
            raise ValueError("host-worker requires protocol=bbmux1 and workers=8")
        if (args.worker_cpus is None or len(args.worker_cpus) != execution["workers"] or
                len(set(args.worker_cpus)) != len(args.worker_cpus) or min(args.worker_cpus) < 0):
            raise ValueError("host-worker requires one distinct explicit CPU ID per worker")
        if args.model_storage == "initramfs":
            subprocess.run([sys.executable, str(Path(__file__).with_name("memory-budget.py")),
                            "--resources", str(source), "--guest-memory-mib", str(args.guest_memory_mib)], check=True)
        else:
            subprocess.run([sys.executable, str(Path(__file__).with_name("pmem-image.py")), "check",
                            "--source", str(source), "--guest-memory-mib", str(args.guest_memory_mib)], check=True)
        if dataset:
            raise ValueError("host-worker inputs are supplied by the host, not a dataset launcher")
    elif execution["kind"] != "native":
        raise ValueError("kernel images require a native model")
    if args.model_storage == "pmem" and execution["kind"] != "host-worker":
        raise ValueError("pmem storage requires host-worker")
    (source / binary).resolve(strict=True)
    settings = tomllib.loads(config.read_text()) if execution["kind"] == "native" else None
    if has_p2e:
        destination.mkdir(parents=True, exist_ok=True)
        installed = [binary, "layout.json"]
        if args.model_storage == "initramfs":
            installed.extend(layout["resources"].values())
        for value in installed:
            relative = Path(value)
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError("invalid native artifact path")
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source / relative, target)
    else:
        shutil.copytree(source, destination, dirs_exist_ok=True)
    if execution["kind"] == "host-worker":
        model_dir = "/model" if args.model_storage == "pmem" else "/root"
        if args.model_storage == "pmem":
            records = []
            for name in sorted(set(layout["resources"].values())):
                records.append(f"{name}\t{(source / name).stat().st_size}\n")
            (destination / "model-resources.list").write_text("".join(records))
        launcher = destination / "run-model"
        prepare = ""
        if args.model_storage == "pmem":
            prepare = "mount -t ext2 -o ro,dax /dev/pmem0 /model\n/root/guest/dax-check /model /root/model-resources.list\n"
        launcher.write_text("#!/bin/sh\nset -eu\n" + prepare + "exec /root/guest/worker-mux " +
                            shlex.join(["/root/" + binary, model_dir, *map(str, args.worker_cpus)]) + "\n")
        launcher.chmod(0o755)
        return
    commands = []
    if dataset:
        arguments = [arg for arg in execution["arguments"] if arg != "--image"]
        commands.append(["./" + binary, *arguments, "--dataset", dataset])
    else:
        inputs = settings["inputs"]
        if execution["input"] == "resource":
            if inputs != execution["reference_inputs"]:
                raise ValueError("kernel requests differ from prepared model inputs")
            inputs = execution["inputs"]
        for index, value in enumerate(inputs):
            if execution["input"] == "file":
                path = (config.parent / value).resolve(strict=True)
                target = Path("inputs") / f"{index}{path.suffix}"
                (destination / target).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, destination / target)
                value = str(target)
            elif execution["input"] == "resource":
                path = Path(value)
                if path.is_absolute() or ".." in path.parts:
                    raise ValueError("invalid prepared input resource")
                (source / path).resolve(strict=True)
            elif execution["input"] != "text":
                raise ValueError(f"unknown input kind: {execution['input']}")
            commands.append(["./" + binary, *execution["arguments"], value])
    launcher = destination / "run-model"
    launcher.write_text("#!/bin/sh\nset -eu\ncd /root\n" +
                        "\n".join(shlex.join(command) for command in commands) + "\n")
    launcher.chmod(0o755)


if __name__ == "__main__":
    main()

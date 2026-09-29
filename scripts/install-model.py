import argparse
import json
import shlex
import shutil
import tomllib
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("binary")
    parser.add_argument("destination", type=Path)
    parser.add_argument("config", type=Path)
    parser.add_argument("--dataset", default="")
    args = parser.parse_args()
    source, binary, destination, config, dataset = args.source, args.binary, args.destination, args.config, args.dataset
    execution = json.loads((source / "layout.json").read_text())["execution"]
    if execution["kind"] != "native":
        raise ValueError("kernel images require a native model")
    (source / binary).resolve(strict=True)
    settings = tomllib.loads(config.read_text())
    shutil.copytree(source, destination, dirs_exist_ok=True)
    commands = []
    if dataset:
        arguments = [arg for arg in execution["arguments"] if arg != "--image"]
        commands.append(["./" + binary, *arguments, "--dataset", dataset])
    else:
        for index, value in enumerate(settings["inputs"]):
            if execution["input"] == "file":
                path = (config.parent / value).resolve(strict=True)
                target = Path("inputs") / f"{index}{path.suffix}"
                (destination / target).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, destination / target)
                value = str(target)
            elif execution["input"] != "text":
                raise ValueError(f"unknown input kind: {execution['input']}")
            commands.append(["./" + binary, *execution["arguments"], value])
    launcher = destination / "run-model"
    launcher.write_text("#!/bin/sh\nset -eu\ncd /root\n" +
                        "\n".join(shlex.join(command) for command in commands) + "\n")
    launcher.chmod(0o755)


if __name__ == "__main__":
    main()

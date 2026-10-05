#!/usr/bin/env python3
"""Extract workload stems from a regression workloads-*.toml into a line list."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def extract_tests(toml_text: str) -> list[str]:
    import tomllib

    tests = tomllib.loads(toml_text)["workloads"]["tests"]
    if not tests:
        raise ValueError("no tests found in toml")
    return tests


def bindings(tests: list[str], placement: dict) -> str:
    rows = []
    for program in tests:
        targets = {
            hart["target"]
            for hart in placement["harts"]
            if any(
                program.startswith(f"{placement['name']}-{hart['target']}-{kind}-")
                for kind in ("ctest", "mlirtest", "soctest")
            )
        }
        if len(targets) != 1:
            raise ValueError(
                f"workload must identify one registered core profile: {program}"
            )
        target = targets.pop()
        hart = min(h["hart_id"] for h in placement["harts"] if h["target"] == target)
        rows.append(f"{hart}\t{program}")
    return "\n".join(rows) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("toml", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--suffix", default="")
    parser.add_argument("--placement", type=Path)
    parser.add_argument("--bindings", type=Path)
    args = parser.parse_args()

    if not args.toml.is_file():
        print(f"error: toml not found: {args.toml}", file=sys.stderr)
        return 1

    try:
        tests = [
            stem
            for stem in extract_tests(args.toml.read_text())
            if stem.endswith(args.suffix)
        ]
        if not tests:
            raise ValueError("no workloads match the requested suffix")
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if args.placement is not None:
        if args.bindings is None:
            parser.error("--placement requires --bindings")
        args.bindings.write_text(
            bindings(tests, json.loads(args.placement.read_text()))
        )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(tests) + "\n")
    print(f"wrote {len(tests)} stems to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

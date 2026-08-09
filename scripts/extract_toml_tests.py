#!/usr/bin/env python3
"""Extract workload stems from a regression workloads-*.toml into a line list."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def extract_tests(toml_text: str) -> list[str]:
    tests: list[str] = []
    in_tests = False
    for raw in toml_text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("tests"):
            in_tests = True
            continue
        if not in_tests:
            continue
        if line.startswith("]"):
            break
        if not (line.startswith('"') and line.endswith('",') or line.endswith('"')):
            raise ValueError(f"unexpected tests entry: {raw}")
        stem = line.strip().rstrip(",").strip()
        if not (stem.startswith('"') and stem.endswith('"')):
            raise ValueError(f"unexpected tests entry: {raw}")
        stem = stem[1:-1]
        if not stem:
            raise ValueError("empty test stem in toml")
        tests.append(stem)
    if not tests:
        raise ValueError("no tests found in toml")
    return tests


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("toml", type=Path)
    parser.add_argument("out", type=Path)
    args = parser.parse_args()

    if not args.toml.is_file():
        print(f"error: toml not found: {args.toml}", file=sys.stderr)
        return 1

    try:
        tests = extract_tests(args.toml.read_text())
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(tests) + "\n")
    print(f"wrote {len(tests)} stems to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

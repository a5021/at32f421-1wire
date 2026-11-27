#!/usr/bin/env python3
"""Check the demo's register constants against the firmware register map."""

import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "manager.h"
DEMO = ROOT / "tools" / "onewire_demo.py"


def defines(path):
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(
        r"^\s*#\s*define\s+([A-Z][A-Z0-9_]*)\s+"
        r"(0x[0-9A-Fa-f]+|[0-9]+)[uUlL]*\b",
        re.MULTILINE,
    )
    return {name: int(value, 0) for name, value in pattern.findall(text)}


def main():
    spec = importlib.util.spec_from_file_location("onewire_demo", DEMO)
    demo = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(demo)

    expected = defines(HEADER)
    names = sorted(
        name for name in dir(demo) if name.startswith(("REG_", "FLAG_"))
    )
    errors = []
    for name in names:
        if name not in expected:
            errors.append(f"{name}: missing from {HEADER}")
        elif getattr(demo, name) != expected[name]:
            errors.append(
                f"{name}: demo={getattr(demo, name)}, firmware={expected[name]}"
            )
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"register map constants: {len(names)} match")
    return 0


if __name__ == "__main__":
    sys.exit(main())

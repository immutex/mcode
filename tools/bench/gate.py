#!/usr/bin/env python3
"""Gate benchmark output against budgets.toml.

Reads `key=value` lines from mcode_bench (or a file) and checks each gated
metric. Exits non-zero with a per-metric report on any breach.

    mcode_bench 50 8 | python tools/bench/gate.py
    python tools/bench/gate.py --report bench-output.txt

Two gate kinds, because the two metric families behave differently (docs/28):

  ceiling   value <= limit          -- a documented budget
  baseline  within tolerance of expected -- a regression gate, deterministic
                                            metrics only
"""

from __future__ import annotations

import argparse
import sys
import tomllib
from pathlib import Path

BUDGETS = Path(__file__).with_name("budgets.toml")


def parse_output(text: str) -> dict[str, float]:
    metrics: dict[str, float] = {}

    for line in text.splitlines():
        line = line.strip()

        if not line or "=" not in line:
            continue

        key, _, value = line.partition("=")

        try:
            metrics[key.strip()] = float(value.strip())
        except ValueError:
            continue

    return metrics


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--report",
        type=Path,
        help="Read benchmark output from a file instead of stdin.",
    )
    parser.add_argument(
        "--budgets",
        type=Path,
        default=BUDGETS,
        help="Budget file (default: tools/bench/budgets.toml).",
    )
    arguments = parser.parse_args()

    if arguments.report:
        text = arguments.report.read_text(encoding="utf-8")
    else:
        text = sys.stdin.read()

    metrics = parse_output(text)

    if not metrics:
        print("gate: no metrics found in input", file=sys.stderr)
        return 2

    with arguments.budgets.open("rb") as handle:
        budgets = tomllib.load(handle)

    # Every table in the budget file is a group of gated metrics. A metric may
    # appear in more than one group (a deterministic baseline AND a documented
    # ceiling), and both must be checked -- keeping only one would silently drop
    # the budget.
    gates: dict[str, list[dict]] = {}

    for group in budgets.values():
        if isinstance(group, dict):
            for name, rule in group.items():
                if isinstance(rule, dict) and "kind" in rule:
                    gates.setdefault(name, []).append(rule)

    failures: list[str] = []
    checked = 0

    for name, rules in sorted(gates.items()):
        if name not in metrics:
            # load_total_ms is only emitted for a specific extension count, so a
            # missing metric is not a failure -- it is a metric this run does not
            # produce.
            continue

        value = metrics[name]

        for rule in rules:
            checked += 1
            kind = rule["kind"]

            if kind == "ceiling":
                limit = float(rule["limit"])

                if value > limit:
                    failures.append(f"{name}: {value:g} exceeds the ceiling {limit:g}")
                else:
                    print(f"  ok    {name} = {value:g} (<= {limit:g})")

            elif kind == "baseline":
                expected = float(rule["expected"])
                tolerance = float(rule.get("tolerance", 0.05))
                low = expected * (1.0 - tolerance)
                high = expected * (1.0 + tolerance)

                if not (low <= value <= high):
                    delta = (value - expected) / expected * 100.0
                    failures.append(
                        f"{name}: {value:g} is {delta:+.1f}% from the baseline {expected:g} "
                        f"(tolerance {tolerance * 100:.0f}%)"
                    )
                else:
                    print(f"  ok    {name} = {value:g} (baseline {expected:g})")

            else:
                failures.append(f"{name}: unknown gate kind {kind!r}")

    if checked == 0:
        print("gate: no gated metrics were produced by this run", file=sys.stderr)
        return 2

    if failures:
        print(f"\ngate: {len(failures)} metric(s) out of budget", file=sys.stderr)

        for failure in failures:
            print(f"  FAIL  {failure}", file=sys.stderr)

        return 1

    print(f"\ngate: {checked} metric(s) within budget")

    return 0


if __name__ == "__main__":
    sys.exit(main())

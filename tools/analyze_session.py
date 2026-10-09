#!/usr/bin/env python3
"""Dissect an mcode session log end to end.

Reports what a harness can act on: tool usage and outcomes, wasted calls,
reasoning behaviour (length, repetition, loops), and the token/cost shape of
the run. Reads the JSONL session log written under the state directory.
"""

import json
import sys
from collections import Counter, defaultdict


def load(path):
    events = []
    with open(path, encoding="utf-8", errors="replace") as handle:
        for number, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError:
                print(f"  [malformed line {number}]", file=sys.stderr)
    return events


def payload(event):
    raw = event.get("payload")
    if isinstance(raw, str):
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return {}
    return raw or {}


def section(title):
    print()
    print("=" * 78)
    print(title)
    print("=" * 78)


def main(path):
    events = load(path)
    print(f"session: {path}")
    print(f"events:  {len(events)}")

    kinds = Counter(e.get("kind") for e in events)

    section("1. EVENT MIX")
    for kind, count in kinds.most_common():
        print(f"  {kind:24s} {count}")

    # ---- tools -------------------------------------------------------------
    calls = [e for e in events if e.get("kind") == "tool.call"]
    outputs = [e for e in events if e.get("kind") == "tool.output"]
    results = [e for e in events if e.get("kind") == "tool.result"]

    section("2. TOOL USAGE")
    histogram = Counter(payload(e).get("tool") for e in calls)
    for tool, count in histogram.most_common():
        print(f"  {tool:20s} {count}")
    print(f"  {'TOTAL':20s} {len(calls)}")

    section("3. TOOL ARGUMENTS (each call)")
    for index, event in enumerate(calls):
        body = payload(event)
        args = body.get("args", {})
        rendered = json.dumps(args)[:150]
        print(f"  {index:3d} {body.get('tool',''):12s} {rendered}")

    section("4. TOOL OUTCOMES")
    failures = 0
    sizes = []
    for event in results:
        body = payload(event)
        if not body.get("ok", True):
            failures += 1
            print(f"  FAIL {body.get('tool','?'):12s} {json.dumps(body)[:160]}")
    for event in outputs:
        body = payload(event)
        sizes.append((body.get("tool", "?"), len(body.get("content", ""))))
    print(f"  calls={len(calls)} results={len(results)} outputs={len(outputs)} failures={failures}")

    section("5. OUTPUT SIZE BY TOOL (bytes returned)")
    by_tool = defaultdict(list)
    for tool, size in sizes:
        by_tool[tool].append(size)
    for tool, values in sorted(by_tool.items(), key=lambda kv: -sum(kv[1])):
        total = sum(values)
        print(f"  {tool:20s} n={len(values):3d} total={total:8d} mean={total // max(len(values),1):8d} max={max(values):8d}")
    grand = sum(size for _, size in sizes)
    print(f"  {'TOTAL':20s} {grand} bytes ({grand/1024:.1f} KiB)")

    # ---- repeated work -----------------------------------------------------
    section("6. REPEATED CALLS (same tool + same args)")
    seen = Counter()
    for event in calls:
        body = payload(event)
        key = (body.get("tool"), json.dumps(body.get("args", {}), sort_keys=True))
        seen[key] += 1
    repeats = [(k, v) for k, v in seen.items() if v > 1]
    if not repeats:
        print("  none")
    for (tool, args), count in sorted(repeats, key=lambda kv: -kv[1]):
        print(f"  x{count} {tool:12s} {args[:130]}")

    # ---- reasoning ---------------------------------------------------------
    thinking = [payload(e).get("text", "") for e in events if e.get("kind") == "message.thinking"]
    section("7. REASONING")
    if not thinking:
        print("  none recorded")
    else:
        lengths = [len(t) for t in thinking]
        print(f"  responses with reasoning: {len(thinking)}")
        print(f"  chars: total={sum(lengths)} mean={sum(lengths)//len(lengths)} max={max(lengths)}")
        for index, text in enumerate(thinking):
            flag = ""
            if len(text) > 6000:
                flag = "  <== LONG"
            # crude loop signal: is the tail periodic?
            unit = detect_period(text)
            if unit:
                flag += f"  <== PERIODIC tail, unit={unit}"
            print(f"  {index:3d} {len(text):7d} chars{flag}")

    section("8. PERIODIC-TAIL SCAN (thinking, per response)")
    loops = 0
    for index, text in enumerate(thinking):
        unit = detect_period(text)
        if unit:
            loops += 1
            print(f"  response {index}: period {unit}")
            print(f"    sample: {text[-unit-20:-unit+40]!r}")
    print(f"  periodic responses: {loops}/{len(thinking)}")

    # ---- assistant text ----------------------------------------------------
    assistant = [payload(e) for e in events if e.get("kind") == "message.assistant"]
    section("9. ASSISTANT MESSAGES")
    total_text = 0
    total_blocks = 0
    for index, message in enumerate(assistant):
        blocks = message.get("blocks", [])
        text = "".join(b.get("text", "") for b in blocks if b.get("kind") == "text")
        calls_made = [b for b in blocks if b.get("kind") == "tool_call"]
        total_text += len(text)
        total_blocks += len(blocks)
        kinds_here = Counter(b.get("kind") for b in blocks)
        print(f"  {index:3d} blocks={dict(kinds_here)} text={len(text)} chars")
    print(f"  total assistant text: {total_text} chars")

    section("10. RUN SUMMARY")
    for event in events:
        if event.get("kind") == "run.end":
            body = payload(event)
            for key, value in body.items():
                print(f"  {key:22s} {value}")
            cached = body.get("cached_read_tokens", 0)
            fresh = body.get("input_tokens", 0)
            write = body.get("cache_write_tokens", 0)
            denominator = cached + fresh + write
            if denominator:
                print(f"  {'cache_hit_rate':22s} {cached/denominator*100:.1f}%")

    section("11. TIMING")
    stamps = [e.get("timestamp_ms", 0) for e in events if e.get("timestamp_ms")]
    if stamps:
        span = (max(stamps) - min(stamps)) / 1000.0
        print(f"  wall span: {span:.1f}s across {len(stamps)} events")
        gaps = []
        for previous, current in zip(stamps, stamps[1:]):
            gaps.append(current - previous)
        gaps.sort(reverse=True)
        print("  largest gaps between events (ms):")
        for gap in gaps[:8]:
            print(f"    {gap}")


def detect_period(text, minimum_unit=30, minimum_copies=2):
    """Length of the shortest unit repeating at the tail, or 0."""
    if len(text) < minimum_unit * minimum_copies:
        return 0
    longest = len(text) // minimum_copies
    for unit in range(minimum_unit, longest + 1):
        tail = text[-unit:]
        copies = 1
        cursor = len(text) - unit
        while cursor >= unit and text[cursor - unit:cursor] == tail:
            copies += 1
            cursor -= unit
        if copies >= minimum_copies:
            return unit
    return 0


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "session.jsonl")

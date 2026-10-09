#!/usr/bin/env python3
"""Compare two mcode session logs on the axes a harness change should move."""

import json
import sys
from collections import Counter


def load(path):
    events = []
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return events


def body(event):
    raw = event.get("payload")
    if isinstance(raw, str):
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return {}
    return raw or {}


def summarize(path):
    events = load(path)
    out = {
        "path": path,
        "events": len(events),
        "tool_calls": 0,
        "tools": Counter(),
        "failures": 0,
        "failed_tools": Counter(),
        "thinking_chars": 0,
        "thinking_blocks": 0,
        "requests": 0,
        "cache_hits": [],
        "fresh_input": 0,
        "cached_input": 0,
        "output_tokens": 0,
        "cost": 0.0,
        "reasoning_tokens": 0,
    }

    for event in events:
        kind = event.get("kind")
        payload = body(event)

        if kind == "tool.call":
            out["tool_calls"] += 1
            out["tools"][payload.get("tool")] += 1
        elif kind == "tool.result":
            if payload.get("ok") is False:
                out["failures"] += 1
                out["failed_tools"][payload.get("tool") or "?"] += 1
        elif kind == "message.thinking":
            text = payload.get("text", "")
            out["thinking_chars"] += len(text)
            out["thinking_blocks"] += 1
        elif kind == "model.usage":
            out["requests"] += 1
            inp = payload.get("input", 0)
            cached = payload.get("cached_read", 0)
            out["fresh_input"] += inp
            out["cached_input"] += cached
            out["output_tokens"] += payload.get("output", 0)
            out["cost"] += payload.get("cost_usd", 0.0)
            out["reasoning_tokens"] += payload.get("reasoning", 0)
            if inp:
                out["cache_hits"].append(cached / inp)

    return out


def main(first, second):
    left = summarize(first)
    right = summarize(second)

    def row(label, a, b, better=None):
        delta = ""
        if isinstance(a, (int, float)) and isinstance(b, (int, float)) and a:
            change = (b - a) / a * 100
            arrow = "="
            if better == "lower":
                arrow = "BETTER" if b < a else ("worse" if b > a else "=")
            elif better == "higher":
                arrow = "BETTER" if b > a else ("worse" if b < a else "=")
            delta = f"{change:+.1f}%  {arrow}"
        print(f"  {label:26s} {str(a):>10s} {str(b):>10s}   {delta}")

    print(f"  {'':26s} {'run 1':>10s} {'run 2':>10s}")
    print("  " + "-" * 62)
    row("events", left["events"], right["events"])
    row("tool calls", left["tool_calls"], right["tool_calls"], "lower")
    row("tool failures", left["failures"], right["failures"], "lower")
    row("thinking blocks", left["thinking_blocks"], right["thinking_blocks"])
    row("thinking chars", left["thinking_chars"], right["thinking_chars"])
    row("provider requests", left["requests"], right["requests"], "lower")
    row("input tokens", left["fresh_input"], right["fresh_input"], "lower")
    row("  of which cached", left["cached_input"], right["cached_input"])
    row("output tokens", left["output_tokens"], right["output_tokens"])
    row("cost (USD)", round(left["cost"], 4), round(right["cost"], 4), "lower")

    print()
    print("  tool mix:")
    for tool in sorted(set(left["tools"]) | set(right["tools"])):
        a = left["tools"].get(tool, 0)
        b = right["tools"].get(tool, 0)
        print(f"    {tool:20s} {a:>4d} -> {b:>4d}")

    if left["failed_tools"] or right["failed_tools"]:
        print("  failures by tool:")
        for tool in sorted(set(left["failed_tools"]) | set(right["failed_tools"])):
            print(f"    {tool:20s} {left['failed_tools'].get(tool,0):>4d} -> "
                  f"{right['failed_tools'].get(tool,0):>4d}")

    for label, data in (("run 1", left), ("run 2", right)):
        hits = data["cache_hits"]
        if hits:
            mean = sum(hits) / len(hits)
            print(f"  {label} per-request cache hit: first={hits[0]*100:.1f}% "
                  f"mean={mean*100:.1f}% max={max(hits)*100:.1f}%")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

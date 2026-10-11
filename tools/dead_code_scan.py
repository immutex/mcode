#!/usr/bin/env python3
"""Dead-code scan: header-declared functions with no reference outside their own signature.

A false positive here costs a deleted function that something needed, so this
reports rather than deletes. Every occurrence counts, including a bare name: a
function pointer (`&handle_bash`) has no parens, so a paren-only search reports
live functions as dead.
"""

import os
import re
import sys

IDENT = re.compile(r"\b([a-z_][a-z0-9_]{4,})\b")

# The declaration and the definition both look like this; a call does not.
SIGNATURE = re.compile(
    r"^\s*(?:\[\[nodiscard\]\]\s+)?(?:virtual\s+|static\s+|inline\s+|constexpr\s+)*"
    r"auto\s+([a-z_][a-z0-9_]{4,})\s*\("
)

SCAN_ROOTS = ("src", "tests", "extensions")
SUFFIXES = (".cxx", ".hxx", ".luau")


def main():
    occurrences = {}
    headers = {}

    for base in SCAN_ROOTS:
        for root, _, names in os.walk(base):
            for name in names:
                if not name.endswith(SUFFIXES):
                    continue

                path = os.path.join(root, name).replace("\\", "/")

                for number, line in enumerate(
                    open(path, encoding="utf-8", errors="replace"), 1
                ):
                    if line.strip().startswith("//"):
                        continue

                    signature = SIGNATURE.match(line)
                    is_signature = signature.group(1) if signature else None

                    for match in IDENT.finditer(line):
                        occurrences.setdefault(match.group(1), []).append(
                            (path, number, is_signature == match.group(1))
                        )

                    if path.endswith(".hxx") and is_signature:
                        headers.setdefault(is_signature, set()).add(path)

    dead = []
    for name, where in headers.items():
        if name.startswith(("operator", "~")):
            continue

        uses = [site for site in occurrences.get(name, []) if not site[2]]
        if uses:
            continue

        dead.append((name, sorted(where)[0], len(occurrences.get(name, []))))

    dead.sort()
    print(f"header-declared, never referenced outside a signature: {len(dead)}")
    for name, header, count in dead:
        print(f"  {name:34s} {count} site(s)  {header}")

    return 0


if __name__ == "__main__":
    sys.exit(main())

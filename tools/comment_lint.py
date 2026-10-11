#!/usr/bin/env python3
"""Comment discipline for mcode.

The rule (AGENTS.md, "Comments"): a comment exists only when a reader would
otherwise get the code wrong. Everything else is deleted, not reworded.

This gate is deliberately mechanical. Judgment calls are what let the ratio
drift back, so the checks below are the ones that have no legitimate exception:
history narration, restated names, section banners, and the ratio itself.
"""

import os
import re
import sys

SOURCE_SUFFIXES = (".cxx", ".hxx")

# A comment may not narrate the code's history. The reader has the current
# source; what it used to do is in the log, not here.
#
# "before the" and "is now" are deliberately absent: both are ordinary in a
# present-tense sentence about ordering or current state ("set before the first
# mode change"), and a false positive here pushes an accurate comment toward a
# worse rewording.
HISTORY = re.compile(
    r"//.*\b("
    r"it was|they were|used to|previously|formerly|before this|"
    r"the old|the original|was declared|was written|was set only|was a plain|"
    r"it had|that made|this replaced|no longer|had already|"
    r"this used to|we now|changed to|renamed from"
    r")\b",
    re.IGNORECASE,
)

# A comment may not restate what the next line already says.
NARRATION = re.compile(
    r"^\s*//\s*(the|a|an)?\s*(loop|iterate|return|set|get|check|call|create|"
    r"build|open|close|read|write|init|increment|decrement|append|push|pop)\w*\b.*\b"
    r"(here|now|next|below|above)\b",
    re.IGNORECASE,
)

# Section banners and decorative dividers.
BANNER = re.compile(r"^\s*//\s*[-=*#_]{4,}\s*$")
DECORATED = re.compile(r"^\s*//\s*[-=*#_]{2,}\s*\w")

# A comment citing a design doc. The doc cites the code; the code does not cite
# the doc, because the doc moves and the comment does not.
DOC_CITATION = re.compile(r"//.*\bdocs?/\d{2}\b")

# Rationale blocks. One line of justification is often load-bearing; a paragraph
# is a design doc that was left in the wrong file.
MAX_BLOCK = 4


def scan(path):
    problems = []
    lines = open(path, encoding="utf-8", errors="replace").read().split("\n")

    code = comment = 0
    run_start = None

    for index, line in enumerate(lines):
        stripped = line.strip()
        is_comment = stripped.startswith("//")

        if is_comment:
            comment += 1
        elif stripped:
            code += 1

        if not is_comment and run_start is not None:
            length = index - run_start
            if length > MAX_BLOCK:
                problems.append((run_start + 1, f"{length}-line comment block (max {MAX_BLOCK})"))
            run_start = None
        elif is_comment and run_start is None:
            run_start = index

        if not is_comment:
            continue

        if HISTORY.search(line):
            problems.append((index + 1, "narrates history"))
        if NARRATION.search(line):
            problems.append((index + 1, "narrates the next line"))
        if BANNER.match(line) or DECORATED.match(line):
            problems.append((index + 1, "section banner"))
        if DOC_CITATION.search(line):
            problems.append((index + 1, "cites a design doc"))

    if run_start is not None and len(lines) - run_start > MAX_BLOCK:
        problems.append((run_start + 1, f"{len(lines) - run_start}-line comment block"))

    return problems, comment, code


def main():
    arguments = sys.argv[1:] or ["src"]
    total_comment = total_code = 0
    offenders = []
    findings = []

    for argument in arguments:
        if os.path.isfile(argument):
            targets = [argument]
        else:
            targets = [
                os.path.join(directory, name)
                for directory, _, names in os.walk(argument)
                for name in sorted(names)
                if name.endswith(SOURCE_SUFFIXES)
            ]

        for path in targets:
            problems, comment, code = scan(path)
            total_comment += comment
            total_code += code

            if problems:
                offenders.append((len(problems), path, problems))
            for line, reason in problems:
                findings.append(f"{path}:{line}: {reason}")

    ratio = total_comment / total_code if total_code else 0.0

    for finding in findings:
        print(finding)

    print(f"\n{len(findings)} findings in {len(offenders)} files")
    print(f"comment/code ratio: {ratio:.3f} ({total_comment}/{total_code})")

    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())

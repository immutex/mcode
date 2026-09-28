"""Local cross-compiler warning gate.

MSVC is the only compiler installed on this workstation, so a warning that only
GCC or Clang emits is invisible locally and costs a ten-minute CI round trip to
discover. This compiles the translation units with clang++ and the same warning
set the CI Clang leg uses, against the real Conan include paths from
compile_commands.json.

    python _clgate.py                  # every translation unit
    python _clgate.py "loader\\.cxx$"   # only matching files

Reports in seconds what CI reports in ten minutes. It is not a substitute for
CI -- clang is not GCC, and neither is Apple Clang -- but it catches the whole
-Wshadow / -Wsign-compare / -Wconversion / -Wunused class before pushing.
"""

from __future__ import annotations

import json
import pathlib
import re
import shutil
import subprocess
import sys

repo = pathlib.Path(__file__).parent
build = repo / "build" / "Release"

WARN = (
    "-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast "
    "-Wcast-align -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion "
    "-Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Werror "
    "-Wno-unknown-warning-option"
)

# MSVC-only switches and outputs that clang rejects outright.
STRIP = (
    "/nologo", "/TP", "/FS", "/EHsc", "/O2", "/Ob2", "/DNDEBUG", "/MT",
    "/utf-8", "/permissive-", "/W4", "/wd4127",
)


def find_clang() -> str | None:
    """The first clang++ on PATH or in a known install location."""
    if found := shutil.which("clang++"):
        return found

    candidates = [
        pathlib.Path.home() / "AppData/Local/Microsoft/WinGet/Packages",
        pathlib.Path("/usr/bin"),
        pathlib.Path("/usr/local/bin"),
    ]

    for root in candidates:
        if not root.is_dir():
            continue

        for path in root.rglob("clang++.exe" if sys.platform == "win32" else "clang++"):
            return str(path)

    return None


def find_vcvars() -> str | None:
    """The MSVC environment script, so clang can find the Windows SDK headers."""
    root = pathlib.Path(r"C:\Program Files\Microsoft Visual Studio")

    if not root.is_dir():
        return None

    for path in sorted(root.glob("*/Community/VC/Auxiliary/Build/vcvars64.bat")):
        return str(path)

    return None


def to_clang(command: str) -> str:
    """Rewrites one MSVC compile command into a clang++ -fsyntax-only command."""
    text = re.sub(r'^"[^"]*cl\.exe"\s+', "", command)

    for flag in STRIP:
        text = text.replace(flag + " ", "")

    # MSVC spells an external include `-external:I<path>`; clang wants -isystem.
    text = re.sub(r"-external:I(\S+)", r"-isystem \1", text)
    text = re.sub(r"-external:W0\s*", "", text)
    text = re.sub(r"-std:\S+\s*", "", text)
    text = re.sub(r"/Zc:\S+\s*", "", text)
    text = re.sub(r"@\S+\.modmap\s*", "", text)
    text = re.sub(r"(^|\s)-MT(\s|$)", " ", text)
    text = re.sub(r"(^|\s)-c(\s|$)", " ", text)
    text = re.sub(r"/Fo\S+\s*", "", text)
    text = re.sub(r"/Fd\S+\s*", "", text)
    text = re.sub(r"-scanDependencies\s*", "", text)
    text = re.sub(r"-showIncludes\s*", "", text)
    text = text.replace("/DWIN32", "-DWIN32").replace("/D_WINDOWS", "-D_WINDOWS")
    text = re.sub(r"/(D\S+)", r"-\1", text)

    return re.sub(r"\s+", " ", text).strip()


def main() -> int:
    clang = find_clang()

    if clang is None:
        print("clang++ not found; install LLVM or put clang++ on PATH", file=sys.stderr)

        return 2

    commands = build / "compile_commands.json"

    if not commands.is_file():
        print(f"no {commands}; configure the Release preset first", file=sys.stderr)

        return 2

    entries = json.loads(commands.read_text(encoding="utf-8"))
    pattern = sys.argv[1] if len(sys.argv) > 1 else r"\.cxx$"
    targets = [entry for entry in entries if re.search(pattern, entry["file"])]

    if not targets:
        print(f"no translation units match {pattern!r}", file=sys.stderr)

        return 2

    vcvars = find_vcvars() if sys.platform == "win32" else None
    lines = ["@echo off"] if vcvars else ["set -e"]

    if vcvars:
        lines += [f'call "{vcvars}" >nul 2>&1', f'cd /d "{repo}"']

    for entry in targets:
        name = pathlib.Path(entry["file"]).name
        include_root = f'-I"{repo}/src"'
        rewritten = to_clang(entry["command"])
        extra = "-DWIN32 -D_WINDOWS -DNOMINMAX -DWIN32_LEAN_AND_MEAN" if vcvars else ""

        lines.append(f"echo === {name} ===")

        # `grep` is not on PATH in a plain cmd.exe, so the filter is chosen per
        # platform rather than assumed.
        if vcvars:
            lines.append(
                f'"{clang}" -fsyntax-only -std=c++23 {WARN} {extra} {include_root} '
                f'{rewritten} 2>&1 | findstr /C:"error" /C:"warning"'
            )
        else:
            lines.append(
                f'"{clang}" -fsyntax-only -std=c++23 {WARN} {extra} {include_root} '
                f'{rewritten} 2>&1 | grep -E "error|warning" || true'
            )

    lines.append('echo GATE_DONE')

    if vcvars:
        script = repo / "_clgate.bat"
        script.write_text("\r\n".join(lines) + "\r\n", encoding="utf-8")
        completed = subprocess.run(["cmd", "/c", str(script)], capture_output=True,
                                   text=True, timeout=3600)
        script.unlink()
    else:
        script = repo / "_clgate.sh"
        script.write_text("\n".join(lines) + "\n", encoding="utf-8")
        completed = subprocess.run(["bash", str(script)], capture_output=True,
                                   text=True, timeout=3600)
        script.unlink()

    output = (completed.stdout or "") + (completed.stderr or "")
    print(output)

    # The gate fails on any diagnostic, which is the point: the warning set is
    # the one CI treats as errors.
    return 1 if re.search(r"\b(error|warning):", output) else 0


if __name__ == "__main__":
    raise SystemExit(main())

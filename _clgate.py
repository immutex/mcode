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
    # -Wshadow-all, not -Wshadow: GCC's -Wshadow rejects a local that shadows an
    # enclosing local, and clang's plain -Wshadow does NOT. A name reuse inside a
    # nested lambda or block therefore passed this gate and failed the GCC leg of
    # CI. -Wshadow-all closes the gap; -Wno-unknown-warning-option keeps an older
    # clang from failing on the flag itself.
    "-Wall -Wextra -Wpedantic -Wshadow-all -Wnon-virtual-dtor -Wold-style-cast "
    "-Wcast-align -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion "
    "-Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Werror "
    "-Wno-unknown-warning-option"
)

# MSVC-only switches and outputs that clang rejects outright.
# MSVC-only switches and outputs that clang rejects outright.
#
# A literal list is not enough on its own: the set MSVC emits depends on the
# configure, and a switch nobody listed reaches clang and fails the whole gate
# with `no such file or directory: '/WX'` rather than with a diagnostic about
# the code. That happened as soon as a local configure added
# `-DMCODE_WARNINGS_AS_ERRORS=ON`, which injects `/WX`.
STRIP = (
    "/nologo", "/TP", "/FS", "/EHsc", "/O2", "/Ob2", "/DNDEBUG", "/MT",
    "/utf-8", "/permissive-", "/W4", "/wd4127",
)

# Every `/W...` warning switch and every `/wdNNNN` suppression: clang has its own
# warning set and its own flags, and the gate passes them explicitly. Matching the
# family rather than listing members means a configure that adds `/WX`, `/W3` or
# `/wd4996` cannot break the gate.
MSVC_FLAG_FAMILY = (
    r"/W[0-4X]\b",
    r"/wd\d+",
    r"/we\d+",
    r"/wo\d+",
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

    for pattern in MSVC_FLAG_FAMILY:
        text = re.sub(pattern + r"\s*", " ", text)

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


# --- Preflight: two classes the clang compile cannot see -------------------
#
# Both of these reached `master` and failed CI, and neither is catchable by the
# clang pass below:
#
#   1. A parameter whose name equals a nested type of the same class. GCC reports
#      it as -Wshadow ("shadows a member"); clang does not implement that warning
#      at all, so `-Wshadow-all` stays silent. Verified against clang 21.
#
#   2. A Win32-only identifier outside a `#if defined(_WIN32)` guard. The gate
#      compiles with `_WIN32` defined, so the POSIX branch of every conditional is
#      never even parsed here -- the failure only exists on the Linux and macOS
#      legs.
#
# Both are textual and deterministic, so they are checked directly rather than
# waiting for a ten-minute CI round trip.

WIN32_HEADERS = (
    "windows.h", "wincrypt.h", "winsock2.h", "ws2tcpip.h", "shlobj.h",
    "conio.h", "direct.h", "windowsx.h", "processthreadsapi.h", "handleapi.h",
)

WIN32_IDENTIFIERS = (
    "GetModuleFileNameA", "GetModuleFileNameW", "GetModuleFileNameExA",
    "GetLastError", "FormatMessageA", "LocalFree", "CloseHandle",
    "CreateProcessA", "CreateProcessW", "OpenProcess", "TerminateProcess",
    "CreateFileA", "CreateFileW", "GetStdHandle", "SetConsoleMode",
    "GetConsoleScreenBufferInfo", "CreateJobObjectW", "AssignProcessToJobObject",
    "CreateRestrictedToken", "CertOpenSystemStoreW", "CertEnumCertificatesInStore",
    "CertCloseStore", "CreateAppContainerProfile", "WSAStartup", "WSACleanup",
    "DWORD", "HANDLE", "BOOL", "WCHAR", "LPCWSTR", "LPWSTR", "LPVOID",
    "INVALID_HANDLE_VALUE", "MAX_PATH", "CONSOLE_SCREEN_BUFFER_INFO",
    "STARTUPINFOA", "PROCESS_INFORMATION", "SECURITY_ATTRIBUTES", "FILETIME",
)

def code_only( line: str ) -> str:
    """The line with `//` comments and string literals removed.

    Both are prose as far as these checks are concerned. A `TEST_CASE` name
    containing `MAX_PATH`, or an error message quoting a Win32 call, is not a
    use of it -- and matching those produced exactly that false positive.
    """
    code = line.split("//", 1)[0]

    return re.sub(r'"(?:[^"\\]|\\.)*"', '""', code)

def check_platform_guards( files: list[ pathlib.Path ] ) -> list[ str ]:
    """Win32 identifiers used where `_WIN32` is not defined."""
    problems = []
    stack: list[ dict ] = []

    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")

        for number, line in enumerate(text.splitlines(), 1):
            stripped = line.strip()

            if stripped.startswith("#"):
                if re.match(r"#\s*(if|ifdef|ifndef)\b", stripped):
                    tests_windows = bool(re.search(r"_WIN32|_MSC_VER|_WINDOWS", stripped))
                    stack.append({"chain_windows": tests_windows, "in_windows": tests_windows})
                elif re.match(r"#\s*elif\b", stripped) and stack:
                    tests_windows = bool(re.search(r"_WIN32|_MSC_VER|_WINDOWS", stripped))
                    stack[-1] = {"chain_windows": tests_windows, "in_windows": tests_windows}
                elif re.match(r"#\s*else\b", stripped) and stack:
                    stack[-1]["in_windows"] = not stack[-1]["in_windows"]
                elif re.match(r"#\s*endif\b", stripped) and stack:
                    stack.pop()

                continue

            if any(frame["in_windows"] for frame in stack):
                continue

            code = code_only(line)

            if not code.strip():
                continue

            for header in WIN32_HEADERS:
                if re.search(r"#\s*include\s*<" + re.escape(header) + ">", code):
                    problems.append(f"{path}:{number} includes <{header}> outside a _WIN32 guard")

            # Skip a line that is only a comment: `code_only` already stripped
            # `//`, but a block comment's interior lines are still prose.
            if code.strip().startswith(("*", "/*")):
                continue

            for token in WIN32_IDENTIFIERS:
                if re.search(r"\b" + re.escape(token) + r"\b", code):
                    problems.append(f"{path}:{number} uses {token} outside a _WIN32 guard")
                    break

    return problems

def check_shadowed_member_types( files: list[ pathlib.Path ] ) -> list[ str ]:
    """A parameter named after a nested type of the same class (GCC -Wshadow)."""
    problems = []

    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")

        # Nested types AND aliases. A `using handler = std::function<...>` shadows
        # exactly like a nested struct does, and covering only struct/class/enum
        # let a fourth instance through to CI.
        nested = set(re.findall(r"^\t+(?:struct|class|enum class)\s+([a-z_][a-z0-9_]*)\s*\{", text, re.M))
        nested |= set(re.findall(r"^\s+using\s+([a-z_][a-z0-9_]*)\s*=", text, re.M))
        nested |= set(re.findall(r"^\s+typedef\s+[^;]*?\b([a-z_][a-z0-9_]*)\s*;", text, re.M))

        if not nested:
            continue

        for name in sorted(nested):
            # A parameter list containing `name` immediately before `,` or `=`.
            pattern = r"\(\s*[^;()]*?\b[a-z_][a-z0-9_:<>,\s\*&]*\b" + re.escape(name) + r"\s*[=,)]"
            match = re.search(pattern, text)

            if match:
                line = text[:match.start()].count("\n") + 1
                problems.append(
                    f"{path}:{line} a parameter named '{name}' shadows the nested type "
                    f"'{name}'; GCC rejects this (-Wshadow), clang does not warn"
                )

    return problems

# POSIX-only breakages the Windows compile cannot see. The gate compiles with
# `_WIN32` defined, so the `#else` branch of every platform conditional is
# skipped entirely -- three separate macOS-only failures reached `master` this
# way, including a `tests/` directory that had never compiled there at all.

DARWIN_MACRO_FUNCTIONS = (
    # Functions in glibc, cast-like MACROS in Darwin's <sys/_endian.h>. A leading
    # `::` is valid on Linux and a syntax error on macOS, where the macro expands
    # to `::((__uint32_t)(x))`.
    "htonl", "htons", "ntohl", "ntohs",
    "bswap_16", "bswap_32", "bswap_64",
)

# A macro defined in a system header whose body contains a C-style cast. GCC
# attributes that cast to the call site and rejects it under -Wold-style-cast;
# clang attributes it to the system header and stays quiet. The call is fine on
# clang and a hard error on GCC, so it is a Linux-only failure by construction.
THIRD_PARTY_MACRO_TRAPS = {
    "SSL_set_tlsext_host_name": (
        "OpenSSL's SNI macro casts to `void *` with a C-style cast. Call "
        "SSL_ctrl(handle, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, "
        "const_cast< char* >( host.c_str( ) )) instead."
    ),
}


def check_posix_call_syntax( files: list[ pathlib.Path ] ) -> list[ str ]:
    """`::name(` where `name` is a macro on some platform, and Win32 calls.

    Both checks are skipped inside a `_WIN32` branch: a `static_cast< int >` for
    `send` is *correct* on Windows, and the point is to find the POSIX path that
    lacks the branch, not to ban the Windows one.
    """
    problems = []

    for path in files:
        stack: list[ dict ] = []

        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            stripped = line.strip()

            if stripped.startswith("#"):
                if re.match(r"#\s*(if|ifdef|ifndef)\b", stripped):
                    tests_windows = bool(re.search(r"_WIN32|_MSC_VER|_WINDOWS", stripped))
                    stack.append({"chain_windows": tests_windows, "in_windows": tests_windows})
                elif re.match(r"#\s*elif\b", stripped) and stack:
                    tests_windows = bool(re.search(r"_WIN32|_MSC_VER|_WINDOWS", stripped))
                    stack[-1] = {"chain_windows": tests_windows, "in_windows": tests_windows}
                elif re.match(r"#\s*else\b", stripped) and stack:
                    stack[-1]["in_windows"] = not stack[-1]["in_windows"]
                elif re.match(r"#\s*endif\b", stripped) and stack:
                    stack.pop()

                continue

            if any(frame["in_windows"] for frame in stack):
                continue

            code = code_only(line)

            if not code.strip() or code.strip().startswith(("*", "/*")):
                continue

            for name in DARWIN_MACRO_FUNCTIONS:
                if re.search(r"::\s*" + re.escape(name) + r"\s*\(", code):
                    problems.append(
                        f"{path}:{number} calls `::{name}(`, which does not compile on macOS "
                        f"({name} is a cast-like macro in <sys/_endian.h>, so it cannot follow `::`)"
                    )

            for name, advice in THIRD_PARTY_MACRO_TRAPS.items():
                if re.search(r"\b" + re.escape(name) + r"\s*\(", code):
                    problems.append(f"{path}:{number} uses `{name}`. {advice}")

            # A `static_cast< int >` on a length passed to send/recv is a
            # -Wsign-conversion error on POSIX under -Werror.
            if re.search(r"::\s*(send|recv)\s*\(", code) and "static_cast< int >" in code:
                problems.append(
                    f"{path}:{number} casts a length to int for send/recv; POSIX takes size_t "
                    f"and -Wsign-conversion rejects the cast"
                )

    return problems

def main() -> int:
    clang = find_clang()

    if clang is None:
        print("clang++ not found; install LLVM or put clang++ on PATH", file=sys.stderr)

        return 2

    commands = build / "compile_commands.json"

    if not commands.is_file():
        print(f"no {commands}; configure the Release preset first", file=sys.stderr)

        return 2

    sources = sorted((repo / "src").rglob("*.cxx")) + sorted((repo / "src").rglob("*.hxx"))
    sources += sorted((repo / "tests").rglob("*.cxx")) + sorted((repo / "tests").rglob("*.hxx"))
    problems = (
        check_platform_guards(sources)
        + check_shadowed_member_types(sources)
        + check_posix_call_syntax(sources)
    )

    if problems:
        print("preflight failed:", file=sys.stderr)

        for problem in problems:
            print("  " + problem, file=sys.stderr)

        return 1

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

"""Local cross-compiler warning gate.

MSVC is the only compiler installed here, so a warning only GCC or Clang emits
would otherwise cost a ten-minute CI round trip to discover. This compiles the
changed translation units with clang++ and the same warning set the CI Clang leg
uses, against the real Conan include paths.
"""
import json, pathlib, re, subprocess, sys

repo = pathlib.Path(__file__).parent
clang = (r"C:\Users\mutex\AppData\Local\Microsoft\WinGet\Packages"
         r"\BrechtSanders.WinLibs.POSIX.UCRT.LLVM_Microsoft.Winget.Source_8wekyb3d8bbwe"
         r"\mingw64\bin\clang++.exe")
vs18 = (r"C:\Program Files\Microsoft Visual Studio\18\Community"
        r"\VC\Auxiliary\Build\vcvars64.bat")

WARN = ("-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast "
        "-Wcast-align -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion "
        "-Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Werror")

cc = json.loads((repo / "build/Release/compile_commands.json").read_text(encoding="utf-8"))
pattern = sys.argv[1] if len(sys.argv) > 1 else r"\.cxx$"
targets = [e for e in cc if re.search(pattern, e["file"])]

lines = ["@echo off", f'call "{vs18}" >nul 2>&1', f'cd /d "{repo}"']
for e in targets:
    c = e["command"]
    c = re.sub(r'^"[^"]*cl\.exe"\s+', "", c)
    # drop MSVC-only switches and file outputs
    for flag in ("/nologo", "/TP", "/FS", "/EHsc", "/O2", "/Ob2", "/DNDEBUG", "/MT",
                 "/utf-8", "/permissive-", "/W4", "/wd4127"):
        c = c.replace(flag + " ", "")
    c = re.sub(r"-external:I(\S+)", r"-isystem \1", c)
    c = re.sub(r"-external:W0\s*", "", c)
    c = re.sub(r"-std:\S+\s*", "", c)
    c = re.sub(r"/Zc:\S+\s*", "", c)
    c = re.sub(r"@\S+\.modmap\s*", "", c)
    c = re.sub(r"(^|\s)-MT(\s|$)", " ", c)
    c = re.sub(r"(^|\s)-c(\s|$)", " ", c)
    c = re.sub(r"/Fo\S+\s*", "", c)
    c = re.sub(r"/Fd\S+\s*", "", c)
    c = re.sub(r"-scanDependencies\s*", "", c)
    c = re.sub(r"-showIncludes\s*", "", c)
    c = c.replace("/DWIN32", "-DWIN32").replace("/D_WINDOWS", "-D_WINDOWS")
    c = re.sub(r"/(D\S+)", r"-\1", c)
    c = re.sub(r"\s+", " ", c).strip()
    name = pathlib.Path(e["file"]).name
    lines.append(f'echo === {name} ===')
    lines.append(f'"{clang}" -fsyntax-only -std=c++23 {WARN} -Wno-unknown-warning-option '
                 f'-DWIN32 -D_WINDOWS -DNOMINMAX -DWIN32_LEAN_AND_MEAN -I"{repo}/src" {c} '
                 f'2>&1 | findstr /C:"error" /C:"warning"')
lines.append("echo GATE_DONE")

bat = repo / "_clgate.bat"
bat.write_text("\r\n".join(lines) + "\r\n", encoding="utf-8")
r = subprocess.run(["cmd", "/c", str(bat)], capture_output=True, text=True, timeout=3600)
bat.unlink()
print((r.stdout or "") + (r.stderr or ""))

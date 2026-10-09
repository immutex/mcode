#!/usr/bin/env python3
"""Drive the mcode TUI under a real ConPTY and assert the screen it produces.

The unit suite cannot see this class of defect. `tui_test::visible_text` strips
the positioning from the byte stream, so a screen whose header scrolled off the
top and a screen that kept it yield the *same* string -- which is how a header
that never appeared passed 118 `tui` assertions. A real console also differs in
kind: `GetConsoleMode`/`ReadConsoleInputA` only engage under a genuine console,
so raw mode, the capability probe and the Win32 backend are untested without a
pty.

This is the gate for both. It runs the product, reads the screen a terminal
would actually show, and fails on a breach.

    python tools/tui_check/tui_screen.py --exe build/Release/bin/mcode.exe
    python tools/tui_check/tui_screen.py --exe ... --keep   # leave the screen dump

Requires `pywinpty` and `pyte` (`tools/tui_check/requirements.txt`). Windows
only: ConPTY is a Win32 facility, and pywinpty does not build elsewhere. The
cross-platform half of this invariant is `tests/test_tui_opening.cxx`, which
models the screen in-process.

Exit codes: 0 all assertions passed, 1 a breach, 2 usage or a missing dependency.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

try:
    import pyte
    import winpty
except ImportError as error:  # pragma: no cover - depends on the host
    print(f"tui-check: missing dependency: {error}", file=sys.stderr)
    print("tui-check: pip install -r tools/tui_check/requirements.txt", file=sys.stderr)
    sys.exit(2)

# A row of the live region while a turn is in flight.
SPINNER = re.compile(r"[\u2800-\u28ff]\s+\S+")

# The meter: tokens, optional context percentage, cost, elapsed.
METER = re.compile(r"\d+(\.\d+)?k? tok( \d+%)? . \$0\.\d+")

# Startup is `\x1b[1t`, then the capability probe, then the first frame about
# three seconds later. Keys sent before raw mode is up are eaten by the line
# discipline, so nothing is written before the first frame.
FIRST_FRAME_SECONDS = 5.0

# A minimal config the gate owns, so the run does not depend on the developer's
# `%APPDATA%\mcode\config.toml`. Without it the gate only passed on a machine
# that had already been configured: on a clean runner mcode exits with "no
# provider configured" before drawing a frame, the screen stays empty, and every
# assertion fails. A gate that reads the host's state is not a gate.
#
# The provider is declared and never contacted -- the gate drives startup,
# typing and exit, and no turn is ever submitted -- so the endpoint and the
# credential name are placeholders.
FIXTURE_CONFIG = """\
[model]
provider = "openai-chat-completions"
model = "tui-gate-placeholder"
base_url = "https://tui-gate.invalid/v1/chat/completions"
api_key_env = "MCODE_TUI_GATE_KEY"

[models."tui-gate-placeholder"]
caching = "implicit"
context_window = 100000
max_output_tokens = 8192
price_input = 0.15
price_output = 0.6
"""

def isolated_appdata(root: str) -> str:
    """A private `%APPDATA%` holding the fixture config, and return it."""
    directory = os.path.join(root, "mcode")

    os.makedirs(directory, exist_ok=True)

    with open(os.path.join(directory, "config.toml"), "w", encoding="utf-8") as handle:
        handle.write(FIXTURE_CONFIG)

    return root


class Terminal:
    """A pty, a reader thread, and the screen the bytes produce."""

    def __init__(self, executable: str, rows: int, columns: int, appdata: str) -> None:
        self.rows = rows
        self.columns = columns
        self.raw = bytearray()
        self._exe_name = os.path.basename(executable)
        self._lock = threading.Lock()
        self._alive = True

        environment = dict(os.environ)
        environment["TERM"] = "xterm-256color"
        environment["COLORTERM"] = "truecolor"

        # Both, because config is read from APPDATA and state is written under
        # LOCALAPPDATA: pointing only one at the fixture would still let the run
        # touch the developer's real session store.
        environment["APPDATA"] = appdata
        environment["LOCALAPPDATA"] = appdata

        # The fixture declares a credential source, and mcode refuses to start
        # when the named variable is absent -- fail-closed, which is right. The
        # value is never used: the gate submits no turn.
        environment["MCODE_TUI_GATE_KEY"] = "tui-gate-placeholder"

        self.screen = pyte.Screen(columns, rows)
        self._stream = pyte.ByteStream(self.screen)
        self.child = winpty.PtyProcess.spawn(
            [executable], dimensions=(rows, columns), env=environment
        )

        # The reader must own the pty: `read()` blocks indefinitely while the
        # child is quiet, so a poll loop in the main thread could never observe
        # its own deadline.
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        while self._alive:
            try:
                chunk = self.child.read(65536)
            except (EOFError, OSError):
                return

            if not chunk:
                continue

            data = chunk if isinstance(chunk, bytes) else chunk.encode()
            with self._lock:
                self.raw.extend(data)
                self._stream.feed(data)

    def write(self, text: str) -> None:
        # pywinpty takes `str`, not `bytes`.
        self.child.write(text)

    def display(self) -> list[str]:
        with self._lock:
            return [row.rstrip() for row in self.screen.display]

    def text(self) -> str:
        return "\n".join(self.display())

    def wait_for_quiet(self, timeout: float) -> None:
        """Sleep until no new bytes have arrived for `timeout`, or give up."""
        deadline = time.time() + 30.0

        while time.time() < deadline:
            with self._lock:
                seen = len(self.raw)

            time.sleep(timeout)

            with self._lock:
                if len(self.raw) == seen:
                    return

    def close(self) -> None:
        self._alive = False

        try:
            self.child.terminate(force=True)
        except Exception:
            pass

        # `terminate` can leave the child holding the executable open, and the
        # next link then fails with LNK1104 -- a build failure caused by a test.
        subprocess.run(
            ["taskkill", "/F", "/IM", self._exe_name],
            capture_output=True,
        )


class Report:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.checks = 0

    def check(self, label: str, ok: bool, detail: str = "") -> None:
        self.checks += 1

        if not ok:
            self.failures.append(label)

        mark = "ok  " if ok else "FAIL"
        suffix = f" -- {detail}" if detail else ""
        print(f"  [{mark}] {label}{suffix}")

    def finish(self, screen_text: str) -> int:
        print()

        if not self.failures:
            print(f"tui-check: {self.checks} assertions passed")
            return 0

        print(f"tui-check: {len(self.failures)} of {self.checks} failed:")
        for label in self.failures:
            print(f"  - {label}")

        print("\n--- screen as rendered ---")
        print(screen_text)

        return 1


def assert_startup(report: Report, terminal: Terminal) -> None:
    rows = terminal.rows
    display = terminal.display()
    bottom = display[rows - 1]
    meter = display[rows - 2]

    print("== startup ==")

    report.check("the prompt is the last row and bare", bottom == ">", repr(bottom))
    report.check("the meter sits directly above the prompt", bool(METER.search(meter)), repr(meter))

    # The model name is one token: a wrap inside it means the width math
    # disagrees with what the terminal does with the string.
    report.check("the model name is not split", " ▸ " in meter and " " not in meter.split(" ▸ ")[0], repr(meter))

    report.check("the banner version line rendered", "mcode " in terminal.text())
    report.check("the banner platform line rendered", "windows-" in terminal.text() or "linux-" in terminal.text() or "macos-" in terminal.text())
    report.check("the banner hint line rendered", "/help for commands" in terminal.text())

    banner = next(
        (index for index, row in enumerate(display) if re.search(r"mcode \d+\.\d+\.\d+", row)),
        None,
    )
    report.check("the banner is on screen at all", banner is not None, f"row {banner}")

    if banner is not None:
        report.check("the banner is above the meter", banner < rows - 2, f"row {banner}")
        report.check(
            "the banner sits just above the region, not scrolled away",
            banner < rows - 2 and banner > rows - 2 - 16,
            f"row {banner} of {rows}",
        )

        # The banner ends with one deliberate blank separator, so a couple of
        # blank rows between it and the meter is the layout. More than that is
        # residue from a region height that did not match what was on screen.
        gap = sum(1 for index in range(banner, rows - 2) if not display[index])
        report.check("no runaway blank gap under the banner", gap <= 4, f"{gap} blank rows")


def assert_typing(report: Report, terminal: Terminal) -> None:
    print("== typing ==")

    terminal.write("hello")
    time.sleep(1.5)

    display = terminal.display()
    report.check("typed text echoes on the prompt row", display[terminal.rows - 1] == "> hello", repr(display[terminal.rows - 1]))
    report.check("the meter is still above the prompt", bool(METER.search(display[terminal.rows - 2])), repr(display[terminal.rows - 2]))

    # Escape returns to the live view and must not disturb the region.
    terminal.write("\x1b")
    time.sleep(1.5)

    display = terminal.display()
    report.check("the region survives Escape", display[terminal.rows - 1].startswith(">"), repr(display[terminal.rows - 1]))
    report.check("the banner survives Escape", bool(re.search(r"mcode \d+\.\d+\.\d+", terminal.text())))


def assert_exit(report: Report, terminal: Terminal) -> None:
    print("== exit ==")

    terminal.write("\x04")

    deadline = time.time() + 20.0
    while time.time() < deadline and terminal.child.isalive():
        time.sleep(0.25)

    status = getattr(terminal.child, "exitstatus", None)
    report.check("Ctrl+D exits with status 0", status == 0, repr(status))

    display = terminal.display()
    tail = display[terminal.rows - 2:]
    report.check("the region rows are blank after exit", all(not row for row in tail), repr(tail))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--exe", required=True, help="the mcode binary to drive")
    parser.add_argument("--rows", type=int, default=30)
    parser.add_argument("--columns", type=int, default=100)
    parser.add_argument("--keep", action="store_true", help="print the screen dump")
    arguments = parser.parse_args()

    if not os.path.isfile(arguments.exe):
        print(f"tui-check: no such executable: {arguments.exe}", file=sys.stderr)
        return 2

    scratch = tempfile.mkdtemp(prefix="mcode-tui-gate-")
    appdata = isolated_appdata(scratch)

    try:
        terminal = Terminal(arguments.exe, arguments.rows, arguments.columns, appdata)

        try:
            time.sleep(FIRST_FRAME_SECONDS)

            report = Report()
            assert_startup(report, terminal)
            assert_typing(report, terminal)
            assert_exit(report, terminal)

            screen_text = terminal.text() if arguments.keep else ""
            return report.finish(screen_text)
        finally:
            terminal.close()
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

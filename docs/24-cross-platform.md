# Cross-Platform

> TL;DR: Windows, Linux, and macOS are all tier-1 — every OS-touching concern sits behind one of **seven interfaces** from M0, because retrofitting a platform is how a core ends up Windows-shaped. macOS cannot be fully static-linked, Windows needs manifest opt-ins and lock retries, and Linux needs runtime kernel-feature detection.

## Support matrix

| | Windows | Linux | macOS |
|---|---|---|---|
| **Minimum** | Windows 10 1809+ (ConPTY floor) | kernel 5.13+ (Landlock ABI 1); glibc 2.31+ or musl | macOS 13 Ventura, arm64 |
| **Architectures** | x64, arm64 | x86_64, aarch64 | arm64 (x86_64 tarball optional) |
| **Toolchain** | MSVC 19.4x | GCC 14+ / Clang 18+ | Apple Clang 15+ (SDK required) |
| **Linking** | `/MT` full static | **musl static** (primary) | **dynamic libSystem + static everything else** |
| **Sandbox** | restricted token + Job Object; AppContainer tier-2 | Landlock (primary) + optional bubblewrap | Seatbelt via `sandbox_init_with_parameters` |
| **PTY** | ConPTY | `posix_openpt` / `forkpty` | `openpty` + `posix_spawn` |
| **Signing** | Authenticode OV | none | ad-hoc minimum; Developer ID + notarize for browser distribution |
| **CI cost/min** | $0.010 | $0.005–0.006 | $0.062–0.102 |

macOS CI is **10× Linux per minute**. Keep the macOS job to build + smoke; run the full suite on Linux and Windows.

## The seven interfaces

Everything OS-specific lives behind one of these. No `#ifdef` leaks into portable code.

| # | Interface | Divergence |
|---|---|---|
| 1 | `PtySession` | ConPTY (blocking I/O on dedicated threads) vs `openpty`/`posix_spawn`. Largest divergence in the codebase |
| 2 | `Sandbox` | Three incompatible models: Windows token/Job is *code*, Seatbelt is a *profile*, Landlock is *syscalls with runtime ABI probing* |
| 3 | `Termination` | Windows has no real signals — `SetConsoleCtrlHandler` + `TerminateProcess` + Job close. Child-tree kill differs per OS |
| 4 | `fs` helpers | Canonicalize (with case-fold policy), realpath, temp dir, long-path prefixing |
| 5 | `paths` | Config/data/cache/state/log locations |
| 6 | `ResizeSource` | `SIGWINCH` vs `WINDOW_BUFFER_SIZE_EVENT` vs optional in-band (DECSET 2048) |
| 7 | `FileWatch` | `ReadDirectoryChangesW` vs `inotify` vs `FSEvents` |

Everything else is portable: JSON/TOML, HTTP, Lua VM hosting, session files, prompt rendering, argv parsing, `std::expected` error model. **VT emission is portable** — write VT to stdout everywhere; the only Windows-specific part is a one-time `ENABLE_VIRTUAL_TERMINAL_PROCESSING` + UTF-8 manifest in the Windows `main` shim.

`FileWatch` is deliberately the odd one out: the three APIs are so different that this belongs in a **Lua extension over a small core API**, not in the core event loop (`23`).

## Sandboxing per OS

### macOS — Seatbelt, still, with a better entry point

`sandbox-exec(1)` is marked **DEPRECATED** (man page dated 2017) and prints a deprecation warning on stderr **every invocation** on macOS 15. It still works. **Apple has published no replacement** for non-App-Store CLI process sandboxing — App Sandbox needs entitlements and Xcode signing; Endpoint Security is an EDR-style authorize framework needing a System Extension; System Extensions are distribution-gated.

The better path is **`sandbox_init_with_parameters()`** from `libSystem.dylib`: undocumented, not in public headers, but used in production by Chrome, Firefox, and Nix. It takes the same SBPL profiles with `(param "NAME")` substitution and avoids both the exec-wrapper hop and the stderr warning.

Profile hygiene is non-negotiable: **`(deny default)` + `(import "system.sb")`**. `(allow default)` denylists are structurally escapable — a devfs-mount → no-xattr bundle → Launch Services chain escaped a real product's `(allow default)` profile.

A working deny-default profile must allow: `process-exec`, `process-fork`, `signal (target self)`, `sysctl-read`, `mach-lookup`, `file-read` on `/bin /dev /System /usr` + the workspace + the sandbox temp dir, `file-read-metadata` on the workspace parent, `file-write*` on the workspace + `/var/folders`, and an explicit `(deny file-write* (subpath "<ws>/.git"))`. Missing rules surface as `Operation not permitted` or `signaled(6)` crashes — not as a clear message.

### Linux — Landlock primary, bubblewrap optional

Landlock ABI ladder (feature-detect at runtime with `landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION)` — **never infer from kernel version**):

| ABI | Kernel | Adds |
|---|---|---|
| 1 | 5.13 | Filesystem rights only. `REFER` absent ⇒ **all cross-directory rename/link denied** |
| 2 | — | `FS_REFER` (reparenting) |
| 3 | 6.2 | `FS_TRUNCATE` |
| 4 | 6.7 | `NET_BIND_TCP` / `NET_CONNECT_TCP` — **ports only, no hostnames, no UDP/ICMP** |
| 5 | 6.10 | `FS_IOCTL_DEV` |
| 6 | 6.12 | Scopes: abstract UNIX sockets, signals |
| 7 | 6.15 | Audit logging of denials |
| 9 | — | `FS_RESOLVE_UNIX` |

RHEL 9.6 backports only to **ABI 5** on a 5.14 kernel. Two rules follow: probe, never assume; and degrade gracefully, because a missing right is a silently weaker sandbox.

Rules only apply to file descriptors opened **after** `restrict_self` — pre-opened descriptors bypass them. OverlayFS layers do not restrict the merged view, which matters for containerized workspaces.

**bubblewrap is a distro-policy lottery.** Setuid mode was removed; it needs unprivileged user namespaces, which **Ubuntu 23.10/24.04 deny by default** via an AppArmor restriction. bwrap fails out of the box on stock 24.04 without a profile shim. Treat it as an optional enhancer, never the primary mechanism.

### Windows — restricted token, not the experimental API

**Tier 1 (default, no admin):** `CreateRestrictedToken` with deny-only SIDs and stripped privileges + low integrity + Job Object (kill-on-close, UI restrictions) + ACL write boundary. This is the Chrome-style approach and needs no elevation.

**Tier 2 (AppContainer):** `CreateAppContainerProfile` + `PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES`. Works today. Caveat that bites our design: AppContainer networking is **default-deny and loopback is special-cased**, which collides with the loopback egress proxy — the loopback capability must be granted explicitly.

**`Experimental_CreateProcessInSandbox` is a dead end.** It is still marked experimental (docs updated 2026-06), and with `app_container=false` most fields — `fs_read_write`, `fs_read_only`, `network_policy`, `capabilities` — **silently do not take effect**. The FS and network policy is AppContainer-only, so the API adds nothing over Tier 2 while adding an experimental dependency.

**Windows Sandbox CLI is not a programmatic tier.** `wsb exec` has no process I/O capture and needs an active RDP session. It is the paranoid tier, driven interactively.

## Filesystem

| Concern | Rule |
|---|---|
| Case | Windows NTFS and macOS APFS are case-insensitive + case-preserving. Case-fold path comparisons on both. A case-only rename is delete + create |
| Long paths | Ship the `longPathAware` manifest **and** use `\\?\` for paths we construct >260 chars. The registry key is machine-wide and cannot be set by us. Note `\\?\` disables `/`→`\` conversion and `.`/`..` normalization |
| Realpath | Never shell out — `realpath(1)` is **absent on macOS**. Use `GetFinalPathNameByHandleW` on Windows, `std::filesystem` elsewhere |
| Unicode | Windows treats path components as opaque `WCHAR` (no normalization); APFS is normalization-insensitive; Linux is byte-preserving. Never assume two visually identical names are the same bytes on macOS |
| Locking | **Mandatory on Windows, advisory on POSIX.** An editor or AV holding a file makes atomic-replace `rename` fail with a sharing violation, and Windows has no unlink-while-open. Write-temp-then-rename everywhere, with a **retry loop on Windows only** |
| Line endings | Byte-preserving edit tool: detect and preserve the file's dominant EOL, never normalize silently |
| Temp | `%TEMP%` / `$TMPDIR`→`/tmp` / macOS per-user `/var/folders/.../T` via `confstr(_CS_DARWIN_USER_TEMP_DIR)`. **The macOS temp path must be in the sandbox allowlist** |

**Config directories** follow platformdirs semantics: `%APPDATA%\mcode` / `~/.config/mcode` / `~/Library/Application Support/mcode`. Honor `XDG_CONFIG_HOME` as an override on macOS too — it costs nothing and many CLI users expect it.

## Distribution and signing

| Platform | Requirement | Real failure mode |
|---|---|---|
| macOS arm64 | **Every executable must be signed; ad-hoc is sufficient.** Post-link tools (`strip`, `install_name_tool`) can invalidate the signature — re-sign after | Unsigned arm64 binaries simply do not run |
| macOS notarization | Only applies to **quarantined** files (browser/AirDrop downloads). Files fetched by `curl` or Homebrew are not quarantined | "Works from curl, blocked from browser download" |
| Homebrew | 5.0 (Nov 2025) deprecated `--no-quarantine` and **disables casks failing Gatekeeper from Sept 2026** | A cask needs notarization; a formula does not |
| Windows | Authenticode OV cert is the baseline; SmartScreen reputation builds with download volume | Full-screen "Windows protected your PC" on unsigned binaries; **Defender false positives occur even when signed** |
| Linux | No gatekeeper. glibc version skew is the real failure mode | A binary built on Ubuntu 24.04 fails on Debian 12 with `GLIBC_2.xx not found` |

**Linux ships a static musl tarball** (x86_64 + aarch64) as the primary artifact — this is the specific fix for glibc skew, and musl static has no external dependencies even for DNS. AppImage buys a CLI nothing and drags in a `libfuse2` dependency Ubuntu stopped shipping.

> **Not yet true.** The tag-triggered release pipeline currently packages the
> **glibc** build from `conan/profiles/linux-gcc`, so the tarball is
> dynamically linked and carries the skew this section warns about. The
> `linux-gcc` profile says as much in its own comment. A musl profile and a
> static release build are outstanding work; until they land, the Linux artifact
> is built on `ubuntu-24.04` and will not run on an older glibc.

**macOS ships arm64-only.** Universal binaries double size (~5–9 MB → ~10–18 MB), and Intel macOS is being wound down by Homebrew from Sept 2026. An optional x86_64 tarball covers stragglers.

## The static-linking correction

`01`'s "static binary ≤25 MB" budget **does not hold on macOS**. Apple does not support fully static executables — `libSystem` must remain dynamic. The correct statement per platform:

| Platform | Linking | Result |
|---|---|---|
| Windows | `/MT` fully static | single `.exe` |
| Linux | musl static | single binary, no libc dependency |
| macOS | **dynamic libSystem + libc++, static everything else** | a small set of system dylibs, no bundled third-party libraries |

The user-facing promise survives — *no third-party runtime dependencies on any platform* — but "one self-contained file" is a Windows/Linux property, not a macOS one.

## Windows runtime specifics

- **Signals:** `signal(SIGINT)` on Windows runs the handler on a **new thread** the OS spawns, and `SIGTERM`/`SIGILL` are never generated. All graceful-shutdown logic goes through `SetConsoleCtrlHandler`, never portable signal code.
- **Resize:** there is no `SIGWINCH`. ConPTY resize is a `ResizePseudoConsole` call by the host, observed as a `WINDOW_BUFFER_SIZE_EVENT`. In-band resize (DECSET 2048) is **not supported by Windows Terminal** and is missing from tmux, screen, and VTE — so it is an optional fast path, never a requirement.
- **Console mode:** set `ENABLE_VIRTUAL_TERMINAL_PROCESSING` at startup and write VT everywhere. Never call Console API directly — that keeps ConPTY's translation layer out of the way.
- **UTF-8:** set `activeCodePage=UTF-8` in the fusion manifest so `-A` APIs are UTF-8, and still pass `CP_UTF8` explicitly in conversions.
- **Exec resolution differs:** `CreateProcess` searches the app directory, system directories, then PATH (plus cwd unless suppressed); `execvp` searches PATH only. A tool that runs `git` resolves differently per OS.
- **Environment variables are case-insensitive** on Windows. Env-scrubbing code must fold keys.
- **argv:** Windows receives one command line re-parsed by the child CRT. Never pass raw user strings through `cmd /c`; use the spawn API's argument handling.

## CI

| Job | Scope |
|---|---|
| Linux x86_64 | full test suite + eval suite |
| Linux aarch64 | build + smoke |
| Windows x64 | full test suite + **eval suite** (MSVC is on the runner; fixtures build with the same toolchain as the product) |
| macOS arm64 | build + smoke by default; full suite on PR label (10× cost) |

The eval suite (`11`) runs on Linux and Windows because those are the two where a full toolchain is present by default. macOS runs it on demand. Platform-specific fixtures — path handling, atomic-replace retry, PTY spawn/resize, sandbox enforcement, config-directory resolution — run on every platform, since they are the only tests that catch a platform regression.

Cross-compiling macOS from Linux is not viable for Apple Clang. Budget accordingly.

## Localization: an explicit non-goal

mcode is **English-only** for its own UI strings, prompts, and error messages. Stated rather than implied, because the absence is otherwise read as an oversight.

What is *not* a non-goal, and is handled:

| Concern | Position |
|---|---|
| Non-ASCII content in files, diffs, and tool output | Full UTF-8 (simdutf) end to end, including invalid-sequence replacement. Non-ASCII is data, not UI |
| Display width | Per-terminal ambiguous-width policy (`MCODE_AMBIGUOUS_WIDTH`, `13`) — this is a *rendering* problem, not a translation one |
| Paths and filenames | Native encoding, NFC/NFD-insensitive comparison on macOS (HFS+/APFS normalize; a repo can contain both forms) |
| Locale-aware sorting | Not used. Ordering is byte-wise and stable, so output is reproducible across machines |
| Case folding | ASCII-only folding for globs, deliberately — Unicode case folding is locale- and version-dependent and would make matches non-reproducible |

The reasoning: prompts are English, so model behavior is anchored in English; translating the UI without translating the prompt creates a mismatch worse than either. Revisit only if there is demand for a fully localized harness.

## Traps

- **`#ifdef` in portable code.** If a platform check appears outside one of the seven interfaces, the abstraction is wrong.
- **Assuming POSIX.** `fork` does not exist on Windows and is unsafe between-fork on macOS (the ObjC runtime); the spawn model must be `posix_spawn`/`CreateProcess` from day one.
- **Assuming static linking is universal.** macOS cannot do it. State the per-platform promise, not a single one.
- **`Experimental_CreateProcessInSandbox`.** Experimental, and its policy fields silently no-op without AppContainer.
- **bubblewrap as the Linux primary.** Broken out of the box on stock Ubuntu 24.04; Landlock is the mechanism that always works.
- **Assuming Landlock ABI from the kernel version.** RHEL 9.6 is ABI 5 on 5.14.
- **`(allow default)` Seatbelt profiles.** Structurally escapable; deny-default is mandatory.
- **Unsigned macOS arm64 binaries.** They do not run at all.
- **Shelling out to `realpath`.** Absent on macOS.
- **No retry on Windows atomic replace.** Sharing violations are normal, not exceptional.
- **Shipping universal macOS binaries by default.** 2× size for an architecture being retired.

## Open questions

- macOS 13 vs 14 as the floor: 13 keeps older hardware, 14 has a better `sandbox_init` story. Needs a decision at M1.
- Windows arm64: Luau is portable C++ with an MSVC build, so arm64 is a toolchain question rather than a VM one. Does it earn a tier-1 claim at M0, or stay x64-emulated for now?
- Should the macOS sandbox use `sandbox-exec` (documented-but-deprecated, warns) or `sandbox_init_with_parameters` (undocumented, clean)? Leaning the latter, with `sandbox-exec` as a fallback probe.
- Does the loopback egress proxy work under AppContainer on Windows without granting a broad capability? If not, Tier 1 may be the only honest Windows sandbox.
- APFS normalization specifics need verification before the path-handling code is written.

## Sources

- https://learn.microsoft.com/en-us/windows/win32/secauthz/createprocessinsandbox — experimental API, AppContainer-gated policy fields
- https://learn.microsoft.com/en-us/windows/win32/secauthz/restricted-tokens — Tier 1 mechanism
- https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation — MAX_PATH, `\\?\`, `longPathAware`
- https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page — UTF-8 manifest
- https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences — VT mode, 24-bit color mapping
- https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/signal — Windows signal semantics
- https://learn.microsoft.com/en-us/windows/security/operating-system-security/virus-and-threat-protection/microsoft-defender-smartscreen/available-settings — SmartScreen
- https://devblogs.microsoft.com/commandline/windows-command-line-introducing-the-windows-pseudo-console-conpty/ — ConPTY model
- https://devblogs.microsoft.com/commandline/windows-terminal-preview-1-22-release/ — WT 1.22 fidelity/throughput
- https://github.com/microsoft/terminal/issues/1985 — passthrough mode never shipped
- https://man7.org/linux/man-pages/man7/landlock.7.html — Landlock ABI ladder, pre-opened-fd and OverlayFS caveats
- https://landlock.io/news/5/ — ABI 5–7, RHEL 9.6 backport
- https://discourse.ubuntu.com/t/understanding-apparmor-user-namespace-restriction/58007 — bwrap on Ubuntu 24.04
- https://github.com/containers/bubblewrap — setuid removal, `--new-session`, TIOCSTI
- https://github.com/apple/containerization/issues/737 — no sandbox-exec replacement, still open
- https://manp.gs/mac/1/sandbox-exec — DEPRECATED marker
- https://www.pillar.security/blog/escaping-antigravitys-allow-default-seatbelt — `(allow default)` escape chain
- https://zameermanji.com/blog/2025/4/1/sandboxing-subprocesses-in-python-on-macos/ — `sandbox_init_with_parameters`, Chrome/Firefox/Nix usage
- https://alejandromp.com/development/blog/sandboxing-an-ai-harness-on-macos — working deny-default profile
- https://keith.github.io/xcode-man-pages/openpty.3.html — macOS `openpty`/`forkpty`, `ptsname_r`
- https://github.com/microsoft/node-pty/issues/590 — macOS forkpty avoidance
- https://eclecticlight.co/2024/08/10/gatekeeper-and-notarization-in-sequoia/ — quarantine semantics
- https://developer.apple.com/documentation/macos-release-notes/macos-big-sur-11_0_1-universal-apps-release-notes — arm64 signature requirement
- https://developer.apple.com/forums/thread/706419 — no fully static macOS executables
- https://brew.sh/2025/11/12/homebrew-5.0.0/ — `--no-quarantine` deprecation, cask Gatekeeper deadline
- https://docs.github.com/en/billing/reference/actions-runner-pricing — CI per-minute rates
- https://platformdirs.readthedocs.io/en/latest/explanation.html — config/cache dir table
- https://vtdn.dev/docs/decset/mode2048-in-band-resize — in-band resize support matrix
- https://www.musl-libc.org/intro.html — musl static, no external deps
- https://docs.appimage.org/user-guide/troubleshooting/fuse.html — libfuse2 problem
- https://en.wikipedia.org/wiki/File_locking — mandatory vs advisory locking
- https://wcwidth.readthedocs.io/en/latest/intro.html — ambiguous-width semantics
- https://projectzero.google/2021/08/understanding-network-access-windows-app.html — AppContainer networking
- https://learn.microsoft.com/en-us/windows/security/application-security/application-isolation/windows-sandbox/windows-sandbox-cli — `wsb exec` limits

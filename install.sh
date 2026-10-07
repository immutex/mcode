#!/bin/sh
# mcode installer for Linux and macOS.
#
#   curl -fsSL https://raw.githubusercontent.com/immutex/mcode/master/install.sh | sh
#
# Downloads the release binary for this platform, verifies its SHA-256 against
# the release's SHA256SUMS, installs it, and hands off to `mcode setup`.
#
# POSIX sh, not bash: `curl | sh` runs under whatever /bin/sh is, which is dash
# on Debian and Ubuntu. No arrays, no [[ ]], no local in the outer scope.

set -eu

REPOSITORY="immutex/mcode"
RELEASE_BASE="https://github.com/${REPOSITORY}/releases"

# Overridable so the script is testable without touching a real release.
MCODE_VERSION="${MCODE_VERSION:-}"
MCODE_INSTALL_DIR="${MCODE_INSTALL_DIR:-}"
MCODE_NO_SETUP="${MCODE_NO_SETUP:-}"
MCODE_BASE_URL="${MCODE_BASE_URL:-}"

# ---------------------------------------------------------------------------
# Output. Colour is opt-out via NO_COLOR (https://no-color.org) and off
# automatically when stdout is not a terminal, so a log file gets clean text.
# ---------------------------------------------------------------------------

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-}" != "dumb" ]; then
	bold=$(printf '\033[1m')
	dim=$(printf '\033[2m')
	reset=$(printf '\033[0m')
	accent=$(printf '\033[38;5;110m')
	green=$(printf '\033[38;5;114m')
	yellow=$(printf '\033[38;5;179m')
	red=$(printf '\033[38;5;174m')
else
	bold='' dim='' reset='' accent='' green='' yellow='' red=''
fi

say( ) {
	printf '%s\n' "$*"
}

step( ) {
	printf '  %s%s%s %s\n' "$accent" "::" "$reset" "$*"
}

ok( ) {
	printf '  %s%s%s %s\n' "$green" "ok" "$reset" "$*"
}

warn( ) {
	printf '  %s%s%s %s\n' "$yellow" "!!" "$reset" "$*" >&2
}

die( ) {
	printf '\n  %s%serror%s %s\n\n' "$red" "$bold" "$reset" "$*" >&2
	exit 1
}

banner( ) {
	printf '\n'
	printf '  %s%smcode%s %s%s%s\n' "$bold" "$accent" "$reset" "$dim" "$1" "$reset"
	printf '\n'
}

# ---------------------------------------------------------------------------
# Platform detection. An unsupported target is refused by name, because the
# failure mode this avoids is downloading a binary that cannot exec.
# ---------------------------------------------------------------------------

detect_platform( ) {
	uname_s=$(uname -s 2>/dev/null || echo unknown)
	uname_m=$(uname -m 2>/dev/null || echo unknown)

	case "$uname_s" in
		Linux)  os=linux ;;
		Darwin) os=macos ;;
		*) die "unsupported operating system: $uname_s
    mcode ships Linux, macOS and Windows builds.
    On Windows, use: irm https://raw.githubusercontent.com/${REPOSITORY}/master/install.ps1 | iex" ;;
	esac

	# Normalized to the two names the release artifacts use.
	case "$uname_m" in
		x86_64|amd64) arch="x86_64" ;;
		aarch64|arm64) arch="arm64" ;;
		*) die "unsupported architecture: $uname_m
    mcode ships x86_64 and arm64 builds." ;;
	esac

	# Only these two pairs are built. Naming the gap beats a 404 on the tarball.
	if [ "$os" = "linux" ] && [ "$arch" != "x86_64" ]; then
		die "unsupported target: linux-$arch
    mcode ships linux-x86_64 and macos-arm64 today."
	fi

	if [ "$os" = "macos" ] && [ "$arch" != "arm64" ]; then
		die "unsupported target: macos-$arch
    mcode ships macos-arm64 today (no Intel macOS build)."
	fi

	platform="${os}-${arch}"
}

# ---------------------------------------------------------------------------
# Download. curl first, wget as the fallback; either one is present on the
# platforms this script targets.
# ---------------------------------------------------------------------------

fetch( ) {
	url="$1"
	destination="$2"

	if command -v curl >/dev/null 2>&1; then
		curl -fsSL "$url" -o "$destination" && return 0
	elif command -v wget >/dev/null 2>&1; then
		wget -qO "$destination" "$url" && return 0
	fi

	return 1
}

fetch_stdout( ) {
	url="$1"

	if command -v curl >/dev/null 2>&1; then
		curl -fsSL "$url" && return 0
	elif command -v wget >/dev/null 2>&1; then
		wget -qO- "$url" && return 0
	fi

	return 1
}

resolve_version( ) {
	# An explicit version wins. Otherwise follow the `releases/latest` redirect,
	# which needs no API token and cannot be rate-limited the way the JSON API is.
	if [ -n "$MCODE_VERSION" ]; then
		printf '%s' "${MCODE_VERSION#v}"
		return 0
	fi

	location=$(curl -fsSLI -o /dev/null -w '%{url_effective}' \
		"${RELEASE_BASE}/latest" 2>/dev/null || true)

	case "$location" in
		*/tag/v*) printf '%s' "${location##*/tag/v}" ;;
		*) return 1 ;;
	esac
}

sha256_of( ) {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	elif command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	elif command -v openssl >/dev/null 2>&1; then
		openssl dgst -sha256 "$1" | awk '{print $NF}'
	else
		return 1
	fi
}

# ---------------------------------------------------------------------------
# Install location. A user-writable prefix by default, so the common path needs
# no sudo; MCODE_INSTALL_DIR overrides for a system install.
# ---------------------------------------------------------------------------

choose_prefix( ) {
	if [ -n "$MCODE_INSTALL_DIR" ]; then
		printf '%s' "$MCODE_INSTALL_DIR"
		return 0
	fi

	if [ -w /usr/local/bin ] 2>/dev/null; then
		printf '%s' "/usr/local/bin"
		return 0
	fi

	printf '%s' "${HOME}/.local/bin"
}

# Adds the prefix to the user's shell rc exactly once. Never rewrites an rc that
# already mentions the path, so re-running the installer is idempotent.
wire_path( ) {
	prefix="$1"

	case ":${PATH}:" in
		*":${prefix}:"*)
			return 0 ;;
	esac

	line="export PATH=\"${prefix}:\$PATH\""
	updated=""

	for rc in "${HOME}/.bashrc" "${HOME}/.zshrc" "${HOME}/.profile"; do
		# Only touch a shell that is actually present on this machine.
		case "$rc" in
			*/.bashrc)  [ -f "$rc" ] || continue ;;
			*/.zshrc)   [ -f "$rc" ] || continue ;;
			*/.profile) [ -f "$rc" ] || continue ;;
		esac

		grep -qF "$prefix" "$rc" 2>/dev/null && continue

		printf '\n# added by the mcode installer\n%s\n' "$line" >> "$rc"
		updated="${updated} ${rc##*/}"
	done

	if [ -n "$updated" ]; then
		ok "added ${prefix} to PATH in${updated}"
	else
		warn "${prefix} is not on PATH; add it yourself: ${line}"
	fi
}

# ---------------------------------------------------------------------------
main( ) {
	banner "installer"

	detect_platform
	step "platform ${bold}${platform}${reset}"

	version=$(resolve_version) || die "could not determine the latest version.
    Set MCODE_VERSION=x.y.z to pin one, or check your network connection."
	step "version ${bold}v${version}${reset}"

	archive="mcode-${version}-${platform}.tar.gz"
	if [ -n "$MCODE_BASE_URL" ]; then
		url="${MCODE_BASE_URL}/${archive}"
		sums_url="${MCODE_BASE_URL}/SHA256SUMS"
	else
		url="${RELEASE_BASE}/download/v${version}/${archive}"
		sums_url="${RELEASE_BASE}/download/v${version}/SHA256SUMS"
	fi

	work=$(mktemp -d 2>/dev/null || mktemp -d -t mcode)
	trap 'rm -rf "$work"' EXIT INT TERM

	step "downloading ${dim}${archive}${reset}"
	fetch "$url" "$work/$archive" \
		|| die "download failed: $url
    Check the version exists: ${RELEASE_BASE}/tag/v${version}"

	# Verify before anything is unpacked or executed. A missing SHA256SUMS is a
	# hard failure rather than a skipped check: an unverified binary is the one
	# thing this step exists to prevent.
	step "verifying checksum"
	fetch "$sums_url" "$work/SHA256SUMS" \
		|| die "could not fetch SHA256SUMS, so the download cannot be verified"

	# Tolerant of a `./` prefix and of the `*` binary marker, so the parse does
	# not depend on how the checksum file was generated.
	expected=$(awk -v want="$archive" '
		{ name = $2; sub( /^\*/, "", name ); sub( /^\.\//, "", name ); if ( name == want ) { print $1; exit } }
	' "$work/SHA256SUMS" | head -n1)

	[ -n "$expected" ] || die "SHA256SUMS has no entry for ${archive}"

	actual=$(sha256_of "$work/$archive") \
		|| die "no sha256sum, shasum or openssl available to verify the download"

	if [ "$expected" != "$actual" ]; then
		die "checksum mismatch for ${archive}
    expected ${expected}
    got      ${actual}
    The download is corrupt or tampered with; nothing was installed."
	fi
	ok "sha256 ${dim}${actual}${reset}"

	step "unpacking"
	tar -xzf "$work/$archive" -C "$work" || die "could not unpack ${archive}"

	# The archive holds `mcode` at its root. Search as a fallback so a change to
	# the packaging layout cannot silently produce "no binary in the archive".
	binary="$work/mcode"
	if [ ! -f "$binary" ]; then
		binary=$(find "$work" -type f -name mcode -print 2>/dev/null | head -n1)
	fi

	[ -n "$binary" ] && [ -f "$binary" ] || die "the archive does not contain an mcode binary"
	chmod +x "$binary"

	prefix=$(choose_prefix)
	mkdir -p "$prefix" || die "could not create ${prefix}"

	step "installing to ${bold}${prefix}${reset}"
	# Install beside the destination and rename, so a running mcode is replaced
	# atomically rather than truncated under itself.
	cp "$binary" "${prefix}/.mcode.new" || die "could not write to ${prefix}"
	mv -f "${prefix}/.mcode.new" "${prefix}/mcode" || die "could not replace ${prefix}/mcode"
	chmod +x "${prefix}/mcode"
	ok "installed ${prefix}/mcode"

	wire_path "$prefix"

	installed=$("${prefix}/mcode" --version 2>/dev/null || echo "mcode")
	ok "runs: ${dim}${installed}${reset}"

	printf '\n'
	printf '  %sinstalled%s\n' "$green$bold" "$reset"
	printf '\n'

	# The binary carries the wizard, so the interactive part lives in C++ and
	# stays identical on every platform and in the packaged build.
	if [ -n "$MCODE_NO_SETUP" ]; then
		say "  Next: run ${bold}mcode setup${reset} to choose a model and provider."
		printf '\n'
		return 0
	fi

	if [ -t 0 ] && [ -t 1 ]; then
		exec "${prefix}/mcode" setup
	fi

	say "  Next: run ${bold}mcode setup${reset} to choose a model and provider."
	printf '\n'
}

main "$@"

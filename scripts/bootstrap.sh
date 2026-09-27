#!/usr/bin/env bash
#
# Bootstrap a Linux or macOS development environment.
#
#   ./scripts/bootstrap.sh [--debug] [--no-configure]

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

configuration="Release"
configure=1

for argument in "$@"; do
    case "$argument" in
        --debug)        configuration="Debug" ;;
        --no-configure) configure=0 ;;
        -h|--help)
            sed -n '2,6p' "$0"
            exit 0
            ;;
        *)
            echo "unknown argument: $argument" >&2
            exit 2
            ;;
    esac
done

step() { printf '\n==> %s\n' "$1"; }
ok()   { printf '    %s\n' "$1"; }
warn() { printf '    %s\n' "$1" >&2; }

step 'Checking the toolchain'

case "$(uname -s)" in
    Linux)
        profile="$repo_root/conan/profiles/linux-gcc"
        preset='linux-gcc'

        if command -v g++ >/dev/null 2>&1; then
            gcc_major="$(g++ -dumpversion | cut -d. -f1)"
            if [ "$gcc_major" -lt 14 ]; then
                warn "g++ $gcc_major found; GCC 14+ is required for C++23 library support."
            else
                ok "g++ $gcc_major"
            fi
        elif command -v clang++ >/dev/null 2>&1; then
            ok "clang++ $(clang++ -dumpversion)"
        else
            echo 'No C++ compiler found.' >&2
            exit 1
        fi
        ;;
    Darwin)
        profile="$repo_root/conan/profiles/macos-clang"
        preset='macos-clang'

        if ! xcode-select -p >/dev/null 2>&1; then
            echo 'Xcode command line tools not found. Run: xcode-select --install' >&2
            exit 1
        fi
        ok "Apple Clang $(clang++ -dumpversion)"
        ;;
    *)
        echo "Unsupported platform: $(uname -s)." >&2
        exit 1
        ;;
esac

if ! command -v ninja >/dev/null 2>&1; then
    echo 'ninja not found. Install it (apt install ninja-build / brew install ninja).' >&2
    exit 1
fi
ok "ninja $(ninja --version)"

if ! command -v cmake >/dev/null 2>&1; then
    echo 'cmake not found. CMake 3.29 or newer is required.' >&2
    exit 1
fi
ok "cmake $(cmake --version | head -1 | awk '{print $3}')"

step 'Checking Conan'

if ! command -v conan >/dev/null 2>&1; then
    echo 'Conan is not installed. Run: python3 -m pip install "conan==2.32.0"' >&2
    exit 1
fi
ok "$(conan --version)"

step 'Installing dependencies (builds LuaJIT and Boost on first run)'

conan install . --profile "$profile" --build=missing -s "build_type=$configuration"

ok 'dependencies installed'

if [ "$configure" -eq 0 ]; then
    printf '\nBootstrap complete (configure skipped).\n'
    exit 0
fi

step 'Configuring CMake'

cmake --preset "$preset"

cat <<EOF

Bootstrap complete.

Next:
    cmake --build build/Release
    ctest --preset $preset
    ./build/Release/src/mcode
EOF

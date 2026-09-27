#!/usr/bin/env bash
#
# Verifies that the shipped API definition file matches the documented surface.
#
# `luau-analyze` cannot do this: definition files require
# ParseOptions::allowDeclarationSyntax and Mode::Definition, and the CLI parses
# every input as an ordinary script. So the check runs through
# tools/api_check, which calls Luau's Frontend::loadDefinitionFile.
#
# Two assertions, both required:
#   1. fixtures/valid.luau type-checks clean against extensions/mcode.d.luau.
#   2. fixtures/invalid.luau does NOT. If it ever passes, the definition file
#      has degraded to `any` and stopped protecting anything.
#
# Usage: tools/api_check/run.sh <path-to-luau-checkout> [build-dir]

set -euo pipefail

luau_source="${1:?usage: run.sh <path-to-luau-checkout> [build-dir]}"
build_dir="${2:-${TMPDIR:-/tmp}/mcode-api-check-build}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
definition="${repo_root}/extensions/mcode.d.luau"
fixtures="${repo_root}/tools/api_check/fixtures"

cmake -S "${repo_root}/tools/api_check" -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLUAU_SOURCE_DIR="${luau_source}" >/dev/null

cmake --build "${build_dir}" >/dev/null

checker="${build_dir}/mcode_api_check"
if [ ! -x "${checker}" ]; then
    checker="${build_dir}/mcode_api_check.exe"
fi

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

# The positive fixture requires a module, which the resolver looks up relative
# to the fixture's directory.
mkdir -p "${work}/lib"
printf 'return { name = "util" }\n' > "${work}/lib/util.luau"
cp "${fixtures}/valid.luau" "${fixtures}/invalid.luau" "${work}/"

cd "${work}"

echo "== valid.luau must type-check =="
"${checker}" "${definition}" valid.luau

echo
echo "== invalid.luau must NOT type-check =="
if "${checker}" "${definition}" invalid.luau 2>/dev/null; then
    echo "FAIL: invalid.luau type-checked clean -- the definition file no longer constrains anything" >&2
    exit 1
fi
echo "ok: invalid.luau was rejected as expected"

echo
echo "API definition check passed."

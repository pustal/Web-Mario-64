#!/usr/bin/env bash
# build-webos.sh
#
# One-shot build of the webOS IPK.
#
# Requirements:
#   - The webosbrew toolchain (arm-webos-linux-gnueabi_sdk-buildroot), unpacked
#     and relocated (run its relocate-sdk.sh once). Point WEBOS_SDK at it, e.g.
#       export WEBOS_SDK=$HOME/arm-webos-linux-gnueabi_sdk-buildroot
#     Releases: https://github.com/openlgtv/buildroot-nc4/releases
#     The toolchain is Linux x86_64 only; on macOS build inside a Linux VM or container.
#   - ares-package from @webos-tools/cli (npm install -g @webos-tools/cli)
#   - CMake 3.26+, Ninja or Make, git, Python 3, a host C/C++ compiler
#     (Torch, which packs Ghostship's own assets, runs on the build machine)
#
# Usage: scripts/build-webos.sh [extra cmake args...]
# Output: dist/org.webmario64.app_<version>_arm.ipk (+ the Homebrew Channel manifest)

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${WEBOS_SDK:?Set WEBOS_SDK to the unpacked arm-webos-linux-gnueabi_sdk-buildroot directory}"

toolchain_file="${WEBOS_SDK}/share/buildroot/toolchainfile.cmake"
if [[ ! -f "${toolchain_file}" ]]; then
    echo "No toolchain file at ${toolchain_file}" >&2
    exit 1
fi

generator=()
if command -v ninja >/dev/null 2>&1; then
    generator=(-G Ninja)
fi

# Ghostship (pinned in cmake/FetchGhostship.cmake) is downloaded and patched at configure time.
cmake -S "${repo_root}" -B "${repo_root}/build-webos" "${generator[@]}" \
    -DCMAKE_TOOLCHAIN_FILE="${toolchain_file}" \
    -DCMAKE_BUILD_TYPE=Release \
    "$@"
cmake --build "${repo_root}/build-webos" --parallel
cmake --build "${repo_root}/build-webos" --target webos-package

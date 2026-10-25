#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# reproduce-xbox-uwp-dev.sh:
# Creates or updates the `xbox-uwp-dev` feature branch off `windows` on a user fork of
# https://github.com/Supermedo/bloodborne_pc when operating outside Arena's fixed-branch sandbox.
set -euo pipefail

UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/Supermedo/bloodborne_pc.git}"
FORK_URL="${1:-}"
TARGET_BRANCH="xbox-uwp-dev"
BASE_BRANCH="windows"
PATCH_SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ -z "${FORK_URL}" ]]; then
    echo "Usage: $0 <git@github.com:YOUR_USER/bloodborne_pc.git>"
    echo "Creates branch '${TARGET_BRANCH}' from '${BASE_BRANCH}' (never touching main/windows directly)"
    echo "and applies the BBPort UWP port commits cleanly."
    exit 1
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

git clone --branch "${BASE_BRANCH}" "${UPSTREAM_URL}" "${WORK_DIR}/repo"
cd "${WORK_DIR}/repo"
git remote add fork "${FORK_URL}"
git checkout -b "${TARGET_BRANCH}"

# Copy modified shared files, root docs, and in-tree /uwp/ directory from this checkout
rsync -a --exclude='.git' \
    "${PATCH_SOURCE_DIR}/.gitignore" \
    "${PATCH_SOURCE_DIR}/README.md" \
    "${PATCH_SOURCE_DIR}/XBOX_UWP.md" \
    "${PATCH_SOURCE_DIR}/gpu" \
    "${PATCH_SOURCE_DIR}/src" \
    "${PATCH_SOURCE_DIR}/uwp" \
    "${WORK_DIR}/repo/"

git add .gitignore README.md XBOX_UWP.md gpu/bbgpu.h gpu/CMakeLists.txt \
    src/compat_win.c src/probe.c src/runtime_audio.c \
    src/runtime_file.c src/runtime_memory.c src/runtime_pad.c uwp/

git commit -m "uwp: add Xbox Dev Mode UWP port (D3D12 FL 12_0, WGI gamepad, StorageLayer, LauncherPage)"
git push -u fork "${TARGET_BRANCH}"
echo "Pushed ${TARGET_BRANCH} to ${FORK_URL}."

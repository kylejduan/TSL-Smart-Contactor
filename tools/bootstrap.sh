#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
patch_only=false
sdk_dir=.tools/esp-idf
if (( $# )); then
  if [[ $# != 2 || $1 != --patch-sdk ]]; then
    printf '%s\n' 'Usage: tools/bootstrap.sh [--patch-sdk EXISTING_SDK_DIRECTORY]' >&2
    exit 2
  fi
  patch_only=true
  sdk_dir=$2
elif [[ ! -e "$sdk_dir/.git" ]]; then
  mkdir -p .tools
  git clone --depth 1 --branch v5.5.2 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git "$sdk_dir"
fi
if [[ "$(git -C "$sdk_dir" rev-parse HEAD)" != 30aaf64524299d3bde422ca9a2848090d1bc5d0f ]]; then
  printf '%s\n' 'ESP-IDF does not match the pinned v5.5.2 commit; SDK left unchanged.' >&2
  exit 1
fi
sdk_patch=$PWD/tools/patches/esp-idf-v5.5.2-https-oom.patch
if git -C "$sdk_dir" apply --check "$sdk_patch" 2>/dev/null; then
  git -C "$sdk_dir" apply "$sdk_patch"
elif ! git -C "$sdk_dir" apply --reverse --check "$sdk_patch" 2>/dev/null; then
  printf '%s\n' 'ESP-IDF HTTPS cleanup patch does not match this SDK; inspect local changes.' >&2
  exit 1
fi
if "$patch_only"; then
  exit 0
fi
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$PWD/.tools/toolchain}"
"$sdk_dir/install.sh" esp32s3
printf '%s\n' 'Next: source .tools/esp-idf/export.sh; idf.py set-target esp32s3; idf.py build'

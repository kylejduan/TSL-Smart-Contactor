#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .tools
if [[ ! -d .tools/esp-idf/.git ]]; then
  git clone --depth 1 --branch v5.5.2 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git .tools/esp-idf
fi
[[ "$(git -C .tools/esp-idf rev-parse HEAD)" == 30aaf64524299d3bde422ca9a2848090d1bc5d0f ]]
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$PWD/.tools/toolchain}"
.tools/esp-idf/install.sh esp32s3
printf '%s\n' 'Next: source .tools/esp-idf/export.sh; idf.py set-target esp32s3; idf.py build'

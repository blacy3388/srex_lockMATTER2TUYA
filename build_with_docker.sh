#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

docker run --rm \
  -v "$PROJECT_DIR:/project" \
  -w /project \
  espressif/esp-matter:release-v1.6 \
  bash /project/build_release.sh

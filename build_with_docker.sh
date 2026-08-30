#!/usr/bin/env bash
set -euo pipefail

docker run --rm \
  -v "$PWD:/project" \
  -w /project \
  espressif/esp-matter:release-v1.6 \
  bash /project/build_release.sh

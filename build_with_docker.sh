#!/usr/bin/env bash
set -euo pipefail

docker run --rm -it \
  -v "$PWD:/project" \
  -w /project \
  espressif/esp-matter:latest \
  bash -lc './build_release.sh'

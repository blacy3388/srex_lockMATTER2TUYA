#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

rm -rf build release
mkdir -p release

idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.c6_thread" set-target esp32c6
idf.py build
idf.py merge-bin -o build/srex-c6-matter-thread-lock-merged.bin -f raw

cp build/flasher_args.json release/
cp build/flash_args release/
cp build/bootloader/bootloader.bin release/
cp build/partition_table/partition-table.bin release/
cp build/srex_c6_matter_thread_lock.bin release/
cp build/srex-c6-matter-thread-lock-merged.bin release/

{
  printf 'ESP-IDF: '
  git -C "$IDF_PATH" rev-parse HEAD
  printf 'ESP-Matter: '
  git -C "$ESP_MATTER_PATH" rev-parse HEAD
} > release/build-versions.txt

echo
echo "READY FOR ESP LAUNCHPAD DIY:"
echo "  release/srex-c6-matter-thread-lock-merged.bin"
echo "  Flash address: 0x0"
echo

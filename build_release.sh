#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

: "${IDF_PATH:?IDF_PATH must point to an ESP-IDF checkout}"
: "${ESP_MATTER_PATH:?ESP_MATTER_PATH must point to an ESP-Matter checkout}"
command -v idf.py >/dev/null 2>&1 || {
  echo "ERROR: idf.py is not available in PATH" >&2
  exit 1
}

rm -rf build
rm -f sdkconfig sdkconfig.old
mkdir -p release
# Keep the tracked usage note, but never allow stale firmware files into an
# artifact uploaded after a subsequent build.
find release -mindepth 1 -maxdepth 1 ! -name README.txt -exec rm -rf -- {} +

# Passing SDKCONFIG_DEFAULTS explicitly replaces IDF's automatic defaults, so
# include the project-wide settings and the C6 Thread overrides together.
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.c6_thread" set-target esp32c6

if ! grep -q '^CONFIG_MBEDTLS_HKDF_C=y$' sdkconfig; then
  echo "ERROR: CONFIG_MBEDTLS_HKDF_C=y was not applied to sdkconfig" >&2
  exit 1
fi

idf.py build
idf.py merge-bin -o srex-c6-matter-thread-lock-merged.bin -f raw

cp build/flasher_args.json release/
cp build/flash_args release/
cp build/bootloader/bootloader.bin release/
cp build/partition_table/partition-table.bin release/
cp build/srex_c6_matter_thread_lock.bin release/
cp build/srex-c6-matter-thread-lock-merged.bin release/

print_revision() {
  local repo="$1"
  local revision
  if revision="$(git -C "$repo" rev-parse HEAD 2>/dev/null)"; then
    printf '%s\n' "$revision"
  else
    printf 'unknown\n'
  fi
}

{
  printf 'ESP-IDF: '
  print_revision "$IDF_PATH"
  printf 'ESP-Matter: '
  print_revision "$ESP_MATTER_PATH"
} > release/build-versions.txt

echo
echo "READY FOR ESP LAUNCHPAD DIY:"
echo "  release/srex-c6-matter-thread-lock-merged.bin"
echo "  Flash address: 0x0"
echo

#!/usr/bin/env bash
set -euo pipefail

rm -rf build release
mkdir -p release

idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.c6_thread" set-target esp32c6
idf.py build

cp build/flasher_args.json release/
cp build/flash_args release/
cp build/bootloader/bootloader.bin release/
cp build/partition_table/partition-table.bin release/
cp build/*.bin release/ 2>/dev/null || true

cd build
APP_BIN=$(python - <<'PY'
import json
with open('project_description.json') as f:
    print(json.load(f)['app_bin'])
PY
)
if command -v esptool.py >/dev/null 2>&1; then
  ESPTOOL=esptool.py
else
  ESPTOOL=esptool
fi
$ESPTOOL --chip esp32c6 merge_bin -o ../release/srex-c6-matter-thread-lock-merged.bin $(cat flash_args | tr '\n' ' ')
cd ..

echo
echo "READY FOR ESP LAUNCHPAD DIY:"
echo "  release/srex-c6-matter-thread-lock-merged.bin"
echo "  Flash address: 0x0"
echo

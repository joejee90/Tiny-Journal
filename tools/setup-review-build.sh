#!/bin/sh
set -eu
TJ_CLI=${TJ_CLI:-arduino-cli}
TJ_SKETCH=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TJ_ENV=$(dirname "$TJ_SKETCH")/ReviewToolchain
mkdir -p "$TJ_ENV/data" "$TJ_ENV/downloads" "$TJ_ENV/user"
cat > "$TJ_ENV/arduino-cli.yaml" <<CONFIG
board_manager:
  additional_urls:
    - https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
directories:
  data: $TJ_ENV/data
  downloads: $TJ_ENV/downloads
  user: $TJ_ENV/user
CONFIG
"$TJ_CLI" --config-file "$TJ_ENV/arduino-cli.yaml" core update-index
"$TJ_CLI" --config-file "$TJ_ENV/arduino-cli.yaml" core install m5stack:esp32@3.2.2
"$TJ_CLI" --config-file "$TJ_ENV/arduino-cli.yaml" lib install 'M5GFX@0.3.0' 'M5Unified@0.3.0' 'M5Cardputer@1.1.1' 'NimBLE-Arduino@2.5.1' 'ArduinoJson@7.4.3'
python3 "$TJ_SKETCH/tools/generate-guide.py" --check
TJ_PREFIX_FLAGS="-ffile-prefix-map=$HOME=/build -fmacro-prefix-map=$HOME=/build -ffile-prefix-map=/private/tmp=/build -fmacro-prefix-map=/private/tmp=/build -ffile-prefix-map=/tmp=/build -fmacro-prefix-map=/tmp=/build -ffile-prefix-map=$TJ_SKETCH=/Tiny_Journal -fmacro-prefix-map=$TJ_SKETCH=/Tiny_Journal"
"$TJ_CLI" --config-file "$TJ_ENV/arduino-cli.yaml" compile --build-property "compiler.c.extra_flags=$TJ_PREFIX_FLAGS" --build-property "compiler.cpp.extra_flags=$TJ_PREFIX_FLAGS" --fqbn m5stack:esp32:m5stack_cardputer:USBMode=default,CDCOnBoot=cdc,UploadMode=cdc,FlashSize=8M,PartitionScheme=huge_app "$TJ_SKETCH"

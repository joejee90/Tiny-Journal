#!/bin/sh
set -eu
TJ_CLI=${TJ_CLI:-arduino-cli}
TJ_BOARD='m5stack:esp32:m5stack_cardputer:USBMode=default,CDCOnBoot=cdc,UploadMode=cdc,FlashSize=8M,PartitionScheme=huge_app'
TJ_SKETCH=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python3 "$TJ_SKETCH/tools/generate-guide.py" --check
# Map local checkout, home and temporary paths out of __FILE__ and debug data.
# Apply to C too: core assertions can otherwise expose a personal home path.
TJ_PREFIX_FLAGS="-ffile-prefix-map=$HOME=/build -fmacro-prefix-map=$HOME=/build -ffile-prefix-map=/private/tmp=/build -fmacro-prefix-map=/private/tmp=/build -ffile-prefix-map=/tmp=/build -fmacro-prefix-map=/tmp=/build -ffile-prefix-map=$TJ_SKETCH=/Tiny_Journal -fmacro-prefix-map=$TJ_SKETCH=/Tiny_Journal"
exec "$TJ_CLI" compile --build-property "compiler.c.extra_flags=$TJ_PREFIX_FLAGS" --build-property "compiler.cpp.extra_flags=$TJ_PREFIX_FLAGS" --fqbn "$TJ_BOARD" "$TJ_SKETCH" "$@"

#!/bin/sh
set -eu
TJ_TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TJ_TEST_TMP=$(mktemp -d)
trap 'rm -rf "$TJ_TEST_TMP"' EXIT
for test in menu_ui_test menu_animation history_test bluetooth_retry storage_faults layout_limits guide_stream template_metadata input_test modal_repeat printer_text_test; do
  "${CXX:-c++}" -std=c++17 -fsanitize=address,undefined -g "$TJ_TEST_ROOT/tests/$test.cpp" -o "$TJ_TEST_TMP/$test"
  "$TJ_TEST_TMP/$test"
done
"${CXX:-c++}" -std=c++17 -pthread -I"$TJ_TEST_ROOT/tests/shims" "$TJ_TEST_ROOT/tests/printer_tcp_test.cpp" -o "$TJ_TEST_TMP/printer_tcp_test"
"$TJ_TEST_TMP/printer_tcp_test"

python3 "$TJ_TEST_ROOT/tests/bluetooth_shutdown.py"

python3 "$TJ_TEST_ROOT/tests/bluetooth_shortcuts.py"

python3 "$TJ_TEST_ROOT/tests/wifi_save.py"
python3 "$TJ_TEST_ROOT/tests/bulk_confirmation.py"
python3 "$TJ_TEST_ROOT/tests/release_privacy.py"
python3 "$TJ_TEST_ROOT/tests/guide_generation.py"
python3 "$TJ_TEST_ROOT/tests/factory_reset.py"
python3 "$TJ_TEST_ROOT/tests/factory_reset_completion.py"
python3 "$TJ_TEST_ROOT/tests/config_persistence.py"
python3 "$TJ_TEST_ROOT/tools/generate-guide.py" --check

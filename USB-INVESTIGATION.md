# USB mass-storage repair

A concrete descriptor-registration cause was found in the pinned Arduino ESP32 3.2.2 core:

- `cores/esp32/main.cpp` calls `USB.begin()` from `app_main()` when USB-OTG and CDC-on-boot are enabled. This happens before sketch `setup()`.
- `cores/esp32/USBMSC.cpp` registers the MSC interface from `USBMSC::USBMSC()`.
- `cores/esp32/esp32-hal-tinyusb.c`, `tinyusb_enable_interface2()`, rejects interface registration after TinyUSB has initialized.
- The sketch allocated `new USBMSC()` only after entering its dedicated storage boot function in `setup()`. Rebooting did not help: CDC-on-boot had already frozen the interface set on each reboot. Calling `USB.begin()` again is idempotent and cannot add the missing interface.

The repair constructs the MSC interface globally, before `app_main()`. In normal journal mode it advertises no media, so the host cannot read or write the card. Only the dedicated storage boot enables the callbacks, sets valid capacity and then publishes media. The editor, BLE and network tasks do not start in that mode. There are no FAT filesystem operations after host exposure; a reboot remounts the card after eject.

The host may display an empty removable device while the journal is running. That is intentional: the early descriptor is needed for CDC-on-boot, but the SD card is unavailable to USB until storage mode is selected.

The eject callback now marks media absent as well as disarming raw callbacks. A physical key exits storage mode only after host eject. A diagnostic build also accepts the single serial character `X` after successful host eject; this is compiled out of the normal build.

Hardware acceptance passed on the connected Cardputer: storage mode exposed the 3.9 GB FAT32 SD card to macOS. Journal files were read, copied to a private backup and SHA-256 checked against the source. The host ejected the disk successfully, and the diagnostic exit returned to normal journal mode. No filesystem formatting or USB driver changes were needed.

## Serial-triggered reboot while the storage screen remains visible

A later failure presented as the ESP32 ROM USB JTAG/serial device rather than the Cardputer application, even though the LCD still showed the storage screen. The LCD can retain its previous contents across that reboot; it is not proof that the MSC application is running.

The pinned core enables CDC-triggered reboot by default. USBCDC::_onLineCoding() enters RESTART_BOOTLOADER on a 1200-baud request; _onLineState() also recognizes a DTR/RTS bootloader sequence. A controlled baseline test mounted the card, safely ejected it, then reproduced the ROM device identity with a 1200-baud request. The source of the user's original triggering request was not established.

The dedicated storage boot now calls Serial.enableReboot(false), conditional on CDC-on-boot. Normal journal boot retains the default upload/reset behaviour. The guarded acceptance build mounted the 3.9 GB FAT32 card; journal files were read. After host eject, both the 1200-baud request and complete DTR/RTS reset sequence left the native application interface running. The guarded exit then returned to normal journal mode. Tests sent reboot requests only after eject to avoid interrupting host filesystem writes.

Leave storage mode before uploading firmware. No driver or filesystem changes were made.

Final production acceptance also passed through the normal on-device menu, selected by the user: macOS mounted the 3.9 GB FAT32 volume and journal files were readable. The volume was left mounted for the user; no serial reset tests were performed while it was mounted.

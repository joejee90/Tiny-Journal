# Tiny Journal V. 2026.11.14 — release review

This review covers the source and production firmware for **V. 2026.11.14**. It is a local release candidate; GitHub publication and M5Burner packaging have not been performed.

## Tested build configuration

M5Stack ESP32 **3.2.2**, M5Cardputer **1.1.1**, M5Unified **0.3.0**, M5GFX **0.3.0**, NimBLE-Arduino **2.5.1** and ArduinoJson **7.4.3**. Target: original Cardputer, USB-OTG/TinyUSB with CDC-on-boot, 8 MB flash and Huge APP (3 MB). Production builds exclude `TJ_DIAGNOSTICS`.

Board **3.3.10** was compared separately with the same libraries and microphone diagnostic. Across six capture/speaker-handoff cycles, 3.2.2 produced 600 varying sample blocks; 3.3.10 settled at constant -8 samples with zero variance. This establishes the 3.2.2 pin for the tested unit; it is not a claim about every board variant. Only numerical metrics were retained.

## Automated checks

The combined production build fits the 3 MB application partition. Source, generated guide and compiled app/bootloader/partition images pass the privacy gate; additional direct scans confirm zero personal home paths, local username or diagnostic-command strings in the production app. Dependency pins retain their actual tested versions.

The host suite exercises storage replacement/recovery with 59 injected failures, strict UTF-8 and HID report parsing, six layouts, bounded layout preflight, undo/history, modal input and repeat ownership, Bluetooth retry/shutdown and settings shortcuts, menu geometry and buffered row animation, plus receipt encoding/TCP delivery, cutter configuration, Wi-Fi ownership and allocation failures.

The release review adds regressions for malformed UTF-8 bypassing layout limits, bounded template metadata and template counts, fresh-key bulk-delete confirmation, Wi-Fi configuration save failures, reset-failure handling, guide provisioning/paging and privacy checks. These checks run actual shared helpers or extracted handlers; they do not emulate an entire ESP32/FreeRTOS system.

## Hardware checks completed

- Original flash and journal were backed up privately before implementation. Recovery images and journal contents are excluded from public assets.
- SD write/readback/cleanup passed using a uniquely allocated test file.
- USB storage mounted through the normal device menu and journal files were readable. Eject and return to the journal passed. Early descriptor registration and storage-only suppression of serial-triggered bootloader resets were tested. See [USB-INVESTIGATION.md](USB-INVESTIGATION.md).
- Exactly three failed automatic Bluetooth attempts were observed, followed by stack/controller shutdown. Manual radio Off and independent power-on-connect preferences were exercised and restored.
- A short test receipt reached an ESC/POS network printer. The tester confirmed both printing and cutting. No personal printer address is included in release source or firmware.
- The embedded Google trust roots validated unauthenticated Google OAuth/Drive endpoint TLS. No account credentials were sent in that check.

The reviewed production firmware was installed with a verified flash hash. The tester confirmed the new guide opens, pages and returns correctly, then selected USB storage. The installed `/journal/README.txt` matched the generated marker plus the reviewed README byte-for-byte and carried **V. 2026.11.14**. The SD volume was left mounted for the owner.

## Still requiring acceptance

- **Google account upload/update/repeat-sync and interrupted-request recovery:** pending. The owner explicitly chose to leave account testing pending. No credentialed account operation was run during this release review.
- **Keyboard compatibility:** pairing, typing, all navigation screens and sleep/wake should be checked with each claimed keyboard model. Host HID tests do not establish universal device compatibility.
- **Audio:** normal recording/playback and acoustic quality need a listening check, including recording again after playback.
- **UI:** the guide reader was opened, paged and exited successfully by the tester. Other menu/editor text sizes and cursor/wrapping still benefit from a final visual check.
- **Printing:** the short receipt/cut passed; longer notes, optional live printing, paper width and non-ASCII output need checking on each supported printer.

These are explicit limits of the validation, not a claim that every path has been tested. The release should retain its experimental Google Drive description until account testing is complete.

## Publication hygiene

The original local review history contains private test documentation and machine-derived commit metadata. Keep it private. Publish from the clean release checkout/history supplied with this review, not the old development bundle.

Public firmware must be freshly compiled from source, with home/check-out paths removed from both C and C++ objects. Publish only the intended source files and clean compiled firmware assets. Exclude SD contents, config credentials, full-device flash recovery dumps, NVS/bonds, debug ELF/map files, build options, local logs and private backups. Public Google CA certificates and synthetic loopback tests are intentional.

The README is the source for the generated on-device guide. Run `tools/generate-guide.py --check`, the host suite, a production compile and the release privacy scan before packaging. Full version is centralised in `JournalVersion.h`; the SCSI inquiry field uses a documented four-character release code because that field cannot hold the full date.

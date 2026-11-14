# Release review — V. 2026.11.14

## Decision

Source, documentation, privacy checks, production compilation and automated regressions pass. No confirmed code blocker remains after the fixes below. Treat this as a release candidate: account-specific Google Drive acceptance remains pending by the owner's choice, and the hardware checks listed in VALIDATION.md must not be presented as universally validated compatibility.

GitHub publication and M5Burner packaging are intentionally not part of this review. No account upload was performed. On-device guide acceptance passed: the tester opened, paged and exited the reader, and the SD README matched the reviewed source byte-for-byte.

## Issues found and resolved

- Malformed UTF-8 could bypass the wrapped-line memory estimate. Preflight now uses the same strict sequence lengths as the editor.
- Template discovery could read arbitrarily large bodies into heap at boot. Metadata parsing, line/header size, file size and template count are bounded; body reads are checked before creating a document.
- Bulk document/audio deletion lacked a fresh confirmation. It now requires a new Y keypress after held/queued input is released. Failed audio deletions remain listed.
- Wi-Fi setup could claim a save succeeded after an SD failure. It now stays in setup and reports failure.
- Clock/last-document state writes could fail silently. Settings errors are reported accurately; a successfully committed note remains saved when only auxiliary state fails.
- Factory reset could report success after deletion or recreation failed. Completion now verifies essential directories, configuration and default templates, plus the checked blank-note save and last-document tracking.
- The README and previous review notes contained a personal test printer address. Public documentation now uses instructions/placeholders; printer defaults remain empty.
- Compiler assertions embedded local home/temp paths. Both C and C++ builds now use prefix mapping; the final production image was scanned directly.
- Firmware branding used a shortened release date. Full version is centralised in JournalVersion.h and used by About and the generated guide; README and release filenames use V. 2026.11.14. Dependency versions retain their correct tested values.
- The root SD guide was not selectable in the category-only document browser. A Main-menu read-only guide reader now streams bounded pages without modifying editor text/history.

## Verification

- Full combined host regression suite passed, including storage fault injection, UTF-8/HID/layout/history, Bluetooth retries/shutdown/shortcuts, menu animation, printer loopback/cutting/allocation failures, configuration failures, deletion/reset failures, guide ownership/paging/generation and privacy-gate tests.
- Production build: approximately 2.05 MB of the 3 MB application partition; static globals 79,264 bytes. Pinned board package 3.2.2 and dependency versions are documented in README.
- Embedded Google trust roots verified live TLS to OAuth and Drive endpoints without credentials. Unauthenticated HTTP error responses were expected; account sync was not run.
- Actual M5GFX font tables contain the required Latin/currency glyphs and all 66 Russian letters.
- Source and compiled application/bootloader/partition images passed the privacy gate. Additional scans found no local username, home/temp paths or acceptance-command strings in the production app.
- The generated offline guide matches README byte-for-byte after its ownership/version marker. Licence terms were preserved unchanged.

## Publication boundary

Use the clean release checkout/source archive. The old private development history and recovery material must not be pushed or included in assets. Public source contains placeholders/public trust certificates, not SD credentials, notes, printer configuration, NVS or bonds. Debug ELF/map files, build options and local logs stay private.

Only freshly compiled images may be used for a future M5Burner package. A full-device recovery dump is not a release image. Privacy scanning is heuristic and complements manual review; it is not a guarantee against every possible unknown secret format.

Follow-up: editor top-bar geometry is fixed at the Standard height independently of menu font scaling; editor viewport calculations use the same fixed height.

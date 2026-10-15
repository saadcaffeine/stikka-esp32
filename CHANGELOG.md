# Changelog (SG fork)

Changes relative to upstream [stikka-factory/stikka-esp32](https://github.com/stikka-factory/stikka-esp32) at `3e263b6`.

## Unreleased

### Fixed
- **Settings could not be saved once a Wi-Fi network was saved.** The Forget button was rendered as a `<form>` inside the page's main `<form>`. Browsers close the outer form at the first inner `</form>`, which left the Save button and every field after the Wi-Fi card outside any form, so Save did nothing. Forget is now a script-driven `type="button"` that POSTs to `/wifi-forget`. This also restores saving of printer and label settings. (`2b9a007`)
- Wi-Fi names or values containing an apostrophe broke the single-quoted HTML attributes; `'` is now escaped.

### Changed
- **Web UI redesigned** (`28d0b77`): mobile-first layout, colour-coded sections, sticky header with live Wi-Fi / MQTT / Printer pills, sticky Save bar with an unsaved-changes indicator.
- Save, test print, Wi-Fi scan/connect, log level and log clear now run in the page and show button states (busy, done, failed) plus a toast, instead of loading plain-text pages.
- After saving, the page waits for the board to reconnect and reloads.
- Firmware upload shows progress and waits for the reboot.
- Changing printer type is in its own red card, apart from the Save form.
- Logs page keeps at most 300 lines in the browser and shows Live / Paused / Offline.

### Added
- `GET /status.json`: `{wifi, ip, ap, mqtt, printer, method}`, used by the status pills.
- `usbHostTargetReady()`: reports whether a USB printer has been claimed (`targets/usb_host_target.cpp`).

### Known issues
- After a USB transfer timeout (`0x10c`, `ESP_ERR_NOT_FINISHED`) the USB host target cannot recover until the board is rebooted.

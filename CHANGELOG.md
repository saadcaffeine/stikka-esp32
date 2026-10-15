# Changelog (SG fork)

Changes relative to upstream [stikka-factory/stikka-esp32](https://github.com/stikka-factory/stikka-esp32) at `3e263b6`.

## Unreleased

### Fixed
- **A failed print no longer wedges the bridge.** Previously a USB transfer that timed out was flushed but its cancellation was never collected, so the USB host library kept it "in flight" and every later job failed with `0x10c` (`ESP_ERR_NOT_FINISHED`) until a reboot. Timed-out transfers are now cancelled properly (halt, flush, wait for the callback, clear), bytes the printer already accepted are counted so nothing is sent twice, and a transfer that never reports back is replaced. Only if the USB stack is stuck beyond that does the bridge fail queued jobs with a clear message and reboot itself.
- **Printing no longer blocks the board.** Jobs used to be sent synchronously inside the MQTT message callback, freezing MQTT keepalives, the web UI and later jobs for as long as the printer took (minutes for a slow printer). Jobs now go into a queue and are sent one USB transfer per `loop()` pass.
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
- **Print queue** (`print_queue.cpp`): up to 4 jobs (MQTT and the web test print), sent in order. A job that fails is reported and the next one starts. Jobs that waited more than 2 minutes are dropped. New per-job statuses on the status topic: `queued`, `printing`, then `done` / `failed` with a reason.
- **Timeouts per job:** 3 s per USB transfer, failure after 10 s with no bytes accepted ("printer stopped accepting data (check labels, cover and power)"), 90 s hard cap. USB error statuses are retried with a short backoff (250/500/750 ms) before the job fails.
- Non-blocking target API (`targetSendBegin()` / `targetSendPoll()` in `targets/target.h`). USB host is truly incremental; network/serial keep their existing time-bounded send.
- Web UI: the Printer pill shows "Printing 45% (+1 queued)" and "Last print failed" (reason on hover), polling every 1.5 s while printing. Test print is queued instead of blocking the page.
- `tools/usb-sim/`: host-side simulation of the USB send path against a scripted fake printer (normal, partial, stalled, erroring, unplugged, wedged). Run `tools/usb-sim/run.sh`.
- `GET /status.json`: `{wifi, ip, ap, mqtt, printer, method}`, used by the status pills.
- `usbHostTargetReady()`: reports whether a USB printer has been claimed (`targets/usb_host_target.cpp`).

### Known issues
- The USB recovery path is verified in simulation; the cancel/flush behaviour on real hardware still needs confirming against a stalled printer (e.g. open the cover mid-print).
- Network and serial targets still send each job in one blocking call (bounded by their own 7 s idle / 60 s total timeouts).

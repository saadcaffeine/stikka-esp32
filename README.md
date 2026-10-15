# Stikka ESP32 bridge (SG fork)

Firmware with a web UI for the ESP32-S3 N16R8. It connects a label printer to [Stikka-MQTT](https://github.com/saadcaffeine/stikka-mqtt): the printer's USB port plugs into the ESP32, and the bridge forwards print jobs it receives over MQTT.

Fork of [stikka-factory/stikka-esp32](https://github.com/stikka-factory/stikka-esp32), used for the Singapore instance at https://stikka.ediblemakerspace.com.

## Supported hardware

- Board: ESP32-S3 DevKitC-1 **N16R8** (16 MB flash, 8 MB octal PSRAM; the only supported board).
- Printers:

| Printer | Method | Notes |
|---|---|---|
| Zebra / ZPL | Network | Printer on the same network (not recommended) |
| Zebra / ZPL | USB | Printer's USB-B port plugged into the ESP32 |
| Brother QL | USB | Tested on QL-500 with DK-22205. QL-500/550 need legacy mode in the frontend |
| Seiko SLP | USB | Printer's USB-B port plugged into the ESP32 |

## Using the web UI

Open `http://<printer-name>.local/` (or the board's IP). On a fresh board, join its `Stikka-XXXXXX` network (password `stikkaesp32`) and a setup page opens. The interface is designed for phones.

- **Header:** three live pills, always visible: Wi-Fi (with IP), MQTT, and Printer (ready / no printer; USB printers only). They refresh every 5 seconds and show "Device offline" if the board stops answering.
- **Config:** device summary, a test print button, then Wi-Fi, printer, label, MQTT and debug settings, with a Save bar at the bottom. Changing the printer type (which reboots the board) is in its own red card.
- **Firmware:** upload a `firmware.bin` over Wi-Fi, with a progress bar. The page waits for the reboot and reloads.
- **Logs:** live device log with level selector, pause and clear.

Colours mean the same on every page: blue Wi-Fi, violet MQTT, teal printer, amber label and firmware, slate device and debug, red destructive actions.

Buttons always show what is happening: idle, then a spinner with a verb ("Saving…", "Scanning…", "Uploading 43%"), then green on success or red on failure. The Save bar is grey with "No changes" until you edit something, then turns green with "Unsaved changes".

### Wi-Fi

You can save up to 10 networks. Scan, pick a network and Connect, or Connect/Forget saved ones. Saved passwords stay on the board and are never sent back to the browser. After Save the board reconnects and the page waits for it to come back.

## What this fork changes

See [CHANGELOG.md](CHANGELOG.md) for details.

- **Fixed:** Save and "Save and continue" silently did nothing as soon as one Wi-Fi network was saved, so printer and label settings could not be saved. Cause: the Forget button was a nested form, which HTML forbids.
- **Redesigned web UI:** mobile-first, colour-coded, live status pills, button states, in-page save/test/scan/connect.
- **New:** `/status.json` endpoint and printer-ready detection for USB printers.
- **Fixed:** Wi-Fi names containing an apostrophe broke the page markup.

## Build and flash

Requires [PlatformIO](https://platformio.org/). The only environment is `esp32-s3-devkitc-1`.

```
pio run -e esp32-s3-devkitc-1
```

If `pio` isn't on your PATH, use `python -m platformio run -e esp32-s3-devkitc-1`, or build from VS Code with the PlatformIO extension. The result is `.pio/build/esp32-s3-devkitc-1/firmware.bin`.

Flash by either:
- the **Firmware** page of the bridge's web UI (Wi-Fi, no cable), or
- USB with the web flasher on the Stikka site (`scripts/build-firmware.sh` stages the binaries).

Settings live in the board's flash (NVS) and survive reflashing.

## Troubleshooting

- **Job received but the printer only blinks or nothing prints:** check the printer and frontend setup first. For a Brother QL-500/550, enable legacy mode in the frontend (see the [stikka-mqtt README](https://github.com/saadcaffeine/stikka-mqtt#brother-ql-500--ql-550-legacy-raster-mode)).
- **Log shows `0x10c` on USB send:** a USB transfer timed out and left the host stack in a stuck state. Reboot the board. (Known limitation: the firmware does not recover on its own yet.)
- **No "Printer ready" pill:** the printer isn't enumerated. Check the cable, that the printer is on, and that the board's native USB port is used (not COM).

## More detail

[README_DETAILS.md](README_DETAILS.md) covers the firmware source layout, MQTT job formats, PSRAM, protocols and the Brother QL raster protocol.

## Credits

Upstream firmware by the stikka-factory contributors. Fork by [Saad Chinoy](https://github.com/saadcaffeine).

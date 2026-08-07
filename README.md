# ESP32 MQTT ZPL Bridge (MVP)

This folder contains the first firmware implementation for the ESP32 bridge.

Current scope:

- Configure Wi-Fi, MQTT and printer settings via web UI
- Subscribe to /<printername>/command/
- Publish status to /<printername>/status/ (retained)
- Accept ZPL jobs (utf8/base64_utf8) and raw/base64
  image jobs, forwarding them to whichever transport method the active
  firmware build compiles in: a network printer host:port, a dedicated
  hardware UART, or the board's own USB/programming port (see "Protocol +
  method" below)
- On STIKKA_PROTOCOL="ql" builds, accepts pre-rasterized Brother QL raster
  protocol jobs (`payload_type: "ql_raster"`) and forwards them byte-for-byte,
  same as "zpl" -- the frontend does the rasterization client-side (see
  "Brother QL raster protocol" below)
- Fallback AP mode if Wi-Fi is missing or unavailable
- mDNS responder once connected to a station network -- reachable at
  `http://<printerName>.local/` instead of needing to find its DHCP-assigned
  IP (see "Finding the device" below)
- Logs tab in the web UI, with a configurable log level

MQTT client: [espMqttClient](https://github.com/bertmelis/espMqttClient)
(`mqtt_bridge.cpp`), not PubSubClient. PubSubClient reads an entire MQTT
packet into one contiguous, pre-sized buffer before handing it to your
callback -- that buffer's size field is a `uint16_t`, an unavoidable hard
65535-byte ceiling per message regardless of the broker's own max packet
size, and the whole buffer has to be reserved up front (a fallback ladder in
`mqtt_bridge.cpp` used to try progressively smaller sizes on a fragmented
heap). espMqttClient instead streams a PUBLISH's payload to `onMqttMessage()`
in pieces through a small fixed ~1.4KB internal buffer regardless of the
message's total size (`index`/`len`/`total` in the callback), which
`mqtt_bridge.cpp` reassembles into the same JSON handling this firmware
already had -- so there's no more library-imposed per-message ceiling.

### PSRAM (esp32-s3-devkitc-1, required)

`esp32-s3-devkitc-1` (an N16R8 module -- 16MB flash + 8MB octal-SPI PSRAM,
`board_build.arduino.memory_type = qio_opi` in `platformio.ini`) is the only
supported board. ESP-IDF's allocator routes large mallocs/reallocs (the
reassembled JSON `String`, then its base64-decoded twin) to external PSRAM
automatically once it's enabled for the build, so a whole job -- ZPL text,
an image, or a Brother QL raster stream -- fits in memory at once regardless
of label size. Every job goes out and is processed as a single MQTT message;
there is no chunking protocol.

## Firmware source layout

`src/` is split by concern rather than one monolithic file:

- `main.cpp` -- setup()/loop() orchestration only, wiring the modules below together
- `config.h/.cpp` -- `AppConfig`, NVS load/save, runtime settings dump
- `logging.h/.cpp` -- ring-buffer logger backing the web Logs tab + serial/UART output
- `status_led.h/.cpp` -- NeoPixel/RGB status LED
- `wifi_manager.h/.cpp` -- station Wi-Fi connect/retry + fallback AP + captive DNS
- `mqtt_bridge.h/.cpp` -- MQTT connect, topics, status publishing, command parsing, dispatch to a target
- `web_ui.h/.cpp` -- config + logs web UI, adapting which fields it shows/saves to the compiled-in method and protocol
- `targets/target.h/.cpp` -- the shared target contract (`targetSend`/`targetSendString`/`targetSetup`/`targetMethodName`) plus a write-loop helper shared by the serial/usb targets
- `targets/network_target.cpp` -- **method="network"**: relays bytes to a TCP printer host:port (`cfg.zplTargetHost`/`zplTargetPort`). Compiled in when the active env defines `TARGET_NETWORK`
- `targets/serial_target.cpp` -- **method="serial"**: relays bytes to a dedicated hardware UART (UART2, `cfg.printerUartTxPin`/`printerUartRxPin`/`printerUartBaud`) wired straight to the printer -- distinct from both the debug UART and the USB port. Compiled in when the active env defines `TARGET_SERIAL`
- `targets/usb_target.cpp` -- **method="usb"**: relays bytes over the board's own USB/programming port (`Serial`/UART0, `cfg.printerUsbBaud`). Compiled in when the active env defines `TARGET_USB`. Reserves that port for print data -- serial debug output can't also use `usb` mode on this build (see "Debug output vs. the usb method" below)
- `targets/usb_host_target.cpp` -- **method="usb_host"**: not a serial-over-cable relay like "usb" above -- the board acts as a genuine USB host, enumerating whatever's plugged into its native USB-OTG port, claiming it if it's a USB Printer-class device (bInterfaceClass 0x07), and forwarding bytes as real USB bulk transfers. Compiled in when the active env defines `TARGET_USB_HOST`. Needs a chip with USB-OTG (ESP32-S2/S3) and board circuitry that can source VBUS to the downstream printer -- confirmed via schematic for `esp32-s2-saola-1` only; `esp32-s3-devkitc-1`'s VBUS-sourcing capability is **not** confirmed, see "Brother QL over USB host" below
- **protocol="ql"** has no dedicated target module -- the frontend builds the complete Brother QL raster byte stream client-side (`frontend/src/zpl-image.ts`) and `mqtt_bridge.cpp` forwards it byte-for-byte via the compiled-in method target, same as "zpl" (see "Brother QL raster protocol" below)

A target module implements the shared contract in `target.h` for one
transport **method**; `mqtt_bridge.cpp` decodes the MQTT job (**protocol**)
and hands the resulting bytes to whichever target the active PlatformIO env
compiles in via its `TARGET_NETWORK`/`TARGET_SERIAL`/`TARGET_USB` build flag.
Adding a new method means adding a new `targets/*_target.cpp` implementing
that contract, guarded by its own `TARGET_*` flag; adding a new protocol
(e.g. a Seiko SLP command translator) means adding a new
`targets/*_protocol.cpp` plus a `PROTOCOL_*` flag that `mqtt_bridge.cpp`
branches on, alongside new `<board>_<protocol>_<method>` env(s) in
`platformio.ini` -- each combination is its own firmware build, not a
runtime option.

### Protocol + method

`platformio.ini` factors envs into shared `env_<protocol>_<method>` base
sections (each setting `PROTOCOL`/`METHOD` string macros for display, plus
the real `TARGET_*`/`PROTOCOL_*` compile switches) that per-board leaf envs
`extends`. Currently:

| protocol | method | env base | status |
|---|---|---|---|
| zpl | network | `env_zpl_network` | implemented (`esp32-s3-devkitc-1_zpl_network` -- default env) |
| zpl | serial | `env_zpl_serial` | implemented in source (`targets/serial_target.cpp`), no board env currently defined |
| zpl | usb_host | `env_zpl_usb_host` | implemented in source, no board env currently defined |
| zpl | usb | -- | implemented in source (`targets/usb_target.cpp`), no env base or board env currently defined in `platformio.ini` |
| ql | usb_host | `env_ql_usb_host` | implemented (`esp32-s3-devkitc-1_ql_usb_host`, active) -- see "Brother QL over USB host" below |

"zpl" protocol jobs (both `payload_type: "zpl"` and `payload_type: "image"`)
are forwarded byte-for-byte regardless of method, same as before. "ql"
protocol builds only accept `payload_type: "ql_raster"` jobs (the frontend
pre-rasterizes to the Brother QL raster protocol and sends the ready-to-print
bytes) -- a `"zpl"` or `"image"` job is rejected with a clear MQTT job-status
error, since this build no longer decodes any image format itself.

### Debug output vs. the `usb`/`usb_host` methods

On a `usb`-method build, the board's USB/programming port is a single
physical UART (UART0, `Serial`) reserved for print data, so serial debug
output can't also live there. On a `usb_host`-method build, `Serial` itself
isn't touched (the printer talks over real USB bulk transfers via the native
OTG peripheral, not `Serial`) -- but on a board like `esp32-s2-saola-1`
where that native peripheral and the debug-UART bridge chip's "COM port"
share the same physical connector (see "Brother QL over USB host" below),
claiming it for host mode leaves debug output nowhere to go either way.

Either case is gated the same way in code, via `#if defined(TARGET_USB) ||
defined(USB_HOST_SHARES_DEBUG_PORT)`: `DEFAULT_DEBUG_OUTPUT_MODE` is
overridden to `"uart"` for those envs, the web UI's debug-mode field is
read-only on that build, `handleSave()` refuses to persist `"usb"` for it
regardless of what's posted, and `logging.cpp`'s
`applyDebugOutputSetting()` refuses `"usb"` as a last-resort guard (falling
back to disabled if the configured debug UART pins are also unset). Use
`uart` mode with `cfg.debugUartTxPin`/`debugUartRxPin` (a third, separate
UART) if you need serial logs on one of these builds. `USB_HOST_SHARES_DEBUG_PORT`
is only set per-board where that hardware conflict is real (currently just
`esp32-s2-saola-1`) -- a `usb_host` board without it (currently
`esp32-s3-devkitc-1`) can use `"usb"` debug mode freely alongside `usb_host`,
since its native USB and bridge-chip ports are physically independent.

### `Serial` on ESP32-S3 boards (esp32-s3-devkitc-1)

This board has two USB-C connectors, silkscreened "USB" (wired straight to
the S3's native USB peripheral, no chip in between) and "UART" (wired to an
onboard USB-to-UART bridge chip -- the classic "COM port"), and whether
`Serial` means one or the other depends on `ARDUINO_USB_MODE`/
`ARDUINO_USB_CDC_ON_BOOT`, set differently per env in `platformio.ini`:

- **`esp32-s3-devkitc-1_zpl_network`** -- no physical printer is ever wired
  to `Serial` on this env (method is a plain TCP connection), so it's free
  to use for debug logging however's more convenient: `ARDUINO_USB_MODE=1`
  (the board's own default, left alone) + `ARDUINO_USB_CDC_ON_BOOT=1` makes
  `Serial` the native `HWCDC` object, so `logging.cpp`'s `"usb"` debug
  output mode (the default here) goes out the **"USB"** port as raw USB-CDC,
  not through the bridge chip.
- **`esp32-s3-devkitc-1_zpl_usb`/`_ql_usb`** (`TARGET_USB`: `Serial` *is*
  the physical wire to the printer, see `usb_target.cpp`) --
  `ARDUINO_USB_MODE=0` overrides the board default so `Serial` stays real
  UART0, reaching the printer through the **"UART"** bridge-chip port
  (harmless "ARDUINO_USB_MODE redefined" compiler warning from overriding
  the board default -- not an error). These builds already force debug
  output onto a third, separate UART (`"uart"` mode,
  `cfg.debugUartTxPin`/`debugUartRxPin`) instead of `Serial`, so this
  override only affects where print bytes go, not where debug logs go.
- **`esp32-s3-devkitc-1_ql_usb_host`** (`TARGET_USB_HOST`: the printer talks
  over real USB bulk transfers via the native OTG peripheral, see
  `usb_host_target.cpp` -- `Serial` isn't involved in the printer connection
  at all) -- `ARDUINO_USB_MODE` is left at the board default (`1`), and
  `ARDUINO_USB_CDC_ON_BOOT` stays `0` (inherited from `env_ql_usb_host`,
  still required so Arduino's native-USB-CDC auto-init doesn't fight
  `usb_host_install()` for the OTG peripheral) -- so `Serial` resolves to
  real UART0, same as the `_zpl_usb`/`_ql_usb` case. Unlike those two,
  though, debug output stays on `"usb"` mode here (`DEFAULT_DEBUG_OUTPUT_MODE`
  overridden back from `env_ql_usb_host`'s `"uart"` default) since nothing
  about this board's `usb_host` use claims UART0 or the **"UART"** port --
  see "Brother QL over USB host" below.

Flash over either port on any of these envs (the ROM bootloader's own
download-mode USB handling is independent of any of this); which port to
plug a monitor into for *debug logs* depends on the env, per above.

### Brother QL over USB host

`esp32-s2-saola-1_ql_usb_host` and `esp32-s3-devkitc-1_ql_usb_host`
(`TARGET_USB_HOST`, `usb_host_target.cpp`) connect directly to a Brother QL
printer's own USB port -- the board enumerates it as a genuine USB host,
claims its USB Printer-class interface (bInterfaceClass `0x07`), and issues
real USB bulk transfers, unlike the `usb` method's plain byte-stream cable
relay (which only works if the printer accepts a serial/UART connection).
Ported from a standalone test sketch validated against a real
Brother QL-720NW before being integrated here.

This needs a chip with a USB-OTG peripheral (ESP32-S2/S3) **and** board
circuitry that can actually source VBUS power to the downstream printer in
host mode -- confirmed via schematic for `esp32-s2-saola-1`
(`ESP32-S2-SAOLA-1_V1.1_schematics.pdf`) only. `esp32-s3-devkitc-1`'s native
"USB" port circuitry sourcing VBUS is **not confirmed** -- some compact S2/S3
boards are wired power-in-only and can't (e.g. UM TinyS2, ruled out this way
before `esp32-s2-saola-1` was chosen for the original implementation). If
the printer never enumerates on `esp32-s3-devkitc-1`, try a powered USB-OTG
hub/cable that injects its own VBUS between the board and the printer,
rather than assuming the board's own port can supply it.

`ARDUINO_USB_CDC_ON_BOOT=0` is required on every `usb_host` env regardless of
board -- without it, the Arduino core's default native-USB-CDC auto-init
fights `usb_host_install()` for the same OTG peripheral and bootloops. Where
debug logs go from there is board-specific, see "Debug output vs. the
usb/usb_host methods" above and `platformio.ini`'s
`USB_HOST_SHARES_DEBUG_PORT` comments.

### Brother QL raster protocol

This conversion runs **client-side**, in `frontend/src/zpl-image.ts`
(`imageDataURLToQLRasterBase64`) -- not on the ESP32. The firmware used to
decode an incoming PNG on-device (via the
[PNGdec](https://github.com/bitbank2/PNGdec) library) and rasterize it there;
that path was removed since PNG payload size is entirely at the mercy of how
compressible the image content happens to be (a dithered photo could balloon
to 10x a flat text label of the same physical size), which repeatedly
exhausted the ESP32's limited/fragmented heap. Moving it to the browser makes
the wire payload -- and this firmware's RAM footprint, now that PNGdec is
gone entirely -- bounded and predictable from the label's physical dimensions
alone, same as the "zpl" protocol already was.

The frontend resizes the rendered label to `label.width`/`label.length` (mm,
from this printer's status/capabilities) at the QL family's native 300dpi,
thresholds it to 1-bit, and builds the complete raster command stream
(header + one raster-line command per row + footer) before ever publishing
it. The exact command bytes are taken from the same reference open-source
implementation ([pklaus/brother_ql](https://github.com/pklaus/brother_ql))
the old firmware code cited, ported rather than reconstructed from memory.
What's deliberately **not** replicated, since there's no hardware here to
validate against and no per-model/media database:

- Brother's exact per-media-width offset table -- the image is centered
  instead (`qlRightMarginDots = 0`), or offset a fixed distance from the
  printhead's right edge if you look up and set the real value for your tape
- Reading the printer's status response (`ESC i S`) before printing -- this
  build only ever writes to the target
- Dithering (plain 50% threshold, same as the ZPL path), red/black
  two-color printing, and 600dpi mode
- Raster compression -- rows are always sent uncompressed, which every QL
  model accepts regardless of whether it also supports the optional
  PackBits mode

`cfg.qlPrintheadPx` selects between the two known printhead widths: 720px
(90 bytes/row -- QL-500/550/560/570/580N/650TD/700/710W/720NW/800/810W/
820NWB) and 1296px (162 bytes/row -- QL-1050/1060N/1100/1110NWB/1115NWB).
`cfg.qlInvalidateBytes` should be 200 for most models, 400 for
QL-800/810W/820NWB. All of this is still web-UI-editable on a `PROTOCOL_QL`
build's Config page ("Brother QL raster" box) exactly as before -- only the
consumer changed, from firmware to the frontend (reported to it via the
status/capabilities JSON, see "MQTT contract used by firmware" below).

## Fallback AP mode

If station Wi-Fi is not configured or cannot connect for about 20 seconds,
the bridge opens an AP so you can still configure it.

- SSID: Stikka-<chip suffix>
- Password: stikkaesp32
- AP IP: 192.168.4.1

The AP state is shown on the setup page.

## Finding the device

Once connected to your station Wi-Fi, the bridge advertises itself over
mDNS as `<printerName>.local` (`wifi_manager.cpp`'s `startMdns()`) --
`cfg.printerName` sanitized to a valid DNS label (lowercase, alphanumeric
and hyphens only; invalid characters collapse to a single hyphen). Browse to
`http://<printerName>.local/` instead of hunting for its DHCP-assigned IP in
your router. The responder (re)starts on every fresh station connection, so
changing the printer name via the web UI's Save button (which forces a
Wi-Fi reconnect) picks up the new hostname automatically. Not available
while only the fallback AP is up (see above) -- `192.168.4.1` already serves
that purpose there. mDNS resolution depends on your OS/browser (works out of
the box on macOS/iOS and most Linux with Avahi; Windows needs Bonjour or an
mDNS-aware browser) -- if `.local` doesn't resolve for you, fall back to the
IP shown in the serial log (`[wifi] connected, ip=...`, always logged
regardless of configured log level) or your router's DHCP client list.

## Build and flash with PlatformIO

1. Open this folder in VS Code with PlatformIO plugin:

esp32

2. Select environment:

env:esp32-s3-devkitc-1_zpl_network (the default -- see `[platformio]` in
platformio.ini)

Envs are named `<board>_<protocol>_<method>`. `esp32-s3-devkitc-1` (PSRAM) is
the only supported board, and only two leaf envs are currently defined in
`platformio.ini`:

- env:esp32-s3-devkitc-1_zpl_network (default)
- env:esp32-s3-devkitc-1_ql_usb_host

Add a new `[env:esp32-s3-devkitc-1_<protocol>_<method>]` leaf extending the
matching `env_<protocol>_<method>` base (see the protocol/method matrix
above) to build for a different combination on this board.

Example CLI usage:

pio run -e esp32-s3-devkitc-1_zpl_network -t upload

To build all configured board environments and stage web-flasher artifacts, use the repo dev-shell command:

build-firmware

Output location:

- frontend/public/firmware/index.json
- frontend/public/firmware/<env>/firmware.bin (and bootloader/partitions when available)

3. Build and upload from PlatformIO UI.

4. Open serial monitor at 115200 baud.

## First-time setup

1. Power the board and open its IP in browser (from your router DHCP list),
   or `http://stikka-esp32.local/` (the default printer name, before you've
   changed it -- see "Finding the device" above) once it's joined your Wi-Fi.
2. Fill in:
- Wi-Fi SSID and password
- MQTT broker host/port and credentials
- Printer name
- ZPL target host and port
- "Printer supports compressed graphics (:Z64:/:B64:)" -- leave this off unless you've
  confirmed image labels print correctly with it on. Not every ZPL-compatible printer
  implements this optional encoding; when unsupported it's a silent aborted download
  (nothing prints, no error on the wire). When on, image jobs are sent zlib-compressed
  (`:Z64:`), which keeps the message smaller on the wire.
3. Click Save and reconnect.
4. Click Send test ZPL.

## Logs tab

The web UI has a second page, at `/logs`, showing the device's recent log
lines (an in-memory ring buffer of the last ~120 entries -- it resets on
reboot, it isn't persisted to flash).

- A **log level** dropdown (`ERROR`/`WARN`/`INFO`/`DEBUG`) controls verbosity.
  It's a single knob for both the Logs tab and the serial/UART output
  configured on the main Config page -- lines more verbose than the selected
  level are dropped entirely, not just hidden in the UI. `INFO` (the default)
  shows connection lifecycle and print job events; switch to `DEBUG` for full
  per-byte tracing (noisy -- meant for troubleshooting a specific
  issue, not for leaving on continuously). Changing it takes effect
  immediately, with no Wi-Fi/MQTT reconnect (unlike the main Save button).
  Unlike the log history itself, the chosen level *is* persisted (saved with
  the rest of the config) and survives a reboot. Wi-Fi connect/disconnect
  (including the device's IP address) always shows regardless of the level
  selected -- that's how you find the device to reconfigure it even at the
  quietest setting.
- Each line's timestamp is device uptime (`HH:MM:SS` since boot), not
  wall-clock time -- this firmware has no RTC/NTP.
- The page polls `/logs.json` every ~1.5s and appends new lines; `Clear`
  empties the buffer.
- Because the ring buffer is populated independently of the serial/UART
  toggle's enabled state, the Logs tab works even with serial output turned
  off -- useful once a device is deployed without a serial cable attached.

## MQTT contract used by firmware

Subscribe:

/<printername>/command/

Publish retained status:

/<printername>/status/

Every job is a single MQTT message -- there is no chunking protocol.

Command payload example:

{
  "job_id": "job-123",
  "printer_name": "my-printer",
  "payload_type": "zpl",
  "payload_encoding": "utf8",
  "payload": "^XA^FO40,40^FDHello^FS^XZ"
}

Non-`PROTOCOL_QL` builds accept `payload_type: "zpl"` (`payload_encoding:
"utf8"` or `"base64_utf8"`) and `payload_type: "image"` (`payload_encoding:
"data_url"` or `"base64_png"`).

`PROTOCOL_QL` builds instead accept only `payload_type: "ql_raster"`, with
`payload` holding the base64-encoded, already-rasterized Brother QL byte
stream (`payload_encoding: "base64_bytes"`) -- see "Brother QL raster
protocol" above.

Job status payload example:

{
  "printer_name": "my-printer",
  "job_id": "job-123",
  "status": "accepted|done|failed",
  "message": "..."
}

## Local mock test server (without ESP hardware)

There is a software bridge simulator at:

esp32/tools/mock_bridge_server.py

Run it with uv:

uv sync
uv run python esp32/tools/mock_bridge_server.py --broker-host 127.0.0.1 --broker-port 1883 --printer-name stikka-test

This mock server:

- publishes retained printer status to /stikka-test/status/
- subscribes to /stikka-test/command/
- accepts ZPL and image commands
- starts a local fake TCP printer on 127.0.0.1:9100 and prints received ZPL to console

Point frontend static config to the same broker and printer name to test end-to-end.

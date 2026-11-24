# ESP32 GATTacker

ESP32-S3 firmware for local BLE security research. The firmware runs on its own with
a web UI and an authenticated MCP endpoint. An optional Flipper Zero app can control
the same scan and session state over GPIO UART and save captures to its SD card.
The firmware implementation is C; the bundled web UI is React.

GitHub is the primary source for code, issues, and releases. GitHub Actions builds
the web UI and ESP32-S3 firmware on pull requests, pushes to `main`, and version tags.
Build artifacts are available from each workflow run; version tags also publish the
merged firmware image to [GitHub Releases](https://github.com/p0px/ESP32-Gattacker/releases).

## Hardware

The tested board is an ESP32-S3 with 8 MB octal PSRAM. The current configuration
uses ESP32 GPIO43 for TX and GPIO44 for RX at 115200 baud. On the Flipper, pin 13
is TX and pin 14 is RX. Connect TX to the other device's RX and share ground.
The board also works without a Flipper attached.

## Build and flash

Use ESP-IDF 5.5.2. Apply `bad_blues.patch` from this repository at the ESP-IDF
root before building. The patch changes Bluedroid's GATT server handle layout
and service deletion behavior used by the clone engine.

```sh
cd /path/to/esp-idf
git apply /path/to/esp32-gattacker/bad_blues.patch

cd /path/to/esp32-gattacker/UI
npm ci
npm run build
python ../scripts/package_ui.py

cd ../Firmware
idf.py reconfigure build
idf.py -p PORT flash monitor
```

`idf.py flash` writes the app and web UI partitions and preserves NVS settings.
A merged release image written at offset zero replaces NVS. The tracked
`Firmware/sdkconfig` targets the tested board. For different ESP32-S3 wiring or
PSRAM, run the configuration script from the repository root with your board
values, then rebuild:

```sh
python scripts/configure_board.py Firmware/sdkconfig \
  --tx 43 --rx 44 --baud 115200 --psram octal
```

Supported UART rates are 115200, 230400, and 460800; choose the same rate in
the Flipper app.

## Use

Connect to the ESP32 setup AP and open `http://1.3.3.7`, or use its station IP
after joining a network. The web UI and Flipper app share target selection,
session state, hooks, and capture controls. Browser routes such as `/settings`
load directly on either network.

Packet capture records host/controller HCI traffic in PCAPNG format. This is
not an over-the-air RF sniffer. Web capture uses available PSRAM, up to 4 MiB on
the tested board. A web user can save a completed capture to the Flipper SD
card when the app is open, or download it from the board. Captures in PSRAM
must be downloaded or exported before powering off. Live traffic preview is
sampled for UI responsiveness; PCAP capture is independent of that preview.

The MCP endpoint is `/mcp`. It requires the board access key; inspect the
available tools in `Firmware/docs/mcp-tools.json`. The key can be managed from
the setup AP or approved through the Flipper app.

## Checks

```sh
cd Firmware
python -m unittest discover -s tests
python tests/hook_exchange_test.py
python tests/ws_queue_test.py
```

The PCAP test requires `gcc` and Wireshark's `tshark` and `capinfos` tools.
The MCP test needs ESP-IDF's cJSON headers and a local cJSON library. With a
flashed board and an open Flipper app, run the hardware smoke check from the
repository root:

```sh
uv run --with websockets python scripts/hardware_smoke.py BOARD_IP capture.pcapng
```

## License

MIT. See [LICENSE](LICENSE).

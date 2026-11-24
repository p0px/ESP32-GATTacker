"""Configure ESP32-S3 UART, USB console, and board-specific PSRAM support."""

import argparse
from pathlib import Path


def configure(
  path: Path, tx: int = 43, rx: int = 44, baud: int = 115200, psram: str = "octal"
) -> None:
  """Set board wiring and RAM; raise ValueError before writing unsupported settings.

  Octal PSRAM matches the connected ESP32-S3 with embedded 8 MB RAM. Other boards
  must select their actual RAM type. Bluetooth allocations prefer PSRAM when enabled
  so the UART bridge and Wi-Fi do not exhaust the internal heap during BLE startup.
  """
  if not (0 <= tx <= 48 and 0 <= rx <= 48) or tx == rx:
    raise ValueError("TX and RX must be different ESP32-S3 GPIOs in the range 0..48")
  if baud not in (115200, 230400, 460800):
    raise ValueError("Baud must match one of the Flipper app's supported rates")
  if psram not in ("none", "quad", "octal"):
    raise ValueError("PSRAM must be none, quad, or octal")
  values: dict[str, str | None] = {
    "CONFIG_GATTACKER_UART_TX": str(tx),
    "CONFIG_GATTACKER_UART_RX": str(rx),
    "CONFIG_GATTACKER_UART_BAUD": str(baud),
    "CONFIG_ESP_CONSOLE_UART_DEFAULT": None,
    "CONFIG_ESP_CONSOLE_UART_CUSTOM": None,
    "CONFIG_ESP_CONSOLE_USB_CDC": None,
    "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG": "y",
    "CONFIG_ESP_CONSOLE_SECONDARY_NONE": "y",
    "CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG": None,
    "CONFIG_CONSOLE_UART_DEFAULT": None,
    "CONFIG_CONSOLE_UART_CUSTOM": None,
    "CONFIG_SPIRAM": None if psram == "none" else "y",
    "CONFIG_ESP32S3_SPIRAM_SUPPORT": None if psram == "none" else "y",
    "CONFIG_SPIRAM_MODE_QUAD": "y" if psram == "quad" else None,
    "CONFIG_SPIRAM_MODE_OCT": "y" if psram == "octal" else None,
    "CONFIG_SPIRAM_USE_MALLOC": None if psram == "none" else "y",
    "CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST": None if psram == "none" else "y",
    "CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP": None if psram == "none" else "y",
    "CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM": "10",
    "CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM": "32",
    "CONFIG_ESP32_WIFI_STATIC_RX_BUFFER_NUM": "10",
    "CONFIG_ESP32_WIFI_DYNAMIC_RX_BUFFER_NUM": "32",
  }
  lines = []
  for line in path.read_text(encoding="utf-8").splitlines():
    key = line.removeprefix("# ").split("=", 1)[0].split(" ", 1)[0]
    if key not in values:
      lines.append(line)
  lines.extend(
    f"{key}={value}" if value else f"# {key} is not set" for key, value in values.items()
  )
  path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def main() -> None:
  """Apply board wiring and memory settings to sdkconfig for the next IDF build."""
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("sdkconfig", type=Path)
  parser.add_argument("--tx", type=int, default=43)
  parser.add_argument("--rx", type=int, default=44)
  parser.add_argument("--baud", type=int, default=115200)
  parser.add_argument("--psram", choices=("none", "quad", "octal"), default="octal")
  args = parser.parse_args()
  configure(args.sdkconfig, args.tx, args.rx, args.baud, args.psram)


if __name__ == "__main__":
  main()

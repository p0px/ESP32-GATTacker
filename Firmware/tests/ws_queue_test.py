"""Check production C WebSocket queue ownership and backpressure with ASan/UBSan."""

import logging
import subprocess
import tempfile
from pathlib import Path

LOGGER = logging.getLogger(__name__)


def main() -> None:
  """Exercise saturation, enqueue failure, disconnection, and byte limits in the C transport."""
  tests = Path(__file__).resolve().parent
  source = (tests.parent / "main/wifi/ws.c").read_text(encoding="utf-8")
  transport = source[source.index("struct async_resp_arg {"):
                     source.index("esp_err_t ws_handler(")]
  with tempfile.TemporaryDirectory() as temporary:
    folder = Path(temporary)
    (folder / "ws_queue_production.inc").write_text(transport, encoding="utf-8")
    binary = folder / "ws_queue_test"
    subprocess.run([
      "cc", "-std=c17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
      "-fsanitize=address,undefined", "-I", str(folder),
      str(tests / "ws_queue_test.c"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
  LOGGER.info("C WebSocket queue tests passed")


if __name__ == "__main__":
  logging.basicConfig(level=logging.INFO)
  main()

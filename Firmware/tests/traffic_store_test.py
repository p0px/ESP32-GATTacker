"""Compile the production GATT event ring against deterministic host adapters."""

import logging
import subprocess
import tempfile
from pathlib import Path

LOGGER = logging.getLogger(__name__)


def main() -> None:
  """Check record ordering, ring overwrite, metadata, and value truncation."""
  tests = Path(__file__).resolve().parent
  source = (tests.parent / "main/ble/traffic_store.c").read_text(encoding="utf-8")
  body = source[source.index("static TrafficEvent *events;"):]
  with tempfile.TemporaryDirectory() as temporary:
    folder = Path(temporary)
    (folder / "traffic_store_production.inc").write_text(body, encoding="utf-8")
    binary = folder / "traffic_store_test"
    subprocess.run([
      "cc", "-std=c17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
      "-I", str(folder), "-I", str(tests.parent / "main"),
      str(tests / "traffic_store_test.c"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
  LOGGER.info("C GATT traffic ring tests passed")


if __name__ == "__main__":
  logging.basicConfig(level=logging.INFO)
  main()

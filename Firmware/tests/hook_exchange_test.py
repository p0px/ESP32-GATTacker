"""Compile the production C hook exchange against deterministic RTOS/transport adapters."""

import logging
import subprocess
import tempfile
from pathlib import Path

LOGGER = logging.getLogger(__name__)


def main() -> None:
  """Check reply ownership, deadlines, cancellation, and queue errors with ASan/UBSan."""
  tests = Path(__file__).resolve().parent
  source = (tests.parent / "main/ble/GattAttackApp.c").read_text(encoding="utf-8")
  config = source[source.index("static const uint32_t HOOK_TIMEOUT_MS"):
                  source.index("GattAttackState gattAttackState")]
  run = source[source.index("char *run_hook("):source.index("void gatt_send_output(")]
  reply = source[source.index('  if (strcmp(ps->action, "hookret")'):
                 source.index('  if (strcmp(ps->action, "enable_hooks")')]
  with tempfile.TemporaryDirectory() as temporary:
    folder = Path(temporary)
    (folder / "hook_production.inc").write_text(
      config + run + "\nstatic void receive(Params *ps) {\n" + reply + "\n}\n", encoding="utf-8")
    binary = folder / "hook_test"
    subprocess.run([
      "cc", "-std=c17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
      "-I", str(folder), str(tests / "hook_exchange_test.c"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
  LOGGER.info("C hook exchange tests passed")


if __name__ == "__main__":
  logging.basicConfig(level=logging.INFO)
  main()

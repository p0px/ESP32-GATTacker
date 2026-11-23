"""Check the C MCP dispatcher against its catalog with a native compiler."""

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
JSON_HEADERS = Path(os.environ.get("IDF_PATH", "/nonexistent")) / "components/json/cJSON"
if not (JSON_HEADERS / "cJSON.h").exists():
  JSON_HEADERS = Path("/usr/include/cjson")


@unittest.skipUnless(shutil.which("cc") and (JSON_HEADERS / "cJSON.h").exists(),
                     "requires a C compiler and libcjson-dev")
class McpProtocolTest(unittest.TestCase):
  """Exercise the protocol independently of ESP32 transport and hardware."""

  def test_dispatcher(self) -> None:
    """Compile the real dispatcher and exercise its tool schemas and error paths."""
    with tempfile.TemporaryDirectory() as temporary:
      folder = Path(temporary)
      catalog = json.loads((ROOT / "docs/mcp-tools.json").read_text())
      literal = json.dumps(json.dumps(catalog))
      generated = folder / "catalog.c"
      generated.write_text(
        '#include "agent_api.h"\n'
        f"cJSON *agent_catalog(void) {{ return cJSON_Parse({literal}); }}\n"
      )
      component = ROOT / "components/mcp_protocol"
      binary = folder / "mcp-test"
      command = [
        "cc", "-std=c17", "-Wall", "-Wextra", "-Werror",
        "-I", str(component), "-I", str(JSON_HEADERS),
        str(ROOT / "tests/mcp_protocol_test.c"), str(component / "mcp_protocol.c"),
        str(generated), "-l:libcjson.so.1", "-lm", "-o", str(binary),
      ]
      subprocess.run(command, check=True)
      subprocess.run([str(binary)], check=True)

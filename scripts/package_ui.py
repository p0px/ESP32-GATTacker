"""Package a built Vite UI as gzip assets for the ESP32 LittleFS partition."""

import gzip
from pathlib import Path


def main() -> None:
  """Replace packaged assets with compressed files from the current UI build."""
  repository = Path(__file__).resolve().parents[1]
  source = repository / "UI" / "dist"
  target = repository / "Firmware" / "data"
  if not (source / "index.html").is_file() or not target.is_dir():
    raise SystemExit("Build UI first and run this script inside the firmware repository")
  for stale in target.rglob("*.gz"):
    stale.unlink()
  for file in source.rglob("*"):
    if not file.is_file() or file.suffix == ".gz":
      continue
    output = target / file.relative_to(source).with_name(file.name + ".gz")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as destination:
      with gzip.GzipFile(file.name, "wb", fileobj=destination, mtime=0) as packed:
        packed.write(file.read_bytes())


if __name__ == "__main__":
  main()

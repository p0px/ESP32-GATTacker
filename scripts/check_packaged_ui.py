"""Verify that the checked-in ESP32 UI contains the current Vite build."""

import gzip
from pathlib import Path


def main() -> None:
  """Compare each built file with its packaged gzip asset and reject stale files."""
  repository = Path(__file__).resolve().parents[1]
  source = repository / "UI" / "dist"
  target = repository / "Firmware" / "data"
  if not (source / "index.html").is_file() or not target.is_dir():
    raise SystemExit("Build UI first and run this script inside the firmware repository")

  expected = {
    file.relative_to(source).with_name(file.name + ".gz"): file
    for file in source.rglob("*")
    if file.is_file() and file.suffix != ".gz"
  }
  actual = {
    file.relative_to(target)
    for file in target.rglob("*.gz")
  }
  failures = [f"Missing: {path}" for path in sorted(expected.keys() - actual)]
  failures.extend(f"Unexpected: {path}" for path in sorted(actual - expected.keys()))

  for path, file in sorted(expected.items()):
    if path not in actual:
      continue
    try:
      packaged = gzip.decompress((target / path).read_bytes())
    except (OSError, EOFError) as error:
      failures.append(f"Unreadable: {path}: {error}")
      continue
    if packaged != file.read_bytes():
      failures.append(f"Stale: {path}")

  if failures:
    raise SystemExit("Packaged UI differs from UI/dist:\n" + "\n".join(failures))
  print(f"Verified {len(expected)} packaged UI assets")


if __name__ == "__main__":
  main()

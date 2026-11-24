"""Exercise the flashed ESP32's web, BLE scan, and PCAP-to-Flipper path."""

import argparse
import asyncio
import json
import time
from pathlib import Path
from urllib.request import Request, urlopen

from websockets.asyncio.client import connect


def request(host: str, path: str, payload: dict | None = None) -> dict:
  """Read a JSON endpoint or post a small JSON control body."""
  body = json.dumps(payload).encode() if payload is not None else None
  headers = {"Content-Type": "application/json"} if body else {}
  with urlopen(Request(f"http://{host}{path}", data=body, headers=headers), timeout=8) as reply:
    return json.load(reply)


async def scan(host: str) -> list[dict]:
  """Start a BLE scan through the same WebSocket path used by the UI."""
  async with connect(f"ws://{host}/ws", open_timeout=8) as socket:
    await socket.send(json.dumps({"opt": 2, "action": "scan"}))
    deadline = time.monotonic() + 25
    next_status = 0.0
    requested_results = False
    saw_scanning = False
    while time.monotonic() < deadline:
      now = time.monotonic()
      if now >= next_status:
        await socket.send(json.dumps({"opt": 90, "command": "status"}))
        next_status = now + 2
      try:
        event = json.loads(await asyncio.wait_for(socket.recv(), timeout=1))
      except TimeoutError:
        continue
      if event.get("state") == 1:
        saw_scanning = True
      if event.get("scan_done") or (saw_scanning and event.get("state") == 2):
        await socket.send(json.dumps({"opt": 2, "action": "results"}))
        requested_results = True
      if "results" in event:
        return event["results"]
    raise RuntimeError("BLE scan did not complete" if not requested_results else
                       "BLE results did not arrive")


async def main() -> None:
  """Record the scan in PSRAM, export it to the Flipper, and save a local PCAP."""
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("host")
  parser.add_argument("output", type=Path)
  parser.add_argument("--destination", choices=("web", "flipper"), default="flipper")
  args = parser.parse_args()
  state = request(args.host, "/api/capture")
  if state["state"] != "empty" or (
    args.destination == "flipper" and not state["sd"]["available"]
  ):
    raise RuntimeError(f"Expected empty capture and selected sink available: {state}")
  started = request(args.host, "/api/capture/start", {
    "seconds": int(time.time()), "destination": args.destination,
  })
  capture_id = started["id"]
  try:
    devices = await scan(args.host)
    print(f"Scan returned {len(devices)} devices")
    for device in devices[:8]:
      print(f"  {device.get('mac')} {device.get('name')} RSSI {device.get('rssi')}")
  finally:
    request(args.host, "/api/capture/stop", {"id": capture_id})
  deadline = time.monotonic() + 45
  while time.monotonic() < deadline:
    state = request(args.host, "/api/capture")
    export_finished = args.destination == "web" or state["export"]["state"] in (
      "complete", "failed"
    )
    if state["state"] == "ready" and export_finished:
      break
    await asyncio.sleep(0.5)
  if state["state"] != "ready" or (
    args.destination == "flipper" and state["export"]["state"] != "complete"
  ):
    raise RuntimeError(f"Capture or Flipper export failed: {state}")
  with urlopen(f"http://{args.host}/api/capture/download?id={capture_id}", timeout=20) as reply:
    payload = reply.read()
  if payload[:4] != b"\x0a\x0d\x0d\x0a" or len(payload) != state["bytes"]:
    raise RuntimeError("Downloaded PCAPNG header or byte count is invalid")
  args.output.write_bytes(payload)
  print(f"Capture {capture_id}: {state['packets']} packets, {state['dropped']} dropped")
  if args.destination == "flipper":
    print(f"Flipper SD: {state['sd']['path']} ({state['sd']['bytes']} bytes)")
  print(f"Local PCAP: {args.output} ({len(payload)} bytes)")


if __name__ == "__main__":
  asyncio.run(main())

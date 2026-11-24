# Contributing

GitHub is the primary repository for ESP32 GATTacker. Please report bugs,
request features, and open pull requests at
[p0px/ESP32-GATTacker](https://github.com/p0px/ESP32-GATTacker).

For bug reports, include the ESP-IDF version, board and wiring, steps to
reproduce, and relevant serial logs. Remove access keys and Wi-Fi credentials
before sharing logs or captures.

For code changes:

1. Fork the GitHub repository and create a branch.
2. Build the UI with `npm ci` and `npm run build` from `UI/`.
3. Run `python3 scripts/package_ui.py` from the repository root and commit any
   changes under `Firmware/data/`.
4. Build the firmware with ESP-IDF 5.5.2 after applying `bad_blues.patch` to
   ESP-IDF, as described in the README.
5. Open a GitHub pull request with the affected paths and manual test steps.

GitHub Actions checks the packaged UI and firmware build on each pull request.

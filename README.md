# pico-read

`pico-read` is an ESP32-C6 speed-reading device built with ESP-IDF and LVGL. The firmware reads `.txt` books from the ESP filesystem at runtime and renders them with a fixed anchor-letter reading UI.

## Warning

This project is currently supported on `macOS` and `Linux` only.

`Windows` support is planned, but it is not documented or tested yet.

## Repository Layout

- `esp32-c6-gui/`: ESP-IDF firmware project
- `books/`: source `.txt` books packaged into the device filesystem image

## Dependency Model

This repo does not commit large vendored ESP-IDF components.

The firmware declares its dependencies in `esp32-c6-gui/main/idf_component.yml`, and ESP-IDF downloads them automatically on the first build. That keeps the repository small while still allowing a fresh clone to build directly.

## Homebrew Dependencies

Install the required host tools with Homebrew:

```bash
brew install git cmake ninja ccache dfu-util python@3.11 pkg-config wget
```

## ESP-IDF Setup

Clone and install ESP-IDF in a separate directory:

```bash
mkdir -p ~/esp
cd ~/esp
git clone -b v5.2 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c6
```

Load the ESP-IDF environment in each shell before building or flashing:

```bash
source ~/esp/esp-idf/export.sh
```

## Build

Use this exact shell flow if you have been switching between Python environments:

```bash
deactivate 2>/dev/null || true
cd /Users/dream/Documents/GitHub/pico-read/esp32-c6-gui
source /Users/dream/esp/esp-idf/export.sh
which python
which idf.py
idf.py --version
idf.py build
```

The expected result is that `which python` points into the Espressif environment under `~/.espressif/...`, not the repo-local `.venv`.

Minimal build-only command:

```bash
cd esp32-c6-gui
idf.py build
```

On the first build, ESP-IDF will fetch managed dependencies such as `lvgl/lvgl` and `espressif/led_strip`.

## Flash

Do not use `/dev/cu.usbmodem1101` blindly. That is only an example.

Find the real port first:

```bash
ls /dev/cu.*
```

Then unplug the board, run it again, plug the board back in, and run it again. The new device is the board you should flash. On macOS it is usually something like:

- `/dev/cu.usbmodem...`
- `/dev/cu.SLAB_USBtoUART`

Full build + flash workflow:

```bash
deactivate 2>/dev/null || true
cd /Users/dream/Documents/GitHub/pico-read/esp32-c6-gui
source /Users/dream/esp/esp-idf/export.sh
ls /dev/cu.*
which python
which idf.py
idf.py --version
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
```

Replace `/dev/cu.usbmodem1101` with the real port you found from `ls /dev/cu.*`.

Flash-only command with the actual port:

```bash
cd esp32-c6-gui
idf.py -p /dev/cu.usbmodemACTUALPORT flash
```

If you want serial logs immediately after flashing:

```bash
cd esp32-c6-gui
idf.py -p /dev/cu.usbmodemACTUALPORT flash monitor
```

If flashing fails with:

- `Could not open /dev/cu....`
- `the port is busy or doesn't exist`

check these first:

1. The port name is real and matches the current plugged-in board.
2. No other serial monitor, terminal, or IDE is already using that port.
3. The board is still connected after the last reset.

If the board does not enter download mode automatically:

1. Hold `BOOT`.
2. Tap `RESET`.
3. Release `BOOT`.
4. Run the flash command again.

If you want a reusable upload script, this is the exact version to run from the repo root after replacing the port:

```bash
#!/usr/bin/env bash
set -euo pipefail

deactivate 2>/dev/null || true
cd /Users/dream/Documents/GitHub/pico-read/esp32-c6-gui
source /Users/dream/esp/esp-idf/export.sh

which python
which idf.py
idf.py --version
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
```

If you save that as `flash.sh`, make it executable with:

```bash
chmod +x flash.sh
```

## Updating Books

Add or edit `.txt` files inside `books/`, then rebuild and reflash:

```bash
cd esp32-c6-gui
idf.py build flash -p /dev/cu.usbmodemACTUALPORT
```

The firmware packages the `books/` folder into a SPIFFS image during build and streams the selected book from the device filesystem at runtime.

## Notes

- Do not activate the project-local `.venv` when running `idf.py`.
- Use the ESP-IDF environment from `source ~/esp/esp-idf/export.sh`.
- The active reader UI code lives in `esp32-c6-gui/main/LVGL_UI/display_words.c`.
- The runtime text filesystem backend lives in `esp32-c6-gui/main/LVGL_UI/display.c`.

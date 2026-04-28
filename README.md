# pico-read

`pico-read` is an ESP32-C6 speed-reading device built with ESP-IDF and LVGL. The current firmware reads `paragraph.txt` from the ESP filesystem at runtime and renders the text with a fixed anchor-letter reading UI.

## Warning

This project is currently supported on `macOS` and `Linux` only.

`Windows` support is planned, but it is not documented or tested yet.

## Repository Layout

- `esp32-c6-gui/`: ESP-IDF firmware project
- `paragraph.txt`: source text file packaged into the device filesystem image

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

```bash
cd esp32-c6-gui
idf.py build
```

On the first build, ESP-IDF will fetch managed dependencies such as `lvgl/lvgl` and `espressif/led_strip`.

## Flash

Replace the serial port with your board's actual device path:

```bash
cd esp32-c6-gui
idf.py -p /dev/cu.usbmodem1101 flash
```

## Updating the Book Text

Edit the root-level `paragraph.txt`, then rebuild and reflash:

```bash
cd esp32-c6-gui
idf.py build flash -p /dev/cu.usbmodem1101
```

The firmware packages `paragraph.txt` into a SPIFFS image during build and streams the text from the device filesystem at runtime.

## Notes

- Do not activate the project-local `.venv` when running `idf.py`.
- Use the ESP-IDF environment from `source ~/esp/esp-idf/export.sh`.
- The active reader UI code lives in `esp32-c6-gui/main/LVGL_UI/display_words.c`.
- The runtime text filesystem backend lives in `esp32-c6-gui/main/LVGL_UI/display.c`.

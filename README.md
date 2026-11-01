# Mosaico USB Webcam

English | [简体中文](README.zh-CN.md)

Turn an ESP-Mosaico (ESP32-S31) into a USB webcam with a touchscreen preview and Chinese / English controls. Requires an SC101IOT camera module.

| Home | Settings |
| --- | --- |
| <img src="docs/images/home-en.png" alt="English home screen with privacy enabled" width="240"> | <img src="docs/images/settings-en.png" alt="English settings screen" width="240"> |

## Features

- UVC MJPEG video at **640×480, targeting 25 FPS**.
- Privacy enabled at every startup; tap **Camera On** to begin capture.
- Local preview and touch controls rotate with the device.
- Independent preview mirroring and USB video rotation: **0° / 90° / 180° / 270°**.
- Brightness, preview, screen timeout, and language settings saved across restarts.

## Installation

Download the firmware from [Releases](https://github.com/eMUQI/esp-mosaico-webcam/releases/latest). Back up any data you need to keep, then choose one method:

- **Iris:** Download the `.irisfw` package. Hold **AI** while restarting to enter Vibe Mode, then install through the official Iris installer or Ideas **Add Iris firmware** option. Requires Vibe Mode **0.2.0 or later**.
- **Wired flashing:** Download `factory-merged.bin`. Hold **BOOT** and reset to enter download mode, then write the image at **`0x0`** through the board's download interface. Release BOOT and restart afterward. This replaces the existing firmware and settings. See the [flashing commands](docs/DEVELOPMENT.md#bin-与烧录).

## Usage

1. Insert the SC101IOT camera module into the **left H2 slot**. Connect the computer to the **High-Speed OTG USB-C** port with a USB data cable.
2. Tap **Camera On** on the device. A black image before this is normal: privacy is enabled at startup.
3. Select **Mosaico Webcam / UVC CAM1** in your computer's camera app:

| Computer | Open the camera |
| --- | --- |
| Windows | Open **Camera** and use **Change camera** to select the device. |
| macOS | Open **QuickTime Player → File → New Movie Recording**, then choose the camera from the menu beside the record button. |

Allow camera access if prompted. Tap **Camera Off** to return to a black image.

Open **Settings** to adjust brightness, preview, mirroring, USB video rotation, and screen timeout. Tap **中文 / EN** to switch languages. Preview mirroring affects only the device screen; USB video rotation changes the computer's image.

Tap **Screen Off / Pause preview** to turn off the display while USB video continues. Touch the screen once to wake it.

## Development

See the [development guide](docs/DEVELOPMENT.md) for building and debugging, or the [architecture](docs/ARCHITECTURE.md), [dependencies](docs/DEPENDENCIES.md), and [testing guide](TESTING.md) for technical details. These guides are in Chinese.

## Acknowledgments and licensing

Built with the official [ESP-Mosaico BSP](https://github.com/esp-mosaico/esp-mosaico-bsp), Espressif USB Device UVC, TinyUSB, and LVGL. The camera icon comes from ESP-Mosaico Claw; Chinese fonts use Noto Sans CJK SC.

Project code is licensed under [Apache-2.0](LICENSE). Third-party components and artwork retain their own licenses; see the [dependency notes](docs/DEPENDENCIES.md) and [asset sources and licenses](main/assets/README.md).

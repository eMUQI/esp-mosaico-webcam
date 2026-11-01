# Interface artwork and fonts

This document records the sources, conversion steps, and licenses for the firmware's interface assets. Paths beginning with `main/` or `tools/` are relative to the repository root.

## Camera icon

Source: [ESP-Mosaico Claw camera icon](https://github.com/esp-mosaico/esp-mosaico-claw/blob/2e1017e42d507cd9216d4da5b2c7d45cb2007971/components/mosaic_ui/common/assets/icons/camera.png).

- Pinned commit: `2e1017e42d507cd9216d4da5b2c7d45cb2007971`.
- Git blob SHA: `d2fdab9cb6b46d8dc5700dc424b0fe4e3e7a142f`.

`camera.png` is copied without modification from the Apache-2.0 repository. Its license is included in [LICENSE-Apache-2.0.txt](LICENSE-Apache-2.0.txt).

The icon is displayed at its original 118×118 size, without cropping, scaling, recoloring, or additional artwork. `python3 tools/generate_ui_assets.py` uses FFmpeg to encode it as RGB565 on the screen's black background (27,848 bytes). This changes the storage format and quantizes colors to the display format; it does not redesign the artwork. Normal builds use `main/ui_assets.c` and do not require FFmpeg or a runtime PNG decoder.

## Camera connection guide

`camera-guide-device.png` (240×176) depicts the camera and main board, based on a project-provided device photograph. `camera-guide-arrow.png` (82×36) is a generated transparent arrow. LVGL animates the arrow to indicate the left H2 connection; the device illustration remains static.

`python3 tools/generate_ui_assets.py` encodes the device image as RGB565 (84,480 bytes) and the arrow as ARGB8888 (11,808 bytes). The generated arrays are read directly from firmware; runtime PNG, GIF, and video decoders are not required. The original Apache-2.0 camera icon remains in use for other states.

## Chinese interface fonts

`main/ui_font_14.c` and `main/ui_font_20.c` contain only the Chinese characters, punctuation, and degree symbol used by `main/preview_ui.c`, at 14 and 20 px with 4-bit antialiasing. Latin text and standard LVGL symbols fall back to the existing Montserrat fonts. Normal firmware builds use the generated C files.

- Source: [Noto Sans CJK SC Regular](https://github.com/notofonts/noto-cjk/blob/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf).
- Source SHA-256: `2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b`.
- Copyright 2014-2021 Adobe ([adobe.com](http://www.adobe.com/)).
- License: SIL Open Font License 1.1, included in [LICENSE-NotoSansCJK.txt](LICENSE-NotoSansCJK.txt).

To regenerate after changing Chinese interface text, obtain that source font, install `lv_font_conv@1.5.3`, and run:

```sh
python3 tools/generate_ui_fonts.py --font /path/to/NotoSansCJKsc-Regular.otf
```

The generator validates the source hash before producing the glyph subsets.

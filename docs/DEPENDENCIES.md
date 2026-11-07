# 依赖与本地修改

[English README](../README.md) | [中文 README](../README.zh-CN.md)

本说明记录项目固定的依赖版本、补丁用途和第三方许可。构建和清理命令见[开发指南](DEVELOPMENT.md)。

## 固定版本

| 依赖 | 版本或提交 |
| --- | --- |
| ESP-IDF | `383dda6c182e3dabf6355160166fe007df775629`，6.2 开发版 |
| ESP-Mosaico BSP | `7cbfb390638dcb93522fc3cde094b2d43cf907c2`，Git 子模块 |
| ESP-Mosaico Utils | `c6b43a6be7fb8aba832fc4016d4076702f5414ae`，Git 子模块，与 FluidBox 使用相同版本 |
| USB Device UVC | 1.3.1，项目内副本 |
| TinyUSB | 0.19.0~3 |
| esp_lv_present | 0.1.1 |
| esp_display_present | 1.2.0 |
| LVGL | 9.5.0 |

SDK 提交记录在 `esp-idf.commit`，托管组件的版本和校验值由 `dependencies.lock` 固定。板级支持包（BSP）要求 ESP-IDF 6.2 或更高版本；本项目的构建脚本进一步核对实际 SDK 提交，不接受其他提交。

## ESP-Mosaico BSP

来源：[官方 ESP-Mosaico BSP 仓库](https://github.com/esp-mosaico/esp-mosaico-bsp)。

BSP 在配置阶段自动应用官方摄像头补丁：

- `esp-idf-dvp-frame-capture-stability.patch`：修改 SDK 内的 DVP 采集实现。
- `esp-cam-sensor-sc101iot-pclk-delay.patch`：修改托管 SC101IOT 传感器组件的 PCLK 配置。

建议使用专用 SDK，避免这些修改影响其他工程。`tools/clean.sh` 处理托管传感器补丁与组件管理器文件校验的兼容问题；打包时将 SDK 差异导出为 `esp-idf-local.patch`。

本项目的[显示补丁](patches/esp-mosaico-bsp-display.patch)将 QSPI 单次传输容量扩展至完整的 480×480 RGB565 帧，并启用直接 PSRAM DMA。`tools/build.sh` 检查并应用补丁，重复构建时跳过已应用内容。[迁移补丁](patches/esp-mosaico-bsp-display-migrate-v1.patch)用于已应用旧显示补丁的源码目录。补丁冲突时构建停止，打包导出的 `esp-mosaico-bsp-local.patch` 记录实际 BSP 差异。

显示由 Espressif 的 `esp_lv_present` 和 `esp_display_present` 管理，BSP 的 LVGL 适配器保持关闭，避免重复注册。LVGL 使用单个 32 行绘制缓冲区和两个完整合成缓冲区，通过防撕裂信号（TE）调度显示传输；旋转时重新绑定绘制缓冲区。

## USB Device UVC 与 TinyUSB

`vendor/usb_device_uvc` 来自 Espressif 组件注册表中的 USB Device UVC **1.3.1**，上游提交为 `6a11ddd575777cf5b7722fcd62fd39e18a3372fa`，原路径为 `components/usb/usb_device_uvc`。副本保留上游源码、构建配置、许可证、README 和 CHANGELOG。

本地修改包括：

- 仅声明 MJPEG 640×480、25 FPS，并校验主机 Commit 参数中的接口、格式、帧索引和帧间隔。
- 拒绝负的摄像头索引。
- 初始化失败时返回 PHY、事件组或任务创建错误，并释放已创建的资源；拒绝重复初始化。
- 在同一 TinyUSB 任务中处理帧提交、完成事件、重新协商及超时恢复，避免覆盖借出的发送缓冲区。
- 对超过 1000 ms 的在途传输执行取消，并按帧间隔重新提交。

`vendor/usb_device_uvc/tusb/video-device-recovery.patch` 修复 TinyUSB 视频类的首包失败、续包失败、传输错误、取消及迟到完成事件处理。配置阶段复制视频类源码到构建目录并应用补丁，托管组件文件保持不变。补丁应用失败时中止构建。打包时保留该补丁，并在 `manifest.json` 中记录实际编译源码的哈希值。

`vendor/usb_device_uvc/tusb/dwc2-suspend-status.patch` 修复 USB 恢复后残留的挂起中断导致固件停止提交视频帧的问题。DWC2 驱动清除挂起中断标志后，仅在 `DSTS.SUSPSTS` 仍置位时才上报挂起事件。构建配置只编译应用补丁后的驱动副本，补丁应用失败时中止构建。

修改 USB 模式时，应同步更新描述符、协商校验、应用参数与 `tools/check_firmware.py`。

## 参考与许可

Iris 打包工具、恢复引导程序和 Vibe Mode 0.2.0 预编译镜像来自固定版本的[官方 ESP-Mosaico Utils](https://github.com/esp-mosaico/esp-mosaico-utils/tree/c6b43a6be7fb8aba832fc4016d4076702f5414ae)。应用仅使用 ESP-IDF 的安装确认接口，不链接 ESP-Iris 运行时。ESP-Iris 使用 Apache-2.0；其余工具与恢复子树的许可状态以子模块中的 `LICENSES.md` 和各文件声明为准，不能推定全部适用本项目的许可证。

官方 BSP 的 `camera_lcd_preview`、`wifi_http_cam`、`camera_photo_app` 和 `usb_extend_screen` 示例分别提供摄像头与 PPA、硬件 JPEG、触摸与断电控制（PWDN）、高速 USB OTG 与 UART 的参考实现。

项目代码采用 [Apache-2.0](../LICENSE)，第三方组件遵循各自许可证。摄像头图标源自 ESP-Mosaico Claw；字体源自 Noto Sans CJK SC，采用 SIL OFL 1.1。素材的固定来源、校验值与许可证保留在 `main/assets/`，构建包中对应 `artwork-source/`。

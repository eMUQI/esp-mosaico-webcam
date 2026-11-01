# Mosaico USB Webcam

[English](README.md) | 简体中文

面向 ESP-Mosaico（ESP32-S31）的 USB 摄像头固件，使用 USB 视频类（UVC）协议向电脑传输视频，提供本地预览、隐私控制和中英文触摸设置。

使用 SC101IOT 摄像头子板，安装在主板左侧 H2 插槽。视频连接使用 High-Speed OTG USB-C 接口。

| 首页 | 设置 |
| --- | --- |
| <img src="docs/images/home-zh.png" alt="开启隐私保护的中文首页" width="240"> | <img src="docs/images/settings-zh.png" alt="中文设置界面" width="240"> |

界面示意图由固件的 LVGL 源码在电脑端渲染生成。

## 功能

- 单一 USB 视频模式：**MJPEG 640×480，目标 25 FPS**。
- 每次启动默认开启隐私保护，点击**开启摄像头**后开始采集。
- 本地预览、界面与触摸坐标随设备方向旋转；等比显示完整画面，比例不符时留边。
- 预览镜像与 USB 视频方向独立设置，USB 支持 **0° / 90° / 180° / 270°**。
- 支持亮度、本地预览、自动关屏和显示语言设置，重启后保留。
- 提供摄像头连接引导，采集故障后自动重试。

## 使用

1. 将摄像头子板安装在**左侧 H2 插槽**，用 USB 数据线连接 **High-Speed OTG USB-C** 接口。
2. 在设备上点击「**开启摄像头**」。每次启动默认开启隐私保护，开启摄像头前，电脑显示黑画面属于正常行为。
3. 使用下方的系统自带应用测试，摄像头名称为 **Mosaico Webcam** 或 **UVC CAM1**。

| 电脑系统 | 最简单的测试方法 |
| --- | --- |
| Windows | 在开始菜单搜索并打开「**相机**」。如果显示的是其他摄像头，点击右上角的「**切换摄像头**」，直到出现 Mosaico 的画面。参见 [Microsoft 使用说明](https://support.microsoft.com/en-us/windows/hardware/camera/how-to-use-the-windows-camera-app)。 |
| macOS | 打开 **QuickTime Player**，选择「**文件 → 新建影片录制**」，点击录制按钮旁的下拉箭头，在摄像头列表中选择 **Mosaico Webcam / UVC CAM1**。参见 [Apple 使用说明](https://support.apple.com/guide/quicktime-player/record-a-movie-qtp356b55534/mac)。 |

如提示摄像头权限，选择允许。在镜头前挥手，确认电脑画面实时变化；再点击设备上的「**关闭摄像头**」，画面应在短暂延迟后变黑。无需实际录制视频。

在「**设置**」中调整亮度、预览、镜像、USB 视频旋转和自动关屏时间。预览镜像只影响设备屏幕，USB 视频旋转用于调整电脑画面方向。点击「**中文 / EN**」切换语言。

点击「**关闭屏幕 / 暂停预览**」后，USB 视频继续传输；触摸屏幕一次即可唤醒。详细检查见[测试指南](TESTING.md)。

## 烧录

通过 Iris 安装时，使用 `dist/mosaico-webcam-0.1.0-dev.irisfw`，在官方 Iris 安装器或 Ideas「添加 Iris 固件」中选择该文件。设备需先进入 **0.2.0 或更新版本的 Vibe Mode**；使用官方恢复引导程序的设备可按住 **AI** 键重启进入。更换分区布局前应备份应用数据。更新包仅更新分区表和应用，保留 Vibe Mode 与系统数据。本次接入仅支持固件安装，运行时 USB 继续提供 UVC 视频，不提供 Iris 预览与运行控制。

按下方开发步骤构建，生成 `dist/factory-merged.bin`。这是烧录到 **`0x0`** 的完整镜像，会覆盖镜像地址范围内的原固件、分区布局和非易失性存储（NVS）设置。烧录前备份需要保留的数据。

1. 按住 **BOOT**，复位或重新上电，进入 ROM 下载模式。
2. 在支持 ESP32-S31 的已激活 ESP-IDF 环境中，将 `PORT` 替换为确认后的下载串口，执行：

   ```sh
   python -m esptool --chip esp32s31 --port PORT --no-stub \
     write-flash 0x0 dist/factory-merged.bin
   ```

3. 烧录完成后松开 BOOT，重新启动设备。

烧录使用主板下载接口，运行时的 UVC 接口不提供串口功能。通过波特率为 **115200** 的 UART 查看日志。`dist/app.bin` 仅用于分区兼容时的应用更新，不能写入 `0x0`。产物、偏移和恢复方法见[开发指南](docs/DEVELOPMENT.md#bin-与烧录)。

## 开发

使用 [esp-idf.commit](esp-idf.commit) 固定的 **ESP-IDF 6.2 开发版提交**。板级支持包（BSP）要求 ESP-IDF 6.2 或更高版本，本项目的构建脚本进一步限定了 SDK 提交。摄像头补丁会修改 SDK 文件，建议为本项目单独安装 SDK。

安装 Espressif Installation Manager（EIM）后，在仓库根目录执行：

```sh
eim install -i 383dda6c182e3dabf6355160166fe007df775629 -t esp32s31 \
  --version-name webcam-idf-master -n true --do-not-track true
git submodule update --init
eim run 'sh tools/build.sh' webcam-idf-master
```

已激活相同 SDK 环境时，可直接运行 `sh tools/build.sh`。脚本检查 SDK 提交、应用 BSP 显示补丁、构建固件、核对实际链接的 USB 描述符，再将镜像与校验值打包至 `dist/`。

打包使用固定版本的官方 ESP-Mosaico Utils 子模块，需要 Python `cryptography`，缺失时在 SDK 环境中执行 `python -m pip install cryptography`。产物包括 `.irisfw` 和包含 Vibe Mode 的完整有线 BIN。已有源码目录需在 `idf.py menuconfig` 中选择 QIO Flash 模式和由应用确认回滚状态，或从默认配置重新生成 `sdkconfig`；不兼容配置会被打包脚本拒绝。应用起始地址现为 `0x210000`，不能沿用旧版 `0x10000` 的应用更新方式。

```text
H2 摄像头 → PPA 旋转 / 缩放 → 硬件 JPEG → USB UVC
                       └→ 本地预览 → LVGL / TE 同步显示
```

构建和诊断命令见[开发指南](docs/DEVELOPMENT.md)。其他技术资料包括[视频处理与缓冲区管理](docs/ARCHITECTURE.md)、[依赖与本地修改](docs/DEPENDENCIES.md)及[测试指南](TESTING.md)。

## 致谢与许可

本项目使用官方 [ESP-Mosaico BSP](https://github.com/esp-mosaico/esp-mosaico-bsp)、Espressif USB Device UVC、TinyUSB、LVGL 及 Espressif 显示提交组件。摄像头图标来自 ESP-Mosaico Claw，中文字体由 Noto Sans CJK SC 生成。

项目代码采用 [Apache-2.0 许可](LICENSE)。第三方组件和素材遵循各自许可证，其中 Noto Sans CJK 使用 SIL OFL 1.1。依赖来源见[依赖说明](docs/DEPENDENCIES.md)，素材来源和许可文件位于 `main/assets/`，构建包内对应 `artwork-source/`。

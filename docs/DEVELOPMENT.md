# 开发指南

[English README](../README.md) | [中文 README](../README.zh-CN.md)

本指南面向开发者，介绍固件构建、镜像烧录和运行诊断。所有命令均从仓库根目录运行。

构建需要 Git、Espressif Installation Manager（EIM）及 [esp-idf.commit](../esp-idf.commit) 固定的 ESP-IDF 6.2 开发版，目标芯片为 ESP32-S31。

## 构建与清理

```sh
eim install -i 383dda6c182e3dabf6355160166fe007df775629 -t esp32s31 \
  --version-name webcam-idf-master -n true --do-not-track true
git submodule update --init
eim run 'sh tools/build.sh' webcam-idf-master
```

构建脚本会清空并重新生成 `dist/`；需要保留的镜像应在构建前另行备份。摄像头补丁会修改 SDK 文件，建议为本项目单独安装 SDK。版本与补丁说明见[依赖说明](DEPENDENCIES.md)。

已激活匹配 SDK 环境时，可直接运行 `sh tools/build.sh`。脚本核对 SDK 提交，应用板级支持包（BSP）显示补丁，构建固件，检查 ELF 中的 USB 描述符，最后打包镜像。

打包使用固定版本的官方 ESP-Mosaico Utils 子模块，需要 Python `cryptography`；缺失时在 SDK 环境中执行 `python -m pip install cryptography`。产物包括 `.irisfw` 安装包和包含 Vibe Mode 的完整有线 BIN。

已有源码目录需在 `idf.py menuconfig` 中选择 QIO Flash 模式、启用应用回滚并由应用确认安装状态，或从默认配置重新生成 `sdkconfig`。打包脚本会拒绝不兼容的配置。

干净重建：

```sh
eim run 'sh tools/clean.sh && sh tools/build.sh' webcam-idf-master
```

`clean.sh` 在 `fullclean` 前撤回托管传感器文件的已知补丁，配置阶段重新应用。需要恢复项目默认配置时，可在清理后删除生成的 `sdkconfig` 再构建。

在 `idf.py menuconfig` 的「Mosaico Webcam」菜单中，启用 `WEBCAM_STATIC_FRAME` 可进入静态黑帧诊断模式。设备保持摄像头关闭，向电脑发送黑色 JPEG 帧，用于单独检查 USB 视频传输。常规构建默认关闭此选项。

## BIN 与烧录

| 产物 | 用途 |
| --- | --- |
| `dist/factory-merged.bin` | 完整镜像，写入 `0x0`，覆盖镜像范围内的固件、分区和设置 |
| `dist/mosaico-webcam-0.1.0.irisfw` | Iris 0.2 系统更新包，通过 Vibe Mode 安装 |
| `dist/app.bin` | 应用镜像，仅用于分区布局兼容的应用更新 |
| `dist/manifest.json` | 构建配置、依赖版本、偏移与源码哈希 |
| `dist/SHA256SUMS` | 主要产物的 SHA-256 校验值 |
| `dist/*local.patch` | SDK 与 BSP 的实际本地修改 |
| `dist/tinyusb-video-recovery.patch` | USB 视频传输恢复补丁 |

包内另附中英文 README、测试指南、依赖锁文件、开发文档、素材来源和许可证。

当前采用 `esp-mosaico/v2` 分区布局：引导程序位于 `0x2000`，分区表位于 `0x8000`，Vibe Mode 位于 `0x20000`，应用 NVS 位于 `0x200000`，应用位于 `0x210000`。旧版 `0x10000` 单应用布局不兼容，首次迁移不会自动迁移旧设置。打包脚本读取构建生成的 `flasher_args.json`；烧录时应以对应构建的偏移为准。**应用镜像不能写入 `0x0`。**

Iris 包通过官方 `prepare_system_update.py` 和 `build_system_update_bundle` 生成，再由官方 Ideas 固件校验器检查。包内仅含 4 KiB 分区表和应用，不含引导程序、Vibe Mode、凭据或 NVS 镜像；最低要求为 Vibe Mode 0.2.0。完整有线 BIN 另外包含固定版本的官方 Vibe Mode 镜像，打包前核对其 SHA-256。校验值和工具提交记录在 `manifest.json` 中。

Webcam 不启动 Iris 运行时服务，USB 保持单一 UVC 接口。通过 Vibe Mode 安装后，以设备启动及电脑 UVC 取流检查结果；不提供 Iris 预览、运行控制或重连后的自动固件身份校验。应用完成 UVC、显示和处理任务初始化后，确认待验证镜像，避免下次重启触发回滚。按住 AI 键重启可通过官方引导程序返回 Vibe Mode。

完整镜像会覆盖其地址范围内的固件、分区和 NVS 设置。烧录前备份需要保留的数据，并激活项目固定的 ESP-IDF 环境。

1. 按住 BOOT，复位或重新上电，进入 ROM 下载模式。
2. 将 `PORT` 替换为已确认的主板下载串口，执行以下命令。具体下载接口以主板手册为准。

   ```sh
   python -m esptool --chip esp32s31 --port PORT --no-stub \
     write-flash 0x0 dist/factory-merged.bin
   ```

3. 烧录完成后松开 BOOT，重新启动设备。

已有匹配 Vibe Mode 的设备也可在已激活 SDK 的终端中执行 `idf.py -p PORT flash`，按构建生成的分区偏移烧录；该命令不会写入 Vibe Mode，首次安装应使用完整 BIN。运行时的 UVC 接口不提供串口功能。

恢复原厂固件时，使用官方恢复包或已备份的完整 Flash 镜像。不要将 `erase-flash` 作为常规恢复步骤，以免清除需要保留的数据。

## 硬件与诊断

- SC101IOT 摄像头子板接入左侧 H2 插槽；视频使用 High-Speed OTG USB-C，日志通过波特率为 115200 的 UART 输出。
- BSP 通过 eFuse 识别板卡，通过 EEPROM 识别摄像头。未知板卡报告错误，不使用推测的引脚配置。
- 摄像头 GPIO33 和 GPIO34 与 USB Serial/JTAG 复用，项目默认关闭该控制器及其日志输出。
- SC101IOT 默认以 1280×720、25 FPS 采集。BSP 内另有 OV3640 的 1024×768、25 FPS 配置；本项目的使用和测试指南以 SC101IOT 为准。
- USB 输出模式固定为 MJPEG 640×480，目标帧率为 25 FPS。

UART 每 5 秒输出一次统计信息，包括帧率、处理耗时、JPEG 大小、帧年龄、错误计数、内存和 USB 速度。

| 计数器 | 含义 |
| --- | --- |
| `capture` | 成功取得的摄像头帧 |
| `encoded` | 编码成功并发布的 JPEG 帧 |
| `feed` | UVC 回调取得的全部帧，包含新画面、重复画面和黑帧 |
| `fresh` | UVC 回调取得的新画面 |
| `repeat` | UVC 回调重复取得的最近一帧 |
| `black` | UVC 回调取得的黑帧 |
| `preview` | 成功提交的本地预览更新 |

帧率统计取相邻两次报告之间的平均值。处理耗时取最近一帧的值。主机接收帧率在电脑端测量，面板显示效果在设备屏幕上观察，具体方法见[测试指南](../TESTING.md#帧率与延迟记录)。

## 检查

检查命令、覆盖范围及设备测试步骤见[测试指南](../TESTING.md)。缓冲区生命周期、方向变换和错误恢复见[架构说明](ARCHITECTURE.md)。

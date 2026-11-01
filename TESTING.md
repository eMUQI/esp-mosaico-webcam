# 测试指南

[English README](README.md) | [中文 README](README.zh-CN.md)

本指南面向开发者，介绍主机检查命令、设备测试步骤和故障排查注意事项。测试硬件使用 ESP-Mosaico 主板和 SC101IOT 摄像头子板，USB 视频模式为 MJPEG 640×480，目标帧率为 25 FPS。

## 主机检查

所有命令均在仓库根目录运行。需要以下环境：

- Python 3 和支持 AddressSanitizer（ASan）、UndefinedBehaviorSanitizer（UBSan）的 C 编译器。
- CMake，用于显示缓冲区和界面检查。
- 已安装项目依赖，并已完成一次固件构建，以提供托管组件、ELF 和生成的配置文件。
- 已激活项目固定的 ESP-IDF 环境，以运行 USB 描述符检查。

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main tools/check_policy.c -o /tmp/mosaico-webcam-policy
/tmp/mosaico-webcam-policy
python3 tools/check_firmware.py
python3 tools/check_uvc_recovery.py
python3 tools/check_display_rotation.py
python3 tools/check_display_buffers.py
python3 tools/check_bsp_display_patch.py
```

| 检查项 | 覆盖范围 |
| --- | --- |
| `check_policy.c` | 画面几何、隐私帧生命周期、设置解码和方向判定 |
| `check_firmware.py` | 实际链接到 ELF 的 USB 视频描述符 |
| `check_uvc_recovery.py` | USB 初始化失败后的资源清理，以及视频传输失败、取消和恢复 |
| `check_display_rotation.py` | 旋转时的绘制缓冲区重绑和触摸坐标变换 |
| `check_display_buffers.py` | 延迟消费条件下的显示缓冲区复用 |
| `check_bsp_display_patch.py` | BSP 显示补丁应用、旧补丁迁移和冲突处理 |

### 界面渲染与交互检查

界面检查使用固件的 LVGL 源码，以硬件替代实现运行。执行以下命令，生成主机检查程序和 PPM 格式截图：

```sh
python3 tools/ui_capture/setup.py build-host-ui
cmake -S build-host-ui -B build-host-ui/build
cmake --build build-host-ui/build -j 8
(cd build-host-ui && ./build/capture)
```

摄像头连接状态检查使用独立目录：

```sh
python3 tools/ui_capture/camera/setup.py build-host-camera
cmake -S build-host-camera -B build-host-camera/build
cmake --build build-host-camera/build -j 8
(cd build-host-camera && ./build/capture)
```

主机检查覆盖状态转换和界面逻辑。实际摄像头色彩、DMA 调度、USB 主机驱动和 OLED 显示效果属于下方设备测试的范围。

## 设备测试

### Iris 安装验收

构建期间的官方校验器检查 `.irisfw` 格式、分区布局和应用镜像，不代表设备安装已通过。设备需安装 Vibe Mode 0.2.0 或更新版本：

1. 备份应用数据，进入 Vibe Mode，通过官方 Iris 安装器或 Ideas 安装生成的 `.irisfw`。
2. 确认设备启动 Webcam，隐私保护默认开启，电脑识别 MJPEG 640×480、25 FPS 的 UVC 接口。
3. 再次重启，确认仍进入 Webcam，检查应用安装确认和设置保留。
4. 按住 AI 键重启，确认返回 Vibe Mode，再次安装更新包，验证完整安装循环。

此构建不提供 Iris 运行时预览或控制，重连后的验收使用设备画面、UVC 和 UART。

### 视频与交互验收

测试前，记录设备、摄像头型号、固件版本、主机操作系统、应用及所选视频模式。按以下顺序检查：

1. 重启设备，确认隐私保护已开启，电脑收到黑画面。开启摄像头后应显示实时视频，关闭后应恢复黑画面。
2. 拍摄文字和带有边缘标记的画面，检查方向、完整视野及镜像。分别检查四个 USB 视频旋转角度与四个设备姿态；本地预览镜像不应改变电脑视频。
3. 检查语言切换、亮度和设置重启保留。分别检查自动关屏、触摸唤醒和四个设备姿态下的按钮响应；首次唤醒触摸不应触发按钮。
4. 在相同动态场景下，分别开启预览、关闭预览和关闭屏幕，各记录至少 60 秒，比较电脑收到的新画面帧率、帧间隔、延迟及 UART 统计，同时观察本地预览的撕裂和残影。
5. 检查摄像头缺失、拔出和重新插入后的提示与恢复。执行至少 20 次开关流、10 次 USB 拔插及 30 分钟连续取流，记录错误、内存占用和恢复情况。

### 帧率与延迟记录

USB 视频可能包含重复画面和隐私黑帧。测量新画面帧率时，应使用动态场景，并区分新画面、重复画面和黑帧，分别记录新画面数量、帧间隔与端到端延迟。

UART 中 `fresh` 表示 UVC 回调取得新画面，`feed` 还包括重复帧和黑帧。主机接收帧率以电脑端测量为准。`preview` 表示提交的预览更新次数，面板刷新效果以设备屏幕观察为准。计数器含义见[开发指南](docs/DEVELOPMENT.md#硬件与诊断)。

## 故障排查

| 现象 | 说明与处理 |
| --- | --- |
| 快速拔插摄像头后显示旧画面，或连接引导提前消失 | 旧帧处理可能跨越完整拔插周期。排查时记录拔插顺序、连接提示和出现旧画面的时机。 |
| 持续出现采集或直接内存访问（DMA）错误 | 查看 UART 错误日志；自动重试后仍无法恢复时，可能需要重新上电。 |

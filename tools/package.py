#!/usr/bin/env python3
"""Package Iris installation firmware and a complete wired image."""
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from check_firmware import check

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
DIST = ROOT / "dist"
UTILS = ROOT / "vendor/esp-mosaico-utils"
UTILS_COMMIT = "c6b43a6be7fb8aba832fc4016d4076702f5414ae"


def iris_bundle(description, flash):
    if git(UTILS, "rev-parse", "HEAD") != UTILS_COMMIT or git(UTILS, "status", "--porcelain"):
        raise SystemExit("Iris tools differ from the pinned official revision")
    if flash["flash_files"].get("0x210000") != description["app_bin"]:
        raise SystemExit("Iris requires main_app at 0x210000; rebuild with partitions.csv")
    sys.path[:0] = [str(UTILS / "mosaico-tools/tools"),
                   str(UTILS / "ESP-Iris/components/esp_iris/tools")]
    from mosaico_cli.ideas_firmware import validate_firmware
    from iris_gateway.system_update import build_system_update_bundle

    with tempfile.TemporaryDirectory(prefix="iris-package-", dir=BUILD) as directory:
        stage = Path(directory)
        subprocess.run([
            sys.executable, str(UTILS / "mosaico-tools/tools/prepare_system_update.py"),
            "--partition-csv", str(ROOT / "partitions.csv"),
            "--partition-table", str(BUILD / "partition_table/partition-table.bin"),
            "--application", str(BUILD / description["app_bin"]),
            "--stage-dir", str(stage), "--release", description["project_version"],
        ], check=True)
        bundle = build_system_update_bundle(
            stage / "firmware.irisfw", json.loads((stage / "manifest.json").read_text()), stage)
        raw = bundle.read_bytes()
        validate_firmware(raw)
    recovery = UTILS / "esp-mosaico-vibe-mode/firmware/vibe-mode/prebuilt/recovery"
    entry = json.loads((recovery / "manifest.json").read_text())["images"]["recovery"]
    vibe = recovery / entry["file"]
    if hashlib.sha256(vibe.read_bytes()).hexdigest() != entry["sha256"]:
        raise SystemExit("Vibe Mode does not match its official manifest")
    return raw, entry, vibe


def git(directory, *args):
    return subprocess.check_output(["git", "-C", str(directory), *args], text=True).strip()


def main():
    idf = Path(os.environ["IDF_PATH"])
    expected = (ROOT / "esp-idf.commit").read_text().strip()
    if git(idf, "rev-parse", "HEAD") != expected:
        raise SystemExit("ESP-IDF differs from esp-idf.commit")
    description = json.loads((BUILD / "project_description.json").read_text())
    flash = json.loads((BUILD / "flasher_args.json").read_text())
    if description["target"] != "esp32s31":
        raise SystemExit("Refusing to package a non-ESP32-S31 image")
    config = json.loads((BUILD / "config/sdkconfig.json").read_text())
    expected_config = {
        "TINYUSB_RHPORT_HS": True, "FORMAT_MJPEG_CAM1": True,
        "UVC_CAM1_FRAMESIZE_WIDTH": 640, "UVC_CAM1_FRAMESIZE_HEIGT": 480,
        "UVC_CAM1_FRAMERATE": 25, "UVC_CAM1_MULTI_FRAMESIZE": False,
        "CAMERA_SC101IOT_DVP_DEFAULT_FMT_YUV422_1280X720_25FPS": True,
        "LV_DEF_REFR_PERIOD": 16,
        "WEBCAM_SENSOR_MIRROR_X": True,
        "ESPTOOLPY_FLASHMODE_QIO": True,
        "BOOTLOADER_APP_ROLLBACK": True,
        "BOOTLOADER_APP_ROLLBACK_CONFIRM_BY_APP": True,
        "ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG": False,
        "ESP_CONSOLE_USB_SERIAL_JTAG": False, "USJ_ENABLE_USB_SERIAL_JTAG": False,
    }
    for key, value in expected_config.items():
        if config.get(key, False) != value:
            raise SystemExit(f"Unexpected build setting: {key}={config.get(key)!r}")
    check(BUILD)
    bundle, vibe_entry, vibe = iris_bundle(description, flash)
    prefix = f"mosaico-webcam-{description['project_version']}"
    iris_name = f"{prefix}.irisfw"
    app_name = f"{prefix}-app.bin"
    factory_name = f"{prefix}-factory-merged.bin"
    if DIST.exists():
        shutil.rmtree(DIST)
    DIST.mkdir()
    settings = flash["flash_settings"]
    command = [sys.executable, "-m", "esptool", "--chip", "esp32s31", "merge-bin",
               "--output", str(DIST / factory_name)]
    for name in ("flash_mode", "flash_freq", "flash_size"):
        command += ["--" + name.replace("_", "-"), settings[name]]
    for offset, filename in sorted(flash["flash_files"].items(), key=lambda item: int(item[0], 0)):
        command += [offset, str(BUILD / filename)]
    command += [vibe_entry["offset"], str(vibe)]
    subprocess.run(command, check=True)
    (DIST / iris_name).write_bytes(bundle)
    shutil.copyfile(BUILD / "mosaico-webcam.bin", DIST / app_name)
    shutil.copyfile(ROOT / "dependencies.lock", DIST / "dependencies.lock")
    shutil.copyfile(ROOT / "README.md", DIST / "README.md")
    shutil.copyfile(ROOT / "README.zh-CN.md", DIST / "README.zh-CN.md")
    shutil.copyfile(ROOT / "esp-idf.commit", DIST / "esp-idf.commit")
    shutil.copyfile(ROOT / "TESTING.md", DIST / "TESTING.md")
    shutil.copyfile(ROOT / "LICENSE", DIST / "LICENSE")
    shutil.copyfile(ROOT / "vendor/usb_device_uvc/tusb/video-device-recovery.patch",
                    DIST / "tinyusb-video-recovery.patch")
    shutil.copytree(ROOT / "docs", DIST / "docs")
    shutil.copytree(ROOT / "main/assets", DIST / "artwork-source")
    shutil.copyfile(ROOT / "sdkconfig", DIST / "sdkconfig")
    idf_diff = git(idf, "diff", "--binary")
    (DIST / "esp-idf-local.patch").write_text(idf_diff + ("\n" if idf_diff else ""))
    bsp_diff = git(ROOT / "vendor/esp-mosaico-bsp", "diff", "--binary")
    (DIST / "esp-mosaico-bsp-local.patch").write_text(bsp_diff + ("\n" if bsp_diff else ""))
    sources = [ROOT / "tools/package.py", ROOT / "tools/build.sh",
               ROOT / "docs/patches/esp-mosaico-bsp-display.patch",
               ROOT / "docs/patches/esp-mosaico-bsp-display-migrate-v1.patch",
               ROOT / "vendor/esp-mosaico-bsp/components/esp-mosaico-bsp/onboard/display.c",
               ROOT / "CMakeLists.txt", ROOT / "sdkconfig.defaults", ROOT / "partitions.csv",
               ROOT / "dependencies.lock", *sorted((ROOT / "main").rglob("*")),
               *sorted((ROOT / "vendor/usb_device_uvc").rglob("*"))]
    manifest = {
        "project": description["project_name"], "version": description["project_version"],
        "target": description["target"],
        "idf_version": ".".join(re.findall(r"set\(IDF_VERSION_(?:MAJOR|MINOR|PATCH) (\d+)\)",
                                             (idf / "tools/cmake/version.cmake").read_text())),
        "idf_build_revision": git(idf, "describe", "--always", "--dirty"),
        "idf_commit": expected,
        "bsp_commit": git(ROOT / "vendor/esp-mosaico-bsp", "rev-parse", "HEAD"),
        "iris_tools_commit": UTILS_COMMIT,
        "iris_bundle": iris_name, "vibe_mode": vibe_entry,
        "uvc_version": "1.3.1", "flash_settings": settings,
        "tinyusb_video_source_sha256": hashlib.sha256(
            (BUILD / "esp-idf/usb_device_uvc/video_device.c").read_bytes()).hexdigest(),
        "static_frame_build": config.get("WEBCAM_STATIC_FRAME", False),
        "flash_files": {**flash["flash_files"], vibe_entry["offset"]: vibe_entry["file"]},
        "hardware_verified": False,
        "source_sha256": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sources if p.is_file()},
    }
    (DIST / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    artifacts = [iris_name, app_name, factory_name, "manifest.json", "dependencies.lock", "esp-idf-local.patch",
                 "sdkconfig", "README.md", "README.zh-CN.md", "esp-idf.commit",
                 "TESTING.md", "LICENSE", "tinyusb-video-recovery.patch",
                 "esp-mosaico-bsp-local.patch"]
    (DIST / "SHA256SUMS").write_text("".join(
        f"{hashlib.sha256((DIST / name).read_bytes()).hexdigest()}  {name}\n" for name in artifacts))
    print(f"Packaged {DIST}; hardware acceptance remains pending.")


if __name__ == "__main__":
    main()

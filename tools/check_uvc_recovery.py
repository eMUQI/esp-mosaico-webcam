#!/usr/bin/env python3
"""Fault-inject UVC initialization, frame submission and TinyUSB transfer recovery."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
UVC = ROOT / 'vendor/usb_device_uvc'


def function(source, signature):
    """Extract one known C definition without altering its function body."""
    start = source.index(signature)
    cursor = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[start:cursor] + '\n'


with tempfile.TemporaryDirectory(prefix='mosaico-uvc-recovery-') as directory:
    work = Path(directory)
    original = ROOT / 'managed_components/espressif__tinyusb/src/class/video/video_device.c'
    (work / 'video_device.c').write_bytes(original.read_bytes())
    subprocess.run(['git', 'apply', '--no-index', str(UVC / 'tusb/video-device-recovery.patch')],
                   cwd=work, check=True)
    source = (work / 'video_device.c').read_text()
    functions = ''.join(function(source, signature) for signature in (
        'bool uvc_transport_busy(', 'void uvc_transport_abort(',
        'bool tud_video_n_frame_xfer(', 'bool videod_xfer_cb('))
    production = (UVC / 'usb_device_uvc.c').read_text()
    functions += ''.join(function(production, signature) for signature in (
        'static esp_err_t usb_phy_init(', 'esp_err_t uvc_device_init(',
        'static void video_service(unsigned index)\n{'))
    fixture = (ROOT / 'tools/uvc_recovery_fixture.c').read_text()
    (work / 'check.c').write_text(fixture.replace('/* PRODUCTION_FUNCTIONS */', functions))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(work / 'check.c'),
                    '-o', str(work / 'check')], check=True)
    subprocess.run([str(work / 'check')], check=True)

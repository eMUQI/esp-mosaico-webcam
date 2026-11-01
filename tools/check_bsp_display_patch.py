#!/usr/bin/env python3
"""Exercise the build script's patch selection against clean, current and legacy BSP files."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
relative = Path("components/esp-mosaico-bsp/onboard/display.c")
base = subprocess.check_output(["git", "-C", str(root / "vendor/esp-mosaico-bsp"),
                                "show", f"HEAD:{relative}"])
script = (root / "tools/build.sh").read_text()
selection = script[script.index('patch="$PWD/'):script.index("idf.py --preview build")]

with tempfile.TemporaryDirectory(prefix="webcam-bsp-patch-check-") as directory:
    work = Path(directory)
    patches = work / "docs/patches"
    patches.mkdir(parents=True)
    for name in ("esp-mosaico-bsp-display.patch", "esp-mosaico-bsp-display-migrate-v1.patch"):
        (patches / name).write_bytes((root / "docs/patches" / name).read_bytes())
    bsp = work / "vendor/esp-mosaico-bsp"
    file = bsp / relative
    file.parent.mkdir(parents=True)

    def apply(name, *options):
        subprocess.run(["git", "-C", str(bsp), "apply", *options, str(work / name)], check=True)

    def select():
        return subprocess.run(["sh", "-ec", selection], cwd=work, capture_output=True)

    file.write_bytes(base)
    apply("docs/patches/esp-mosaico-bsp-display.patch")
    expected = file.read_bytes()
    extra = b"\n/* Preserve an unrelated local BSP modification. */\n"
    for state in ("clean", "current", "legacy"):
        file.write_bytes(base)
        if state == "current":
            apply("docs/patches/esp-mosaico-bsp-display.patch")
        elif state == "legacy":
            file.write_bytes(expected)
            apply("docs/patches/esp-mosaico-bsp-display-migrate-v1.patch", "--reverse")
        file.write_bytes(file.read_bytes() + extra)
        assert select().returncode == 0, state
        assert file.read_bytes() == expected + extra, state
        assert select().returncode == 0, state
        assert file.read_bytes() == expected + extra, state
        print(f"{state}: correct patch, unrelated edits preserved, repeat application is safe")

    file.write_bytes(base.replace(b"BSP_LCD_DMA_CHUNK_LINES 8", b"BSP_LCD_DMA_CHUNK_LINES 17"))
    conflicting = file.read_bytes()
    assert select().returncode != 0
    assert file.read_bytes() == conflicting
    print("conflicting edits: rejected without changing the BSP")

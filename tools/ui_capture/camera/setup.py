"""Reuse the native language renderer for camera-removal state transitions."""
from pathlib import Path
import subprocess
import sys

source = Path(__file__).resolve().parent
root = source.parents[2]
work = Path(sys.argv[1]).resolve()
subprocess.run([sys.executable, str(root / "tools/ui_capture/setup.py"), str(work)], check=True)
capture = (work / "capture.c").read_text()
capture = capture.replace('#include "webcam.h"', '#include "webcam.h"\n#include "mosaico_module_mgr.h"', 1)
capture = capture.replace("int main(int argc, char **argv)", "static void language_checks(int argc, char **argv)")
(work / "capture.c").write_text(capture + "\n" + (source / "removal.c").read_text())

# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
"""Exercise Gio desktop files and launching in an isolated persistent test tree."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--package", type=Path, required=True)
parser.add_argument("--state", type=Path, required=True)
args = parser.parse_args()
root = args.state / f"catalogue-{time.monotonic_ns()}"
applications = root / "data/applications"
applications.mkdir(parents=True)
marker = root / "launched.json"
helper = root / "launch-proof.py"
license_tag = "SPDX" + "-License-Identifier: GPL-2.0-only"
copyright_tag = "SPDX" + "-FileCopyrightText: 2026 Zak Noble-Clarke"
helper.write_text(f"# {license_tag}\n# {copyright_tag}\n"
                  "import json,os,pathlib,sys\n"
                  "pathlib.Path(sys.argv[1]).write_text(json.dumps({'pid':os.getpid(),'arguments':sys.argv[1:]}))\n")
def quote(value):
    return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$') + '"'
command = " ".join(quote(value) for value in [sys.executable, helper, marker])
base = (f"# {license_tag}\n# {copyright_tag}\n[Desktop Entry]\nType=Application\n"
        "Name=Session catalogue test\nComment=Real Gio desktop-entry fixture\n"
        "Icon=utilities-terminal\nCategories=Utility;Development;\n"
        f"Exec={command}\n")
for name, settings in {
    "visible": "OnlyShowIn=GNOME;\n",
    "hidden": "Hidden=true\n",
    "nodisplay": "NoDisplay=true\n",
    "other-desktop": "OnlyShowIn=KDE;\n",
    "unavailable": f"TryExec={root / 'absent-executable'}\n",
}.items():
    (applications / f"mx-test-{name}.desktop").write_text(base + settings)
(applications / "org.mx.SessionBridgeTest.desktop").write_text(
    "\n".join(line for line in base.splitlines() if not line.startswith("Exec=")) +
    "\nDBusActivatable=true\n")
environment = dict(os.environ, GIO_USE_VFS="local", XDG_CURRENT_DESKTOP="GNOME",
                   XDG_DATA_HOME=str(root / "data"), XDG_DATA_DIRS=str(root / "empty-data"))
environment.pop("DISPLAY", None)
environment.pop("WAYLAND_DISPLAY", None)
test = Path(__file__).with_name("test-catalogue.js")
result = subprocess.run(["dbus-run-session", "--", "gjs", "-m", str(test),
                         str(args.package.resolve()), str(marker)], env=environment, timeout=5)
if result.returncode:
    raise SystemExit(result.returncode)
proof = json.loads(marker.read_text())
if proof.get("arguments") != [str(marker)] or proof.get("pid", 0) <= 0:
    raise SystemExit("Headless desktop-ID launch produced incorrect invocation proof")
print(f"PASS headless launch proof pid={proof['pid']} persistent_fixture={root}")

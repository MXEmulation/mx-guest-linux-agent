# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
"""Build a session extension package with PCI identity extracted from Core."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    header = args.core.resolve() / "include/mx_versions.h"
    if not header.is_file():
        parser.error("Core must contain include/mx_versions.h")
    args.output.mkdir(parents=True, exist_ok=True)
    license_tag = "SPDX" + "-License-Identifier: GPL-2.0-only"
    copyright_tag = "SPDX" + "-FileCopyrightText: 2026 Zak Noble-Clarke"
    probe = args.output / "core-pci-identity.c"
    probe.write_text(f"/* {license_tag} */\n"
                     f"/* {copyright_tag} */\n"
                     "#include <stdio.h>\n#include <mx_versions.h>\n"
                     "int main(void) { return printf(\"%u %u\\n\", "
                     "(unsigned)MX_PCI_VENDOR_ID, (unsigned)MXGPU_PCI_DEVICE_ID) < 0; }\n")
    binary = args.output / "core-pci-identity"
    subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(header.parent), str(probe), "-o", str(binary)], check=True)
    vendor, device = map(int, subprocess.check_output([str(binary)], text=True).split())
    if not 0 <= vendor <= 65535 or not 0 <= device <= 65535:
        raise ValueError("Core PCI identity exceeds its field width")
    metadata = json.loads((source / "metadata.json").read_text())
    package = args.output / metadata["uuid"]
    package.mkdir(exist_ok=True)
    names = ["extension.js", "backend.js", "files.js", "sysfs.js", "model.js", "api.js", "service.js",
             "clipboard.js", "clipboard-protocol.js", "metadata.json", "README.md", "DEPENDENCIES.md"]
    for name in names:
        shutil.copyfile(source / name, package / name)
    (package / "identity.js").write_text(
        f"// {license_tag}\n"
        f"// {copyright_tag}\n"
        f"export const pciIdentity = Object.freeze({{vendor: {vendor}, device: {device}}});\n")
    shutil.copyfile(source.parent / "LICENCE", package / "LICENSE")
    manifest = {
        "package": str(package.resolve()),
        "core_identity_header_sha256": hashlib.sha256(header.read_bytes()).hexdigest(),
        "pci_identity": {"vendor": vendor, "device": device},
        "sources": {name: hashlib.sha256((source / name).read_bytes()).hexdigest() for name in names},
        "files": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(package.iterdir()) if p.is_file()},
        "installed": False, "enabled": False,
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(package)


if __name__ == "__main__":
    main()

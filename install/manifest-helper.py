# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--icd', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
data = json.loads(args.icd.read_text())
if data.get('file_format_version') != '1.0.0' or data.get('ICD', {}).get('library_path') != 'libvulkan_mxgpu.so':
    raise SystemExit('The built Vulkan ICD has an unexpected library record')
data['ICD']['library_path'] = '/opt/mxgpu/current/lib/libvulkan_mxgpu.so'
args.output.write_text(json.dumps(data, indent=2) + '\n')

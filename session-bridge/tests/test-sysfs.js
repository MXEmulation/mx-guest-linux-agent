// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import {recognizeConnector, resolvePath} from '../sysfs.js';

function assert(value, message) {
    if (!value)
        throw new Error(message);
}

const links = new Map();
const files = new Map();
const entries = [];
const fs = {
    list: () => [...entries],
    read: path => files.get(resolvePath(fs, path)) ?? null,
    link: path => links.get(path) ?? null,
};
const identity = {vendor: 42, device: 73};
function add(card, connector, address, vendor, device, status = 'connected') {
    const name = `${card}-${connector}`;
    const pci = `/sys/devices/pci0000:00/${address}`;
    const drm = `${pci}/drm/${card}`;
    const path = `${drm}/${name}`;
    entries.push(name);
    links.set(`/sys/class/drm/${name}`, `../../devices/pci0000:00/${address}/drm/${card}/${name}`);
    links.set(`${path}/device`, `../../${card}`);
    links.set(`${drm}/device`, `../../../${address}`);
    links.set(`${pci}/subsystem`, '../../../bus/pci');
    files.set(`${path}/status`, status);
    files.set(`${pci}/vendor`, `0x${vendor.toString(16)}`);
    files.set(`${pci}/device`, `0x${device.toString(16)}`);
}
add('card17', 'Virtual-1', '0000:00:04.0', 42, 73);
let found = recognizeConnector(fs, 'Virtual-1', identity);
assert(found.recognized_mxgpu && found.pci_address === '0000:00:04.0', 'Connector-to-card-to-PCI traversal failed');
assert(found.matches[0].drm_name === 'card17-Virtual-1', 'Assumed card index');
add('card3', 'HDMI-A-1', '0000:00:05.0', 42, 99);
assert(!recognizeConnector(fs, 'HDMI-A-1', identity).recognized_mxgpu, 'Unrelated PCI output accepted');
assert(!recognizeConnector(fs, 'missing', identity).recognized_mxgpu, 'Invented absent output');
add('card9', 'Virtual-1', '0000:00:06.0', 42, 99);
found = recognizeConnector(fs, 'Virtual-1', identity);
assert(found.ambiguous && !found.recognized_mxgpu, 'Ambiguous connector guessed');
files.set('/sys/devices/pci0000:00/0000:00:06.0/drm/card9/card9-Virtual-1/status', 'disconnected');
assert(recognizeConnector(fs, 'Virtual-1', identity).recognized_mxgpu, 'Disconnected duplicate invalidated unique active match');
links.set('/cycle-a', '/cycle-b');
links.set('/cycle-b', '/cycle-a');
let rejected = false;
try { resolvePath(fs, '/cycle-a'); } catch (_error) { rejected = true; }
assert(rejected, 'Unbounded symlink cycle');
print('PASS real sysfs link shapes, arbitrary card indices, unrelated devices, absent/disconnected/ambiguous outputs and bounded cycles');

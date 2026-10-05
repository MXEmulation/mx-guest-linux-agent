// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke

function parent(path) {
    return path.slice(0, path.lastIndexOf('/')) || '/';
}

function normalize(path) {
    const parts = [];
    for (const part of path.split('/')) {
        if (!part || part === '.')
            continue;
        if (part === '..')
            parts.pop();
        else
            parts.push(part);
    }
    return `/${parts.join('/')}`;
}

export function resolvePath(fs, input) {
    let path = normalize(input);
    for (let count = 0; count < 64; count++) {
        const parts = path.split('/').filter(Boolean);
        let prefix = '';
        let followed = false;
        for (let i = 0; i < parts.length; i++) {
            prefix += `/${parts[i]}`;
            const target = fs.link(prefix);
            if (target !== null) {
                const joined = target.startsWith('/') ? target : `${parent(prefix)}/${target}`;
                path = normalize(`${joined}/${parts.slice(i + 1).join('/')}`);
                followed = true;
                break;
            }
        }
        if (!followed)
            return path;
    }
    throw new Error('Sysfs symlink traversal limit exceeded');
}

function hex(text) {
    const value = text?.trim();
    return value && /^0x[0-9a-f]+$/i.test(value) ? Number.parseInt(value.slice(2), 16) : null;
}

export function recognizeConnector(fs, connector, identity, root = '/sys') {
    const matches = [];
    for (const entry of fs.list(`${root}/class/drm`).sort()) {
        const match = /^card[0-9]+-(.+)$/.exec(entry);
        if (!match || match[1] !== connector)
            continue;
        const connectorPath = `${root}/class/drm/${entry}`;
        const status = fs.read(`${connectorPath}/status`)?.trim() ?? null;
        let path = resolvePath(fs, connectorPath);
        const visited = new Set();
        let pci = null;
        for (let depth = 0; depth < 64 && path !== root && path !== '/'; depth++) {
            if (visited.has(path))
                throw new Error('Sysfs device ancestry cycle');
            visited.add(path);
            const vendor = hex(fs.read(`${path}/vendor`));
            const device = hex(fs.read(`${path}/device`));
            const subsystem = fs.link(`${path}/subsystem`);
            if (vendor !== null && device !== null && subsystem !== null &&
                resolvePath(fs, `${path}/subsystem`) === normalize(`${root}/bus/pci`)) {
                pci = {address: path.slice(path.lastIndexOf('/') + 1), vendor, device,
                    path, mxgpu: vendor === identity.vendor && device === identity.device};
                break;
            }
            path = fs.link(`${path}/device`) !== null
                ? resolvePath(fs, `${path}/device`) : parent(path);
        }
        matches.push({drm_name: entry, connector_path: resolvePath(fs, connectorPath), status, pci});
    }
    const connected = matches.filter(item => item.status === 'connected');
    const unique = connected.length === 1 ? connected[0] : null;
    return {connector, matches, recognized_mxgpu: Boolean(unique?.pci?.mxgpu),
        pci_address: unique?.pci?.mxgpu ? unique.pci.address : null,
        ambiguous: connected.length > 1};
}

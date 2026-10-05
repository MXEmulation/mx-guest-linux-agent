// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import Gio from 'gi://Gio';
import GioUnix from 'gi://GioUnix';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import {Files} from '../files.js';
import {recognizeConnector} from '../sysfs.js';

if (ARGV.length !== 1)
    throw new Error('Expected a prepared extension package directory');
const packageUri = Gio.File.new_for_path(ARGV[0]).get_uri();
const {Backend, iconDescriptor} = await import(`${packageUri}/backend.js`);
const {pciIdentity} = await import(`${packageUri}/identity.js`);
for (const method of ['list_all_windows', 'sort_windows_by_stacking', 'get_monitor_geometry', 'get_monitor_scale']) {
    if (typeof Meta.Display.prototype[method] !== 'function')
        throw new Error(`Missing GNOME Display API ${method}`);
}
for (const method of ['get_frame_rect', 'get_buffer_rect', 'get_client_content_rect', 'activate_with_workspace', 'delete']) {
    if (typeof Meta.Window.prototype[method] !== 'function')
        throw new Error(`Missing GNOME Window API ${method}`);
}
if (typeof Shell.Global.prototype.create_app_launch_context !== 'function')
    throw new Error('Missing session application launch context');
const catalogue = Backend.prototype.catalogue.call({});
const actual = new Map(Gio.AppInfo.get_all().filter(info => info instanceof GioUnix.DesktopAppInfo &&
    info.should_show() && !info.get_is_hidden() && !info.get_nodisplay()).map(info => [info.get_id(), info]));
if (actual.size !== catalogue.length)
    throw new Error('Installed application catalogue is incomplete or duplicated');
for (const app of catalogue) {
    const info = actual.get(app.id);
    if (!info || app.name !== info.get_name() || app.desktop_file !== info.get_filename() ||
        JSON.stringify(app.icon) !== JSON.stringify(iconDescriptor(info.get_icon())))
        throw new Error(`Invented installed application metadata for ${app.id}`);
}
const fs = new Files();
const connectorNames = new Set(fs.list('/sys/class/drm')
    .map(name => /^card[0-9]+-(.+)$/.exec(name)?.[1]).filter(Boolean));
for (const name of connectorNames) {
    const result = recognizeConnector(fs, name, pciIdentity);
    for (const match of result.matches) {
        if (match.pci && !/^\d{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]$/i.test(match.pci.address))
            throw new Error('Resolved a non-PCI device as PCI');
    }
    print(`SYSFS ${JSON.stringify(result)}`);
}
const file = Gio.File.new_for_path(GLib.build_filenamev([ARGV[0], 'metadata.json']));
if (!file.query_exists(null))
    throw new Error('Package metadata absent');
print(`PASS GNOME 50 installed APIs, ${catalogue.length} truthful installed apps and actual read-only connector traversal; no launch or display activation`);

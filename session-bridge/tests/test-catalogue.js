// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import {Applications} from '../model.js';
if (ARGV.length !== 2)
    throw new Error('Expected extension package and launch marker paths');
const {Backend} = await import(`${Gio.File.new_for_path(ARGV[0]).get_uri()}/backend.js`);
const backend = {
    catalogue: () => Backend.prototype.catalogue.call({}),
    launch: info => Backend.prototype.launch.call({global: {
        get_current_time: () => 0,
        create_app_launch_context: () => Gio.AppLaunchContext.new(),
    }}, info),
};
const apps = new Applications('catalogue-test', backend);
const catalogue = apps.refresh();
if (catalogue.applications.length !== 2 || !catalogue.applications.some(app => app.id === 'mx-test-visible.desktop'))
    throw new Error('Hidden, NoDisplay, wrong-desktop or unavailable TryExec entry leaked');
const app = catalogue.applications.find(item => item.id === 'mx-test-visible.desktop');
const dbusApp = catalogue.applications.find(item => item.id === 'org.mx.SessionBridgeTest.desktop');
if (!dbusApp?.can_launch || !dbusApp.dbus_activatable)
    throw new Error('Valid D-Bus activation without Exec was declared unlaunchable');
if (app.name !== 'Session catalogue test' || app.comment !== 'Real Gio desktop-entry fixture' ||
    app.categories.join(';') !== 'Utility;Development' || !app.can_launch ||
    app.icon.kind !== 'themed' || !app.icon.names.includes('utilities-terminal'))
    throw new Error('Installed desktop metadata was altered or omitted');
let rejected = false;
try { apps.launch('catalogue-test', catalogue.generation, '/usr/bin/true'); } catch (_error) { rejected = true; }
if (!rejected)
    throw new Error('Launch accepted a command instead of an installed desktop ID');
apps.launch('catalogue-test', catalogue.generation, app.id);
const marker = Gio.File.new_for_path(ARGV[1]);
const deadline = GLib.get_monotonic_time() + 2000000;
while (!marker.query_exists(null) && GLib.get_monotonic_time() < deadline)
    GLib.usleep(10000);
if (!marker.query_exists(null))
    throw new Error('Actual Gio desktop launch did not execute the headless test helper');
const desktop = Gio.File.new_for_path(app.desktop_file);
const [, content] = desktop.load_contents(null);
const modified = new TextDecoder().decode(content).replace(/^(Exec=.*)$/m, '$1 --changed');
desktop.replace_contents(new TextEncoder().encode(modified), null, false,
    Gio.FileCreateFlags.REPLACE_DESTINATION, null);
rejected = false;
try { apps.launch('catalogue-test', catalogue.generation, app.id); } catch (_error) { rejected = true; }
if (!rejected || apps.refresh().generation === catalogue.generation)
    throw new Error('Desktop launch-command change did not invalidate catalogue generation');
print('PASS actual installed desktop filtering, categories/icons, command rejection, headless Gio launch and changed-Exec generation rejection');

// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import System from 'system';
import {BUS_NAME, OBJECT_PATH, INTERFACE, XML} from '../api.js';
import {DBusAPI} from '../service.js';
import {Inventory, Applications} from '../model.js';

const target = {};
const calls = [];
const backend = {
    title: 'D-Bus native target',
    scan() { return {windows: [{handle: target, title: this.title}], monitors: []}; },
    catalogue() { return [{handle: target, id: 'test.desktop', can_launch: true}]; },
    activate(handle) { calls.push(['activate', handle]); },
    close(handle) { calls.push(['close', handle]); },
    launch(handle) { calls.push(['launch', handle]); },
};
const object = Gio.DBusExportedObject.wrapJSObject(XML,
    new DBusAPI(new Inventory('test-session', backend), new Applications('test-session', backend)));
object.export(Gio.DBus.session, OBJECT_PATH);
let owner = 0;
let code = 0;
const loop = GLib.MainLoop.new(null, false);
const timeout = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 5, () => {
    console.error('D-Bus test deadline exceeded');
    code = 1;
    loop.quit();
    return GLib.SOURCE_CONTINUE;
});

function call(method, parameters = null) {
    return new Promise((resolve, reject) => {
        Gio.DBus.session.call(BUS_NAME, OBJECT_PATH, INTERFACE, method, parameters,
            new GLib.VariantType('(s)'), Gio.DBusCallFlags.NONE, 1000, null,
            (connection, result) => {
                try {
                    resolve(JSON.parse(connection.call_finish(result).deep_unpack()[0]));
                } catch (error) {
                    reject(error);
                }
            });
    });
}

async function run() {
    await new Promise((resolve, reject) => {
        owner = Gio.bus_own_name_on_connection(Gio.DBus.session, BUS_NAME,
            Gio.BusNameOwnerFlags.DO_NOT_QUEUE, resolve,
            () => reject(new Error('Private D-Bus service name unavailable')));
    });
    const inventory = await call('ListWindows');
    if (inventory.windows[0].title !== backend.title)
        throw new Error('D-Bus inventory changed native data');
    const control = new GLib.Variant('(sss)', ['test-session', inventory.generation, inventory.windows[0].id]);
    await call('Activate', control);
    await call('Close', control);
    backend.title = 'Changed native target';
    let rejected = false;
    try { await call('Activate', control); } catch (_error) { rejected = true; }
    if (!rejected)
        throw new Error('D-Bus accepted stale generation');
    const catalogue = await call('ListApplications');
    await call('Launch', new GLib.Variant('(sss)', ['test-session', catalogue.generation, 'test.desktop']));
    if (calls.length !== 3 || calls.some(item => item[1] !== target))
        throw new Error('D-Bus dispatched an incorrect action or target');
    print('PASS private D-Bus marshalling, native metadata, generation rejection and all three actions');
}

run().catch(error => { console.error(error); code = 1; }).finally(() => loop.quit());
loop.run();
GLib.source_remove(timeout);
object.unexport();
if (owner)
    Gio.bus_unown_name(owner);
System.exit(code);

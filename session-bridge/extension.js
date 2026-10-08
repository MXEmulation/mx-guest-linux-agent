// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import {BUS_NAME, OBJECT_PATH, XML} from './api.js';
import {DBusAPI} from './service.js';
import {Backend} from './backend.js';
import {Applications, Inventory} from './model.js';
import {ClipboardBridge} from './clipboard.js';

export default class SessionBridge extends Extension {
    enable() {
        try {
            this._start();
        } catch (error) {
            this.disable();
            throw error;
        }
    }

    _start() {
        this._connections = [];
        this._windowConnections = new Map();
        this._source = 0;
        this._backend = new Backend(global);
        const session = GLib.uuid_string_random();
        this._inventory = new Inventory(session, this._backend, (id, generation) =>
            this._object?.emit_signal('WindowInventoryChanged', new GLib.Variant('(ss)', [id, generation])));
        this._applications = new Applications(session, this._backend, (id, generation) =>
            this._object?.emit_signal('ApplicationCatalogueChanged', new GLib.Variant('(ss)', [id, generation])));
        this._object = Gio.DBusExportedObject.wrapJSObject(XML,
            new DBusAPI(this._inventory, this._applications));
        this._object.export(Gio.DBus.session, OBJECT_PATH);
        this._owner = Gio.bus_own_name_on_connection(Gio.DBus.session, BUS_NAME,
            Gio.BusNameOwnerFlags.DO_NOT_QUEUE, null, () => {
                console.error('MX session bridge lost its local D-Bus name');
                this.disable();
            });
        this._connect(global.display, 'window-created', () => this._schedule());
        this._connect(global.display, 'notify::focus-window', () => this._schedule());
        this._connect(global.display, 'restacked', () => this._schedule());
        this._connect(this._backend.tracker, 'tracked-windows-changed', () => this._schedule());
        this._connect(this._backend.monitorManager, 'monitors-changed', () => this._schedule());
        this._connect(global.workspace_manager, 'active-workspace-changed', () => this._schedule());
        this._connect(Gio.AppInfoMonitor.get(), 'changed', () => {
            try {
                this._applications.refresh();
            } catch (error) {
                console.error(error);
            }
        });
        this._syncWindows();
        this._inventory.refresh();
        this._applications.refresh();
        this._clipboard = new ClipboardBridge();
    }

    _connect(object, signal, callback) {
        this._connections.push([object, object.connect(signal, callback)]);
    }

    _syncWindows() {
        const current = new Set(global.display.list_all_windows().filter(window => !window.is_override_redirect()));
        for (const [window, signals] of this._windowConnections) {
            if (!current.has(window)) {
                for (const id of signals)
                    window.disconnect(id);
                this._windowConnections.delete(window);
            }
        }
        for (const window of current) {
            if (this._windowConnections.has(window))
                continue;
            const signals = ['position-changed', 'size-changed', 'workspace-changed', 'unmanaged',
                'notify::title', 'notify::minimized', 'notify::fullscreen', 'notify::wm-class',
                'notify::gtk-application-id', 'notify::mapped', 'notify::on-all-workspaces', 'notify::window-type']
                .map(name => window.connect(name, () => this._schedule()));
            this._windowConnections.set(window, signals);
        }
    }

    _schedule() {
        if (this._source)
            return;
        this._source = GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => {
            this._source = 0;
            try {
                this._syncWindows();
                this._inventory.refresh();
            } catch (error) {
                console.error(error);
            }
            return GLib.SOURCE_REMOVE;
        });
    }

    disable() {
        this._clipboard?.destroy();
        this._clipboard = null;
        if (this._source)
            GLib.source_remove(this._source);
        this._source = 0;
        for (const [window, signals] of this._windowConnections ?? []) {
            for (const id of signals)
                window.disconnect(id);
        }
        this._windowConnections?.clear();
        for (const [object, id] of this._connections ?? [])
            object.disconnect(id);
        this._connections = [];
        this._object?.unexport();
        this._object = null;
        if (this._owner)
            Gio.bus_unown_name(this._owner);
        this._owner = 0;
        this._inventory = null;
        this._applications = null;
        this._backend = null;
    }
}

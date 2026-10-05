// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import GLib from 'gi://GLib';
import {API, INTERFACE} from './api.js';

export class DBusAPI {
    constructor(inventory, applications) {
        this._api = new API(inventory, applications);
        for (const method of ['ListWindows', 'ListApplications', 'Activate', 'Close', 'Launch']) {
            this[`${method}Async`] = (parameters, invocation) => {
                try {
                    invocation.return_value(new GLib.Variant('(s)', [this._api[method](...parameters)]));
                } catch (error) {
                    invocation.return_dbus_error(`${INTERFACE}.Rejected`, error.message);
                }
            };
        }
    }
}

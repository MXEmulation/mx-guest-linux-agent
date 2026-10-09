// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import GLib from 'gi://GLib';
import {FLAG_DESKTOP_BRIDGE_READY, FLAG_ICON_RESOLUTION_READY, FLAG_WINDOW_INVENTORY_READY, describeWindows,
    encodeStatus, fitWindows, sameInventory} from './integration-protocol.js';
import {MESSAGE_ACTION, MESSAGE_WINDOWS, decodeAction} from './session-protocol.js';

const COALESCE_MS = 100;

export class IntegrationBridge {
    constructor(link, inventory, icons) {
        this._link = link;
        this._inventory = inventory;
        this._icons = icons;
        this._source = 0;
        this._sent = null;
        this._destroyed = false;
        link.onConnected(() => {
            this._sent = null;
            this.changed();
        });
        link.onDisconnected(() => {
            this._sent = null;
        });
        link.onMessage(MESSAGE_ACTION, message => this._action(message.payload));
    }

    changed() {
        if (this._source || this._destroyed)
            return;
        this._source = GLib.timeout_add(GLib.PRIORITY_DEFAULT, COALESCE_MS, () => {
            this._source = 0;
            try {
                this._publish();
            } catch (error) {
                console.error(`MX window inventory not published: ${error.message}`);
            }
            return GLib.SOURCE_REMOVE;
        });
    }

    _publish() {
        if (!this._link.connected)
            return;
        const snapshot = this._inventory.last() ?? this._inventory.refresh();
        const windows = fitWindows(describeWindows(snapshot, descriptor => this._icons.resolve(descriptor)));
        const flags = FLAG_DESKTOP_BRIDGE_READY | FLAG_WINDOW_INVENTORY_READY |
            (this._icons.ready ? FLAG_ICON_RESOLUTION_READY : 0);
        const generation = BigInt(snapshot.generation);
        const payload = encodeStatus({generation, flags, windows});
        if (this._sent && sameInventory(this._sent.payload, payload))
            return;
        if (!this._link.send(MESSAGE_WINDOWS, payload, true))
            return;
        this._sent = {generation, ids: new Set(windows.map(window => window.id)), payload};
    }

    _action(payload) {
        let request;
        try {
            request = decodeAction(payload);
        } catch (error) {
            console.error(`MX window action ignored: ${error.message}`);
            return;
        }
        if (!this._sent || request.generation > this._sent.generation || !this._sent.ids.has(request.windowId)) {
            console.error('MX window action ignored: the window is not in the published inventory');
            return;
        }
        try {
            this._inventory.act(request.action, String(request.windowId));
        } catch (error) {
            console.error(`MX window action failed: ${error.message}`);
        }
    }

    destroy() {
        this._destroyed = true;
        if (this._source)
            GLib.source_remove(this._source);
        this._source = 0;
        this._sent = null;
    }
}

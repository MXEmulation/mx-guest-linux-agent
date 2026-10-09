// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import St from 'gi://St';
import {EchoFilter} from './clipboard-protocol.js';
import {MESSAGE_CLIPBOARD} from './session-protocol.js';

const COALESCE_MS = 50;

export class ClipboardBridge {
    constructor(link) {
        this._link = link;
        this._filter = new EchoFilter();
        this._destroyed = false;
        this._readSource = 0;
        this._selection = global.display.get_selection();
        this._ownerSignal = this._selection.connect('owner-changed', (_selection, type) => {
            if (type === Meta.SelectionType.SELECTION_CLIPBOARD)
                this._scheduleRead();
        });
        link.onConnected(() => {
            this._filter.reset();
            this._scheduleRead();
        });
        link.onMessage(MESSAGE_CLIPBOARD, message => this._apply(message.text));
    }

    _apply(text) {
        this._filter.noteApplied(text);
        St.Clipboard.get_default().set_text(St.ClipboardType.CLIPBOARD, text);
    }

    _scheduleRead() {
        if (this._readSource || this._destroyed)
            return;
        this._readSource = GLib.timeout_add(GLib.PRIORITY_DEFAULT, COALESCE_MS, () => {
            this._readSource = 0;
            this._readClipboard();
            return GLib.SOURCE_REMOVE;
        });
    }

    _readClipboard() {
        if (!this._link.connected)
            return;
        St.Clipboard.get_default().get_text(St.ClipboardType.CLIPBOARD, (_clipboard, text) => {
            if (this._destroyed || !this._link.connected || !this._filter.shouldSend(text))
                return;
            const payload = new TextEncoder().encode(text);
            if (!this._link.send(MESSAGE_CLIPBOARD, payload))
                return;
            this._filter.noteSent(text);
        });
    }

    destroy() {
        if (this._destroyed)
            return;
        this._destroyed = true;
        if (this._ownerSignal)
            this._selection.disconnect(this._ownerSignal);
        this._ownerSignal = 0;
        if (this._readSource)
            GLib.source_remove(this._readSource);
        this._readSource = 0;
    }
}

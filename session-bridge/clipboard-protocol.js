// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
export class EchoFilter {
    constructor() {
        this.reset();
    }

    reset() {
        this._sent = null;
        this._applied = null;
    }

    shouldSend(text) {
        return text !== null && text !== undefined && text !== this._sent && text !== this._applied;
    }

    noteSent(text) {
        this._sent = text;
        this._applied = null;
    }

    noteApplied(text) {
        this._applied = text;
        this._sent = null;
    }
}

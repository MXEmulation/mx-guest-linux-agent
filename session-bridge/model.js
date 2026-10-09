// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke

export class Inventory {
    constructor(sessionId, backend, changed = () => {}) {
        this.sessionId = sessionId;
        this.backend = backend;
        this.changed = changed;
        this._ids = new WeakMap();
        this._nextId = 1;
        this._generation = 0;
        this._serialized = null;
        this._handles = new Map();
        this._last = null;
    }

    last() {
        return this._last;
    }

    _id(handle) {
        if (!this._ids.has(handle)) {
            if (!Number.isSafeInteger(this._nextId))
                throw new Error('Window identity space exhausted');
            this._ids.set(handle, String(this._nextId++));
        }
        return this._ids.get(handle);
    }

    refresh() {
        const observed = this.backend.scan();
        const handles = new Map();
        const windows = observed.windows.map(({handle, ...description}) => {
            const id = this._id(handle);
            if (handles.has(id))
                throw new Error('Duplicate window in session inventory');
            handles.set(id, handle);
            return {id, ...description};
        });
        const body = {...observed, windows};
        const serialized = JSON.stringify(body);
        this._handles = handles;
        let advanced = false;
        if (serialized !== this._serialized) {
            if (!Number.isSafeInteger(this._generation + 1))
                throw new Error('Inventory generation space exhausted');
            this._serialized = serialized;
            this._generation++;
            advanced = true;
        }
        this._last = {session_id: this.sessionId, generation: String(this._generation), ...body};
        if (advanced)
            this.changed(this.sessionId, String(this._generation));
        return this._last;
    }

    _perform(action, id) {
        const handle = this._handles.get(id);
        if (!handle)
            throw new Error('Window no longer exists');
        if (action === 'activate')
            this.backend.activate(handle);
        else if (action === 'close')
            this.backend.close(handle);
        else
            throw new Error('Unsupported window action');
    }

    control(action, sessionId, generation, id) {
        const current = this.refresh();
        if (sessionId !== this.sessionId || generation !== current.generation)
            throw new Error('Stale session or window inventory generation');
        this._perform(action, id);
        return {accepted: true, session_id: this.sessionId, generation: current.generation, window_id: id};
    }

    // Acts on a window of this session by identity alone, for callers that validated their own generation.
    act(action, id) {
        const current = this.refresh();
        this._perform(action, id);
        return {accepted: true, session_id: this.sessionId, generation: current.generation, window_id: id};
    }
}

export class Applications {
    constructor(sessionId, backend, changed = () => {}) {
        this.sessionId = sessionId;
        this.backend = backend;
        this.changed = changed;
        this._generation = 0;
        this._serialized = null;
        this._handles = new Map();
    }

    refresh() {
        const observed = this.backend.catalogue();
        const handles = new Map();
        const applications = observed.map(({handle, ...description}) => {
            if (!description.id || handles.has(description.id))
                throw new Error('Invalid or duplicate installed desktop ID');
            handles.set(description.id, handle);
            return description;
        }).sort((a, b) => a.id < b.id ? -1 : a.id > b.id ? 1 : 0);
        const serialized = JSON.stringify(applications);
        this._handles = handles;
        if (serialized !== this._serialized) {
            if (!Number.isSafeInteger(this._generation + 1))
                throw new Error('Application generation space exhausted');
            this._serialized = serialized;
            this._generation++;
            this.changed(this.sessionId, String(this._generation));
        }
        return {session_id: this.sessionId, generation: String(this._generation), applications};
    }

    launch(sessionId, generation, id) {
        const current = this.refresh();
        if (sessionId !== this.sessionId || generation !== current.generation)
            throw new Error('Stale session or application catalogue generation');
        const app = this._handles.get(id);
        const description = current.applications.find(item => item.id === id);
        if (!app || !description.can_launch)
            throw new Error('Installed application is unavailable for launch');
        this.backend.launch(app);
        return {accepted: true, session_id: this.sessionId, generation: current.generation, desktop_id: id};
    }
}

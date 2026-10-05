// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import {Inventory, Applications} from '../model.js';
import {API} from '../api.js';

function assert(value, message) {
    if (!value)
        throw new Error(message);
}

function rejects(callback, message) {
    try {
        callback();
    } catch (_error) {
        return;
    }
    throw new Error(message);
}

const first = {}, second = {}, replacement = {};
const backend = {
    observed: [{handle: first, title: 'Native Wayland', client_type: 'wayland',
        frame: {x: -40, y: 0, width: 100, height: 80}},
    {handle: second, title: 'Native X11', client_type: 'x11',
        frame: {x: 200, y: 20, width: 120, height: 90}}],
    apps: [{handle: {}, id: 'one.desktop', name: 'One', can_launch: true}],
    actions: [],
    scan() { return {coordinate_space: 'mutter-stage', monitors: [], windows: this.observed}; },
    activate(window) { this.actions.push(['activate', window]); },
    close(window) { this.actions.push(['close', window]); },
    catalogue() { return this.apps; },
    launch(app) { this.actions.push(['launch', app]); },
};
const signals = [];
const inventory = new Inventory('session-a', backend, (...args) => signals.push(args));
const applications = new Applications('session-a', backend);
const api = new API(inventory, applications);
const original = JSON.parse(api.ListWindows());
assert(original.generation === '1' && original.windows.length === 2, 'Initial truthful inventory');
assert(original.windows[0].frame.x === -40, 'No invented scanout geometry');
assert(!('handle' in original.windows[0]), 'Private native handle leaked');
assert(JSON.parse(api.ListWindows()).generation === original.generation, 'Stable generation');
assert(signals.length === 1, 'Unchanged inventory emitted a change');
api.Activate('session-a', '1', original.windows[0].id);
api.Close('session-a', '1', original.windows[1].id);
assert(backend.actions[0][1] === first && backend.actions[1][1] === second, 'Wrong native target');
rejects(() => api.Close('other-session', '1', original.windows[0].id), 'Accepted stale session');
backend.observed[0].title = 'Changed title';
rejects(() => api.Activate('session-a', '1', original.windows[0].id), 'Accepted stale generation');
const changed = JSON.parse(api.ListWindows());
assert(changed.generation === '2' && changed.windows[0].id === original.windows[0].id, 'Identity changed with title');
backend.observed = [{handle: second, title: 'Native X11', client_type: 'x11'}];
const removed = JSON.parse(api.ListWindows());
rejects(() => api.Close('session-a', removed.generation, original.windows[0].id), 'Accepted removed native target');
backend.observed.push({handle: replacement, title: 'New window', client_type: 'wayland'});
const added = JSON.parse(api.ListWindows());
assert(added.windows[1].id !== original.windows[0].id, 'Reused retired window identity');
const catalogue = JSON.parse(api.ListApplications());
api.Launch('session-a', catalogue.generation, 'one.desktop');
assert(backend.actions.at(-1)[0] === 'launch', 'Launch was not dispatched');
backend.apps[0].name = 'Changed app name';
rejects(() => api.Launch('session-a', catalogue.generation, 'one.desktop'), 'Accepted stale app catalogue');
const current = JSON.parse(api.ListApplications());
rejects(() => api.Launch('session-a', current.generation, 'arbitrary command'), 'Accepted an uninstalled desktop ID');
backend.apps[0].can_launch = false;
const unavailable = JSON.parse(api.ListApplications());
rejects(() => api.Launch('session-a', unavailable.generation, 'one.desktop'), 'Accepted unlaunchable app');
const restarted = new Inventory('session-b', backend);
rejects(() => restarted.control('close', 'session-a', '1', original.windows[1].id), 'Accepted old shell session');
print('PASS native identity, mutation generations, Wayland/X11 geometry, removed targets, stale sessions, installed-ID launch and unlaunchable apps');

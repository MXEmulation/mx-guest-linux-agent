// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import GLib from 'gi://GLib';
import {BRIDGE_FLAGS, FLAG_DESKTOP_BRIDGE_READY, FLAG_ICON_RESOLUTION_READY, FLAG_WINDOW_INVENTORY_READY,
    MAX_TEXT_BYTES, MAX_WINDOWS, SEGMENT_BYTES, STATUS_HEADER_BYTES, WINDOW_BYTES, WINDOW_GEOMETRY_RELIABLE,
    describeWindows, encodeStatus, fitWindows, sameInventory, toPremultipliedBgra} from '../integration-protocol.js';
import {IntegrationBridge} from '../integration.js';
import {MAX_PAYLOAD_BYTES, MESSAGE_ACTION, MESSAGE_WINDOWS, decodeAction, encodeAction} from '../session-protocol.js';

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

function settle(milliseconds = 300) {
    const loop = new GLib.MainLoop(null, false);
    GLib.timeout_add(GLib.PRIORITY_DEFAULT, milliseconds, () => {
        loop.quit();
        return GLib.SOURCE_REMOVE;
    });
    loop.run();
}

function segment(scanout, x, y, width, height, destinationX, destinationY, destinationWidth, destinationHeight) {
    return {scanout, x, y, width, height, destinationX, destinationY, destinationWidth, destinationHeight};
}

function simple(id, overrides = {}) {
    return {id: BigInt(id), x: 10, y: 20, width: 300, height: 200, flags: WINDOW_GEOMETRY_RELIABLE, title: 'Term',
        applicationId: 'app.id', icon: null, scanout: 0, outputWidth: 300, outputHeight: 200,
        segments: [segment(0, 10, 20, 300, 200, 0, 0, 300, 200)], ...overrides};
}

const icon = {width: 2, height: 2, bgra: Uint8Array.from({length: 16}, (_value, index) => index + 1)};

const encoded = encodeStatus({generation: 7n, flags: BRIDGE_FLAGS, windows: [simple(5, {icon, title: 'é'})]});
const view = new DataView(encoded.buffer);
assert(encoded.length === STATUS_HEADER_BYTES + WINDOW_BYTES + 2 + 6 + 16 + SEGMENT_BYTES, 'Wrong payload size');
assert(view.getUint16(0, true) === 4 && view.getUint16(2, true) === 28 && view.getBigUint64(4, true) === 7n &&
    view.getUint32(12, true) === 1, 'Wrong header');
assert(view.getBigUint64(16, true) === 5n && view.getUint32(24, true) === 10 && view.getUint32(28, true) === 20 &&
    view.getUint32(32, true) === 300 && view.getUint32(36, true) === 200 && view.getUint32(40, true) === 1,
'Wrong window geometry');
assert(view.getUint16(44, true) === 2 && view.getUint16(46, true) === 6 && view.getUint16(48, true) === 2 &&
    view.getUint16(50, true) === 2 && view.getUint32(52, true) === 16, 'Wrong text and icon lengths');
assert(view.getUint16(56, true) === 0 && view.getUint16(58, true) === 1 && view.getUint32(60, true) === 300 &&
    view.getUint32(64, true) === 200, 'Wrong scanout, segment count or output size');
const text = STATUS_HEADER_BYTES + WINDOW_BYTES;
assert(new TextDecoder().decode(encoded.subarray(text, text + 2)) === 'é', 'Wrong title');
assert(new TextDecoder().decode(encoded.subarray(text + 2, text + 8)) === 'app.id', 'Wrong application id');
assert(encoded[text + 8] === 1 && encoded[text + 23] === 16, 'Wrong icon bytes');
const seg = text + 24;
assert(view.getUint16(seg, true) === 0 && view.getUint16(seg + 2, true) === 0 && view.getUint32(seg + 4, true) === 10 &&
    view.getUint32(seg + 8, true) === 20 && view.getUint32(seg + 12, true) === 300 &&
    view.getUint32(seg + 16, true) === 200 && view.getUint32(seg + 20, true) === 0 &&
    view.getUint32(seg + 28, true) === 300 && view.getUint32(seg + 32, true) === 200, 'Wrong segment');
assert(encodeStatus({generation: 1n, flags: 0, windows: []}).length === STATUS_HEADER_BYTES, 'Empty inventory size');

const status = windows => ({generation: 1n, flags: BRIDGE_FLAGS, windows});
rejects(() => encodeStatus({generation: 0n, flags: 0, windows: []}), 'Accepted generation zero');
rejects(() => encodeStatus({generation: 1, flags: 0, windows: []}), 'Accepted a numeric generation');
rejects(() => encodeStatus({generation: 1n, flags: 3, windows: []}), 'Accepted a flag owned by the daemon');
rejects(() => encodeStatus({generation: 1n, flags: 1 << 5, windows: []}), 'Accepted an unknown flag');
rejects(() => encodeStatus(status([simple(0)])), 'Accepted window zero');
rejects(() => encodeStatus(status([simple(1, {x: -1, segments: [segment(0, -1, 20, 300, 200, 0, 0, 300, 200)]})])),
    'Accepted a negative origin');
rejects(() => encodeStatus(status([simple(1, {width: 0})])), 'Accepted an empty window');
rejects(() => encodeStatus(status([simple(1, {flags: 2})])), 'Accepted an unknown window flag');
rejects(() => encodeStatus(status([simple(1, {icon: {width: 3, height: 2, bgra: new Uint8Array(16)}})])),
    'Accepted an icon with the wrong byte length');
rejects(() => encodeStatus(status([simple(1, {icon: {width: 257, height: 1, bgra: new Uint8Array(257 * 4)}})])),
    'Accepted an oversized icon');
rejects(() => encodeStatus(status([simple(1, {segments: []})])), 'Accepted no segments');
rejects(() => encodeStatus(status([simple(1, {segments: [segment(1, 10, 20, 300, 200, 0, 0, 300, 200)]})])),
    'Accepted a primary segment on another scanout');
rejects(() => encodeStatus(status([simple(1, {segments: [segment(0, 10, 20, 300, 200, 0, 0, 300, 100)]})])),
    'Accepted segments that do not cover the output');
rejects(() => encodeStatus(status([simple(1, {outputWidth: 400})])), 'Accepted an output larger than its segments');
rejects(() => encodeStatus(status([simple(1, {outputWidth: 200})])), 'Accepted a destination outside the output');
rejects(() => encodeStatus(status([simple(1, {outputWidth: 600, segments: [
    segment(0, 10, 20, 300, 200, 0, 0, 300, 200), segment(0, 0, 0, 300, 200, 300, 0, 300, 200)]})])),
'Accepted a repeated scanout');
rejects(() => encodeStatus(status([simple(1, {outputWidth: 300, segments: [
    segment(0, 10, 20, 300, 200, 0, 0, 300, 200), segment(1, 0, 0, 300, 200, 0, 0, 300, 200)]})])),
'Accepted overlapping destinations');
rejects(() => encodeStatus(status(Array.from({length: MAX_WINDOWS + 1}, (_value, index) => simple(index + 1)))),
    'Accepted too many windows');
const manySegments = Array.from({length: 33}, (_value, index) => segment(index, 10, 20, 1, 1, index, 0, 1, 1));
rejects(() => encodeStatus(status([simple(1, {width: 1, height: 1, outputWidth: 33, outputHeight: 1,
    segments: manySegments})])), 'Accepted too many segments');
assert(encodeStatus(status(Array.from({length: MAX_WINDOWS}, (_value, index) => simple(index + 1)))).length > 0,
    'Refused the maximum window count');

const long = encodeStatus(status([simple(1, {title: 'é'.repeat(3000) + 'x', applicationId: 'a'.repeat(5000)})]));
const longView = new DataView(long.buffer);
assert(longView.getUint16(STATUS_HEADER_BYTES + 28, true) === MAX_TEXT_BYTES, 'Title not cut to the limit');
assert(longView.getUint16(STATUS_HEADER_BYTES + 30, true) === MAX_TEXT_BYTES, 'Application id not cut to the limit');
const odd = encodeStatus(status([simple(1, {title: 'a' + 'é'.repeat(3000)})]));
const oddLength = new DataView(odd.buffer).getUint16(STATUS_HEADER_BYTES + 28, true);
assert(oddLength === 4095 && !new TextDecoder('utf-8', {fatal: true}).decode(
    odd.subarray(STATUS_HEADER_BYTES + WINDOW_BYTES, STATUS_HEADER_BYTES + WINDOW_BYTES + oddLength)).includes('�'),
'Title cut inside a character');

const first = encodeStatus({generation: 1n, flags: BRIDGE_FLAGS, windows: [simple(1)]});
const second = encodeStatus({generation: 2n, flags: BRIDGE_FLAGS, windows: [simple(1)]});
const third = encodeStatus({generation: 2n, flags: BRIDGE_FLAGS, windows: [simple(1, {title: 'Other'})]});
assert(sameInventory(first, second) && !sameInventory(first, third) && !sameInventory(first, first.subarray(1)),
    'Wrong inventory comparison');

const big = {width: 64, height: 64, bgra: new Uint8Array(64 * 64 * 4)};
const crowded = Array.from({length: 200}, (_value, index) => simple(index + 1, {icon: big}));
const fitted = fitWindows(crowded);
assert(fitted.length === MAX_WINDOWS && fitted[0].id === 1n && fitted[127].id === 128n, 'Wrong window selection');
const kept = fitted.filter(window => window.icon).length;
assert(kept > 0 && kept < MAX_WINDOWS && fitted.slice(0, kept).every(window => window.icon) &&
    fitted.slice(kept).every(window => !window.icon), 'Icons not dropped in order');
assert(encodeStatus(status(fitted)).length <= MAX_PAYLOAD_BYTES, 'Fitted inventory exceeds the frame');

const rgba = toPremultipliedBgra(Uint8Array.of(200, 100, 50, 128, 1, 2, 3, 255, 9, 9), 2, 1, 10, 4);
assert(rgba.join() === '25,50,100,128,3,2,1,255', `Wrong RGBA conversion ${rgba}`);
const rgb = toPremultipliedBgra(Uint8Array.of(10, 20, 30, 99, 40, 50, 60), 1, 2, 4, 3);
assert(rgb.join() === '30,20,10,255,60,50,40,255', `Wrong RGB conversion ${rgb}`);
assert(toPremultipliedBgra(new Uint8Array(4), 2, 1, 8, 4) === null, 'Converted truncated pixels');
assert(toPremultipliedBgra(new Uint8Array(4), 1, 1, 4, 2) === null, 'Converted a two-channel image');
assert(toPremultipliedBgra(new Uint8Array(4), 0, 1, 4, 4) === null, 'Converted an empty image');
assert(toPremultipliedBgra(new Uint8Array(4 * 257), 257, 1, 4 * 257, 4) === null, 'Converted an oversized image');

function monitor(index, x, scale, mxgpu) {
    return {index, geometry: {x, y: 0, width: 1920, height: 1080}, scale, primary: index === 0,
        outputs: [{connector: `Virtual-${index}`, active: true, device: {recognized_mxgpu: mxgpu}}]};
}

function native(id, frame, overrides = {}) {
    return {id: String(id), title: `Window ${id}`, app_id: `app${id}.desktop`, icon: {serialized: `icon${id}`}, frame,
        mapped: true, minimized: false, hidden: false, showing_on_workspace: true, all_workspaces: false,
        workspace: 0, ...overrides};
}

function snapshot(windows, monitors = [monitor(0, 0, 1, true), monitor(1, 1920, 1, true), monitor(2, 3840, 1, false)],
    generation = 1) {
    return {session_id: 's', generation: String(generation), coordinate_space: 'mutter-stage', active_workspace: 0,
        monitors, windows};
}

const requested = [];
const described = describeWindows(snapshot([
    native(10, {x: 5, y: 5, width: 10, height: 10}, {minimized: true}),
    native(9, {x: 3900, y: 100, width: 300, height: 300}),
    native(8, {x: 0, y: 0, width: 100, height: 100}, {workspace: 1}),
    native(7, {x: 0, y: 0, width: 100, height: 100}, {mapped: false}),
    native(6, {x: 1500, y: 900, width: 800, height: 400}),
    native(5, {x: 10, y: 10, width: 100, height: 100}, {icon: null, workspace: 3, all_workspaces: true}),
    native(3, {x: -100, y: 50, width: 400, height: 300}),
    native(2, {x: 1500, y: 100, width: 800, height: 400}),
    native(1, {x: 100, y: 50, width: 640, height: 480}),
]), descriptor => {
    requested.push(descriptor.serialized);
    return icon;
});
assert(described.map(window => window.id).join() === '1,2,3,5,6', `Wrong window set ${described.map(w => w.id)}`);
assert(requested.join() === 'icon6,icon3,icon2,icon1', `Wrong icon requests ${requested}`);
const [inside, spanning, clipped, everywhere, partial] = described;
assert(inside.x === 100 && inside.y === 50 && inside.width === 640 && inside.height === 480 && inside.scanout === 0 &&
    inside.outputWidth === 640 && inside.outputHeight === 480 && inside.segments.length === 1 &&
    inside.flags === WINDOW_GEOMETRY_RELIABLE && inside.title === 'Window 1' && inside.applicationId === 'app1.desktop' &&
    inside.icon === icon, 'Wrong window inside one monitor');
assert(spanning.scanout === 0 && spanning.x === 1500 && spanning.width === 420 && spanning.height === 400 &&
    spanning.outputWidth === 800 && spanning.outputHeight === 400 && spanning.flags === WINDOW_GEOMETRY_RELIABLE,
'Wrong spanning window');
assert(JSON.stringify(spanning.segments) === JSON.stringify([segment(0, 1500, 100, 420, 400, 0, 0, 420, 400),
    segment(1, 0, 100, 380, 400, 420, 0, 380, 400)]), `Wrong spanning segments ${JSON.stringify(spanning.segments)}`);
assert(clipped.x === 0 && clipped.y === 50 && clipped.width === 300 && clipped.height === 300 &&
    clipped.outputWidth === 300 && clipped.flags === 0 && clipped.segments.length === 1, 'Wrong clipped window');
assert(everywhere.icon === null && everywhere.id === 5n, 'Wrong window on all workspaces');
assert(partial.x === 1500 && partial.y === 900 && partial.width === 420 && partial.height === 180 &&
    partial.outputWidth === 420 && partial.outputHeight === 180 && partial.flags === 0 &&
    partial.segments.length === 1, 'Wrong partially covered window');
assert(encodeStatus(status(described)).length > 0, 'Described windows are not encodable');

const scaled = describeWindows(snapshot([native(1, {x: 10, y: 20, width: 100, height: 50})],
    [monitor(0, 0, 2, true)]));
assert(scaled[0].x === 20 && scaled[0].y === 40 && scaled[0].width === 200 && scaled[0].height === 100 &&
    scaled[0].outputWidth === 200 && scaled[0].outputHeight === 100, 'Monitor scale not applied');
const ordinal = describeWindows(snapshot([native(1, {x: 1930, y: 20, width: 100, height: 50})],
    [monitor(0, 0, 1, false), monitor(1, 1920, 1, true)]));
assert(ordinal[0].scanout === 0 && ordinal[0].x === 10, 'Scanout is not the MXGPU monitor ordinal');
assert(describeWindows(snapshot([native(1, {x: 0, y: 0, width: 10, height: 10})], [monitor(0, 0, 1, false)])).length === 0,
    'Windows reported without an MXGPU monitor');
assert(FLAG_DESKTOP_BRIDGE_READY === 4 && FLAG_WINDOW_INVENTORY_READY === 8 && FLAG_ICON_RESOLUTION_READY === 16,
    'Bridge flag values changed');

const decoded = decodeAction(encodeAction('activate', 9n, 3n));
assert(decoded.action === 'activate' && decoded.generation === 9n && decoded.windowId === 3n, 'Wrong action');
assert(decodeAction(encodeAction('close', 1n, 2n)).action === 'close', 'Wrong close action');
rejects(() => decodeAction(encodeAction('close', 0n, 2n)), 'Accepted generation zero');
rejects(() => decodeAction(encodeAction('close', 1n, 0n)), 'Accepted window zero');
rejects(() => decodeAction(new Uint8Array(19)), 'Accepted a short action');

function fakeLink() {
    const handlers = {connected: [], disconnected: [], messages: new Map()};
    return {
        connected: true, sent: [], handlers,
        onConnected(handler) { handlers.connected.push(handler); },
        onDisconnected(handler) { handlers.disconnected.push(handler); },
        onMessage(type, handler) { handlers.messages.set(type, handler); },
        send(type, payload, replace) { this.sent.push({type, payload, replace}); return true; },
    };
}

const link = fakeLink();
let current = snapshot([native(1, {x: 100, y: 50, width: 640, height: 480})], undefined, 1);
const acts = [];
const inventory = {last: () => current, refresh: () => current, act: (action, id) => acts.push([action, id])};
const bridge = new IntegrationBridge(link, inventory, {ready: true, resolve: () => icon});
link.handlers.connected[0]();
settle();
assert(link.sent.length === 1 && link.sent[0].type === MESSAGE_WINDOWS && link.sent[0].replace === true,
    'Inventory not published on connect');
const published = new DataView(link.sent[0].payload.buffer);
assert(published.getUint16(2, true) === BRIDGE_FLAGS && published.getBigUint64(4, true) === 1n &&
    published.getUint32(12, true) === 1, 'Wrong published header');

current = snapshot(current.windows, undefined, 2);
bridge.changed();
settle();
assert(link.sent.length === 1, 'Unchanged content republished');

current = snapshot([...current.windows, native(2, {x: 0, y: 0, width: 50, height: 50})], undefined, 3);
bridge.changed();
bridge.changed();
settle();
assert(link.sent.length === 2 && new DataView(link.sent[1].payload.buffer).getBigUint64(4, true) === 3n,
    'Changed inventory not published once');

const handler = link.handlers.messages.get(MESSAGE_ACTION);
handler({type: MESSAGE_ACTION, payload: encodeAction('activate', 3n, 2n)});
handler({type: MESSAGE_ACTION, payload: encodeAction('close', 1n, 1n)});
assert(acts.join('|') === 'activate,2|close,1', `Wrong forwarded actions ${acts.join('|')}`);
handler({type: MESSAGE_ACTION, payload: encodeAction('close', 4n, 1n)});
handler({type: MESSAGE_ACTION, payload: encodeAction('close', 3n, 9n)});
handler({type: MESSAGE_ACTION, payload: new Uint8Array(20)});
assert(acts.length === 2, 'Forwarded an action for a future generation, unknown window or malformed payload');

link.handlers.disconnected[0]();
handler({type: MESSAGE_ACTION, payload: encodeAction('close', 3n, 1n)});
assert(acts.length === 2, 'Forwarded an action with no published inventory');
link.handlers.connected[0]();
settle();
assert(link.sent.length === 3, 'Inventory not republished after reconnect');

const notReady = fakeLink();
const quiet = new IntegrationBridge(notReady, inventory, {ready: false, resolve: () => null});
notReady.handlers.connected[0]();
settle();
assert(new DataView(notReady.sent[0].payload.buffer).getUint16(2, true) === (FLAG_DESKTOP_BRIDGE_READY | FLAG_WINDOW_INVENTORY_READY),
    'Icon flag set without a working resolver');
quiet.destroy();
bridge.destroy();
print('PASS status encoding and validation, text and frame limits, icon conversion, MXGPU window placement, action decoding and bridge publication');

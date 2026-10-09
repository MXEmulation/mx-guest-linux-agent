// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import {MAX_PAYLOAD_BYTES} from './session-protocol.js';

export const STATUS_VERSION = 4;
export const STATUS_HEADER_BYTES = 16;
export const WINDOW_BYTES = 52;
export const SEGMENT_BYTES = 36;
export const MAX_WINDOWS = 128;
export const MAX_SEGMENTS = 32;
export const MAX_TEXT_BYTES = 4096;
export const MAX_ICON_DIMENSION = 256;
export const FLAG_DESKTOP_BRIDGE_READY = 1 << 2;
export const FLAG_WINDOW_INVENTORY_READY = 1 << 3;
export const FLAG_ICON_RESOLUTION_READY = 1 << 4;
export const BRIDGE_FLAGS = FLAG_DESKTOP_BRIDGE_READY | FLAG_WINDOW_INVENTORY_READY | FLAG_ICON_RESOLUTION_READY;
export const WINDOW_GEOMETRY_RELIABLE = 1;

const U16_MAX = 0xffff;
const U32_MAX = 0xffffffff;
const I32_MAX = 0x7fffffff;
const U64_MAX = (1n << 64n) - 1n;
const encoder = new TextEncoder();

function inRange(value, minimum, maximum) {
    return Number.isInteger(value) && value >= minimum && value <= maximum;
}

function utf8Limited(text) {
    const bytes = encoder.encode(text ?? '');
    if (bytes.length <= MAX_TEXT_BYTES)
        return bytes;
    let end = MAX_TEXT_BYTES;
    while (end > 0 && (bytes[end] & 0xc0) === 0x80)
        end--;
    return bytes.slice(0, end);
}

function checkSource(x, y, width, height) {
    if (!inRange(x, 0, I32_MAX) || !inRange(y, 0, I32_MAX) || !inRange(width, 1, U32_MAX) ||
        !inRange(height, 1, U32_MAX) || x + width > U32_MAX || y + height > U32_MAX)
        throw new RangeError('Invalid source rectangle');
}

function checkSegments(window) {
    const {segments} = window;
    if (!inRange(window.outputWidth, 1, U32_MAX) || !inRange(window.outputHeight, 1, U32_MAX) ||
        !Array.isArray(segments) || segments.length < 1 || segments.length > MAX_SEGMENTS)
        throw new RangeError('Invalid segment set');
    const area = BigInt(window.outputWidth) * BigInt(window.outputHeight);
    let covered = 0n;
    segments.forEach((segment, index) => {
        checkSource(segment.x, segment.y, segment.width, segment.height);
        if (!inRange(segment.scanout, 0, U16_MAX) || !inRange(segment.destinationX, 0, U32_MAX) ||
            !inRange(segment.destinationY, 0, U32_MAX) || !inRange(segment.destinationWidth, 1, U32_MAX) ||
            !inRange(segment.destinationHeight, 1, U32_MAX) ||
            segment.destinationX + segment.destinationWidth > window.outputWidth ||
            segment.destinationY + segment.destinationHeight > window.outputHeight)
            throw new RangeError('Invalid segment destination');
        if (index === 0 && (segment.scanout !== window.scanout || segment.x !== window.x ||
            segment.y !== window.y || segment.width !== window.width || segment.height !== window.height))
            throw new RangeError('Primary segment does not match the window');
        for (const earlier of segments.slice(0, index)) {
            if (earlier.scanout === segment.scanout ||
                (earlier.destinationX < segment.destinationX + segment.destinationWidth &&
                segment.destinationX < earlier.destinationX + earlier.destinationWidth &&
                earlier.destinationY < segment.destinationY + segment.destinationHeight &&
                segment.destinationY < earlier.destinationY + earlier.destinationHeight))
                throw new RangeError('Segments overlap or repeat a scanout');
        }
        covered += BigInt(segment.destinationWidth) * BigInt(segment.destinationHeight);
    });
    if (covered !== area)
        throw new RangeError('Segments do not exactly cover the output');
}

function prepare(window) {
    if (typeof window.id !== 'bigint' || window.id <= 0n || window.id > U64_MAX)
        throw new RangeError('Invalid window id');
    checkSource(window.x, window.y, window.width, window.height);
    if (!inRange(window.scanout, 0, U16_MAX) || (window.flags & ~WINDOW_GEOMETRY_RELIABLE) !== 0)
        throw new RangeError('Invalid window scanout or flags');
    const icon = window.icon ?? null;
    if (icon && (!inRange(icon.width, 1, MAX_ICON_DIMENSION) || !inRange(icon.height, 1, MAX_ICON_DIMENSION) ||
        icon.bgra.length !== icon.width * icon.height * 4))
        throw new RangeError('Invalid icon');
    checkSegments(window);
    return {window, title: utf8Limited(window.title), applicationId: utf8Limited(window.applicationId), icon};
}

function recordBytes(record) {
    return WINDOW_BYTES + record.title.length + record.applicationId.length + (record.icon?.bgra.length ?? 0) +
        SEGMENT_BYTES * record.window.segments.length;
}

// Encodes an MXGA integration status payload; throws RangeError for anything the host would reject.
export function encodeStatus({generation, flags, windows}) {
    if (typeof generation !== 'bigint' || generation <= 0n || generation > U64_MAX)
        throw new RangeError('Invalid generation');
    if (!inRange(flags, 0, U16_MAX) || (flags & ~BRIDGE_FLAGS) !== 0)
        throw new RangeError('Invalid bridge flags');
    if (windows.length > MAX_WINDOWS)
        throw new RangeError('Too many windows');
    const records = windows.map(prepare);
    const size = records.reduce((sum, record) => sum + recordBytes(record), STATUS_HEADER_BYTES);
    if (size > MAX_PAYLOAD_BYTES)
        throw new RangeError('Inventory exceeds the frame limit');
    const out = new Uint8Array(size);
    const view = new DataView(out.buffer);
    view.setUint16(0, STATUS_VERSION, true);
    view.setUint16(2, flags, true);
    view.setBigUint64(4, generation, true);
    view.setUint32(12, records.length, true);
    let at = STATUS_HEADER_BYTES;
    for (const {window, title, applicationId, icon} of records) {
        view.setBigUint64(at, window.id, true);
        view.setUint32(at + 8, window.x, true);
        view.setUint32(at + 12, window.y, true);
        view.setUint32(at + 16, window.width, true);
        view.setUint32(at + 20, window.height, true);
        view.setUint32(at + 24, window.flags, true);
        view.setUint16(at + 28, title.length, true);
        view.setUint16(at + 30, applicationId.length, true);
        view.setUint16(at + 32, icon?.width ?? 0, true);
        view.setUint16(at + 34, icon?.height ?? 0, true);
        view.setUint32(at + 36, icon?.bgra.length ?? 0, true);
        view.setUint16(at + 40, window.scanout, true);
        view.setUint16(at + 42, window.segments.length, true);
        view.setUint32(at + 44, window.outputWidth, true);
        view.setUint32(at + 48, window.outputHeight, true);
        at += WINDOW_BYTES;
        out.set(title, at);
        at += title.length;
        out.set(applicationId, at);
        at += applicationId.length;
        if (icon) {
            out.set(icon.bgra, at);
            at += icon.bgra.length;
        }
        for (const segment of window.segments) {
            view.setUint16(at, segment.scanout, true);
            view.setUint32(at + 4, segment.x, true);
            view.setUint32(at + 8, segment.y, true);
            view.setUint32(at + 12, segment.width, true);
            view.setUint32(at + 16, segment.height, true);
            view.setUint32(at + 20, segment.destinationX, true);
            view.setUint32(at + 24, segment.destinationY, true);
            view.setUint32(at + 28, segment.destinationWidth, true);
            view.setUint32(at + 32, segment.destinationHeight, true);
            at += SEGMENT_BYTES;
        }
    }
    return out;
}

// Converts straight RGB or RGBA rows to the premultiplied BGRA of the status icon; null when the input is unusable.
export function toPremultipliedBgra(pixels, width, height, stride, channels) {
    if (!inRange(width, 1, MAX_ICON_DIMENSION) || !inRange(height, 1, MAX_ICON_DIMENSION) ||
        (channels !== 3 && channels !== 4) || stride < width * channels ||
        pixels.length < stride * (height - 1) + width * channels)
        return null;
    const out = new Uint8Array(width * height * 4);
    for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
            const source = y * stride + x * channels;
            const target = (y * width + x) * 4;
            const alpha = channels === 4 ? pixels[source + 3] : 255;
            out[target] = Math.floor((pixels[source + 2] * alpha + 127) / 255);
            out[target + 1] = Math.floor((pixels[source + 1] * alpha + 127) / 255);
            out[target + 2] = Math.floor((pixels[source] * alpha + 127) / 255);
            out[target + 3] = alpha;
        }
    }
    return out;
}

export function sameInventory(a, b) {
    if (a.length !== b.length)
        return false;
    for (let index = 0; index < a.length; index++) {
        if ((index < 4 || index >= 12) && a[index] !== b[index])
            return false;
    }
    return true;
}

// Keeps what fits the frame limit: windows in order up to the bounds, then icons in order while they fit.
export function fitWindows(windows) {
    const budget = MAX_PAYLOAD_BYTES - STATUS_HEADER_BYTES;
    const kept = [];
    let used = 0;
    for (const window of windows) {
        const base = WINDOW_BYTES + utf8Limited(window.title).length + utf8Limited(window.applicationId).length +
            SEGMENT_BYTES * window.segments.length;
        if (kept.length === MAX_WINDOWS || used + base > budget)
            break;
        used += base;
        kept.push(window);
    }
    return kept.map(window => {
        const bytes = window.icon?.bgra.length ?? 0;
        if (!bytes)
            return window;
        if (used + bytes > budget)
            return {...window, icon: null};
        used += bytes;
        return window;
    });
}

function intersect(a, b) {
    const x = Math.max(a.x, b.x);
    const y = Math.max(a.y, b.y);
    const right = Math.min(a.x + a.width, b.x + b.width);
    const bottom = Math.min(a.y + a.height, b.y + b.height);
    return right > x && bottom > y ? {x, y, width: right - x, height: bottom - y} : null;
}

function overlaps(a, b) {
    return intersect(a, b) !== null;
}

function mxgpuMonitors(snapshot) {
    return snapshot.monitors
        .filter(monitor => monitor.outputs.some(output => output.active !== false && output.device?.recognized_mxgpu))
        .map((monitor, scanout) => ({scanout, geometry: monitor.geometry,
            scale: monitor.scale > 0 ? monitor.scale : 1}));
}

// Source rectangles are in the monitor's pixels; destination rectangles are relative to the composed window.
function compose(pieces, origin, scale) {
    const scaled = value => Math.round(value * scale);
    const segments = pieces.map(({monitor, rect}) => {
        const x = scaled(rect.x - monitor.geometry.x);
        const y = scaled(rect.y - monitor.geometry.y);
        const destinationX = scaled(rect.x - origin.x);
        const destinationY = scaled(rect.y - origin.y);
        return {scanout: monitor.scanout, x, y,
            width: scaled(rect.x + rect.width - monitor.geometry.x) - x,
            height: scaled(rect.y + rect.height - monitor.geometry.y) - y,
            destinationX, destinationY,
            destinationWidth: scaled(rect.x + rect.width - origin.x) - destinationX,
            destinationHeight: scaled(rect.y + rect.height - origin.y) - destinationY};
    });
    const valid = segments.every(segment => segment.width > 0 && segment.height > 0 &&
        segment.destinationWidth > 0 && segment.destinationHeight > 0);
    return valid ? {segments, outputWidth: scaled(origin.width), outputHeight: scaled(origin.height)} : null;
}

function place(frame, monitors) {
    const pieces = monitors.map(monitor => ({monitor, rect: intersect(frame, monitor.geometry)}))
        .filter(piece => piece.rect)
        .sort((a, b) => b.rect.width * b.rect.height - a.rect.width * a.rect.height ||
            a.monitor.scanout - b.monitor.scanout);
    if (!pieces.length)
        return null;
    const area = piece => piece.rect.width * piece.rect.height;
    const tiled = pieces.length > 1 && pieces.length <= MAX_SEGMENTS &&
        pieces.reduce((sum, piece) => sum + area(piece), 0) === frame.width * frame.height &&
        pieces.every(piece => piece.monitor.scale === pieces[0].monitor.scale) &&
        pieces.every((piece, index) => pieces.slice(index + 1).every(other => !overlaps(piece.rect, other.rect)));
    if (tiled) {
        const composed = compose(pieces, frame, pieces[0].monitor.scale);
        if (composed)
            return {...composed, reliable: true};
    }
    const [primary] = pieces;
    const composed = compose([primary], primary.rect, primary.monitor.scale);
    return composed && {...composed, reliable: area(primary) === frame.width * frame.height};
}

function visibleOnDesktop(snapshot, window) {
    return window.mapped && !window.minimized && !window.hidden && window.showing_on_workspace &&
        (window.all_workspaces || window.workspace === null || snapshot.active_workspace === undefined ||
        window.workspace === snapshot.active_workspace);
}

// Describes the application windows with visible pixels on MXGPU monitors, ordered by window id.
export function describeWindows(snapshot, iconFor = () => null) {
    const monitors = mxgpuMonitors(snapshot);
    const described = [];
    for (const window of snapshot.windows) {
        if (!visibleOnDesktop(snapshot, window) || !/^[1-9][0-9]*$/.test(window.id))
            continue;
        const placement = place(window.frame, monitors);
        if (!placement)
            continue;
        const [first] = placement.segments;
        described.push({id: BigInt(window.id), x: first.x, y: first.y, width: first.width, height: first.height,
            flags: placement.reliable ? WINDOW_GEOMETRY_RELIABLE : 0, title: window.title ?? '',
            applicationId: window.app_id ?? window.gtk_application_id ?? window.wm_class ?? '',
            icon: window.icon ? iconFor(window.icon) : null, scanout: first.scanout,
            outputWidth: placement.outputWidth, outputHeight: placement.outputHeight,
            segments: placement.segments});
    }
    return described.sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
}

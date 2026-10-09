// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
export const MESSAGE_CLIPBOARD = 1;
export const MESSAGE_WINDOWS = 2;
export const MESSAGE_ACTION = 3;
export const MAX_PAYLOAD_BYTES = 1048552;
export const MAX_CLIPBOARD_BYTES = 1048536;
export const ACTION_BYTES = 20;
export const ACTION_VERSION = 1;
export const ACTION_ACTIVATE = 1;
export const ACTION_CLOSE = 2;
export const BACKOFF_INITIAL_MS = 1000;
export const BACKOFF_MAX_MS = 30000;

const OUTGOING_LIMITS = new Map([[MESSAGE_CLIPBOARD, MAX_CLIPBOARD_BYTES], [MESSAGE_WINDOWS, MAX_PAYLOAD_BYTES]]);

function incomingValid(type, length) {
    if (type === MESSAGE_CLIPBOARD)
        return length <= MAX_CLIPBOARD_BYTES;
    return type === MESSAGE_ACTION && length === ACTION_BYTES;
}

// A message is a u32 little-endian length N, then N bytes: a u8 type and the payload.
export function encodeMessage(type, payload) {
    const limit = OUTGOING_LIMITS.get(type);
    if (limit === undefined || payload.length > limit)
        return null;
    const frame = new Uint8Array(5 + payload.length);
    new DataView(frame.buffer).setUint32(0, payload.length + 1, true);
    frame[4] = type;
    frame.set(payload, 5);
    return frame;
}

export class MessageDecoder {
    constructor() {
        this._buffer = new Uint8Array(0);
        this._decoder = new TextDecoder('utf-8', {fatal: true});
    }

    // Returns the messages completed by this chunk; clipboard messages also carry their text.
    // Throws on a zero or over-long length, a type the daemon may not send, a wrong action size or invalid UTF-8.
    push(chunk) {
        const joined = new Uint8Array(this._buffer.length + chunk.length);
        joined.set(this._buffer, 0);
        joined.set(chunk, this._buffer.length);
        const messages = [];
        let offset = 0;
        while (joined.length - offset >= 4) {
            const size = new DataView(joined.buffer, joined.byteOffset + offset, 4).getUint32(0, true);
            if (size === 0 || size > MAX_PAYLOAD_BYTES + 1)
                throw new RangeError(`Session message length ${size} is outside 1..${MAX_PAYLOAD_BYTES + 1}`);
            if (joined.length - offset - 4 < size)
                break;
            const type = joined[offset + 4];
            const payload = joined.subarray(offset + 5, offset + 4 + size);
            if (!incomingValid(type, payload.length))
                throw new RangeError(`Invalid session message type ${type} with ${payload.length} payload bytes`);
            messages.push(type === MESSAGE_CLIPBOARD
                ? {type, payload, text: this._decoder.decode(payload)} : {type, payload});
            offset += 4 + size;
        }
        this._buffer = joined.slice(offset);
        return messages;
    }
}

export function decodeAction(payload) {
    if (payload.length !== ACTION_BYTES)
        throw new RangeError('Window action has the wrong size');
    const view = new DataView(payload.buffer, payload.byteOffset, payload.length);
    const code = view.getUint16(2, true);
    const generation = view.getBigUint64(4, true);
    const windowId = view.getBigUint64(12, true);
    if (view.getUint16(0, true) !== ACTION_VERSION || generation === 0n || windowId === 0n ||
        (code !== ACTION_ACTIVATE && code !== ACTION_CLOSE))
        throw new RangeError('Invalid window action');
    return {action: code === ACTION_ACTIVATE ? 'activate' : 'close', generation, windowId};
}

export function encodeAction(action, generation, windowId) {
    const payload = new Uint8Array(ACTION_BYTES);
    const view = new DataView(payload.buffer);
    view.setUint16(0, ACTION_VERSION, true);
    view.setUint16(2, action === 'activate' ? ACTION_ACTIVATE : ACTION_CLOSE, true);
    view.setBigUint64(4, generation, true);
    view.setBigUint64(12, windowId, true);
    return payload;
}

export class Backoff {
    constructor(initial = BACKOFF_INITIAL_MS, maximum = BACKOFF_MAX_MS) {
        this._initial = initial;
        this._maximum = maximum;
        this._next = initial;
    }

    next() {
        const delay = this._next;
        this._next = Math.min(this._next * 2, this._maximum);
        return delay;
    }

    reset() {
        this._next = this._initial;
    }
}

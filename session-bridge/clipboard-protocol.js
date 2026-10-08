// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
export const MAX_FRAME_BYTES = 1048536;
export const BACKOFF_INITIAL_MS = 1000;
export const BACKOFF_MAX_MS = 30000;

export function encodeFrame(text) {
    const body = new TextEncoder().encode(text);
    if (body.length > MAX_FRAME_BYTES)
        return null;
    const frame = new Uint8Array(4 + body.length);
    new DataView(frame.buffer).setUint32(0, body.length, true);
    frame.set(body, 4);
    return frame;
}

export class FrameDecoder {
    constructor() {
        this._buffer = new Uint8Array(0);
        this._decoder = new TextDecoder('utf-8', {fatal: true});
    }

    // Returns the strings completed by this chunk; throws on an over-long length or invalid UTF-8.
    push(chunk) {
        const joined = new Uint8Array(this._buffer.length + chunk.length);
        joined.set(this._buffer, 0);
        joined.set(chunk, this._buffer.length);
        const texts = [];
        let offset = 0;
        while (joined.length - offset >= 4) {
            const length = new DataView(joined.buffer, joined.byteOffset + offset, 4).getUint32(0, true);
            if (length > MAX_FRAME_BYTES)
                throw new RangeError(`Clipboard frame length ${length} exceeds ${MAX_FRAME_BYTES}`);
            if (joined.length - offset - 4 < length)
                break;
            texts.push(this._decoder.decode(joined.subarray(offset + 4, offset + 4 + length)));
            offset += 4 + length;
        }
        this._buffer = joined.slice(offset);
        return texts;
    }
}

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

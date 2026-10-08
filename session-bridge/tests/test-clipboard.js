// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import {Backoff, EchoFilter, FrameDecoder, MAX_FRAME_BYTES, encodeFrame} from '../clipboard-protocol.js';

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

function raw(length, body = []) {
    const bytes = new Uint8Array(4 + body.length);
    new DataView(bytes.buffer).setUint32(0, length, true);
    bytes.set(body, 4);
    return bytes;
}

const hello = encodeFrame('héllo');
assert(hello.length === 4 + 6 && hello[0] === 6 && hello[1] === 0 && hello[2] === 0 && hello[3] === 0, 'Wrong frame header');
assert(new TextDecoder().decode(hello.subarray(4)) === 'héllo', 'Wrong frame body');
assert(encodeFrame('').length === 4 && encodeFrame('')[0] === 0, 'Empty text is not a bare header');
assert(encodeFrame('a'.repeat(MAX_FRAME_BYTES)).length === 4 + MAX_FRAME_BYTES, 'Maximum length refused');
assert(encodeFrame('a'.repeat(MAX_FRAME_BYTES + 1)) === null, 'Over-long text encoded');
assert(encodeFrame('é'.repeat(MAX_FRAME_BYTES / 2 + 1)) === null, 'Length counted in characters, not bytes');

let decoder = new FrameDecoder();
assert(decoder.push(hello).join() === 'héllo', 'Whole frame not decoded');

decoder = new FrameDecoder();
const results = [];
for (const byte of hello)
    results.push(...decoder.push(Uint8Array.of(byte)));
assert(results.length === 1 && results[0] === 'héllo', 'Byte-by-byte chunks not reassembled');

decoder = new FrameDecoder();
const joined = new Uint8Array([...encodeFrame('one'), ...encodeFrame('two'), ...encodeFrame('three').subarray(0, 5)]);
const batch = decoder.push(joined);
assert(batch.length === 2 && batch[0] === 'one' && batch[1] === 'two', 'Multiple frames in one chunk');
const rest = decoder.push(encodeFrame('three').subarray(5));
assert(rest.length === 1 && rest[0] === 'three', 'Partial trailing frame lost');

decoder = new FrameDecoder();
const empties = decoder.push(new Uint8Array([...encodeFrame(''), ...encodeFrame('x'), ...encodeFrame('')]));
assert(empties.length === 3 && empties[0] === '' && empties[1] === 'x' && empties[2] === '', 'Zero-length frames');
assert(decoder.push(new Uint8Array(0)).length === 0, 'Empty chunk produced frames');

rejects(() => new FrameDecoder().push(raw(MAX_FRAME_BYTES + 1)), 'Accepted over-long length');
rejects(() => new FrameDecoder().push(raw(0xffffffff)), 'Accepted 32-bit maximum length');
assert(new FrameDecoder().push(raw(MAX_FRAME_BYTES)).length === 0, 'Rejected the maximum length');
rejects(() => new FrameDecoder().push(raw(2, [0xc3, 0x28])), 'Accepted invalid UTF-8');
rejects(() => new FrameDecoder().push(raw(1, [0xff])), 'Accepted a lone invalid byte');

const filter = new EchoFilter();
assert(filter.shouldSend('a'), 'Fresh text suppressed');
assert(!filter.shouldSend(null), 'Null text sent');
filter.noteSent('a');
assert(!filter.shouldSend('a'), 'Repeat of last sent text sent');
assert(filter.shouldSend('b'), 'New text suppressed');
filter.noteApplied('from daemon');
assert(!filter.shouldSend('from daemon'), 'Echo of applied text sent');
assert(filter.shouldSend('a'), 'Earlier sent text suppressed after an apply');
filter.noteSent('b');
assert(filter.shouldSend('from daemon'), 'Applied text suppressed after a newer send');
filter.reset();
assert(filter.shouldSend('b'), 'Reset did not allow resending');

const backoff = new Backoff();
const delays = Array.from({length: 8}, () => backoff.next());
assert(delays.join() === '1000,2000,4000,8000,16000,30000,30000,30000', `Wrong backoff schedule ${delays}`);
backoff.reset();
assert(backoff.next() === 1000, 'Backoff did not reset');
print('PASS clipboard frame encoding, chunked decoding, length and UTF-8 rejection, echo suppression and backoff');

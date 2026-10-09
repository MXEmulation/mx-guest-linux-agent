// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import {EchoFilter} from '../clipboard-protocol.js';
import {ACTION_BYTES, Backoff, MAX_CLIPBOARD_BYTES, MAX_PAYLOAD_BYTES, MESSAGE_ACTION, MESSAGE_CLIPBOARD,
    MESSAGE_WINDOWS, MessageDecoder, encodeAction, encodeMessage} from '../session-protocol.js';

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

function raw(size, type, body = []) {
    const bytes = new Uint8Array(4 + (size > 0 ? 1 : 0) + body.length);
    new DataView(bytes.buffer).setUint32(0, size, true);
    if (size > 0)
        bytes[4] = type;
    bytes.set(body, size > 0 ? 5 : 4);
    return bytes;
}

function clipboard(text) {
    return encodeMessage(MESSAGE_CLIPBOARD, new TextEncoder().encode(text));
}

const hello = clipboard('héllo');
assert(hello.length === 5 + 6 && hello[0] === 7 && hello[1] === 0 && hello[2] === 0 && hello[3] === 0, 'Wrong frame header');
assert(hello[4] === MESSAGE_CLIPBOARD, 'Wrong message type');
assert(new TextDecoder().decode(hello.subarray(5)) === 'héllo', 'Wrong frame body');
assert(clipboard('').length === 5 && clipboard('')[0] === 1, 'Empty text is not a header and a type');
assert(encodeMessage(MESSAGE_CLIPBOARD, new Uint8Array(MAX_CLIPBOARD_BYTES)).length === 5 + MAX_CLIPBOARD_BYTES, 'Maximum length refused');
assert(encodeMessage(MESSAGE_CLIPBOARD, new Uint8Array(MAX_CLIPBOARD_BYTES + 1)) === null, 'Over-long text encoded');
assert(encodeMessage(MESSAGE_WINDOWS, new Uint8Array(MAX_PAYLOAD_BYTES)).length === 5 + MAX_PAYLOAD_BYTES, 'Maximum inventory refused');
assert(encodeMessage(MESSAGE_WINDOWS, new Uint8Array(MAX_PAYLOAD_BYTES + 1)) === null, 'Over-long inventory encoded');
assert(encodeMessage(MESSAGE_ACTION, new Uint8Array(ACTION_BYTES)) === null, 'Client encoded a daemon message');
assert(encodeMessage(9, new Uint8Array(1)) === null, 'Unknown type encoded');

let decoder = new MessageDecoder();
const whole = decoder.push(hello);
assert(whole.length === 1 && whole[0].type === MESSAGE_CLIPBOARD && whole[0].text === 'héllo', 'Whole frame not decoded');

decoder = new MessageDecoder();
const results = [];
for (const byte of hello)
    results.push(...decoder.push(Uint8Array.of(byte)));
assert(results.length === 1 && results[0].text === 'héllo', 'Byte-by-byte chunks not reassembled');

decoder = new MessageDecoder();
const action = encodeAction('close', 12n, 34n);
const actionFrame = raw(1 + ACTION_BYTES, MESSAGE_ACTION, action);
const joined = new Uint8Array([...clipboard('one'), ...actionFrame, ...clipboard('three').subarray(0, 6)]);
const batch = decoder.push(joined);
assert(batch.length === 2 && batch[0].text === 'one' && batch[1].type === MESSAGE_ACTION, 'Multiple frames in one chunk');
assert(batch[1].payload.length === ACTION_BYTES && batch[1].payload[2] === 2, 'Action payload not preserved');
const rest = decoder.push(clipboard('three').subarray(6));
assert(rest.length === 1 && rest[0].text === 'three', 'Partial trailing frame lost');

decoder = new MessageDecoder();
const empties = decoder.push(new Uint8Array([...clipboard(''), ...clipboard('x'), ...clipboard('')]));
assert(empties.length === 3 && empties[0].text === '' && empties[1].text === 'x' && empties[2].text === '', 'Zero-length text');
assert(decoder.push(new Uint8Array(0)).length === 0, 'Empty chunk produced frames');

rejects(() => new MessageDecoder().push(raw(0, 0)), 'Accepted a zero length');
rejects(() => new MessageDecoder().push(raw(MAX_PAYLOAD_BYTES + 2, MESSAGE_CLIPBOARD)), 'Accepted over-long length');
rejects(() => new MessageDecoder().push(raw(0xffffffff, MESSAGE_CLIPBOARD)), 'Accepted 32-bit maximum length');
rejects(() => new MessageDecoder().push(raw(MAX_CLIPBOARD_BYTES + 2, MESSAGE_CLIPBOARD, new Uint8Array(MAX_CLIPBOARD_BYTES + 1))),
    'Accepted over-long clipboard text');
assert(new MessageDecoder().push(raw(MAX_CLIPBOARD_BYTES + 1, MESSAGE_CLIPBOARD)).length === 0, 'Rejected the maximum clipboard length');
rejects(() => new MessageDecoder().push(raw(3, MESSAGE_CLIPBOARD, [0xc3, 0x28])), 'Accepted invalid UTF-8');
rejects(() => new MessageDecoder().push(raw(2, MESSAGE_CLIPBOARD, [0xff])), 'Accepted a lone invalid byte');
rejects(() => new MessageDecoder().push(raw(1, 9)), 'Accepted an unknown type');
rejects(() => new MessageDecoder().push(raw(1, MESSAGE_WINDOWS)), 'Accepted a client-only type from the daemon');
rejects(() => new MessageDecoder().push(raw(1 + ACTION_BYTES - 1, MESSAGE_ACTION, action.subarray(1))), 'Accepted a short action');
rejects(() => new MessageDecoder().push(raw(1 + ACTION_BYTES + 1, MESSAGE_ACTION, [...action, 0])), 'Accepted a long action');

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
print('PASS typed clipboard framing, chunked decoding, length, type and UTF-8 rejection, echo suppression and backoff');

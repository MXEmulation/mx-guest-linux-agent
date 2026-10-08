// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import St from 'gi://St';
import {Backoff, EchoFilter, FrameDecoder, encodeFrame} from './clipboard-protocol.js';

const SOCKET_PATH = '/run/mxguest-agent/session.sock';
const READ_SIZE = 65536;
const COALESCE_MS = 50;

const STABLE_MICROSECONDS = 5000000;

export class ClipboardBridge {
    constructor(socketPath = SOCKET_PATH) {
        this._address = Gio.UnixSocketAddress.new(socketPath);
        this._backoff = new Backoff();
        this._filter = new EchoFilter();
        this._destroyed = false;
        this._session = null;
        this._retrySource = 0;
        this._readSource = 0;
        this._selection = global.display.get_selection();
        this._ownerSignal = this._selection.connect('owner-changed', (_selection, type) => {
            if (type === Meta.SelectionType.SELECTION_CLIPBOARD)
                this._scheduleRead();
        });
        this._connect();
    }

    _connect() {
        if (this._destroyed)
            return;
        const session = {
            cancellable: new Gio.Cancellable(),
            connection: null,
            decoder: new FrameDecoder(),
            queue: [],
            writing: false,
            closed: false,
        };
        this._session = session;
        new Gio.SocketClient().connect_async(this._address, session.cancellable, (client, result) => {
            if (session.closed)
                return;
            try {
                session.connection = client.connect_finish(result);
            } catch (_error) {
                this._drop(session);
                return;
            }
            session.connected = GLib.get_monotonic_time();
            this._filter.reset();
            this._readLoop(session);
            this._scheduleRead();
        });
    }

    _drop(session) {
        if (session.closed)
            return;
        session.closed = true;
        session.cancellable.cancel();
        try {
            session.connection?.close(null);
        } catch (_error) {
            // The peer may already be gone; the connection is discarded either way.
        }
        session.connection = null;
        session.queue = [];
        if (this._session === session)
            this._session = null;
        if (this._destroyed || this._retrySource)
            return;
        if (session.connected && GLib.get_monotonic_time() - session.connected >= STABLE_MICROSECONDS)
            this._backoff.reset();
        this._retrySource = GLib.timeout_add(GLib.PRIORITY_DEFAULT, this._backoff.next(), () => {
            this._retrySource = 0;
            this._connect();
            return GLib.SOURCE_REMOVE;
        });
    }

    _readLoop(session) {
        if (session.closed)
            return;
        session.connection.get_input_stream().read_bytes_async(READ_SIZE, GLib.PRIORITY_DEFAULT,
            session.cancellable, (stream, result) => {
                if (session.closed)
                    return;
                let chunk;
                try {
                    chunk = stream.read_bytes_finish(result).get_data();
                } catch (_error) {
                    this._drop(session);
                    return;
                }
                if (!chunk || chunk.length === 0) {
                    this._drop(session);
                    return;
                }
                try {
                    for (const text of session.decoder.push(chunk))
                        this._apply(text);
                } catch (error) {
                    console.error(`MX clipboard bridge closing connection: ${error.message}`);
                    this._drop(session);
                    return;
                }
                this._readLoop(session);
            });
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
        const session = this._session;
        if (!session?.connection)
            return;
        St.Clipboard.get_default().get_text(St.ClipboardType.CLIPBOARD, (_clipboard, text) => {
            if (session.closed || this._session !== session || !this._filter.shouldSend(text))
                return;
            const frame = encodeFrame(text);
            if (!frame)
                return;
            this._filter.noteSent(text);
            session.queue.push(frame);
            this._pump(session);
        });
    }

    _pump(session) {
        if (session.writing || session.closed || session.queue.length === 0)
            return;
        session.writing = true;
        this._write(session, session.queue.shift());
    }

    _write(session, frame) {
        session.connection.get_output_stream().write_bytes_async(new GLib.Bytes(frame), GLib.PRIORITY_DEFAULT,
            session.cancellable, (stream, result) => {
                if (session.closed)
                    return;
                let written;
                try {
                    written = stream.write_bytes_finish(result);
                } catch (_error) {
                    this._drop(session);
                    return;
                }
                if (written <= 0) {
                    this._drop(session);
                    return;
                }
                if (written < frame.length) {
                    this._write(session, frame.subarray(written));
                    return;
                }
                session.writing = false;
                this._pump(session);
            });
    }

    destroy() {
        if (this._destroyed)
            return;
        this._destroyed = true;
        if (this._ownerSignal)
            this._selection.disconnect(this._ownerSignal);
        this._ownerSignal = 0;
        if (this._retrySource)
            GLib.source_remove(this._retrySource);
        this._retrySource = 0;
        if (this._readSource)
            GLib.source_remove(this._readSource);
        this._readSource = 0;
        if (this._session)
            this._drop(this._session);
        this._session = null;
    }
}

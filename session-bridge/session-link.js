// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import {Backoff, MessageDecoder, encodeMessage} from './session-protocol.js';

const SOCKET_PATH = '/run/mxguest-agent/session.sock';
const READ_SIZE = 65536;
const STABLE_MICROSECONDS = 5000000;

export class SessionLink {
    constructor(socketPath = SOCKET_PATH) {
        this._address = Gio.UnixSocketAddress.new(socketPath);
        this._backoff = new Backoff();
        this._destroyed = false;
        this._session = null;
        this._retrySource = 0;
        this._connectedHandlers = [];
        this._disconnectedHandlers = [];
        this._messageHandlers = new Map();
    }

    get connected() {
        return Boolean(this._session?.connection);
    }

    onConnected(handler) {
        this._connectedHandlers.push(handler);
    }

    onDisconnected(handler) {
        this._disconnectedHandlers.push(handler);
    }

    onMessage(type, handler) {
        this._messageHandlers.set(type, handler);
    }

    start() {
        this._connect();
    }

    // With replace, a queued message of the same type that has not started writing is superseded.
    send(type, payload, replace = false) {
        const session = this._session;
        const frame = encodeMessage(type, payload);
        if (!frame || !session?.connection)
            return false;
        if (replace)
            session.queue = session.queue.filter(item => item.type !== type);
        session.queue.push({type, frame});
        this._pump(session);
        return true;
    }

    _connect() {
        if (this._destroyed)
            return;
        const session = {
            cancellable: new Gio.Cancellable(),
            connection: null,
            decoder: new MessageDecoder(),
            queue: [],
            writing: false,
            closed: false,
            connected: 0,
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
            this._readLoop(session);
            this._notify(this._connectedHandlers);
        });
    }

    _notify(handlers, ...args) {
        for (const handler of handlers) {
            try {
                handler(...args);
            } catch (error) {
                console.error(`MX session link handler failed: ${error.message}`);
            }
        }
    }

    _drop(session) {
        if (session.closed)
            return;
        session.closed = true;
        session.cancellable.cancel();
        const wasConnected = Boolean(session.connection);
        try {
            session.connection?.close(null);
        } catch (_error) {
            // The peer may already be gone; the connection is discarded either way.
        }
        session.connection = null;
        session.queue = [];
        if (this._session === session)
            this._session = null;
        if (wasConnected)
            this._notify(this._disconnectedHandlers);
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
                let messages;
                try {
                    messages = session.decoder.push(chunk);
                } catch (error) {
                    console.error(`MX session link closing connection: ${error.message}`);
                    this._drop(session);
                    return;
                }
                for (const message of messages) {
                    if (session.closed)
                        return;
                    this._notifyMessage(message);
                }
                this._readLoop(session);
            });
    }

    _notifyMessage(message) {
        const handler = this._messageHandlers.get(message.type);
        if (handler)
            this._notify([handler], message);
    }

    _pump(session) {
        if (session.writing || session.closed || session.queue.length === 0)
            return;
        session.writing = true;
        this._write(session, session.queue.shift().frame);
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
        if (this._retrySource)
            GLib.source_remove(this._retrySource);
        this._retrySource = 0;
        if (this._session)
            this._drop(this._session);
        this._session = null;
    }
}

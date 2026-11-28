/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

'use strict';

const path = require('node:path');
const { EventEmitter } = require('node:events');

function loadNative() {
	try {
		return require('node-gyp-build')(path.join(__dirname, '..'));
	} catch (err) {
		// No prebuild for this platform and no local build. npm 12 skips install
		// scripts by default, so the source build in scripts/install.js may not
		// have run.
		err.message += '\n@xaymar/datapath: no prebuilt binary for '
			+ `${process.platform}-${process.arch}. Build it from source (needs CMake 3.16+ and a C++17 compiler) with\n`
			+ '  npm explore @xaymar/datapath -- npm run build\n'
			+ 'or allow this package\'s install script and reinstall it.';
		throw err;
	}
}

const native = loadNative();

// Mirrors datapath::error.
const codes = Object.freeze({
	Unknown: -1,
	Success: 0,
	Failure: 1,
	CriticalFailure: 2,
	Closed: 3,
	TimedOut: 4,
	InvalidPath: 5,
	NotSupported: 6,
	TooLarge: 7,
	WouldBlock: 8,
	VersionMismatch: 9,
	NotFound: 10,
	AddressInUse: 11,
	AccessDenied: 12,
});

const EV_MESSAGE = 0;
const EV_CLOSE = 1;
const EV_ACCEPT = 2;

function codeName(errno) {
	return native.errorName(errno);
}

class DatapathError extends Error {
	constructor(errno, message) {
		const code = codeName(errno);
		super(message ? `${message}: ${code}` : code);
		this.name = 'DatapathError';
		this.code = code;
		this.errno = errno;
	}
}

class Context {
	#native;

	constructor(options = {}) {
		this.#native = new native.NativeContext(options);
	}

	static _native(ctx) {
		if (ctx === undefined || ctx === null) return undefined;
		if (!(ctx instanceof Context)) throw new TypeError('options.context must be a datapath Context');
		return ctx.#native;
	}
}

class Socket extends EventEmitter {
	#native;
	#open = true;
	#reading = false;

	/** @internal Created by connect() and Server; not for direct use. */
	constructor(nativeSocket) {
		super();
		if (!(nativeSocket instanceof native.NativeSocket)) {
			throw new TypeError('use datapath.connect() to create a Socket');
		}
		this.#native = nativeSocket;
		// Receiving starts with the first 'message' listener, like the C++ API's
		// first on_message(); until then messages wait in the kernel.
		this.on('newListener', (event) => {
			if (event === 'message' && !this.#reading && this.#open) {
				this.#reading = true;
				this.#native.start();
			}
		});
		this.#native.attach((kind, payload) => {
			if (kind === EV_MESSAGE) {
				this.emit('message', payload);
			} else if (kind === EV_CLOSE) {
				this.#open = false;
				const reason = codeName(payload);
				this.emit('close', reason, payload !== codes.Closed);
			}
		});
	}

	/** Queue one message. Returns false when the send queue is full (WouldBlock). */
	send(data) {
		return this.#result(this.#native.send(data));
	}

	/** Queue one message assembled from several parts, without joining them first. */
	sendv(parts) {
		return this.#result(this.#native.sendv(parts));
	}

	#result(errno) {
		if (errno === codes.Success) return true;
		if (errno === codes.WouldBlock) return false;
		throw new DatapathError(errno, 'send failed');
	}

	/** Close the connection. 'close' follows asynchronously. */
	close() {
		this.#native.close();
		return this;
	}

	/** True until the connection is closed (locally or by the peer). */
	get connected() {
		return this.#open && this.#native.good();
	}

	/** Negotiated message size limit of this connection, in bytes. */
	get maxMessageSize() {
		return this.#native.maxMessageSize();
	}

	/** Keep the event loop alive while the socket is open (default). */
	ref() {
		this.#native.ref();
		return this;
	}

	/** Let the process exit even though the socket is open. */
	unref() {
		this.#native.unref();
		return this;
	}
}

class Server extends EventEmitter {
	#native;
	#open = true;

	/** @internal Created by host(); not for direct use. */
	constructor(nativeServer) {
		super();
		this.#native = nativeServer;
		this.#native.attach((kind, nativeSocket) => {
			if (kind !== EV_ACCEPT) return;
			const socket = new Socket(nativeSocket);
			if (!this.#open || this.listenerCount('connection') === 0) {
				socket.close();
				return;
			}
			this.emit('connection', socket);
		});
	}

	/** Stop accepting clients. Connected sockets stay open. */
	close() {
		if (!this.#open) return this;
		this.#open = false;
		this.#native.close();
		process.nextTick(() => this.emit('close'));
		return this;
	}

	get listening() {
		return this.#open;
	}

	ref() {
		this.#native.ref();
		return this;
	}

	unref() {
		this.#native.unref();
		return this;
	}
}

function splitOptions(options) {
	if (options === undefined || options === null) return [{}, undefined];
	if (typeof options !== 'object') throw new TypeError('options must be an object');
	const { context, ...rest } = options;
	return [rest, Context._native(context)];
}

/** Connect to a server. Resolves once the handshake completed. */
async function connect(path, options) {
	if (typeof path !== 'string') throw new TypeError('path must be a string');
	const [opts, ctx] = splitOptions(options);
	let nativeSocket;
	try {
		nativeSocket = await native.connect(path, opts, ctx);
	} catch (errno) {
		if (typeof errno === 'number') throw new DatapathError(errno, `connect to "${path}" failed`);
		throw errno;
	}
	return new Socket(nativeSocket);
}

/** Host a server at `path`. Throws a DatapathError when it cannot. */
function host(path, options, onConnection) {
	if (typeof options === 'function') {
		onConnection = options;
		options = undefined;
	}
	if (typeof path !== 'string') throw new TypeError('path must be a string');
	const [opts, ctx] = splitOptions(options);
	const result = native.host(path, opts, ctx);
	if (typeof result === 'number') throw new DatapathError(result, `host "${path}" failed`);
	const server = new Server(result);
	if (onConnection) server.on('connection', onConnection);
	return server;
}

module.exports = {
	connect,
	host,
	Context,
	Socket,
	Server,
	DatapathError,
	codes,
};

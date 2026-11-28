/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

/// <reference types="node" />

import { EventEmitter } from 'node:events';

/** Names of `datapath::error` values. */
export type ErrorCode =
	| 'Unknown'
	| 'Success'
	| 'Failure'
	| 'CriticalFailure'
	| 'Closed'
	| 'TimedOut'
	| 'InvalidPath'
	| 'NotSupported'
	| 'TooLarge'
	| 'WouldBlock'
	| 'VersionMismatch'
	| 'NotFound'
	| 'AddressInUse'
	| 'AccessDenied';

/** Numeric values of `datapath::error`, by name. */
export declare const codes: Readonly<Record<ErrorCode, number>>;

/** Thrown (or rejected) when a datapath call fails. */
export declare class DatapathError extends Error {
	readonly name: 'DatapathError';
	/** The `datapath::error` name, e.g. `'NotFound'`. */
	readonly code: ErrorCode;
	/** The numeric `datapath::error` value. */
	readonly errno: number;
}

/** Who may connect to a hosted server. */
export type Permissions = 'user' | 'group' | 'world';

/** Settings shared by every socket and server that runs on one context (`datapath::context_options`). */
export interface ContextOptions {
	/** Microseconds an idle I/O thread busy-polls before blocking; 0 disables. Default 50. */
	spin?: number;
	/** Number of I/O threads; 0 = one per physical core. Default 1. */
	ioThreads?: number;
	/** Windows only: register I/O threads with MMCSS ("Pro Audio"). Default false. */
	mmcss?: boolean;
	/** I/O thread priority: 0 normal, 1 above normal, 2 highest, 3 time critical. Default 2. */
	priority?: 0 | 1 | 2 | 3;
	/** Windows only: -1 automatic, -2 off, N >= 0 first physical core. Default -1. */
	idealCore?: number;
	/** Windows only: alertable completion-port wait. Default false. */
	alertable?: boolean;
	/** Yield the CPU after sends and between busy-poll iterations. Default false. */
	yield?: boolean;
	/** Windows only: pair with a peer that also sets this on the two hyper-threads of one core (needs ioThreads 1). Default false. */
	pairPeer?: boolean;
}

/**
 * Owns the I/O threads. Sockets and servers use the process-wide default context
 * unless `options.context` names another one. Its threads stop once the context
 * and everything that runs on it have been garbage collected.
 */
export declare class Context {
	constructor(options?: ContextOptions);
}

/** Per-connection / per-server settings (`datapath::options`). */
export interface Options {
	/** Largest message accepted, in bytes. The effective limit is the smaller of both peers'. Default 64 MiB. */
	maxMessageSize?: number | bigint;
	/** Bytes that may wait in the send queue before `send()` returns false. Default 64 MiB. */
	maxQueuedBytes?: number | bigint;
	/** Receive buffer per connection, in bytes. Default 256 KiB. */
	receiveBuffer?: number;
	/**
	 * Bytes of received messages that may wait for 'message' listeners before the
	 * socket stops reading from the peer (each message counts its size plus 64).
	 * The peer's sends then back up and return false. Applies to every socket a
	 * server accepts. Default 1 MiB.
	 */
	highWaterMark?: number;
	/** Context to run on. Default: the process-wide default context. */
	context?: Context;
}

export interface ConnectOptions extends Options {
	/** Milliseconds to wait for the server's handshake. Default 1000. */
	connectTimeout?: number;
}

export interface HostOptions extends Options {
	/** Who may connect. Default `'user'`. */
	permissions?: Permissions;
	/** Pending connection slots kept ready. Default 8. */
	backlog?: number;
	/** Maximum simultaneously connected clients; 0 = unlimited. Default 0. */
	maxClients?: number;
	/** Windows only: kernel pipe buffer in bytes; 0 = same as receiveBuffer. Default 1 MiB. */
	pipeBuffer?: number;
}

/** Data accepted by `send()`. Strings are sent as UTF-8. */
export type Data = string | NodeJS.ArrayBufferView | ArrayBuffer;

export interface SocketEvents {
	/** One complete message. The Buffer is owned by the listener. */
	message: [data: Buffer];
	/** The connection ended. Fires exactly once. `hadError` is false for a regular close. */
	close: [reason: ErrorCode, hadError: boolean];
	/** The send queue ran empty after `send()` returned false: send again. */
	drain: [];
}

/**
 * One end of a connection. Messages are only read once a `'message'` listener is
 * added; until then they wait in the kernel.
 */
export declare class Socket extends EventEmitter<SocketEvents> {
	private constructor();
	/**
	 * Queue one message. Never blocks and copies `data`, so it may be reused right away.
	 * @returns false when the send queue is full (`WouldBlock`): the message was not
	 * queued; send it again after `'drain'`.
	 * @throws DatapathError `TooLarge` or `Closed`.
	 */
	send(data: Data): boolean;
	/** Queue one message assembled from several parts. Same results as `send()`. */
	sendv(parts: readonly Data[]): boolean;
	/**
	 * Stop emitting `'message'` and stop reading from the peer, whose sends then back
	 * up. Messages already received wait for `resume()`; a remote close is reported
	 * after them. `close()` while paused drops them and reports `'close'` right away.
	 */
	pause(): this;
	/** Emit `'message'` again, starting with the messages that waited. */
	resume(): this;
	/** True between `pause()` and `resume()`. */
	isPaused(): boolean;
	/** Bytes of received messages waiting to be emitted (size + 64 each). */
	readonly receiveQueueBytes: number;
	/** Close the connection; `'close'` follows asynchronously. */
	close(): this;
	/** True until the connection is closed. */
	readonly connected: boolean;
	/** Negotiated message size limit of this connection, in bytes. */
	readonly maxMessageSize: number;
	/** Keep the process alive while the socket is open (default). */
	ref(): this;
	/** Let the process exit even though the socket is open. */
	unref(): this;
}

export interface ServerEvents {
	/** A client passed the handshake. Without a listener, clients are closed. */
	connection: [socket: Socket];
	/** `close()` was called. */
	close: [];
}

export declare class Server extends EventEmitter<ServerEvents> {
	private constructor();
	/** Stop accepting clients. Connected sockets stay open. */
	close(): this;
	readonly listening: boolean;
	/** Keep the process alive while the server is open (default). */
	ref(): this;
	/** Let the process exit even though the server is open. */
	unref(): this;
}

/**
 * Connect to the server hosted at `path`. A plain name works everywhere (Windows:
 * `\\.\pipe\<name>`, Linux/Android: abstract Unix socket); on Linux/Android a
 * path starting with `/` is a filesystem Unix socket.
 */
export declare function connect(path: string, options?: ConnectOptions): Promise<Socket>;

/** Host a server at `path`. Throws a DatapathError (e.g. `AddressInUse`) when it cannot. */
export declare function host(path: string, onConnection?: (socket: Socket) => void): Server;
export declare function host(path: string, options?: HostOptions, onConnection?: (socket: Socket) => void): Server;

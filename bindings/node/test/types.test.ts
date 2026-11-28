/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Compile-only check of lib/index.d.ts: npm run typecheck
import { connect, host, Context, DatapathError, codes, type Socket, type ErrorCode } from '@xaymar/datapath';

async function main(): Promise<void> {
	const context = new Context({ ioThreads: 0, spin: 0, priority: 1 });
	const server = host('typed', { permissions: 'group', maxClients: 4, context }, (sock: Socket) => {
		sock.on('message', (data: Buffer) => {
			sock.send(data);
		});
	});
	server.on('close', () => {});
	const client = await connect('typed', { connectTimeout: 500, maxMessageSize: 1n << 20n });
	client.on('close', (reason: ErrorCode, hadError: boolean) => console.log(reason, hadError));
	const queued: boolean = client.sendv(['a', Buffer.from('b'), new Uint8Array(1), new ArrayBuffer(2)]);
	const limit: number = client.maxMessageSize;
	client.on('drain', () => client.send('again'));
	const paused: boolean = client.pause().resume().isPaused();
	const waiting: number = client.receiveQueueBytes;
	void (await connect('typed', { highWaterMark: 1 << 16 }));
	client.unref().ref().close();
	server.close();
	try {
		await connect('missing');
	} catch (e) {
		if (e instanceof DatapathError && e.code === 'NotFound' && e.errno === codes.NotFound) return;
	}
	void queued;
	void limit;
	void paused;
	void waiting;
	// @ts-expect-error numbers are not sendable
	client.send(42);
	// @ts-expect-error unknown event
	client.on('data', () => {});
}
void main;

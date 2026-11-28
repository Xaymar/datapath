/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

'use strict';
// A hostile client floods a server whose JavaScript thread is busy. The server's
// receive queue must stay at its high-water mark, the client must see send()
// return false, and every message it queued must still arrive, in order.
const { spawn } = require('node:child_process');
const dp = require('../..');

const SIZE = 4096;
const HWM = 64 * 1024;

if (process.argv[2] === 'client') {
	(async () => {
		const sock = await dp.connect(process.argv[3], { maxQueuedBytes: 1 << 20 });
		const buf = Buffer.alloc(SIZE);
		let sent = 0;
		const flood = () => {
			for (;;) {
				buf.writeUInt32LE(sent, 0);
				if (!sock.send(buf)) return;
				sent++;
				if (sent * SIZE > 1 << 30) {
					console.log('never blocked');
					process.exit(1);
				}
			}
		};
		flood();
		console.log(`blocked ${sent}`);
		await new Promise((resolve) => sock.once('drain', resolve));
		console.log('drain');
		sock.close();
		await new Promise((resolve) => sock.once('close', resolve));
		process.stdout.write(`sent ${sent}\n`, () => process.exit(0));
	})().catch((e) => {
		console.error(e);
		process.exit(1);
	});
} else {
	const name = `dp-node-flood-${process.pid}`;
	let received = 0;
	let maxQueued = 0;
	let misordered = false;
	const server = dp.host(name, { highWaterMark: HWM }, (sock) => {
		sock.on('message', (data) => {
			if (data.readUInt32LE(0) !== received) misordered = true;
			received++;
			maxQueued = Math.max(maxQueued, sock.receiveQueueBytes);
		});
		sock.on('close', () => {
			server.close();
			console.log(`received ${received} maxQueued ${maxQueued} misordered ${misordered}`);
		});
		// Keep the JavaScript thread busy while the client floods.
		const until = Date.now() + 500;
		while (Date.now() < until);
		maxQueued = Math.max(maxQueued, sock.receiveQueueBytes);
	});
	const child = spawn(process.execPath, [__filename, 'client', name], { stdio: ['ignore', 'pipe', 'inherit'] });
	child.stdout.on('data', (d) => process.stdout.write(d));
	child.on('exit', (code) => {
		if (code !== 0) process.exitCode = 1;
	});
}

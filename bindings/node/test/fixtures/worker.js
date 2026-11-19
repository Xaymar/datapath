/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

'use strict';
// A worker opens a server and a client, then gets terminated while both are open.
const { Worker, isMainThread, parentPort } = require('node:worker_threads');
if (isMainThread) {
	(async () => {
		for (let i = 0; i < 3; i++) {
			const w = new Worker(__filename);
			await new Promise((resolve, reject) => {
				w.once('message', resolve);
				w.once('error', reject);
			});
			await w.terminate();
		}
		console.log('worker done');
	})().catch((e) => {
		console.error(e);
		process.exit(1);
	});
} else {
	const dp = require('../..');
	(async () => {
		const name = `dp-node-worker-${process.pid}-${Math.random().toString(36).slice(2)}`;
		dp.host(name, (sock) => sock.on('message', (d) => sock.send(d)));
		const client = await dp.connect(name);
		client.on('message', () => parentPort.postMessage('ok'));
		client.send('ping');
		// Keep echoing so I/O threads are busy during terminate().
		setInterval(() => client.send('more'), 1);
	})();
}

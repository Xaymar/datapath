/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

'use strict';
const dp = require('../..');
let received = 0;
let thrown = 0;
process.on('uncaughtException', (err) => {
	if (err.message !== 'boom') throw err;
	thrown++;
});
(async () => {
	const name = `dp-node-throw-${process.pid}`;
	const server = dp.host(name, (sock) => sock.on('message', (d) => sock.send(d)));
	const client = await dp.connect(name);
	client.on('message', () => {
		received++;
		if (received === 3) {
			console.log(`received ${received}, thrown ${thrown}`);
			client.close();
			server.close();
			return;
		}
		throw new Error('boom');
	});
	client.send('1');
	client.send('2');
	client.send('3');
})();

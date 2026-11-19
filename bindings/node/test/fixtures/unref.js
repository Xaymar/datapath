/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

'use strict';
// Exits on its own although a server and a connection stay open.
const dp = require('../..');
(async () => {
	const name = `dp-node-unref-${process.pid}`;
	dp.host(name, (sock) => {
		sock.on('message', () => {});
		sock.unref();
	}).unref();
	const client = await dp.connect(name);
	client.on('message', () => {});
	client.unref();
	console.log('ready');
})();

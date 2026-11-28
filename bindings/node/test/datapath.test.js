/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { once } = require('node:events');
const { spawnSync } = require('node:child_process');
const path = require('node:path');
const dp = require('..');

let serial = 0;
const unique = () => `dp-node-test-${process.pid}-${serial++}`;

// Host an echo server and connect one client to it.
async function echoPair(options = {}) {
	const name = unique();
	const server = dp.host(name, options.host, (sock) => {
		sock.on('message', (data) => sock.send(data));
	});
	const client = await dp.connect(name, options.connect);
	return { name, server, client };
}

function nextMessage(sock) {
	return once(sock, 'message').then(([data]) => data);
}

test('echo round trip with every data type', async () => {
	const { server, client } = await echoPair();
	try {
		const inputs = [
			['string', 'hello, datapath', Buffer.from('hello, datapath')],
			['Buffer', Buffer.from([1, 2, 3]), Buffer.from([1, 2, 3])],
			['Uint8Array', new Uint8Array([4, 5]), Buffer.from([4, 5])],
			['subarray', Buffer.from([9, 8, 7, 6]).subarray(1, 3), Buffer.from([8, 7])],
			['ArrayBuffer', new Uint8Array([10, 11]).buffer, Buffer.from([10, 11])],
			['DataView', new DataView(new Uint8Array([12, 13, 14]).buffer, 1), Buffer.from([13, 14])],
			['empty', Buffer.alloc(0), Buffer.alloc(0)],
		];
		for (const [label, input, expected] of inputs) {
			const reply = nextMessage(client);
			assert.equal(client.send(input), true, label);
			assert.deepEqual(await reply, expected, label);
		}
		const reply = nextMessage(client);
		assert.equal(client.sendv(['ab', Buffer.from('cd'), new Uint8Array([0x65])]), true);
		assert.equal((await reply).toString(), 'abcde');
	} finally {
		client.close();
		server.close();
	}
});

test('many messages arrive complete and in order', async () => {
	const { server, client } = await echoPair();
	const count = 20000;
	let next = 0;
	const done = new Promise((resolve, reject) => {
		client.on('message', (data) => {
			const n = data.readUInt32LE(0);
			if (n !== next) reject(new Error(`expected ${next}, got ${n}`));
			if (++next === count) resolve();
		});
	});
	const buf = Buffer.alloc(16);
	for (let i = 0; i < count; i++) {
		buf.writeUInt32LE(i, 0);
		while (!client.send(buf)) await new Promise((r) => setImmediate(r));
	}
	await done;
	client.close();
	server.close();
});

test('8 MiB message intact', async () => {
	const { server, client } = await echoPair();
	const big = Buffer.alloc(8 << 20);
	for (let i = 0; i < big.length; i += 4) big.writeUInt32LE(i, i);
	const reply = nextMessage(client);
	assert.equal(client.send(big), true);
	assert.ok(big.equals(await reply));
	client.close();
	server.close();
});

for (const [hostShm, connectShm] of [[true, true], [false, true], [true, false]]) {
	test(`sharedMemory host=${hostShm} connect=${connectShm} echoes intact`, async () => {
		const { server, client } = await echoPair({
			host: { sharedMemory: hostShm, sharedMemorySize: 1 << 16 },
			connect: { sharedMemory: connectShm },
		});
		try {
			const data = Buffer.alloc(200000); // larger than the 64 KiB ring
			for (let i = 0; i < data.length; i++) data[i] = (i * 31) & 0xff;
			const reply = nextMessage(client);
			assert.equal(client.send(data), true);
			assert.ok(data.equals(await reply));
		} finally {
			client.close();
			server.close();
		}
	});
}

test('connect to a missing server rejects with NotFound', async () => {
	await assert.rejects(dp.connect(unique()), (err) => {
		assert.ok(err instanceof dp.DatapathError);
		assert.equal(err.code, 'NotFound');
		assert.equal(err.errno, dp.codes.NotFound);
		return true;
	});
});

test('hosting the same path twice throws AddressInUse', () => {
	const name = unique();
	const server = dp.host(name, () => {});
	try {
		assert.throws(() => dp.host(name), { name: 'DatapathError', code: 'AddressInUse' });
	} finally {
		server.close();
	}
});

test('send above the negotiated limit throws TooLarge', async () => {
	const { server, client } = await echoPair({ host: { maxMessageSize: 1024 } });
	assert.equal(client.maxMessageSize, 1024);
	assert.throws(() => client.send(Buffer.alloc(2048)), { code: 'TooLarge' });
	client.close();
	server.close();
});

test('peer close is reported once on both ends', async () => {
	const name = unique();
	let serverSide;
	const accepted = new Promise((resolve) => {
		dp.host(name, (sock) => {
			serverSide = sock;
			sock.on('message', () => {});
			resolve();
		}).unref();
	});
	const client = await dp.connect(name);
	client.on('message', () => {});
	await accepted;
	const serverClosed = once(serverSide, 'close');
	const clientClosed = once(client, 'close');
	client.close();
	const [[reason], [clientReason]] = await Promise.all([serverClosed, clientClosed]);
	assert.equal(typeof reason, 'string');
	assert.equal(clientReason, 'Closed');
	assert.equal(client.connected, false);
	assert.throws(() => client.send('x'), { code: 'Closed' });
});

test('server close emits close and refuses new clients', async () => {
	const name = unique();
	const server = dp.host(name, () => {});
	const closed = once(server, 'close');
	server.close();
	await closed;
	assert.equal(server.listening, false);
	await assert.rejects(dp.connect(name, { connectTimeout: 200 }), { name: 'DatapathError' });
});

test('handshakeTimeout is accepted and spares finished handshakes', async () => {
	const { server, client } = await echoPair({ host: { handshakeTimeout: 50 } });
	await new Promise((resolve) => setTimeout(resolve, 200));
	const reply = nextMessage(client);
	client.send('still here');
	assert.equal((await reply).toString(), 'still here');
	client.close();
	server.close();
});

test('custom context', async () => {
	const context = new dp.Context({ ioThreads: 2, spin: 0 });
	const { server, client } = await echoPair({ host: { context }, connect: { context } });
	const reply = nextMessage(client);
	client.send('ctx');
	assert.equal((await reply).toString(), 'ctx');
	client.close();
	server.close();
	await assert.rejects(dp.connect('x', { context: {} }), TypeError);
});

test('bad arguments throw TypeError', async () => {
	const { server, client } = await echoPair();
	assert.throws(() => client.send(42), TypeError);
	assert.throws(() => client.sendv('nope'), TypeError);
	assert.throws(() => dp.host(5), TypeError);
	await assert.rejects(dp.connect(5), TypeError);
	client.close();
	server.close();
});

// These run in a child process because they decide whether the process exits.
function runFixture(name) {
	return spawnSync(process.execPath, [path.join(__dirname, 'fixtures', name)], {
		encoding: 'utf8',
		timeout: 20000,
	});
}

test('unref() lets the process exit with open connections', () => {
	const r = runFixture('unref.js');
	assert.equal(r.status, 0, r.stderr);
	assert.match(r.stdout, /ready/);
});

test('a throwing listener does not lose later messages', () => {
	const r = runFixture('throwing-listener.js');
	assert.equal(r.status, 0, r.stderr);
	assert.match(r.stdout, /received 3/);
});

test('worker threads can use and tear down sockets', () => {
	const r = runFixture('worker.js');
	assert.equal(r.status, 0, r.stderr);
	assert.match(r.stdout, /worker done/);
});

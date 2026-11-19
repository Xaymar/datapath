/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Round-trip latency of one ping-pong connection between two Node.js processes:
// this one sends, a child process echoes. Runs the addon and, as a reference,
// node:net on the same platform IPC (Unix socket / named pipe) with a 4-byte
// length prefix for framing.
//
//   node test/bench.mjs [iterations]

import { fork } from 'node:child_process';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const dp = require('..');

const iterations = Number(process.argv[2] === '--echo' ? 0 : (process.argv[2] ?? 20000));
const warmup = Math.min(2000, iterations / 10);
const sizes = [64, 4096, 65536, 1 << 20];

function stats(samples) {
	samples.sort((a, b) => a - b);
	const at = (q) => samples[Math.min(samples.length - 1, Math.floor(q * samples.length))];
	const us = (ns) => (Number(ns) / 1000).toFixed(1).padStart(8);
	return `p50 ${us(at(0.5))} us  p99 ${us(at(0.99))} us  p99.9 ${us(at(0.999))} us  max ${us(samples.at(-1))} us`;
}

async function pingPong(send, onReply, size) {
	const payload = Buffer.alloc(size, 0x5a);
	const samples = [];
	for (let i = 0; i < iterations + warmup; i++) {
		const t0 = process.hrtime.bigint();
		await new Promise((resolve) => {
			onReply(resolve);
			send(payload);
		});
		if (i >= warmup) samples.push(Number(process.hrtime.bigint() - t0));
	}
	return stats(samples);
}

// Start the echo side in a child process; resolves once it listens.
function startEcho(transport, where) {
	const child = fork(new URL(import.meta.url).pathname.replace(/^\/(\w:)/, '$1'), ['--echo', transport, where]);
	return new Promise((resolve, reject) => {
		child.once('message', () => resolve(child));
		child.once('error', reject);
		child.once('exit', (code) => reject(new Error(`echo child exited with ${code}`)));
	});
}

function stopEcho(child) {
	child.removeAllListeners('exit');
	child.kill();
}

async function benchDatapath(size) {
	const name = `dp-node-bench-${process.pid}-${size}`;
	const child = await startEcho('datapath', name);
	const client = await dp.connect(name);
	let waiter;
	client.on('message', () => waiter());
	const r = await pingPong((b) => client.send(b), (fn) => (waiter = fn), size);
	client.close();
	stopEcho(child);
	return r;
}

// node:net needs framing; use a 4-byte length prefix like most IPC protocols.
function framed(sock, onMessage) {
	let buf = Buffer.alloc(0);
	sock.on('data', (chunk) => {
		buf = buf.length ? Buffer.concat([buf, chunk]) : chunk;
		while (buf.length >= 4) {
			const n = buf.readUInt32LE(0);
			if (buf.length < 4 + n) break;
			onMessage(buf.subarray(4, 4 + n));
			buf = buf.subarray(4 + n);
		}
	});
	return (data) => {
		const head = Buffer.allocUnsafe(4);
		head.writeUInt32LE(data.length, 0);
		sock.cork();
		sock.write(head);
		sock.write(data);
		sock.uncork();
	};
}

async function benchNet(size) {
	const where =
		process.platform === 'win32'
			? `\\\\.\\pipe\\dp-node-bench-net-${process.pid}-${size}`
			: path.join(os.tmpdir(), `dp-node-bench-net-${process.pid}-${size}.sock`);
	const child = await startEcho('net', where);
	const client = net.connect(where);
	await new Promise((r) => client.once('connect', r));
	let waiter;
	const send = framed(client, () => waiter());
	const r = await pingPong(send, (fn) => (waiter = fn), size);
	client.destroy();
	stopEcho(child);
	return r;
}

if (process.argv[2] === '--echo') {
	const [, , , transport, where] = process.argv;
	if (transport === 'datapath') {
		dp.host(where, { maxMessageSize: 64 << 20 }, (sock) => sock.on('message', (d) => sock.send(d)));
	} else {
		const server = net.createServer((sock) => {
			const send = framed(sock, (m) => send(m));
		});
		await new Promise((r) => server.listen(where, r));
	}
	process.send('ready');
	await new Promise(() => {});
}

console.log(`node ${process.version} ${process.platform}-${process.arch}, ${iterations} round trips per size`);
for (const size of sizes) {
	const label = size >= 1 << 20 ? `${size >> 20} MiB` : size >= 1024 ? `${size >> 10} KiB` : `${size} B`;
	console.log(`${label.padStart(7)} datapath  ${await benchDatapath(size)}`);
	console.log(`${label.padStart(7)} node:net  ${await benchNet(size)}`);
}

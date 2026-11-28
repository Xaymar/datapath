# @xaymar/datapath
DataPath is an IPC library for fast and predictable messaging between processes on the same machine made in C++11 (or newer). Designed from the ground up to care about the worst case it is able to outperform or match existing solutions such as boost interprocess, libuv, Mojo and others. It allows for packets from a few bytes all the way to many megabytes between one server and any number of clients. The exact implementation varies by platform and makes use of the best measured available systems.

These are the official bindings for [DataPath](https://github.xaymar.com/datapath/)

## Features
- **Low Latency, High Bandwidth.** Heavily optimized for even the worst of the worst cases, achieving under 100µs even under near 100% system load for up to 4 KiB messages.
- **Thread-safe by design.** Messages are never interleaved and `send()` can be used from any thread without blocking.
- **No fragmentation.** Every `send()` message arrives as exactly one `message()` event.
- **Cross-Platform by default.** Windows, Linux, macOS supported by design.
- **Access Control built in.** Set `User`, `Group` and `World` access to exactly what you need.
- **One API. One Standard.** The same API available on every platform and even in the official bindings.
- **Near Zero Dependencies.** Only the standard C++ library is required and a functional OS.
- **Built to KISS.** The API is kept as simple as possible so mistakes are much harder to make.

### Node specific Features
- **Built with Node API.** One binary works on every Node.JS release from 18.x onwards. Electron may work too (untested).
- **Interoperable with any DataPath client or server.** The same C++ library is included directly and linked statically so no separate protocol.
- **Typescript included.** No need to guess, Typescript has you covered.

### Node specific Differences
* **Servers and Sockets keep Node.JS alive.** Event driven apps don't need a separate main loop and can enjoy the same behavior as `net.Socket` - or they can opt out with `unref()`.
* **Handlers run on the JavaScript thread.** Unlike C++ where message handlers can be on any thread the JavaScript binding runs everything on the JavaScript thread instead. Messages from `send()` are queued up and the event loop is woken up so that bursts of messages act as one long time slice for the JavaScript handlers. Run `npm run bench` on your machine for numbers that matter.
* **Receive backpressure.** That queue is bounded by `highWaterMark` (default 1 MiB, each message counts its size plus 64 bytes). At the mark the socket stops reading from the peer until JavaScript has caught up, so the peer's sends back up and return `false`. A client that sends faster than a server handles messages, by accident or on purpose, can't make the server buffer without bound, and other connections keep running. `pause()` does the same on demand and also holds back `'message'` events until `resume()`. While paused, a close by the peer is reported after the messages that came before it; `close()` while paused drops the waiting messages and emits `'close'` right away.

## Usage
```bash
npm install @xaymar/datapath
```

```js
const datapath = require('@xaymar/datapath');
// or: import { host, connect } from '@xaymar/datapath';

// Server: echo every message.
const server = datapath.host('my-service', (socket) => {
	socket.on('message', (data) => socket.send(data));
	socket.on('close', (reason) => console.log('client left:', reason));
});

// Client.
const socket = await datapath.connect('my-service');
socket.on('message', (data) => console.log(data.toString()));
socket.send('hello');            // string (UTF-8), Buffer, any TypedArray, DataView or ArrayBuffer
socket.sendv(['he', 'llo']);     // one message from several parts, without joining them first
socket.close();
server.close();
```

### API
* `host(path, [options], [onConnection]) → Server` hosts a server. Throws a `DatapathError` such as `AddressInUse`.
* `connect(path, [options]) → Promise<Socket>` connects and resolves after the handshake. Rejects with a `DatapathError` such as `NotFound` or `TimedOut`.
* `Server` emits `'connection' (socket)` and `'close'`. `close()` stops accepting; connected sockets stay open. A client accepted while no `'connection'` listener is attached is closed right away.
* `Socket` emits `'message' (Buffer)`, `'drain'` and `'close' (reason, hadError)`, which fires exactly once. Its methods are `send(data)` and `sendv(parts)`, which return `false` when the send queue is full (`WouldBlock`, the message was not queued: send it again after `'drain'`) and throw `TooLarge` or `Closed`, plus `pause()`, `resume()`, `isPaused()`, `close()`, `ref()` and `unref()`. Its properties are `connected`, `maxMessageSize` and `receiveQueueBytes`.
* `new Context(options)` creates separate I/O threads with their own tuning: `spin`, `ioThreads`, `priority`, `idealCore`, `mmcss`, `alertable`, `yield`, `pairPeer`. Pass it as `options.context`. Without one, everything uses the process-wide default context.
* Options map 1:1 to the C++ `datapath::options` in camelCase: `maxMessageSize`, `maxQueuedBytes`, `receiveBuffer`, `connectTimeout` (ms), `handshakeTimeout` (ms, host only), `permissions` (`'user'`, `'group'`, `'world'`), `backlog`, `maxClients`, `pipeBuffer`. `highWaterMark` (bytes, default 1 MiB) is Node-only, see above; on `host()` it applies to every accepted socket.
* `DatapathError` has `code` (the `datapath::error` name, e.g. `'NotFound'`) and `errno` (its number). `codes` maps names to numbers.

## License
BSD 3-Clause License, see `LICENSE`.

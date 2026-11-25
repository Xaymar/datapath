# @xaymar/datapath

Node.js bindings for [DataPath](https://github.com/Xaymar/datapath), a low latency IPC library for processes on the same machine: whole, length-framed messages over named pipes (Windows) or Unix domain sockets (Linux, Android).

* Built with Node-API (`node-addon-api`, Node-API version 8), so one binary works on every Node.js release from 18 on (Electron should work too, but is untested).
* Talks to the C++ library directly, and links it statically: the `.node` file has no other runtime dependency.
* Interoperates with C++ (and other bindings') peers: it speaks the same protocol.
* TypeScript typings included.

## Usage
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
* `Socket` emits `'message' (Buffer)` and `'close' (reason, hadError)`, which fires exactly once. Its methods are `send(data)` and `sendv(parts)`, which return `false` when the send queue is full (`WouldBlock`) and throw `TooLarge` or `Closed`, plus `close()`, `ref()` and `unref()`. Its properties are `connected` and `maxMessageSize`.
* `new Context(options)` creates separate I/O threads with their own tuning: `spin`, `ioThreads`, `priority`, `idealCore`, `mmcss`, `alertable`, `yield`, `pairPeer`. Pass it as `options.context`. Without one, everything uses the process-wide default context.
* Options map 1:1 to the C++ `datapath::options` in camelCase: `maxMessageSize`, `maxQueuedBytes`, `receiveBuffer`, `connectTimeout` (ms), `permissions` (`'user'`, `'group'`, `'world'`), `backlog`, `maxClients`, `pipeBuffer`.
* `DatapathError` has `code` (the `datapath::error` name, e.g. `'NotFound'`) and `errno` (its number). `codes` maps names to numbers.

The [root README](../../README.md#semantics-worth-knowing) explains paths, backpressure, closing and tuning. These work the same here.

### How it differs from the C++ API
* **Handlers run on the JavaScript thread.** The library calls its handlers on I/O threads. The addon copies each message into a queue and wakes the event loop, which then delivers every queued message. A burst of messages costs one wake-up, not one per message. The extra thread hop adds latency compared to C++; in a two-process ping-pong on a 4-vCPU Linux VM it measured about the same as `node:net` on a Unix socket for 64 B, 30 to 50% slower for 4 to 64 KiB, and about 2x faster for 1 MiB. Run `npm run bench` on your machine for numbers that matter.
* **Messages are copied once** (they are only valid during the C++ handler), into a Buffer the listener owns.
* **Receiving starts with the first `'message'` listener**, like the first `on_message()` in C++. Until then, data waits in the kernel and a close by the peer is not noticed.
* **No receive backpressure.** Messages that arrive faster than JavaScript handles them queue up in memory. Pair it with an application-level acknowledgement if a peer can outrun you.
* **Lifetime.** An open socket or server keeps the process alive, like `net.Socket`. Call `unref()` to opt out. Worker threads can use the module; terminating a worker closes its sockets and servers.

## Installing
`npm install @xaymar/datapath` uses a prebuilt binary when one matches your platform and architecture. Otherwise it compiles from source, which needs CMake 3.16+ and a C++17 compiler. Set `DATAPATH_BUILD_FROM_SOURCE=1` to always compile.

The source build runs from the package's `install` script. npm 12 and newer skip install scripts unless you allow them, so on a platform without a prebuild either allow this package's scripts (`npm approve-scripts`) and reinstall, or build it once by hand with `npm explore @xaymar/datapath -- npm run build`.

## Developing
From `bindings/node` in a checkout of the repository:
```sh
npm install        # installs dependencies and builds the addon from ../../
npm test           # node:test suite
npm run typecheck  # compiles test/types.test.ts against lib/index.d.ts
npm run bench      # round-trip latency vs node:net
```
`npm run build` rebuilds after changes (`npm run build:debug` for a debug build).

### Releasing
The package is publish-ready, but not published yet. The plan for a release:

1. **Prebuilds.** The `Node.js bindings` workflow (`.github/workflows/node.yml`) builds the addon on every target and runs the tests on Node.js 18, 20, 22 and 24. Each job runs `npm run package-prebuild`, which copies `build/Release/datapath.node` to `prebuilds/<platform>-<arch>/datapath.napi.node`, the layout `node-gyp-build` loads from, and uploads it as an artifact. Targets: `win32-x64`, `win32-arm64`, `linux-x64`, `linux-arm64` (glibc), `linux-x64` with musl (Alpine, tagged `.musl`), and `darwin-x64`/`darwin-arm64` (one universal binary). Android has no prebuild: it builds from source, since its Node.js builds are rare and vary.
2. **Pack.** A final job downloads every artifact into `prebuilds/` and runs `npm pack`. `release.yml` attaches that tarball to the draft GitHub Release; it is published to npm by hand. `prepack` copies the C++ sources into `deps/datapath`, so the tarball can also build from source.
3. **Publish.** Run `npm publish --access public` from that job with an `NPM_TOKEN` secret (and `--provenance` for npm provenance), once the workflow has been triggered for a version tag. That step is not wired up yet; it needs the token and the `@xaymar` npm scope.

Since the addon uses Node-API only, a new Node.js release needs no new prebuilds.

## License
BSD 3-Clause License, see `LICENSE`.

# DataPath
DataPath is an IPC library for fast and predictable messaging between processes on the same machine made in C++11 (or newer). Designed from the ground up to care about the worst case it is able to outperform or match existing solutions such as boost interprocess, libuv, Mojo and others. It allows for packets from a few bytes all the way to many megabytes between one server and any number of clients. The exact implementation varies by platform and makes use of the best measured available systems.

## Features
- **Low Latency, High Bandwidth.** Heavily optimized for even the worst of the worst cases, achieving under 100µs even under near 100% system load for up to 4 KiB messages.
- **Thread-safe by design.** Messages are never interleaved and `send()` can be used from any thread without blocking.
- **No fragmentation.** Every `send()` message arrives as exactly one `on_message()` call.
- **Cross-Platform by default.** Windows, Linux, macOS supported by design, with Android and iOS following eventually.
- **Access Control built in.** Set `User`, `Group` and `World` access to exactly what you need.
- **One API. One Standard.** The same API available on every platform and even in the official bindings.
- **Near Zero Dependencies.** Only the standard C++ library is required and a functional OS.
- **Built to KISS.** The API is kept as simple as possible so mistakes are much harder to make.

## Benchmarks
The library has been extensively benchmarked against competing solutions and in almost every situation we're faster or equal to the competition. A value listed as "did not finish" means it exceeded the allowed 30 minute run time. We only measure worst case, which is what matters: Client and Server are not on the same NUMA, not on the same CCD, or at minimum not in the same Core/Thread group.

### Merenka "GPU0": Linux 6.12, AMD Epyc 7313P (restricted to 2 CCDs), `--pin 1,5`
![Round-trip p99.9 latency on Linux: DataPath over shared memory vs libuv, Chromium Mojo, cpp-ipc and Boost.Interprocess](docs/images/latency-p999-linux.svg)

|                                       | 64 B idle | 4 KiB idle | 64 KiB idle | 1 MiB idle | 64 B stress | 4 KiB stress |
|---------------------------------------|----------:|-----------:|------------:|-----------:|------------:|-------------:|
| **DataPath, shared memory**           |   **1.4** |    **3.4** |      **22** |    **321** |      **10** |       **23** |
| cpp-ipc (shared memory)               |       4.7 |        8.2 |          29 |        341 |       5,758 |        8,015 |
| Boost.Interprocess `message_queue`    |        32 |         41 |         128 |      1,155 |         464 |          313 |
| DataPath, Unix socket                 |        13 |         19 |          60 |        479 |         249 |        5,507 |
| libuv + DataPath's spin/priority      |        14 |         20 |          56 |        541 |       3,494 |        4,924 |
| libuv as shipped                      |        23 |         28 |          64 |        557 |       4,070 |        4,076 |
| Chromium Mojo, bindings (main thread) |        62 |         73 |         439 |      8,704 |       7,852 |        7,997 |

### Irkela: Windows 10, AMD Ryzen 7 7940HS, `--pin 8,12`
![Round-trip p99.9 latency on Windows: DataPath over shared memory vs libuv, cpp-ipc and Boost.Interprocess](docs/images/latency-p999-windows.svg)

|                                    | 64 B idle | 4 KiB idle | 64 KiB idle | 1 MiB idle |    64 B stress |   4 KiB stress |
|------------------------------------|----------:|-----------:|------------:|-----------:|---------------:|---------------:|
| **DataPath, shared memory**        |   **2.2** |    **4.1** |      **19** |    **130** |        **5.8** |         **26** |
| cpp-ipc (shared memory)            |        18 |         31 |         153 |        569 | did not finish | did not finish |
| Boost.Interprocess `message_queue` |       1.1 |        4.1 |          55 |      6,310 | did not finish | did not finish |
| DataPath, named pipe               |       6.9 |         17 |          50 |        754 |             40 |            113 |
| libuv + DataPath's spin/priority   |        15 |         22 |          74 |      9,659 |            104 |            187 |
| libuv as shipped                   |        31 |         35 |          66 |      1,075 |            265 |            401 |

## Usage
DataPath is available for several languages either as a binding or native integration.

### C++
```cpp
#include <datapath/datapath.hpp>

// Server
std::shared_ptr<datapath::server> server;
std::vector<std::shared_ptr<datapath::socket>> clients;
datapath::host(server, "my-service");          // \\.\pipe\my-service, or the abstract socket "datapath.my-service"
server->on_accept([&](std::shared_ptr<datapath::socket> client) {
    datapath::socket* raw = client.get();      // don't capture the shared_ptr in its own handler
    client->on_message([raw](const uint8_t* data, size_t size) {
        raw->send(data, size);                 // echo; send() is safe from handlers
    });
    clients.push_back(client);                 // keep it, or the connection closes
    return true;                               // false rejects; the client's connect() returns Closed
});

// Client
std::shared_ptr<datapath::socket> sock;
if (datapath::connect(sock, "my-service") == datapath::error::Success) {
    sock->on_message([](const uint8_t* data, size_t size) { /* data valid during the call */ });
    sock->send("hello", 5);
}
```
See `samples/echo/main.cpp` for a complete program.

### C
```c
#include <datapath/datapath.h>

static void echo(void* user, datapath_socket* sock, const uint8_t* data, size_t size) {
    datapath_socket_send(sock, data, size);                // safe from callbacks
}
static int on_accept(void* user, datapath_server* server, datapath_socket* client) {
    datapath_socket_set_on_message(client, echo, NULL, NULL);
    return 1;                                              // accepted: datapath_socket_release() it when done
}
static void on_reply(void* user, datapath_socket* sock, const uint8_t* data, size_t size) {
    /* data valid during the call */
}

// Server
datapath_server* server = NULL;
datapath_host("my-service", NULL, NULL, &server);          // NULL: default options and context
datapath_server_set_on_accept(server, on_accept, NULL, NULL);

// Client
datapath_socket* sock = NULL;
if (datapath_connect("my-service", NULL, NULL, &sock) == DATAPATH_SUCCESS) {
    datapath_socket_set_on_message(sock, on_reply, NULL, NULL);
    datapath_socket_send(sock, "hello", 5);
}
```

### [Rust](https://crates.io/crates/com-xaymar-datapath)
```bash
cargo add com-xaymar-datapath --rename datapath
```

```rust
use datapath::Socket;
use std::sync::{Arc, Mutex};

// Server
let server = datapath::host("my-service")?;            // same paths as the C++ API
let clients = Arc::new(Mutex::new(Vec::<Socket>::new()));
let keep = clients.clone();
server.on_accept(move |client: Socket| {
    client.on_message(|sock, data| { let _ = sock.send(data); }).ok();  // echo
    keep.lock().unwrap().push(client);                 // keep it, or the connection closes
})?;

// Client
let sock = datapath::connect("my-service")?;
sock.on_message(|_, data| println!("got {} bytes", data.len()))?;  // data valid during the call
sock.send(b"hello")?;
```

See `bindings/rust/datapath/examples/echo.rs` for a complete program.

### [Python](https://pypi.org/project/com.xaymar.datapath/)
```bash
pip install com.xaymar.datapath
```

```python
import datapath

# Server
clients = []
def on_accept(client):
    clients.append(client)                             # keep it, or the connection closes
    client.on_message(lambda data: client.send(data))  # echo; runs on an I/O thread

server = datapath.host("my-service", on_accept=on_accept)

# Client
sock = datapath.connect("my-service")
sock.on_message(lambda data: print(data))              # b'hello'
sock.send(b"hello")
```

### [Node.js](https://www.npmjs.com/package/@xaymar/datapath)
```bash
npm install @xaymar/datapath
```
```js
const datapath = require('@xaymar/datapath');

// Server
const server = datapath.host('my-service', (client) => {
    client.on('message', (data) => client.send(data));  // echo
});

// Client
const sock = await datapath.connect('my-service');
sock.on('message', (data) => console.log(data.toString()));  // 'hello'
sock.send('hello');
```

## License
BSD 3-Clause License, see [`LICENSE`](LICENSE).

// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::mpsc::{channel, Receiver};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use datapath::{Context, ContextOptions, Error, Options, Server, Socket};

const WAIT: Duration = Duration::from_secs(5);

fn unique_name() -> String {
    static N: AtomicUsize = AtomicUsize::new(0);
    format!("dp-rust-test-{}-{}", std::process::id(), N.fetch_add(1, Ordering::Relaxed))
}

/// An echo server that keeps its clients until dropped.
struct Echo {
    _server: Server,
    clients: Arc<Mutex<Vec<Socket>>>,
}

fn echo_server(name: &str, ctx: Option<&Context>) -> Echo {
    let server = Server::host(name, &Options::default(), ctx).expect("host");
    let clients = Arc::new(Mutex::new(Vec::new()));
    let keep = clients.clone();
    server
        .on_accept(move |client: Socket| {
            client
                .on_message(|s, data| {
                    s.send(data).expect("echo send");
                })
                .expect("on_message");
            keep.lock().unwrap().push(client);
        })
        .expect("on_accept");
    Echo { _server: server, clients }
}

fn collect(socket: &Socket) -> Receiver<Vec<u8>> {
    let (tx, rx) = channel();
    socket
        .on_message(move |_, data| {
            let _ = tx.send(data.to_vec());
        })
        .unwrap();
    rx
}

#[test]
fn defaults_round_trip() {
    let o = Options::default();
    assert_eq!(o.max_message_size, 64 << 20);
    assert_eq!(o.connect_timeout, Duration::from_millis(1000));
    let c = ContextOptions::default();
    assert_eq!(c.io_threads, 1);
    assert_eq!(c.spin, Duration::from_micros(50));
    let (abi, _patch) = datapath::version();
    let _ = abi;
}

#[test]
fn echo_round_trip() {
    let name = unique_name();
    let _echo = echo_server(&name, None);
    let socket = datapath::connect(&name).expect("connect");
    assert!(socket.is_open());
    assert!(socket.max_message_size() > 0);
    let rx = collect(&socket);

    for msg in [&b"hello"[..], &[][..], &vec![0xAB; 1 << 20][..]] {
        socket.send(msg).unwrap();
        assert_eq!(rx.recv_timeout(WAIT).unwrap(), msg);
    }
}

#[test]
fn send_vectored_joins_parts() {
    let name = unique_name();
    let _echo = echo_server(&name, None);
    let socket = datapath::connect(&name).unwrap();
    let rx = collect(&socket);

    socket.send_vectored(&[b"ab", b"", b"cd"]).unwrap();
    assert_eq!(rx.recv_timeout(WAIT).unwrap(), b"abcd");

    // More parts than fit on the stack.
    let parts: Vec<&[u8]> = (0..40).map(|_| &b"x"[..]).collect();
    socket.send_vectored(&parts).unwrap();
    assert_eq!(rx.recv_timeout(WAIT).unwrap(), vec![b'x'; 40]);
}

#[test]
fn own_context() {
    let ctx = Context::new(&ContextOptions::default()).unwrap();
    let name = unique_name();
    let _echo = echo_server(&name, Some(&ctx));
    let socket = Socket::connect(&name, &Options::default(), Some(&ctx)).unwrap();
    let rx = collect(&socket);
    socket.send(b"ctx").unwrap();
    assert_eq!(rx.recv_timeout(WAIT).unwrap(), b"ctx");
}

#[test]
fn errors() {
    let mut o = Options::default();
    o.connect_timeout = Duration::from_millis(200);
    assert_eq!(Socket::connect(&unique_name(), &o, None).unwrap_err(), Error::NotFound);
    assert_eq!(datapath::connect("bad\0path").unwrap_err(), Error::InvalidPath);

    let name = unique_name();
    let _echo = echo_server(&name, None);
    assert_eq!(datapath::host(&name).unwrap_err(), Error::AddressInUse);
    assert_eq!(Error::WouldBlock.to_string(), "WouldBlock");
}

#[test]
fn too_large() {
    let name = unique_name();
    let mut o = Options::default();
    o.max_message_size = 1024;
    let server = Server::host(&name, &o, None).unwrap();
    let keep = Arc::new(Mutex::new(Vec::new()));
    let k = keep.clone();
    server.on_accept(move |c| k.lock().unwrap().push(c)).unwrap();
    let socket = Socket::connect(&name, &o, None).unwrap();
    assert_eq!(socket.max_message_size(), 1024);
    assert_eq!(socket.send(&[0; 2048]).unwrap_err(), Error::TooLarge);
}

#[test]
fn close_handler_fires_when_peer_goes() {
    let name = unique_name();
    let echo = echo_server(&name, None);
    let socket = datapath::connect(&name).unwrap();
    // A peer's close is seen by the receive path, which starts with on_message.
    let _rx = collect(&socket);
    let (tx, rx) = channel();
    socket
        .on_close(move |s, reason| {
            let _ = tx.send((reason, s.is_open()));
        })
        .unwrap();
    // Wait for the server to hold the client, then drop it.
    while echo.clients.lock().unwrap().is_empty() {
        std::thread::sleep(Duration::from_millis(1));
    }
    echo.clients.lock().unwrap().clear();
    let (_reason, open) = rx.recv_timeout(WAIT).unwrap();
    assert!(!open);
    assert_eq!(socket.send(b"x").unwrap_err(), Error::Closed);
}

#[test]
fn reject_by_dropping() {
    let name = unique_name();
    let server = datapath::host(&name).unwrap();
    server.on_accept(drop).unwrap();
    // The client either fails to connect or sees the connection close.
    if let Ok(socket) = datapath::connect(&name) {
        let (tx, rx) = channel();
        socket.on_close(move |_, r| tx.send(r).unwrap()).unwrap();
        if socket.is_open() {
            rx.recv_timeout(WAIT).unwrap();
        }
    }
}

#[test]
fn panicking_handler_is_contained() {
    let name = unique_name();
    let _echo = echo_server(&name, None);
    let socket = datapath::connect(&name).unwrap();
    let (tx, rx) = channel();
    socket
        .on_message(move |_, data| {
            if data == b"boom" {
                panic!("test panic");
            }
            tx.send(data.to_vec()).unwrap();
        })
        .unwrap();
    socket.send(b"boom").unwrap();
    socket.send(b"after").unwrap();
    assert_eq!(rx.recv_timeout(WAIT).unwrap(), b"after");
}

#[test]
fn handler_owning_its_socket_is_released() {
    // A socket moved into its own handler is dropped from user_free when the
    // connection ends; that must not call back into the library on the spot.
    let name = unique_name();
    let echo = echo_server(&name, None);
    let socket = Arc::new(datapath::connect(&name).unwrap());
    let (tx, rx) = channel();
    let me = socket.clone();
    socket
        .on_message(move |_, d| {
            let _keep = &me;
            let _ = tx.send(d.to_vec());
        })
        .unwrap();
    socket.send(b"ping").unwrap();
    assert_eq!(rx.recv_timeout(WAIT).unwrap(), b"ping");
    drop(socket);
    echo.clients.lock().unwrap().clear();
    // The handler (and the last Arc<Socket>) go when the connection closes.
    let start = std::time::Instant::now();
    while rx.recv_timeout(Duration::from_millis(10)).is_ok() || start.elapsed() < Duration::from_millis(200) {}
}

#[test]
fn drop_server_inside_accept() {
    let name = unique_name();
    let slot: Arc<Mutex<Option<Server>>> = Arc::new(Mutex::new(None));
    let server = datapath::host(&name).unwrap();
    let s = slot.clone();
    let (tx, rx) = channel();
    server
        .on_accept(move |client| {
            drop(s.lock().unwrap().take());
            tx.send(client).unwrap();
        })
        .unwrap();
    *slot.lock().unwrap() = Some(server);
    let _socket = datapath::connect(&name).unwrap();
    let _client = rx.recv_timeout(WAIT).unwrap();
    assert!(slot.lock().unwrap().is_none());
}

#[test]
fn many_clients_threads() {
    let name = unique_name();
    let _echo = echo_server(&name, None);
    let handles: Vec<_> = (0..8)
        .map(|i| {
            let name = name.clone();
            std::thread::spawn(move || {
                let socket = datapath::connect(&name).unwrap();
                let rx = collect(&socket);
                for j in 0..200u32 {
                    let msg = [i as u8, (j & 0xff) as u8, (j >> 8) as u8];
                    loop {
                        match socket.send(&msg) {
                            Err(Error::WouldBlock) => std::thread::yield_now(),
                            r => break r.unwrap(),
                        }
                    }
                    assert_eq!(rx.recv_timeout(WAIT).unwrap(), msg);
                }
            })
        })
        .collect();
    for h in handles {
        h.join().unwrap();
    }
}

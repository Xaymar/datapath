// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

#![cfg(any(target_os = "linux", target_os = "android"))]

use datapath_core::{CloseReason, Context, Error, Options, SendError, Server, Socket};
use std::io::IoSlice;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::mpsc::{channel, Receiver, Sender};
use std::sync::{Arc, Mutex};
use std::time::Duration;

static COUNTER: AtomicUsize = AtomicUsize::new(0);

fn unique_name(tag: &str) -> String {
    format!(
        "rt-{}-{}-{}",
        std::process::id(),
        tag,
        COUNTER.fetch_add(1, Ordering::Relaxed)
    )
}

const WAIT: Duration = Duration::from_secs(10);

/// A server that echoes every message back to its sender.
fn echo_server(name: &str, ctx: &Context) -> (Server, Arc<Mutex<Vec<datapath_core::Socket>>>) {
    let sockets = Arc::new(Mutex::new(Vec::new()));
    let keep = sockets.clone();
    let server = Server::host(name)
        .context(ctx)
        .on_accept(move |incoming| {
            // The echo needs the socket's own sender inside its handler: set the
            // handler after accepting.
            let socket = incoming.accept();
            let tx = socket.sender();
            socket.set_on_message(move |msg| {
                let _ = tx.send(msg);
            });
            keep.lock().unwrap().push(socket);
        })
        .open()
        .expect("host");
    (server, sockets)
}

fn collect(tx: Sender<Vec<u8>>) -> impl FnMut(&[u8]) + Send + 'static {
    move |m| {
        let _ = tx.send(m.to_vec());
    }
}

fn recv_n(rx: &Receiver<Vec<u8>>, n: usize) -> Vec<Vec<u8>> {
    (0..n)
        .map(|_| rx.recv_timeout(WAIT).expect("message"))
        .collect()
}

#[test]
fn echoes_messages_in_order() {
    let ctx = Context::new().unwrap();
    let name = unique_name("echo");
    let (_server, _keep) = echo_server(&name, &ctx);
    let (tx, rx) = channel();
    let client = Socket::connect(&name)
        .context(&ctx)
        .on_message(collect(tx))
        .open()
        .unwrap();
    for i in 0..1000u32 {
        client.send(&i.to_le_bytes()).unwrap();
    }
    for (i, m) in recv_n(&rx, 1000).into_iter().enumerate() {
        assert_eq!(m, (i as u32).to_le_bytes());
    }
}

#[test]
fn large_and_vectored_messages_arrive_whole() {
    let ctx = Context::new().unwrap();
    let name = unique_name("large");
    let (_server, _keep) = echo_server(&name, &ctx);
    let (tx, rx) = channel();
    let client = Socket::connect(&name)
        .context(&ctx)
        .on_message(collect(tx))
        .open()
        .unwrap();

    let big: Vec<u8> = (0..3_000_000u32).map(|i| (i % 251) as u8).collect();
    client.send(&big).unwrap();
    assert_eq!(rx.recv_timeout(WAIT).unwrap(), big);

    client
        .send_vectored(&[
            IoSlice::new(b"head"),
            IoSlice::new(b""),
            IoSlice::new(b"tail"),
        ])
        .unwrap();
    assert_eq!(rx.recv_timeout(WAIT).unwrap(), b"headtail");

    client.send(b"").unwrap();
    assert_eq!(rx.recv_timeout(WAIT).unwrap(), b"");
}

#[test]
fn send_errors_are_typed() {
    let ctx = Context::new().unwrap();
    let name = unique_name("errors");
    let (_server, _keep) = echo_server(&name, &ctx);
    let opts = Options {
        max_message_size: 1024,
        ..Options::default()
    };
    let client = Socket::connect(&name)
        .context(&ctx)
        .options(opts)
        .open()
        .unwrap();
    assert_eq!(client.max_message_size(), 1024);
    assert_eq!(client.send(&[0; 2048]), Err(SendError::TooLarge));
    client.close();
    assert!(!client.is_open());
    assert_eq!(client.send(b"x"), Err(SendError::Closed));
}

#[test]
fn closing_reports_the_reason_once() {
    let ctx = Context::new().unwrap();
    let name = unique_name("close");
    let (server, keep) = echo_server(&name, &ctx);
    let (tx, rx) = channel();
    let _client = Socket::connect(&name)
        .context(&ctx)
        .on_message(|_| {})
        .on_close(move |r| tx.send(r).unwrap())
        .open()
        .unwrap();
    // Wait until the server side exists, then drop it: the client sees the peer go.
    while keep.lock().unwrap().is_empty() {
        std::thread::sleep(Duration::from_millis(1));
    }
    keep.lock().unwrap().clear();
    assert_eq!(rx.recv_timeout(WAIT).unwrap(), CloseReason::Remote);
    assert!(
        rx.recv_timeout(Duration::from_millis(100)).is_err(),
        "fires once"
    );
    drop(server);
}

#[test]
fn local_close_fires_the_close_handler() {
    let ctx = Context::new().unwrap();
    let name = unique_name("local");
    let (_server, _keep) = echo_server(&name, &ctx);
    let (tx, rx) = channel();
    let client = Socket::connect(&name)
        .context(&ctx)
        .on_message(|_| {})
        .on_close(move |r| tx.send(r).unwrap())
        .open()
        .unwrap();
    client.close();
    assert_eq!(rx.recv_timeout(WAIT).unwrap(), CloseReason::Local);
}

#[test]
fn dropping_a_socket_runs_no_handler_afterwards() {
    let ctx = Context::new().unwrap();
    let name = unique_name("drop");
    let (_server, _keep) = echo_server(&name, &ctx);
    let hits = Arc::new(AtomicUsize::new(0));
    let closes = Arc::new(AtomicUsize::new(0));
    let (h, c) = (hits.clone(), closes.clone());
    let client = Socket::connect(&name)
        .context(&ctx)
        .on_message(move |_| {
            h.fetch_add(1, Ordering::SeqCst);
        })
        .on_close(move |_| {
            c.fetch_add(1, Ordering::SeqCst);
        })
        .open()
        .unwrap();
    let sender = client.sender();
    drop(client);
    let after = hits.load(Ordering::SeqCst);
    assert_eq!(sender.send(b"late"), Err(SendError::Closed));
    std::thread::sleep(Duration::from_millis(100));
    assert_eq!(hits.load(Ordering::SeqCst), after);
    assert_eq!(
        closes.load(Ordering::SeqCst),
        0,
        "release does not fire on_close"
    );
}

#[test]
fn rejecting_makes_connect_fail_closed() {
    let ctx = Context::new().unwrap();
    let name = unique_name("reject");
    let _server = Server::host(&name)
        .context(&ctx)
        .on_accept(drop)
        .open()
        .unwrap();
    assert_eq!(
        Socket::connect(&name).context(&ctx).open().err(),
        Some(Error::Closed)
    );
}

#[test]
fn connecting_to_nothing_is_not_found() {
    let ctx = Context::new().unwrap();
    let name = unique_name("nothing");
    assert_eq!(
        Socket::connect(&name).context(&ctx).open().err(),
        Some(Error::NotFound)
    );
    assert_eq!(
        Socket::connect("").context(&ctx).open().err(),
        Some(Error::InvalidPath)
    );
}

#[test]
fn a_second_server_on_the_same_name_is_refused() {
    let ctx = Context::new().unwrap();
    let name = unique_name("twice");
    let _a = Server::host(&name)
        .context(&ctx)
        .on_accept(drop)
        .open()
        .unwrap();
    assert_eq!(
        Server::host(&name)
            .context(&ctx)
            .on_accept(drop)
            .open()
            .err(),
        Some(Error::AddressInUse)
    );
}

#[test]
fn a_panicking_handler_closes_only_that_connection() {
    let ctx = Context::new().unwrap();
    let name = unique_name("panic");
    let (_server, _keep) = echo_server(&name, &ctx);
    let (ctx_tx, ctx_rx) = channel();
    let bad = Socket::connect(&name)
        .context(&ctx)
        .on_message(|_| panic!("handler bug"))
        .on_close(move |r| ctx_tx.send(r).unwrap())
        .open()
        .unwrap();
    bad.send(b"boom").unwrap();
    assert_eq!(
        ctx_rx.recv_timeout(WAIT).unwrap(),
        CloseReason::HandlerPanic
    );

    // The I/O thread survived: another connection on the same context works.
    let (tx, rx) = channel();
    let good = Socket::connect(&name)
        .context(&ctx)
        .on_message(collect(tx))
        .open()
        .unwrap();
    good.send(b"still alive").unwrap();
    assert_eq!(rx.recv_timeout(WAIT).unwrap(), b"still alive");
}

#[test]
fn oversized_peer_messages_close_with_a_protocol_error() {
    let ctx = Context::new().unwrap();
    let name = unique_name("toolarge");
    let (tx, rx) = channel();
    let tx = Mutex::new(tx);
    let keep = Arc::new(Mutex::new(Vec::new()));
    let k = keep.clone();
    let _server = Server::host(&name)
        .context(&ctx)
        .max_message_size(1000)
        .on_accept(move |incoming| {
            let tx = tx.lock().unwrap().clone();
            let s = incoming
                .on_message(|_| {})
                .on_close(move |r| tx.send(r).unwrap())
                .accept();
            k.lock().unwrap().push(s);
        })
        .open()
        .unwrap();
    // The client's own limit is larger, but the handshake lowers it to 1000.
    let client = Socket::connect(&name).context(&ctx).open().unwrap();
    assert_eq!(client.max_message_size(), 1000);
    assert_eq!(client.send(&[0; 5000]), Err(SendError::TooLarge));
    assert!(rx.recv_timeout(Duration::from_millis(100)).is_err());
}

#[test]
fn shutting_down_the_context_closes_everything() {
    let ctx = Context::new().unwrap();
    let name = unique_name("shutdown");
    let (server, keep) = echo_server(&name, &ctx);
    let (tx, rx) = channel();
    let client = Socket::connect(&name)
        .context(&ctx)
        .on_message(|_| {})
        .on_close(move |r| tx.send(r).unwrap())
        .open()
        .unwrap();
    while keep.lock().unwrap().is_empty() {
        std::thread::sleep(Duration::from_millis(1));
    }
    drop(ctx);
    assert!(
        rx.recv_timeout(WAIT).is_ok(),
        "close handler fires on shutdown"
    );
    assert!(!client.is_open());
    drop(server);
}

#[test]
fn many_clients_at_once() {
    let ctx = Context::builder().io_threads(2).build().unwrap();
    let name = unique_name("many");
    let (_server, _keep) = echo_server(&name, &ctx);
    let threads: Vec<_> = (0..8)
        .map(|t| {
            let (ctx, name) = (ctx.clone(), name.clone());
            std::thread::spawn(move || {
                let (tx, rx) = channel();
                let c = Socket::connect(&name)
                    .context(&ctx)
                    .on_message(collect(tx))
                    .open()
                    .unwrap();
                for i in 0..500u32 {
                    c.send(&[t as u8, i as u8]).unwrap();
                }
                for i in 0..500u32 {
                    assert_eq!(rx.recv_timeout(WAIT).unwrap(), [t as u8, i as u8]);
                }
            })
        })
        .collect();
    for t in threads {
        t.join().unwrap();
    }
}

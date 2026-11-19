// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! Echo server and client in one process.
//!
//! ```text
//! cargo run --example echo
//! ```

use std::sync::mpsc::channel;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

fn main() -> datapath::Result<()> {
    let name = format!("dp-rust-echo-{}", std::process::id());

    let server = datapath::host(&name)?;
    let clients = Arc::new(Mutex::new(Vec::new()));
    let keep = clients.clone();
    server.on_accept(move |client: datapath::Socket| {
        client
            .on_message(|socket, data| {
                let _ = socket.send(data);
            })
            .ok();
        keep.lock().unwrap().push(client);
    })?;

    let socket = datapath::connect(&name)?;
    let (tx, rx) = channel();
    socket.on_message(move |_, data| {
        let _ = tx.send(data.len());
    })?;

    let rounds = 10_000;
    let payload = [0u8; 64];
    let start = Instant::now();
    for _ in 0..rounds {
        socket.send(&payload)?;
        rx.recv_timeout(Duration::from_secs(5)).expect("echo reply");
    }
    let per = start.elapsed() / rounds;
    println!("{rounds} round trips of {} B, {per:?} each on average", payload.len());
    Ok(())
}

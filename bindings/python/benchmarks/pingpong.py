# Low Latency IPC Library for high-speed traffic
# Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
#
# SPDX-License-Identifier: BSD-3-Clause

"""Round-trip latency of the Python bindings.

Speaks the same protocol as samples/benchmark (first byte 'P' = echo, 'Q' = quit),
so either side can be the C++ `datapath-benchmark server`:

    python pingpong.py local                         # Python server + Python client
    python pingpong.py local --cpp-server PATH       # C++ echo server, Python client
    python pingpong.py server NAME [--mode recv|callback]
    python pingpong.py client NAME [--mode recv|callback]

--mode recv     : the client thread waits in recv() (deprecated since 1.1, removed in 2.0)
--mode callback : the next ping is sent from the on_message callback on the I/O thread
"""

from __future__ import annotations

import argparse
import os
import statistics
import subprocess
import sys
import threading
import time

import datapath

TAG_PING = ord("P")
TAG_QUIT = ord("Q")


def make_context(args) -> datapath.Context:
    return datapath.Context(spin_us=args.spin, priority=args.priority)


def run_server(args) -> int:
    ctx = make_context(args)
    quit_event = threading.Event()
    server = datapath.host(args.name, context=ctx)

    if args.mode == "callback":
        clients = []

        def on_accept(sock: datapath.Socket) -> None:
            clients.append(sock)

            def on_message(d: bytes) -> None:
                if d[:1] == b"Q":
                    quit_event.set()
                elif d:
                    sock.send(d)

            sock.on_message(on_message)

        server.on_accept(on_accept)
        print("ready", flush=True)
        quit_event.wait()
    else:
        print("ready", flush=True)
        sock = server.accept()
        sock.recv_spin_us = args.recv_spin
        recv, send = sock.recv, sock.send
        try:
            while True:
                d = recv()
                if d[:1] == b"Q":
                    break
                send(d)
        except datapath.ClosedError:
            pass
    return 0


def percentile(sorted_samples, p: float) -> float:
    i = min(len(sorted_samples) - 1, int(p / 100.0 * len(sorted_samples)))
    return sorted_samples[i]


def measure_recv(sock: datapath.Socket, payload: bytes, warmup: int, count: int) -> list[int]:
    send, recv, now = sock.send, sock.recv, time.perf_counter_ns
    for _ in range(warmup):
        send(payload)
        recv()
    samples = [0] * count
    for i in range(count):
        t0 = now()
        send(payload)
        recv()
        samples[i] = now() - t0
    return samples


def measure_callback(sock: datapath.Socket, payload: bytes, warmup: int, count: int) -> list[int]:
    samples: list[int] = []
    done = threading.Event()
    now = time.perf_counter_ns
    state = {"n": 0, "t0": 0}
    total = warmup + count

    def on_message(_: bytes) -> None:
        n = state["n"]
        if n >= warmup:
            samples.append(now() - state["t0"])
        n += 1
        state["n"] = n
        if n < total:
            state["t0"] = now()
            sock.send(payload)
        else:
            done.set()

    sock.on_message(on_message)
    state["t0"] = now()
    sock.send(payload)
    done.wait()
    sock.on_message(None)
    return samples


def run_client(args) -> int:
    ctx = make_context(args)
    sock = datapath.connect(args.name, context=ctx, connect_timeout=5.0)
    sock.recv_spin_us = args.recv_spin
    sizes = [int(s) for s in args.sizes.split(",")]
    print(f"python {sys.version.split()[0]}, mode={args.mode}, spin={args.spin} us, "
          f"recv spin={args.recv_spin} us, runs={args.runs}")
    print(f"{'size':>10} {'count':>8} {'p50':>9} {'p99':>9} {'p99.9':>9} {'max':>9}   (us, median of runs)")
    measure = measure_callback if args.mode == "callback" else measure_recv
    for size in sizes:
        payload = bytes([TAG_PING]) + bytes(max(0, size - 1))
        count = max(50, min(args.count, (2 << 30) // max(size, 1) // 4))
        warmup = max(10, count // 20)
        runs = []
        for _ in range(args.runs):
            s = sorted(measure(sock, payload, warmup, count))
            runs.append([percentile(s, 50), percentile(s, 99), percentile(s, 99.9), s[-1]])
        med = [statistics.median(r[i] for r in runs) / 1000.0 for i in range(4)]
        print(f"{size:>10} {count:>8} {med[0]:>9.1f} {med[1]:>9.1f} {med[2]:>9.1f} {med[3]:>9.1f}")
    sock.send(bytes([TAG_QUIT]))
    time.sleep(0.05)
    return 0


def run_local(args) -> int:
    args.name = f"datapath-pybench-{os.getpid()}"
    if args.cpp_server:
        cmd = [args.cpp_server, "server", args.name, "--spin", str(args.spin)]
    else:
        cmd = [sys.executable, __file__, "server", args.name, "--mode", args.server_mode or args.mode,
               "--spin", str(args.spin), "--recv-spin", str(args.recv_spin), "--priority", str(args.priority)]
    server = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True)
    if not args.cpp_server:
        server.stdout.readline()  # "ready"
    else:
        time.sleep(0.5)
    try:
        return run_client(args)
    finally:
        try:
            server.wait(5)
        except subprocess.TimeoutExpired:
            server.kill()


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("role", choices=["local", "server", "client"])
    p.add_argument("name", nargs="?")
    p.add_argument("--mode", choices=["recv", "callback"], default="recv")
    p.add_argument("--server-mode", choices=["recv", "callback"], help="local: server mode (default: --mode)")
    p.add_argument("--cpp-server", help="local: path to datapath-benchmark to use as the echo server")
    p.add_argument("--sizes", default="8,64,4096,65536")
    p.add_argument("--count", type=int, default=100000)
    p.add_argument("--runs", type=int, default=5)
    p.add_argument("--spin", type=int, default=50, help="I/O thread and recv() spin in us")
    p.add_argument("--recv-spin", type=int, help="recv() spin in us (default: --spin)")
    p.add_argument("--priority", type=int, default=2)
    args = p.parse_args()
    if args.recv_spin is None:
        args.recv_spin = args.spin
    if args.role != "local" and not args.name:
        p.error("name is required")
    return {"local": run_local, "server": run_server, "client": run_client}[args.role](args)


if __name__ == "__main__":
    sys.exit(main())

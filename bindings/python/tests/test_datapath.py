# Low Latency IPC Library for high-speed traffic
# Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
#
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

import array
import gc
import os
import subprocess
import sys
import textwrap
import threading
import uuid

import pytest

import datapath

T = 5.0  # generous timeout for CI machines


def name() -> str:
    return f"pytest-{os.getpid()}-{uuid.uuid4().hex[:12]}"


@pytest.fixture
def pair():
    """A connected (server, server-side socket, client socket) triple."""
    path = name()
    server = datapath.host(path)
    client = datapath.connect(path, connect_timeout=T)
    peer = server.accept(timeout=T)
    yield server, peer, client
    client.close()
    peer.close()
    server.close()


def test_roundtrip(pair):
    _, peer, client = pair
    client.send(b"hello")
    assert peer.recv(T) == b"hello"
    peer.send(b"world")
    assert client.recv(T) == b"world"


def test_empty_message(pair):
    _, peer, client = pair
    client.send(b"")
    assert peer.recv(T) == b""


@pytest.mark.parametrize("size", [1, 8, 64, 4096, 65536, 1 << 20, 8 << 20])
def test_sizes(pair, size):
    _, peer, client = pair
    data = os.urandom(size)
    client.send(data)
    assert peer.recv(T) == data


def test_bytes_like(pair):
    _, peer, client = pair
    client.send(bytearray(b"ba"))
    client.send(memoryview(b"xxmvxx")[2:4])
    client.send(array.array("B", [1, 2, 3]))
    assert peer.recv(T) == b"ba"
    assert peer.recv(T) == b"mv"
    assert peer.recv(T) == b"\x01\x02\x03"
    with pytest.raises(TypeError):
        client.send("not bytes")  # type: ignore[arg-type]


def test_sendv(pair):
    _, peer, client = pair
    client.sendv([b"head:", bytearray(b"body"), memoryview(b":tail")])
    assert peer.recv(T) == b"head:body:tail"


def test_recv_into(pair):
    _, peer, client = pair
    client.send(b"0123456789")
    small = bytearray(4)
    with pytest.raises(ValueError):
        peer.recv_into(small, T)
    buf = bytearray(16)
    assert peer.recv_into(buf, T) == 10  # message stayed queued
    assert bytes(buf[:10]) == b"0123456789"


def test_order_and_pending(pair):
    _, peer, client = pair
    for i in range(1000):
        client.send(i.to_bytes(4, "little"))
    got = [int.from_bytes(peer.recv(T), "little") for _ in range(1000)]
    assert got == list(range(1000))
    assert peer.pending == 0


def test_timeout(pair):
    _, peer, _ = pair
    with pytest.raises(datapath.TimedOutError) as info:
        peer.recv(0.01)
    assert isinstance(info.value, TimeoutError)
    assert info.value.error == datapath.Error.TimedOut
    with pytest.raises(ValueError):
        peer.recv(-1)


def test_close_drains_then_raises(pair):
    _, peer, client = pair
    assert peer.good and not peer.closed and peer.close_reason is None
    client.send(b"last")
    client.close()
    assert peer.recv(T) == b"last"
    with pytest.raises(datapath.ClosedError) as info:
        peer.recv(T)
    assert isinstance(info.value, ConnectionError)
    assert peer.close_reason == datapath.Error.Closed
    assert peer.closed
    with pytest.raises(datapath.ClosedError):
        client.send(b"x")


def test_context_manager():
    path = name()
    with datapath.host(path) as server:
        with datapath.connect(path, connect_timeout=T) as client:
            with server.accept(T) as peer:
                client.send(b"x")
                assert peer.recv(T) == b"x"
            assert peer.closed
        assert client.closed


def test_callbacks(pair):
    _, peer, client = pair
    got = []
    closed = []
    done = threading.Event()

    def on_message(data: bytes) -> None:
        got.append(data)
        peer.send(data.upper())  # send from inside a handler

    def on_close(reason) -> None:
        closed.append(reason)
        done.set()

    peer.on_message(on_message)
    peer.on_close(on_close)
    client.send(b"abc")
    assert client.recv(T) == b"ABC"
    with pytest.raises(RuntimeError):
        peer.recv(0)
    client.close()
    assert done.wait(T)
    assert got == [b"abc"]
    assert closed == [datapath.Error.Closed]


def test_switch_to_callback_keeps_order(pair):
    _, peer, client = pair
    peer.recv_spin_us = 0
    with pytest.raises(datapath.TimedOutError):
        peer.recv(0)  # arms reads
    for i in range(500):
        client.send(i.to_bytes(4, "little"))
    got = []
    done = threading.Event()

    def on_message(data: bytes) -> None:
        got.append(int.from_bytes(data, "little"))
        if len(got) == 1000:
            done.set()

    peer.on_message(on_message)
    for i in range(500, 1000):
        client.send(i.to_bytes(4, "little"))
    assert done.wait(T)
    assert got == list(range(1000))
    peer.on_message(None)  # back to recv()
    client.send(b"pull")
    assert peer.recv(T) == b"pull"


def test_callback_exception_is_unraisable(pair, monkeypatch):
    _, peer, client = pair
    seen = []
    done = threading.Event()

    def hook(info):
        seen.append(info.exc_type)
        done.set()

    monkeypatch.setattr(sys, "unraisablehook", hook)

    def boom(data: bytes) -> None:
        raise KeyError("boom")

    peer.on_message(boom)
    client.send(b"x")
    assert done.wait(T)
    assert seen == [KeyError]
    peer.on_message(lambda d: peer.send(d))
    client.send(b"still alive")
    assert client.recv(T) == b"still alive"


def test_on_accept_reject():
    path = name()
    server = datapath.host(path, on_accept=lambda sock: False)
    with pytest.raises(datapath.DatapathError):
        datapath.connect(path, connect_timeout=T)
    server.close()


def test_on_accept_keep():
    path = name()
    keep = []

    def on_accept(sock: datapath.Socket) -> None:
        keep.append(sock)
        sock.on_message(lambda d: sock.send(d[::-1]))

    server = datapath.host(path, on_accept=on_accept)
    with pytest.raises(RuntimeError):
        server.accept(0)
    with datapath.connect(path, connect_timeout=T) as client:
        client.send(b"abc")
        assert client.recv(T) == b"cba"
    server.close()


def test_server_close_wakes_accept():
    server = datapath.host(name())
    t = threading.Timer(0.05, server.close)
    t.start()
    with pytest.raises(datapath.ClosedError):
        server.accept(T)
    t.join()


def test_too_large():
    path = name()
    server = datapath.host(path, max_message_size=1024)
    client = datapath.connect(path, connect_timeout=T)
    server.accept(T)
    assert client.max_message_size == 1024
    with pytest.raises(datapath.TooLargeError) as info:
        client.send(b"x" * 2048)
    assert isinstance(info.value, ValueError)
    client.send(b"x" * 1024)


def test_address_in_use():
    path = name()
    server = datapath.host(path)
    with pytest.raises(datapath.AddressInUseError):
        datapath.host(path)
    server.close()


def test_not_found():
    with pytest.raises(datapath.NotFoundError) as info:
        datapath.connect(name(), connect_timeout=0.2)
    assert isinstance(info.value, FileNotFoundError)


def test_custom_context():
    path = name()
    with datapath.Context(spin_us=0, io_threads=2, priority=0) as ctx:
        server = datapath.host(path, context=ctx)
        client = datapath.connect(path, context=ctx, connect_timeout=T)
        peer = server.accept(T)
        client.send(b"ctx")
        assert peer.recv(T) == b"ctx"
    assert ctx.closed
    with pytest.raises(datapath.ClosedError):
        peer.recv(T)
    with pytest.raises(datapath.ClosedError):
        datapath.connect(path, context=ctx)


def test_many_threads_send(pair):
    _, peer, client = pair
    threads, per_thread = 4, 2000

    def sender(t: int) -> None:
        for i in range(per_thread):
            client.send(bytes([t]) + i.to_bytes(4, "little") + b"." * (i % 300))

    workers = [threading.Thread(target=sender, args=(t,)) for t in range(threads)]
    for w in workers:
        w.start()
    last = [-1] * threads
    for _ in range(threads * per_thread):
        msg = peer.recv(T)
        t, i = msg[0], int.from_bytes(msg[1:5], "little")
        assert len(msg) == 5 + i % 300  # whole, never interleaved
        assert i == last[t] + 1  # per-thread order
        last[t] = i
    for w in workers:
        w.join()


def test_reference_cycle_is_collected():
    path = name()
    server = datapath.host(path)
    client = datapath.connect(path, connect_timeout=T)
    peer = server.accept(T)
    assert gc.is_tracked(peer)
    peer.on_message(lambda d: peer.send(d))  # peer -> lambda -> peer
    del peer
    gc.collect()
    with pytest.raises(datapath.ClosedError):
        client.recv(T)


def test_exit_with_live_callbacks():
    """The interpreter exits cleanly while I/O threads are busy calling into Python."""
    code = textwrap.dedent(
        f"""
        import datapath
        path = {name()!r}
        keep = []
        def on_accept(s):
            keep.append(s)
            s.on_message(lambda d: s.send(d))
        server = datapath.host(path, on_accept=on_accept)
        client = datapath.connect(path)
        client.on_message(lambda d: client.send(d))
        client.send(b"ping-pong forever")
        import time; time.sleep(0.2)
        print("exiting")
        """
    )
    out = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, timeout=30)
    assert out.returncode == 0, out.stderr
    assert "exiting" in out.stdout


def test_cross_process():
    path = name()
    server = datapath.host(path)
    code = textwrap.dedent(
        f"""
        import datapath
        with datapath.connect({path!r}, connect_timeout=5) as s:
            for _ in range(100):
                s.send(s.recv(5))
        """
    )
    proc = subprocess.Popen([sys.executable, "-c", code])
    peer = server.accept(T)
    for i in range(100):
        peer.send(i.to_bytes(4, "little"))
        assert peer.recv(T) == i.to_bytes(4, "little")
    assert proc.wait(30) == 0


def test_version_and_enums():
    assert isinstance(datapath.__version__, str)
    assert datapath.Permissions.World != datapath.Permissions.User
    assert int(datapath.Error.Success) == 0


def test_drop_last_reference_inside_callbacks():
    """A socket may be destroyed from its own handler (as in the README example)."""
    path = name()
    clients = []
    closed = threading.Event()

    def on_accept(sock: datapath.Socket) -> None:
        clients.append(sock)

        def on_message(data: bytes) -> None:
            sock.send(data)
            if data == b"bye":
                clients.clear()  # last reference dropped on the I/O thread

        sock.on_message(on_message)
        sock.on_close(lambda reason: closed.set())

    server = datapath.host(path, on_accept=on_accept)
    client = datapath.connect(path, connect_timeout=T)
    client.send(b"hi")
    assert client.recv(T) == b"hi"
    client.send(b"bye")
    assert client.recv(T) == b"bye"
    del on_accept
    gc.collect()  # the dropped socket is in a cycle with its callback
    with pytest.raises(datapath.ClosedError):
        client.recv(T)

    # Dropping the last reference from on_close.
    keep = []
    done = threading.Event()

    def on_accept2(sock: datapath.Socket) -> None:
        keep.append(sock)

        def on_close(reason) -> None:
            keep.clear()
            done.set()

        sock.on_close(on_close)

    server2 = datapath.host(name(), on_accept=on_accept2)
    server.close()
    server2.close()
    with datapath.Context() as ctx:
        path3 = name()
        s3 = datapath.host(path3, on_accept=on_accept2, context=ctx)
        c3 = datapath.connect(path3, context=ctx, connect_timeout=T)
        c3.close()
        assert done.wait(T)
        s3.close()


@pytest.mark.parametrize("server_shm,client_shm", [(True, True), (False, True), (True, False), (False, False)])
def test_shared_memory_option(server_shm, client_shm):
    # Shared memory is on by default; either side turning it off falls back to
    # the pipe or socket, and messages arrive intact either way.
    path = name()
    server = datapath.host(path, shared_memory=server_shm, shared_memory_size=1 << 16)
    client = datapath.connect(path, connect_timeout=T, shared_memory=client_shm)
    peer = server.accept(timeout=T)
    try:
        data = os.urandom(200_000)  # larger than the 64 KiB ring
        client.send(data)
        assert peer.recv(T) == data
        peer.send(b"back")
        assert client.recv(T) == b"back"
    finally:
        client.close()
        peer.close()
        server.close()

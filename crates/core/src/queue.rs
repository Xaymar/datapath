// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! Bounded, coalescing, multi-producer send queue with a single writer slot.
//!
//! Any number of threads [`push`](SendQueue::push) whole messages. Each message
//! is framed and queued atomically, so messages never interleave on the wire.
//! Exactly one pusher at a time is told it holds the *writer slot*; that caller
//! drains the queue with [`take`](SendQueue::take) and writes the blocks to the
//! transport until `take` returns `false`. Small messages are packed together
//! into blocks of up to [`COALESCE_LIMIT`] bytes, so a burst becomes one write.

use crate::error::Error;
use crate::proto::{encode_frame_header, FRAME_HEADER_SIZE};
use std::collections::VecDeque;
use std::sync::{Mutex, MutexGuard, PoisonError};

/// Small frames are packed together into blocks up to this size.
pub const COALESCE_LIMIT: usize = 64 * 1024;
/// How many small blocks are kept for reuse.
const MAX_FREE_BLOCKS: usize = 4;

struct Inner {
    blocks: VecDeque<Vec<u8>>,
    free: Vec<Vec<u8>>,
    // One recycled large block, reused while large messages keep coming.
    spare_large: Vec<u8>,
    queued: u64,
    limit: u64,
    max_queued: u64,
    writing: bool,
    closed: bool,
}

/// See the [module documentation](self).
pub struct SendQueue {
    inner: Mutex<Inner>,
}

fn append_frame(block: &mut Vec<u8>, total: u64, parts: &[&[u8]]) {
    block.extend_from_slice(&encode_frame_header(total));
    for part in parts {
        block.extend_from_slice(part);
    }
}

impl Inner {
    fn new_block(&mut self, reserve: usize) -> Result<Vec<u8>, Error> {
        let mut block = if reserve <= COALESCE_LIMIT {
            self.free.pop().unwrap_or_default()
        } else {
            Vec::new()
        };
        block.clear();
        block.try_reserve(reserve).map_err(|_| Error::OutOfMemory)?;
        Ok(block)
    }

    fn recycle(&mut self, mut buffer: Vec<u8>) {
        if buffer.capacity() > 0
            && buffer.capacity() <= COALESCE_LIMIT
            && self.free.len() < MAX_FREE_BLOCKS
        {
            buffer.clear();
            self.free.push(buffer);
        }
    }
}

impl SendQueue {
    /// A queue that rejects messages above `max_message_size` and refuses new
    /// messages while more than `max_queued_bytes` wait in it.
    pub fn new(max_message_size: u64, max_queued_bytes: u64) -> SendQueue {
        SendQueue {
            inner: Mutex::new(Inner {
                blocks: VecDeque::new(),
                free: Vec::new(),
                spare_large: Vec::new(),
                queued: 0,
                limit: max_message_size,
                max_queued: max_queued_bytes,
                writing: false,
                closed: false,
            }),
        }
    }

    // No code path panics while holding the lock, and the state stays consistent
    // if a caller's panic unwinds through it, so poisoning is ignored.
    fn lock(&self) -> MutexGuard<'_, Inner> {
        self.inner.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// Frame and enqueue one message made of `parts`, concatenated. Atomic: either
    /// the whole message is queued or nothing is.
    ///
    /// Returns `Ok(true)` for exactly one caller while nobody holds the writer
    /// slot: that caller must drain the queue with [`take`](Self::take) until it
    /// returns `false`.
    ///
    /// Errors: [`Error::Closed`] after [`close`](Self::close), [`Error::TooLarge`]
    /// above the message limit, [`Error::WouldBlock`] when the queue is full (a
    /// message is always accepted into an empty queue) and
    /// [`Error::OutOfMemory`].
    pub fn push(&self, parts: &[&[u8]]) -> Result<bool, Error> {
        let total: u64 = parts.iter().map(|p| p.len() as u64).sum();
        let frame = total
            .checked_add(FRAME_HEADER_SIZE as u64)
            .ok_or(Error::TooLarge)?;

        let mut inner = self.lock();
        if inner.closed {
            return Err(Error::Closed);
        }
        if total > inner.limit {
            return Err(Error::TooLarge);
        }
        if inner.queued > 0 && inner.queued.saturating_add(frame) > inner.max_queued {
            return Err(Error::WouldBlock);
        }

        if frame <= COALESCE_LIMIT as u64 {
            if inner.spare_large.capacity() > 0 {
                // Back to small traffic: release it.
                inner.spare_large = Vec::new();
            }
            // Small: pack into the tail block when it has room, all under the lock.
            let frame_len = frame as usize;
            let needs_new = match inner.blocks.back() {
                None => true,
                Some(b) => b.len() + frame_len > COALESCE_LIMIT || b.capacity() > COALESCE_LIMIT,
            };
            if needs_new {
                let block = inner.new_block(COALESCE_LIMIT)?;
                inner.blocks.push_back(block);
            }
            let tail = inner.blocks.back_mut().expect("a block was just ensured");
            append_frame(tail, total, parts);
            inner.queued += frame;
        } else {
            // Large: reserve queue space, copy outside the lock, then publish.
            let frame_len = usize::try_from(frame).map_err(|_| Error::TooLarge)?;
            inner.queued += frame;
            let mut block = if inner.spare_large.capacity() >= frame_len {
                std::mem::take(&mut inner.spare_large)
            } else {
                Vec::new()
            };
            drop(inner);

            block.clear();
            let reserved = block.try_reserve_exact(frame_len);
            if reserved.is_ok() {
                append_frame(&mut block, total, parts);
            }

            inner = self.lock();
            if reserved.is_err() {
                inner.queued = inner.queued.saturating_sub(frame);
                return Err(Error::OutOfMemory);
            }
            if inner.closed {
                return Err(Error::Closed);
            }
            inner.blocks.push_back(block);
        }

        if inner.writing {
            Ok(false)
        } else {
            inner.writing = true;
            Ok(true)
        }
    }

    /// Writer only. Moves the next block into `batch` and returns `true`; the
    /// storage `batch` held before is recycled. Returns `false` and releases the
    /// writer slot when the queue is empty.
    pub fn take(&self, batch: &mut Vec<u8>) -> bool {
        let mut inner = self.lock();
        let old = std::mem::take(batch);
        if old.capacity() > COALESCE_LIMIT {
            if !inner.closed {
                inner.spare_large = old;
            }
        } else {
            inner.recycle(old);
        }
        if inner.closed || inner.blocks.is_empty() {
            inner.writing = false;
            return false;
        }
        let block = inner.blocks.pop_front().expect("checked non-empty");
        inner.queued = inner.queued.saturating_sub(block.len() as u64);
        *batch = block;
        true
    }

    /// Try to acquire the writer slot without pushing, to resume writing (for
    /// example after the transport reported it can take more data).
    pub fn try_acquire(&self) -> bool {
        let mut inner = self.lock();
        if inner.writing || inner.closed {
            return false;
        }
        inner.writing = true;
        true
    }

    /// Hand a drained block back for reuse.
    pub fn recycle(&self, buffer: Vec<u8>) {
        self.lock().recycle(buffer);
    }

    /// Change the largest accepted message (after the handshake negotiated it).
    pub fn set_limit(&self, max_message_size: u64) {
        self.lock().limit = max_message_size;
    }

    /// The largest accepted message.
    pub fn limit(&self) -> u64 {
        self.lock().limit
    }

    /// Drop everything queued; later pushes return [`Error::Closed`].
    pub fn close(&self) {
        let mut inner = self.lock();
        inner.closed = true;
        inner.blocks.clear();
        inner.free.clear();
        inner.spare_large = Vec::new();
        inner.queued = 0;
    }

    /// Whether [`close`](Self::close) was called.
    pub fn closed(&self) -> bool {
        self.lock().closed
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proto::FrameParser;
    use std::sync::Arc;

    fn drain(q: &SendQueue) -> Vec<u8> {
        let mut out = Vec::new();
        let mut batch = Vec::new();
        while q.take(&mut batch) {
            out.extend_from_slice(&batch);
        }
        out
    }

    fn parse(wire: &[u8]) -> Vec<Vec<u8>> {
        let mut parser = FrameParser::new(u64::MAX, 4096);
        let mut out = Vec::new();
        let mut pos = 0;
        while pos < wire.len() {
            let room = parser.prepare();
            let n = room.len().min(wire.len() - pos);
            room[..n].copy_from_slice(&wire[pos..pos + n]);
            pos += n;
            parser
                .commit(n, |m| {
                    out.push(m.to_vec());
                    true
                })
                .unwrap();
        }
        out
    }

    #[test]
    fn pushes_come_out_framed_in_order() {
        let q = SendQueue::new(1 << 20, 1 << 20);
        assert_eq!(q.push(&[b"hello"]), Ok(true));
        assert_eq!(q.push(&[b"", b"wor", b"ld"]), Ok(false));
        assert_eq!(q.push(&[b""]), Ok(false));
        assert_eq!(
            parse(&drain(&q)),
            [b"hello".to_vec(), b"world".to_vec(), Vec::new()]
        );
    }

    #[test]
    fn small_messages_are_coalesced_into_one_block() {
        let q = SendQueue::new(1 << 20, 1 << 20);
        for _ in 0..100 {
            q.push(&[&[1u8; 10]]).unwrap();
        }
        let mut batch = Vec::new();
        assert!(q.take(&mut batch));
        assert_eq!(batch.len(), 100 * (10 + FRAME_HEADER_SIZE));
        assert!(!q.take(&mut batch));
    }

    #[test]
    fn a_full_block_starts_the_next_one() {
        let q = SendQueue::new(1 << 20, 1 << 20);
        let msg = vec![0u8; 20_000];
        for _ in 0..4 {
            q.push(&[&msg]).unwrap();
        }
        let mut sizes = Vec::new();
        let mut batch = Vec::new();
        while q.take(&mut batch) {
            sizes.push(batch.len());
        }
        let frame = 20_000 + FRAME_HEADER_SIZE;
        assert_eq!(sizes, [3 * frame, frame]);
    }

    #[test]
    fn writer_slot_is_handed_out_once() {
        let q = SendQueue::new(1 << 20, 1 << 20);
        assert_eq!(q.push(&[b"a"]), Ok(true));
        assert_eq!(q.push(&[b"b"]), Ok(false));
        assert!(!q.try_acquire(), "the slot is held");
        let mut batch = Vec::new();
        assert!(q.take(&mut batch));
        assert!(!q.take(&mut batch), "empty: slot released");
        assert!(q.try_acquire());
        assert!(!q.try_acquire());
        assert!(!q.take(&mut batch));
        assert_eq!(
            q.push(&[b"c"]),
            Ok(true),
            "released slot is handed out again"
        );
    }

    #[test]
    fn message_limit_is_enforced_and_adjustable() {
        let q = SendQueue::new(4, 1 << 20);
        assert_eq!(q.push(&[b"12345"]), Err(Error::TooLarge));
        assert_eq!(q.push(&[b"12", b"34"]), Ok(true));
        q.set_limit(100);
        assert_eq!(q.limit(), 100);
        assert_eq!(q.push(&[b"12345"]), Ok(false));
    }

    #[test]
    fn full_queue_returns_would_block_but_accepts_into_an_empty_one() {
        let q = SendQueue::new(1 << 20, 100);
        // Larger than the queue bound, but the queue is empty: accepted.
        assert_eq!(q.push(&[&[0u8; 200]]), Ok(true));
        assert_eq!(q.push(&[b"x"]), Err(Error::WouldBlock));
        drain(&q);
        assert_eq!(q.push(&[b"x"]), Ok(true));

        // Two 48-byte frames fit a 100-byte bound; a third would make 144.
        let q = SendQueue::new(1 << 20, 100);
        assert_eq!(q.push(&[&[0u8; 40]]), Ok(true));
        assert_eq!(q.push(&[&[0u8; 40]]), Ok(false));
        assert_eq!(q.push(&[&[0u8; 40]]), Err(Error::WouldBlock));
    }

    #[test]
    fn close_drops_everything_and_rejects_pushes() {
        let q = SendQueue::new(1 << 20, 1 << 20);
        q.push(&[b"x"]).unwrap();
        q.close();
        assert!(q.closed());
        assert_eq!(q.push(&[b"y"]), Err(Error::Closed));
        let mut batch = Vec::new();
        assert!(!q.take(&mut batch));
        assert!(!q.try_acquire());
    }

    #[test]
    fn large_messages_are_queued_whole_and_recycled() {
        let q = SendQueue::new(1 << 30, 1 << 30);
        let big: Vec<u8> = (0..300_000u32).map(|i| (i % 253) as u8).collect();
        assert_eq!(q.push(&[&big]), Ok(true));
        let mut batch = Vec::new();
        assert!(q.take(&mut batch));
        assert_eq!(batch.len(), big.len() + FRAME_HEADER_SIZE);
        assert_eq!(&batch[FRAME_HEADER_SIZE..], &big[..]);
        // The next take hands the large block back for the next large message.
        assert!(!q.take(&mut batch));
        assert!(batch.is_empty());
        assert_eq!(q.push(&[&big]), Ok(true));
        assert_eq!(parse(&drain(&q)), [big]);
    }

    #[test]
    fn concurrent_senders_never_interleave() {
        const THREADS: usize = 8;
        const PER_THREAD: usize = 400;
        let q = Arc::new(SendQueue::new(1 << 30, 1 << 30));
        let wire = Arc::new(Mutex::new(Vec::new()));

        let handles: Vec<_> = (0..THREADS)
            .map(|t| {
                let q = q.clone();
                let wire = wire.clone();
                std::thread::spawn(move || {
                    for seq in 0..PER_THREAD {
                        // Varied sizes, with an occasional message above the coalescing size.
                        let len = if seq % 100 == 99 {
                            100_000
                        } else {
                            3 + (seq * 37) % 900
                        };
                        let mut msg = vec![t as u8; len];
                        msg[1..3].copy_from_slice(&(seq as u16).to_le_bytes());
                        if q.push(&[&msg]).unwrap() {
                            let mut batch = Vec::new();
                            while q.take(&mut batch) {
                                wire.lock().unwrap().extend_from_slice(&batch);
                            }
                        }
                    }
                })
            })
            .collect();
        for h in handles {
            h.join().unwrap();
        }
        // Anything pushed after the last writer finished is still queued.
        let mut wire = Arc::try_unwrap(wire).unwrap().into_inner().unwrap();
        wire.extend_from_slice(&drain(&q));

        let frames = parse(&wire);
        assert_eq!(frames.len(), THREADS * PER_THREAD);
        let mut next = [0u16; THREADS];
        for f in frames {
            let t = f[0] as usize;
            assert!(
                f.iter()
                    .enumerate()
                    .all(|(i, &b)| (1..3).contains(&i) || b == t as u8),
                "interleaved"
            );
            let seq = u16::from_le_bytes([f[1], f[2]]);
            assert_eq!(seq, next[t], "per-thread order");
            next[t] += 1;
        }
    }
}
